#include "common.glsl"

/* G-buffer, restructured for NAK on Maxwell (gbuffer-spec experiment = gbuffer-uni + textureSize hoisted into the first fetch
 * block). Output is meant to be identical to shaders/gbuffer.frag.
 *
 * Why (from build/perf/gbuffer.fragment.asm and nak/calc_instr_deps.rs):
 *  - NAK SM50 makes every control-flow instruction (bra/sync/brk/cont/exit) wait on ALL outstanding scoreboards
 *    (assign_barriers: is_branch() -> add_barrier). A texture issued before any `if` is drained at that `if`, so the old shader
 *    paid every fetch latency serially (material ld -> ldc handle -> albedo -> dither -> ldc -> normal -> ldc -> specular -> ...).
 *  - The tear path picks the material per pixel, so every texture index was divergent and NAK wrapped each of the ~12 fetches on
 *    the common path in a vote/shfl waterfall loop (74 instructions, ~555 stall cycles, plus a brk that waits on the fetch).
 *  - TexPoint's `% size` emitted two signed 32-bit modulo emulations (~70 dependent integer instructions, ~445 stall cycles).
 * What:
 *  - main() branches once, on the warp-uniform draw.tint.a, into two inlined copies of the body. In the common copy the material
 *    index is draw.ids.y (push constant) and the bindless indices are dynamically uniform, so no nonuniformEXT and no waterfall.
 *    The tear copy keeps nonuniformEXT and is unchanged in behaviour.
 *  - Material members are read where used (MAT.x), not copied into 28 live registers up front.
 *  - albedo, normal, specular, the aux1 texture (translucency / light-cover mask) and the dither texel are fetched together in
 *    the first block, so their latencies overlap and are drained once (at the discard). Every material has a valid normal and
 *    specular index (ModelCache::BuildMaterial defaults them to kFlatNormal / kBlack); aux1 is only read for the two features
 *    that sample it at uv, other materials sample the 1x1 kWhite (index 0) instead.
 *  - Small ALU-only feature branches are written as selects (no branch, no scoreboard drain).
 *  - TexPoint wraps with one compare per axis (texel is in [0, size] because fract() is in [0, 1]); same texel as `% size`.
 *
 * Specialization (optional, C++ side, see the experiment notes): constant_id 0 = material flags, constant_id 1 = mode bits.
 * With the defaults (mode 0) this file is a drop-in replacement for gbuffer.frag in both the opaque and the decal pipeline.
 *   mode bit 0: use kSpecFlags instead of the material's flags (pipeline made for exactly that flag set)
 *   mode bit 1: the draw has tint.a >= 1 (no dithered fade-out), so with no alpha-test flag the shader has no discard at all
 *               (no KILLS_PIXELS in the SPH -> early Z)
 *   mode bit 2: the draw has tint.a <= 1.5 (no tear path compiled)
 *   mode bit 3: the material has neither MAT_ALPHA_TEST nor MAT_DIRECTIVE_ALPHA (lets mode 14 = bits 1|2|3 drop the discard
 *               while still reading the flags at run time: one extra "opaque" pipeline, no per-flag-set pipelines needed)
 */

layout(constant_id = 0) const uint kSpecFlags = 0u;
layout(constant_id = 1) const uint kSpecMode = 0u;

layout(location = 0) in vec2 in_uv0;
layout(location = 1) in vec2 in_uv1;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec3 in_tangent;
layout(location = 4) in vec3 in_bitangent;
layout(location = 5) in vec3 in_normal;
layout(location = 6) in vec3 in_view_position;
layout(location = 7) in vec3 in_world;
layout(location = 8) in vec3 in_view_dir;
layout(location = 9) in vec2 in_uv2;

layout(push_constant) uniform DrawPush {
    mat4 model;
    uvec4 ids;
    vec4 tint;
    vec4 aux0;
    vec4 aux1;
} draw;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_material;

#ifndef GB_FORCE_NO_KILL
#define GB_FORCE_NO_KILL 0
#endif
#if GB_FORCE_NO_KILL
layout(early_fragment_tests) in;
#endif

const bool kUseSpecFlags = (kSpecMode & 1u) != 0u;
const bool kNoFade = (kSpecMode & 2u) != 0u;
const bool kNoTear = (kSpecMode & 4u) != 0u;
const bool kNoAlpha = (kSpecMode & 8u) != 0u;

float g_mip_bias = 0.0;

vec4 TexU(uint index, vec2 uv) {
    return texture(textures[index], uv, g_mip_bias);
}
vec4 TexN(uint index, vec2 uv) {
    return texture(textures[nonuniformEXT(index)], uv, g_mip_bias);
}
#define TEX(i, c) (tear ? TexN((i), (c)) : TexU((i), (c)))

/* size is queried by the caller in the first fetch block (gbuffer-spec): the txq no longer sits in front of the tld in its own
 * drained round trip inside the multi-index branch. */
vec4 TexPoint(uint index, vec2 uv, ivec2 size, const in bool tear) {
    ivec2 texel = ivec2(fract(uv) * vec2(size));
    texel = mix(texel, texel - size, greaterThanEqual(texel, size));
    return tear ? texelFetch(textures[nonuniformEXT(index)], texel, 0) : texelFetch(textures[index], texel, 0);
}

vec3 ViewReflection(vec3 albedo, vec3 n, vec3 view_dir, float specular, float reflection, float u) {
    vec4 color = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(u, 0.25));
    vec4 param = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(u, 0.75));
    vec3 r = mat3(frame.views[draw.ids.x].inv_view) * reflect(-view_dir, n);
    /* draw.aux1.w is a push constant: dynamically uniform, no nonuniformEXT (no waterfall loop). */
    vec3 env = textureLod(cube_textures[uint(draw.aux1.w)], r, 5.0 * (1.0 - reflection)).rgb;
    float k = param.z * (1.0 - reflection) + reflection;
    float amount = param.z * k;
    float keep = 1.0 - (param.z * (1.0 - specular) + specular) * amount;
    return albedo * keep + specular * amount * param.x * k * color.rgb * env;
}

float TearHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float TearNoise(vec2 p) {
    float sum = 0.0;
    float amp = 0.55;
    for (int i = 0; i < 3; ++i) {
        vec2 c = floor(p);
        vec2 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        float a = TearHash(c), b = TearHash(c + vec2(1.0, 0.0)), d = TearHash(c + vec2(0.0, 1.0)), e = TearHash(c + vec2(1.0, 1.0));
        sum += amp * mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
        p *= 2.3;
        amp *= 0.45;
    }
    return sum / 0.95;
}

/* MeshDither (common.glsl) without its `if`: the old asm branched on temporal.w (block 15 -> 16/17), which drained the
 * fetches in flight; here the rotated rank is computed and selected. Same values. */
vec4 GBufferDither(vec2 frag, vec4 temporal) {
    vec4 d = ImgFetch(RES_DITHER, ivec2(frag) & 7);
    float rank = mod(floor((d.x * 255.0 - 2.0) * 0.25 + 0.5) + temporal.x, 64.0);
    vec2 rotated = vec2(4.0 * rank + 2.0, 2.0 * rank + 1.0) / 255.0;
    d.xy = temporal.w > 0.5 ? rotated : d.xy;
    return d;
}

#define MAT materials[mi]
#define HAS(f) ((flags & (f)) != 0u)

void GBuffer(const in bool tear) {
    float tear_edge = 0.0;
    bool creature = false;
    uint mi = draw.ids.y;
    vec2 uv = in_uv0;
    if (tear && in_color.a < 0.999) {
        float noise = 0.15 + 0.85 * TearNoise(in_uv0 * 34.0);
        creature = in_color.a < noise;
        tear_edge = creature ? 0.0 : 1.0 - smoothstep(0.0, 0.12, in_color.a - noise);
        if (creature) {
            mi = uint(in_color.r + 0.5);
            uv = in_color.gb;
        }
    }
    const uint flags = (kUseSpecFlags && !tear) ? kSpecFlags : MAT.flags;
    uint debug_mode = draw.ids.z & 0xFFu;

    /* One block of fetches: their latencies overlap and are drained together at the first branch (the discard). */
    vec4 base = TEX(MAT.albedo, uv);
    vec4 nt = TEX(MAT.normal, uv);
    vec4 srm = TEX(MAT.specular, uv);
    vec4 aux1_uv = TEX(HAS(MAT_TRANSLUCENT_TEX | MAT_LIGHT_COVER) ? MAT.aux1 : 0u, uv);
    const uint multi_aux0 = ((flags >> 12u) & 3u) != 0u ? MAT.aux0 : 0u;
    ivec2 aux0_size = tear ? textureSize(textures[nonuniformEXT(multi_aux0)], 0) : textureSize(textures[multi_aux0], 0);

    if (HAS(MAT_CONSTANT_COLOR)) {
        base = vec4(MAT.albedo_factor.rgb, 1.0);
    }
    const bool may_fade = !(kNoFade && !tear);
    const bool may_alpha = !(kNoAlpha && !tear) && (!(kUseSpecFlags && !tear) || HAS(MAT_ALPHA_TEST | MAT_DIRECTIVE_ALPHA));
#if !GB_FORCE_NO_KILL
    if (may_fade || may_alpha) {
        vec4 dither = GBufferDither(gl_FragCoord.xy, frame.views[draw.ids.x].temporal);
        float fade = min(draw.tint.a, 1.0);
        float alpha = base.a;
        if (HAS(MAT_DIRECTIVE_ALPHA)) {
            alpha = base.a * (abs(in_normal.z) - MAT.extra.w) / (MAT.params.y - MAT.extra.w);
        }
        if ((may_fade && fade - dither.x < 0.0) || (may_alpha && alpha - MAT.params.x * AlphaReference(flags, dither) < 0.0)) {
            discard;
        }
    }
#endif
    vec3 n_geo = normalize(in_normal);
    if (HAS(MAT_TWO_SIDED) && !gl_FrontFacing) {
        n_geo = -n_geo;
    }
    vec3 t_geo = normalize(in_tangent);
    vec3 b_geo = normalize(in_bitangent);
    vec3 n = n_geo;
    if (HAS(MAT_NORMAL_MAP)) {
        vec2 nxy = vec2(nt.w, nt.y) * 2.0 - 1.0;
        float nz = sqrt(Saturate(1.0 - dot(nxy, nxy)) + 1.00016594e-4);
        nz = (HAS(MAT_NORMAL_WAVE) && !gl_FrontFacing) ? -nz : nz;
        vec3 nts = vec3(nxy, nz);
        if (HAS(MAT_SUB_NORMAL)) {
            bool layer = HAS(MAT_LAYER);
            vec4 mask = layer ? TEX(MAT.aux2, in_uv2) : TEX(MAT.aux2, uv);
            vec4 sn = TEX(MAT.aux1, uv * MAT.extra.yz);
            float k = MAT.extra.x * (layer ? mask.y : mask.x);
            nts = normalize(vec3(nxy.x + k * (2.0 * sn.w - 1.0), nxy.y + k * (2.0 * sn.y - 1.0), nz));
        }
        n = normalize(t_geo * nts.x + b_geo * nts.y + n_geo * nts.z);
    }
    if (HAS(MAT_NORMAL_WAVE)) {
        srm = vec4(MAT.extra.x, MAT.extra.y, 0.0, 0.0);
    } else if (HAS(MAT_ALBEDO_VIEW)) {
        srm = vec4(1.0, 0.6430664, 0.0, 0.0);
    }
    float roughness = srm.g;
    float specular = srm.r;
    float reflection = srm.b;
    float index = MAT.indices.x;
    uint multi = (flags >> 12u) & 3u;
    if (multi > 0u) {
        float mk = TexPoint(MAT.aux0, uv, aux0_size, tear).x;
        vec4 indices = MAT.indices;
        index = mk >= 0.25 ? indices.y : index;
        index = (multi >= 2u && mk >= 0.5) ? indices.z : index;
        index = (multi >= 3u && mk >= 0.75) ? indices.w : index;
    }
    vec3 view_dir = normalize(in_view_dir);
    vec3 albedo = base.rgb;
    if (HAS(MAT_LAYER)) {
        vec4 layer = TEX(MAT.aux0, MAT.albedo_factor.xy * (MAT.albedo_factor.zw + in_uv1));
        vec4 layer_mask = TEX(MAT.aux2, in_uv2);
        albedo = mix(albedo, layer.rgb, layer_mask.x * layer.a);
    }
    albedo *= draw.tint.rgb;
    albedo *= 1.0 - 0.75 * tear_edge;
    albedo *= creature ? 1.5 : 1.0;
    if (HAS(MAT_LIGHT_COVER)) {
        vec4 it = TEX(MAT.aux0, uv);
        vec2 ixy = vec2(it.w, it.y) * 2.0 - 1.0;
        vec3 inner = normalize(t_geo * ixy.x + b_geo * ixy.y + n_geo * sqrt(Saturate(1.0 - dot(ixy, ixy)) + 1.00016594e-4));
        vec3 r = mat3(frame.views[draw.ids.x].inv_view) * reflect(-view_dir, inner);
        vec3 cover = texture(cube_textures[uint(draw.aux1.w)], r).rgb;
        albedo = SrgbDecode(mix(cover, SrgbEncode(albedo), aux1_uv.x));
    }
    if (HAS(MAT_EYE)) {
        vec3 v_ts = vec3(dot(view_dir, t_geo), dot(view_dir, b_geo), dot(view_dir, n_geo));
        float h = TEX(MAT.aux2, uv).y;
        vec3 iris = TEX(MAT.aux0, uv - 0.25 * MAT.params.z * h * vec2(v_ts.x, -v_ts.y) / v_ts.z).rgb;
        vec3 sphere = TEX(floatBitsToUint(MAT.extra.x), vec2(0.5 - 0.5 * n.x, 0.5 - 0.5 * n.y)).rgb;
        albedo = SrgbDecode(Saturate(sphere * srm.r + mix(SrgbEncode(albedo), iris, 1.0 - pow(1.0 - h, 4.0))));
    }
    if (HAS(MAT_HAIR)) {
        vec4 dl = frame.views[draw.ids.x].dominant_light;
        float shift = MAT.extra.y * (2.0 * TEX(MAT.aux0, uv * MAT.extra.zw).x - 1.0);
        vec3 ts = normalize(in_tangent + shift * in_bitangent);
        vec3 l = normalize(dl.xyz);
        vec3 lf = vec3(dot(ts, -l), dot(in_bitangent, -l), dot(in_normal, -l));
        vec3 vf = vec3(dot(ts, view_dir), dot(in_bitangent, view_dir), dot(in_normal, view_dir));
        vec3 h = vf * inversesqrt(dot(vf, vf)) + lf * inversesqrt(dot(lf, lf));
        float hl = length(h);
        float hb = h.y / hl;
        float gloss = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(index, 0.75)).w;
        float highlight = dl.w * srm.r * exp2(log2(abs(Saturate(hl * 10.0))) * 4.0);
        highlight *= exp2(MAT.extra.x * exp2(-6.0 * gloss + 7.0) * log2(abs(sqrt(max(1.0 - hb * hb, 0.0)))));
        float rim = MAT.albedo_factor.w * exp2(MAT.params.z * log2(abs(Saturate(1.0 - dot(n_geo, view_dir)))));
        albedo = SrgbDecode(mix(SrgbEncode(albedo), MAT.albedo_factor.rgb, highlight * rim));
        specular = highlight;
    }
    if (HAS(MAT_INCIDENCE)) {
        float k = MAT.albedo_factor.w * srm.r * pow(Saturate(1.0 - dot(n, view_dir)), MAT.params.z);
        albedo = SrgbDecode(mix(SrgbEncode(albedo), MAT.albedo_factor.rgb, k));
    }
    if (HAS(MAT_MICRO_ROUGHNESS)) {
        vec4 params = MAT.params;
        float r18 = 0.5 / max(params.w, 1.0e-3);
        float r20 = 1.0 / max(1.0 - r18, 1.0e-3);
        float delta = Saturate(r18 * r20 - dot(n, view_dir) * r20) * (srm.g * params.z - srm.g);
        roughness = delta + srm.g;
        float r23 = Saturate(roughness / max(0.001, srm.g));
        reflection = 1.0 - (r23 - srm.b * r23);
    }
    if (HAS(MAT_REFLECTOR)) {
        specular *= exp2(MAT.params.z * log2(abs(Saturate(dot(n, view_dir)))));
    }
    if (HAS(MAT_VIEW_REFLECTION)) {
        albedo = ViewReflection(albedo, n, view_dir, specular, reflection, index);
    }
    float translucency = HAS(MAT_TRANSLUCENT_TEX) ? aux1_uv.y : (HAS(MAT_NORMAL_WAVE) ? MAT.extra.z : 0.0);
    if (HAS(MAT_ALBEDO_VIEW)) {
        n = in_normal;
        index = 0.0;
    }
    vec3 encoded_albedo = SrgbEncode(albedo);
    vec3 encoded_normal = EncodeNormal(n);
    if (HAS(MAT_DECAL)) {
        float a = base.a;
        out_albedo = vec4(encoded_albedo, a);
        out_normal = vec4(encoded_normal, a);
        out_material = vec4(srm.g, srm.r, srm.b, a);
    } else {
        out_albedo = vec4(encoded_albedo, 1.0);
        out_normal = vec4(encoded_normal, reflection);
        out_material = vec4(roughness, specular, index, translucency);
    }
    out_albedo = debug_mode == 2u ? vec4(fract(in_uv0), 0.0, 1.0) : (debug_mode == 3u ? vec4(in_color.rgb, 1.0) : out_albedo);
}

void main() {
    g_mip_bias = frame.views[draw.ids.x].jitter.z;
    if (!kNoTear && draw.tint.a > 1.5) {
        GBuffer(true);
    } else {
        GBuffer(false);
    }
}
