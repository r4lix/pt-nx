// probe_resolve.frag extended for half-resolution lights (experiment light-algo-halfres). One full-screen pass writes the full-resolution
// diffuse and specular targets: the probe term exactly as probe_resolve.frag computes it, plus the half-resolution light sums of
// light_all_half.frag brought up with the same four neighbours. The probe weights are kept bit for bit (bilinear / |depth difference|);
// the light weights are bilinear times a relative-depth term and a normal term, so the lights neither bleed across silhouettes nor go
// blocky on slanted planes (the probe weight is close to nearest-depth there, invisible for SH irradiance but not for shadows/specular).
#include "common.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;  // x: probe mode (0 half-res encode at p, 1 shrink upsample, 2 legacy copy, 3 no probes), yz: half-res size, w: bit 0 half-res lights present
    vec4 f0;    // x: relative depth sharpness (0 = default 40), y: normal power (0 = default 8)
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) out vec4 out_diffuse;
layout(location = 1) out vec4 out_specular;

vec3 Encode(vec4 acc) {
    float t = clamp(acc.a, 0.0, 1.0);
    float coverage = 1.0 - (t <= 0.89990234 ? t : 1.0);
    return coverage > 0.0 ? acc.rgb / coverage : vec3(0.0);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint mode = pass.ids.x;
    bool lights = (pass.ids.w & 1u) != 0u;
    float centre = ImgFetch(IMG_DEPTH, p).x;
    if (centre <= 0.0) {
        // sky: compose never reads the light targets there
        out_diffuse = vec4(0.0, 0.0, 0.0, 1.0);
        out_specular = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 probe = vec3(0.0);
    if (mode == 2u) {
        probe = ImgFetch(IMG_PROBE_ACC, p).rgb;
    } else if (mode == 0u) {
        probe = Encode(ImgFetch(IMG_PROBE_ACC, p));
    }
    if (mode == 4u) {
        // probes accumulated at a quarter of the resolution: the same depth-weighted upsample as the half-resolution mode, over 4x4 pixel blocks
        ivec2 full = ivec2(ImgSize(IMG_DEPTH));
        ivec2 quarter = (full + 3) >> 2;
        ivec2 qi = p >> 2;
        vec2 qf = vec2(p & 3) * 0.25;
        vec3 qsum = vec3(0.0);
        float qtotal = 0.0;
        for (int k = 0; k < 4; ++k) {
            ivec2 o = ivec2(k & 1, k >> 1);
            ivec2 t = min(qi + o, quarter - 1);
            float table = (o.x == 1 ? qf.x : 1.0 - qf.x) * (o.y == 1 ? qf.y : 1.0 - qf.y);
            float d = ImgFetch(IMG_DEPTH, min(t * 4, full - 1)).x;
            float w = table / (abs(d - centre) + 1e-6);
            qsum += w * Encode(ImgFetch(IMG_PROBE_ACC, t));
            qtotal += w;
        }
        probe = qtotal > 0.0 ? qsum / qtotal : vec3(0.0);
    }
    bool probe_up = mode == 1u;
    if (!probe_up && !lights) {
        out_diffuse = vec4(probe, 1.0);
        out_specular = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    ivec2 half_size = ivec2(pass.ids.yz);
    ivec2 full_size = ivec2(ImgSize(IMG_DEPTH));
    ivec2 i = p >> 1;
    vec2 f = vec2(p & 1) * 0.5;
    float sharp = pass.f0.x > 0.0 ? pass.f0.x : 40.0;
    float npow = pass.f0.y > 0.0 ? pass.f0.y : 8.0;
    vec3 nc = DecodeNormal(ImgFetch(IMG_NORMAL, p).xyz);
#ifdef PT_LIGHT_HALF_DETAIL
    // translucent pixels wrap the diffuse term around the terminator; the N.L ratio below does not model that, so they keep the plain sum
    bool detail = ImgFetch(IMG_MATERIAL, p).w <= 0.0;
#endif
    vec3 probe_sum = vec3(0.0);
    float probe_total = 0.0;
    vec3 dsum = vec3(0.0);
    vec3 ssum = vec3(0.0);
    float ltotal = 0.0;
    vec3 dnear = vec3(0.0);
    vec3 snear = vec3(0.0);
    float best = 1.0e30;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 t = min(i + o, half_size - 1);
        float table = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        ivec2 tp = min(t * 2, full_size - 1);
        float d = ImgFetch(IMG_DEPTH, tp).x;
        if (probe_up) {
            float w = table / (abs(d - centre) + 1e-6);
            probe_sum += w * Encode(ImgFetch(IMG_PROBE_ACC, t));
            probe_total += w;
        }
        if (lights) {
            // reverse Z: view z is c / depth, so |z_t / z_c - 1| = |centre - d| / d
            float rel = abs(centre - d) / max(d, 1.0e-9);
            float wd = 1.0 / (1.0 + (rel * sharp) * (rel * sharp));
            vec3 nt = DecodeNormal(ImgFetch(IMG_NORMAL, tp).xyz);
            float wn = pow(Saturate(dot(nc, nt) * 0.5 + 0.5), npow);
            float w = table * wd * wn * (d > 0.0 ? 1.0 : 0.0);
            vec3 ld = ImgFetch(IMG_LIGHT_HALF_D, t).rgb;
            vec3 ls = ImgFetch(IMG_LIGHT_HALF_S, t).rgb;
#ifdef PT_LIGHT_HALF_DETAIL
            // the half-resolution sum was shaded with the sample's normal nt; move it to this pixel's normal along the dominant direction
            vec3 dir = ImgFetch(IMG_LIGHT_HALF_L, t).xyz;
            float dir_len = length(dir);
            if (detail && dir_len > 1.0e-6) {
                vec3 Ld = dir / dir_len;
                float ratio = (Saturate(dot(nc, Ld)) + 0.1) / (Saturate(dot(nt, Ld)) + 0.1);
                ld *= clamp(ratio, 0.25, 4.0);
            }
#endif
            dsum += w * ld;
            ssum += w * ls;
            ltotal += w;
            float score = d > 0.0 ? rel : 1.0e30;
            if (score < best) {
                best = score;
                dnear = ld;
                snear = ls;
            }
        }
    }
    if (probe_up) {
        probe = probe_total > 0.0 ? probe_sum / probe_total : vec3(0.0);
    }
    vec3 light_d = vec3(0.0);
    vec3 light_s = vec3(0.0);
    if (lights) {
        // when every neighbour is rejected (a thin feature only this full-res pixel sees) fall back to the nearest one in depth
        bool ok = ltotal > 1.0e-3;
        light_d = ok ? dsum / ltotal : dnear;
        light_s = ok ? ssum / ltotal : snear;
    }
    out_diffuse = vec4(probe + light_d, 1.0);
    out_specular = vec4(light_s, 1.0);
}
