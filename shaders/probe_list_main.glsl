#include "common.glsl"

// All of a view's probes in one full-screen pass (one draw per 56 probes), the same light as probe.frag drawing one box per probe. The depth and
// normal fetch, the position and the sphere-map setup are done once per pixel instead of once per probe: SphereMapLookup (probe_main.glsl)
// is linear in the SH coefficients, so its four bilinear texels fold into nine blended basis values, and a probe that reaches the pixel costs
// nine multiply-adds. The blend of BlendMode::ProbeAccumulate runs in order in the shader: light = max(0, w e) + light t, transmittance *= t
// with t = max(0, 1 - w); the output then blends onto the earlier draws the same way.
// PT_PROBE_SUBGROUP (probe_list_sg.frag): the pixels of a subgroup span a world box, and a probe whose box misses it reaches none of them.
// The lanes test the listed probes against that box side by side, and the subgroup then walks only the probes that passed, in list order
// (entry k is bit k of a 64-bit mask: at most 56 entries a draw).
layout(push_constant) uniform ProbeListPush {
    uvec4 ids;      // x: view, y: probes in this draw (at most 56), z: 0 full, 1 half, 2 quarter resolution accumulation
    uvec4 list[7];  // frame.probes indices in draw order, 16 bits each, two to a word
} pass;

layout(location = 0) out vec4 out_diffuse;

uint ListEntry(uint k) {
    uint word = pass.list[k >> 3u][(k >> 1u) & 3u];
    return (k & 1u) != 0u ? word >> 16u : word & 0xFFFFu;
}

vec3 SphereMapDirection(vec2 t) {
    float len2 = dot(t, t);
    float z = min(1.0, 2.0 * len2 - 1.0);
    vec2 xy = len2 > 0.0 ? vec2(-t.x, t.y) * inversesqrt(len2) * sqrt(max(0.0, 1.0 - z * z)) : vec2(0.0);
    return vec3(xy, z);
}

// basis values 1-8 of EvaluateSh (value 0 is 1) at the probe-space direction of a 16x16 sphere-map texel
void TexelBasis(View v, ivec2 texel, out vec4 lo, out vec4 hi) {
    vec2 s = (vec2(texel) + 0.5) / 16.0;
    vec3 d = SphereMapDirection(vec2(2.0 * s.x - 1.0, 1.0 - 2.0 * s.y));
    vec3 w = mat3(v.inv_view) * d;
    vec3 n = vec3(-w.x, -w.z, w.y);
    lo = vec4(n.y, n.z, n.x, n.x * n.y);
    hi = vec4(n.y * n.z, 3.0 * n.z * n.z - 1.0, n.x * n.z, n.x * n.x - n.y * n.y);
}

// one probe of the list onto the pixel's light and transmittance
void AddProbe(uint index, vec3 world, vec4 lo, vec4 hi, float exposure, inout vec3 light, inout float transmittance, inout bool reached) {
    vec3 q = (frame.probes[index].box * vec4(world, 1.0)).xyz;
    vec4 positive = frame.probes[index].positive;
    vec3 negative = frame.probes[index].negative.xyz;
    vec3 fq = min(clamp((1.0 - q) * positive.xyz, 0.0, 1.0), clamp((1.0 + q) * negative, 0.0, 1.0));
    float weight = positive.w * fq.x * fq.y * fq.z;
    if (weight <= 0.0) {
        return;
    }
    vec3 e = frame.probes[index].sh[0].rgb + frame.probes[index].sh[1].rgb * lo.x + frame.probes[index].sh[2].rgb * lo.y +
             frame.probes[index].sh[3].rgb * lo.z + frame.probes[index].sh[4].rgb * lo.w + frame.probes[index].sh[5].rgb * hi.x +
             frame.probes[index].sh[6].rgb * hi.y + frame.probes[index].sh[7].rgb * hi.z + frame.probes[index].sh[8].rgb * hi.w;
    float t = max(0.0, 1.0 - weight);
    light = max(vec3(0.0), weight * (e * exposure)) + light * t;
    transmittance *= t;
    reached = true;
}

void main() {
    View v = frame.views[pass.ids.x];
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 frag = gl_FragCoord.xy;
    if (pass.ids.z != 0u) {
        pixel *= pass.ids.z == 2u ? 4 : 2;
        frag = vec2(pixel) + 0.5;
    }
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    bool sky = depth <= 0.0;
#ifdef PT_PROBE_SUBGROUP
    // a sky pixel stays to test its share of the list (below) and is discarded at the end; only an all-sky subgroup leaves here
    if (subgroupAll(sky)) {
        discard;
    }
#else
    if (sky) {
        discard;
    }
#endif
    vec3 P = ViewPosition(v, PixelNdc(v, frag), depth);
    vec3 world = (v.inv_view * vec4(P, 1.0)).xyz;
    vec3 N = DecodeNormal(ImgFetch(IMG_NORMAL, pixel).xyz);

    float r = 0.40625 * sqrt(max(0.0, 0.5 * N.z + 0.5));
    float len = length(N.xy);
    vec2 dir = len > 0.0 ? N.xy / len : vec2(1.0, 0.0);
    vec2 texel = (0.5 - r * dir) * 16.0 - 0.5;
    ivec2 i0 = ivec2(floor(texel));
    vec2 f = texel - vec2(i0);
    vec4 alo, ahi, blo, bhi, clo, chi, dlo, dhi;
    TexelBasis(v, i0, alo, ahi);
    TexelBasis(v, i0 + ivec2(1, 0), blo, bhi);
    TexelBasis(v, i0 + ivec2(0, 1), clo, chi);
    TexelBasis(v, i0 + ivec2(1, 1), dlo, dhi);
    vec4 lo = mix(mix(alo, blo, f.x), mix(clo, dlo, f.x), f.y);
    vec4 hi = mix(mix(ahi, bhi, f.x), mix(chi, dhi, f.x), f.y);

    vec3 light = vec3(0.0);
    float transmittance = 1.0;
    bool reached = false;
    uint count = pass.ids.y;
#ifdef PT_PROBE_SUBGROUP
    // sky pixels and helper lanes (a 2x2 quad on an odd-sized target's edge, whose fetch may land outside the images) stay out of the span
    bool outside = sky || gl_HelperInvocation;
    vec3 span_lo = subgroupMin(outside ? vec3(3.0e38) : world);
    vec3 span_hi = subgroupMax(outside ? vec3(-3.0e38) : world);
    vec3 centre = 0.5 * (span_lo + span_hi);
    vec3 half_span = 0.5 * (span_hi - span_lo);
    // the running lanes share the list (lane of rank r tests entries r, r + lanes, ...): bit k of keep (high:low) is set when entry k's box,
    // [-1, 1] on its own axes, overlaps the span; the OR gives every lane the same entries to walk
    uvec4 running = subgroupBallot(true);
    uint lanes = subgroupBallotBitCount(running);
    uint rank = subgroupBallotExclusiveBitCount(running);
    uint keep_low = 0u;
    uint keep_high = 0u;
    for (uint k = rank; k < count; k += lanes) {
        mat4 box = frame.probes[ListEntry(k)].box;
        vec3 qc = (box * vec4(centre, 1.0)).xyz;
        vec3 qe = abs(box[0].xyz) * half_span.x + abs(box[1].xyz) * half_span.y + abs(box[2].xyz) * half_span.z;
        if (all(lessThan(abs(qc) - qe, vec3(1.0)))) {
            if (k < 32u) {
                keep_low |= 1u << k;
            } else {
                keep_high |= 1u << (k - 32u);
            }
        }
    }
    keep_low = subgroupOr(keep_low);
    keep_high = subgroupOr(keep_high);
    if (!sky) {
        for (uint m = keep_low; m != 0u; m &= m - 1u) {
            AddProbe(ListEntry(uint(findLSB(m))), world, lo, hi, v.exposure.x, light, transmittance, reached);
        }
        for (uint m = keep_high; m != 0u; m &= m - 1u) {
            AddProbe(ListEntry(32u + uint(findLSB(m))), world, lo, hi, v.exposure.x, light, transmittance, reached);
        }
    }
#else
    for (uint k = 0u; k < count; ++k) {
        AddProbe(ListEntry(k), world, lo, hi, v.exposure.x, light, transmittance, reached);
    }
#endif
    if (sky || !reached) {
        discard;
    }
    out_diffuse = vec4(light, transmittance);
}
