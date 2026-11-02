// light_all.frag at half resolution. One fragment per 2x2 block of the view: it shades the top-left full-resolution pixel of its block (the
// sample probe.frag uses in its shrink mode, so the resolve can share one set of weights), and light_resolve.frag brings the sums back to full
// resolution with depth and normal aware weights. The shadow dither runs on the half-resolution grid (frag = gl_FragCoord.xy) so the 2x2 PCF
// dither still alternates between neighbouring samples.
// Evaluated in world space: every length and dot product the light terms use is the same in world and view space (the view transform is a
// rotation, a reflection for mirrors, and a translation). Moving each light into view space for every pixel (a 4x4 and a 3x3 transform per
// light) kept the view matrix in registers through the loop; here the pixel is moved into world space once (position, normal, view direction)
// and the loop needs no matrix. The parts of a light needed only once it reaches the pixel (scales, shadow cone) are read after the reach
// test. Measured on a Switch Lite (hallway, 1024x576): the light pass 6.4 -> 5.5 ms.
#define PT_LIGHT_HOIST
#include "common.glsl"
#include "lighting.glsl"

#ifdef PT_LIGHT_HALF_DETAIL
#error "light_half_main.glsl: PT_LIGHT_HALF_DETAIL (normal detail for the half-resolution sums) is not ported to the world-space evaluation"
#endif

struct LightEval {
    vec4 position;
    vec4 color;
    vec4 direction;
    vec4 range;
    vec4 cone;
    vec4 shadow_cone;
    vec4 scales;
    ivec4 info;
};
layout(std140, set = 1, binding = 6) uniform LightEvalData {
    LightEval eval[256];
} light_eval;

layout(push_constant) uniform PassPush {
    uvec4 ids;      // x: view index, y: lights in this draw, z: measurement flags
    uvec4 list[7];  // two lights per word (low half first, 8 per uvec4): bits 0-11 the light index, bit 12 shadows on
} pass;

layout(location = 0) out vec4 out_diffuse;
layout(location = 1) out vec4 out_specular;

struct SurfaceW {
    vec3 world;
    vec3 N;  // world space
    vec3 V;  // world space, towards the eye
    vec3 R;  // reflect(-V, N)
    float roughness;
    float specular;
    float translucency;
    vec4 mat0;
    vec4 mat1;
};

// PointShadow of lighting.glsl from the world position: its receiver was the view position moved along the view's z axis, then taken to
// world space, which is the world position moved along inv_view's third column
float PointShadowW(Light l, View v, vec3 world, vec3 frag) {
    vec3 biased = world + v.inv_view[2].xyz * (l.direction.w * l.cone.w);
    vec3 d = (LT_SHADOW(l) * vec4(biased, 1.0)).xyz;
    float len = max(length(d), 1.0e-5);
    float s = d.z <= 0.0 ? -1.0 : 1.0;
    vec2 p = vec2(-d.x, d.y) / (len + abs(d.z));
    vec2 local = vec2(0.5 * ((p.x * 0.99902344 + s) * 0.5) + 0.5, 0.5 - 0.5 * p.y * 0.99902344);
    float zref = 1.0 - len * l.cone.z;
    vec2 texel = 1.0 / ImgSize(IMG_SHADOW);
    vec2 uv = LT_RECT(l).xy + local * LT_RECT(l).zw;
    float half_width = LT_RECT(l).z * 0.5;
    vec2 lo = vec2(LT_RECT(l).x + (s > 0.0 ? half_width : 0.0), LT_RECT(l).y) + texel;
    vec2 hi = vec2(LT_RECT(l).x + (s > 0.0 ? LT_RECT(l).z : half_width), LT_RECT(l).y + LT_RECT(l).w) - texel;
    float sh = 1.0 - 0.25 * ShadowPcf(uv, zref, lo, hi, texel, frag, v.jitter.w, v.exposure.w, v.temporal);
    return sh * sh;
}

// EvaluateLight of lighting.glsl in world space; l carries position, color, direction, range, cone and info (the rest is read here by index)
bool EvaluateLightW(Light l, uint index, View v, SurfaceW s, bool shadows, vec2 frag, out vec3 diffuse, out vec3 specular) {
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    if (l.info.z != 0) {
        vec3 q = (LT_AREA(l) * vec4(s.world, 1.0)).xyz;
        if (1.0 - max(abs(q.z), max(abs(q.x), abs(q.y))) < 0.0) {
            return false;
        }
    }
    bool spot = l.position.w > 0.5;
    vec3 lv = l.position.xyz - s.world;
    float dist = length(lv);
    vec3 L = lv / max(dist, 1.0e-6);
    float d = spot ? max(l.range.y, dist + l.range.x) : max(l.range.y, dist) + l.range.x;
    float d2 = d * d;
    float att = max(0.0, 1.0 / d2 - d2 * l.range.z);
    vec3 radiance = l.color.rgb * att * v.exposure.x;
    float cos_a = 1.0;
    if (spot) {
        cos_a = dot(l.direction.xyz, -L);
        radiance *= pow(abs(Saturate((cos_a - l.cone.x) * l.cone.y)), l.cone.z);
    }
    if (length(radiance) <= 1.0e-4) {
        return false;
    }
    vec4 scales = light_eval.eval[index].scales;
    if (LIGHT_FLAG(8u)) {
        diffuse = radiance * scales.y;
        return true;
    }
    float shadow = 1.0;
#ifndef PT_LIGHT_LEAN
    if (l.info.x >= 0 && shadows && !LIGHT_FLAG(4u)) {
        const vec3 dither_frag = vec3(frag, v.exposure.w);
        if (spot) {
            vec4 sc4 = light_eval.eval[index].shadow_cone;
            float sc = Saturate((cos_a - sc4.x) * sc4.y);
            shadow = SpotShadow(l, v, s.world, sc * sc, dither_frag);
        } else {
            shadow = PointShadowW(l, v, s.world, dither_frag);
        }
    }
#endif
    float vis = 1.0 + scales.z * (shadow - 1.0);
    vec3 mask = vec3(1.0);
#ifndef PT_LIGHT_LEAN
    if ((l.info.y >= 0 || l.info.w > 0) && !LIGHT_FLAG(2u)) {
        vec4 c = LT_MASK(l) * vec4(s.world, 1.0);
        if (c.w > 0.0) {
            vec2 uv = vec2(0.5 * c.x / c.w + 0.5, 1.0 - (0.5 * c.y / c.w + 0.5));
            if (l.info.w > 0) {
                mask = textureLod(textures[nonuniformEXT(uint(l.info.w - 1))], uv, 0.0).rgb;
            } else {
                mask = ImgLod(uint(l.info.y), SMP_LINEAR_WRAP, uv, 0.0).rgb;
            }
        } else {
            mask = vec3(0.0);
        }
    }
#endif
    vec3 N = s.N;
    vec3 V = s.V;
    vec3 to_ray = s.R - L;
    float k = Saturate(Saturate(0.9 * l.color.w / d) / max(length(to_ray), 1.0e-6));
    vec3 Ls = normalize(L + to_ray * k);
    vec3 H = normalize(Ls + V);
    float NdotH = Saturate(dot(N, H));
    float NdotL = dot(N, L);
    float LdotH = dot(H, L);
    if (LIGHT_FLAG(1u)) {
        float t = s.translucency;
        diffuse = radiance * scales.y * Saturate(max(NdotL + t * (1.0 - NdotL * NdotL), t * s.mat0.w)) * vis * mask;
        return true;
    }
    vec3 lut_uv = vec3(NdotH * NdotH * 0.984375 + 0.0078125, s.roughness * s.roughness * 0.9375 + 0.03125, s.mat1.w * s.mat1.w * 0.9375 + 0.03125);
    vec2 ndf = texture(sampler3D(lut2_image, samplers[SMP_LINEAR_CLAMP]), lut_uv).xy;
    float D = ndf.x * ndf.x / max(ndf.y * ndf.y, 1.0e-8);
    float F = Img(RES_LUT1, SMP_LINEAR_CLAMP, vec2(LdotH, s.mat1.x)).x;
    float spec = Saturate(4.0 * NdotL) * F * D;
    float w = s.translucency;
    float diff = Saturate(max(NdotL + w * (1.0 - NdotL * NdotL), w * s.mat0.w));
    diffuse = radiance * scales.y * diff * vis * mask;
    specular = radiance * scales.x * s.specular * s.mat0.rgb * spec * vis * mask;
    return true;
}

void main() {
    g_light_flags = pass.ids.z;
    View v = frame.views[pass.ids.x];
    ivec2 full = ivec2(ImgSize(IMG_DEPTH));
    ivec2 pixel = min(ivec2(gl_FragCoord.xy) * 2, full - 1);
    vec2 frag_full = vec2(pixel) + 0.5;
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    if (depth <= 0.0) {
        discard;
    }
    vec3 P = ViewPosition(v, PixelNdc(v, frag_full), depth);
    vec4 g_normal = ImgFetch(IMG_NORMAL, pixel);
    vec4 g_material = ImgFetch(IMG_MATERIAL, pixel);
    mat3 to_world = mat3(v.inv_view);
    SurfaceW s;
    vec3 eye_to_p = to_world * P;
    s.world = eye_to_p + v.inv_view[3].xyz;
    s.N = to_world * DecodeNormal(g_normal.xyz);
    s.V = -normalize(eye_to_p);
    s.R = reflect(-s.V, s.N);
    s.roughness = g_material.x;
    s.specular = g_material.y;
    s.translucency = g_material.w;
    s.mat0 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(g_material.z, 0.25));
    s.mat1 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(g_material.z, 0.75));
    vec3 diffuse_sum = vec3(0.0);
    vec3 specular_sum = vec3(0.0);
    for (uint i = 0u; i < pass.ids.y; ++i) {
        uint word = pass.list[i >> 3u][(i >> 1u) & 3u];
        uint entry = (i & 1u) != 0u ? (word >> 16u) : (word & 0xFFFFu);
        uint index = entry & 0xFFFu;
        Light l;
        l.position = light_eval.eval[index].position;
        l.color = light_eval.eval[index].color;
        l.direction = light_eval.eval[index].direction;
        l.range = light_eval.eval[index].range;
        l.cone = light_eval.eval[index].cone;
        l.info = light_eval.eval[index].info;
        g_light_index = index;
        vec3 diffuse;
        vec3 specular;
        if (EvaluateLightW(l, index, v, s, (entry & 0x1000u) != 0u, gl_FragCoord.xy, diffuse, specular)) {
            diffuse_sum += diffuse;
            specular_sum += specular;
        }
    }
    out_diffuse = vec4(diffuse_sum, 1.0);
    out_specular = vec4(specular_sum, 1.0);
}
