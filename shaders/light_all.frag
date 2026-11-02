#version 460
// Every light in one full-screen pass (light.frag draws one volume per light and re-reads the G-buffer, the material table and the two
// light targets for each). The pixel's data is read once, EvaluateLight (lighting.glsl) is the same function the per-light path calls, and
// the sum is written once. Used by SceneRenderer::RecordLighting when RenderToggles::light_single_pass is on (the console build).
#define PT_LIGHT_HOIST
#include "common.glsl"
#include "lighting.glsl"

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
// the per-light members every pixel needs, in a uniform buffer (constant bank reads) instead of the frame storage buffer
layout(std140, set = 1, binding = 6) uniform LightEvalData {
    LightEval eval[256];
} light_eval;

layout(push_constant) uniform PassPush {
    uvec4 ids;      // x: view index, y: lights in this draw
    uvec4 list[7];  // two lights per word (low half first, 8 per uvec4): bits 0-11 the light index, bit 12 shadows on
} pass;

layout(location = 0) out vec4 out_diffuse;
layout(location = 1) out vec4 out_specular;

void main() {
    g_light_flags = pass.ids.z;
    View v = frame.views[pass.ids.x];
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    if (depth <= 0.0) {
        discard;
    }
    Surface s;
    s.P = ViewPosition(v, PixelNdc(v, gl_FragCoord.xy), depth);
    s.world = (v.inv_view * vec4(s.P, 1.0)).xyz;
    vec4 g_normal = ImgFetch(IMG_NORMAL, pixel);
    vec4 g_material = ImgFetch(IMG_MATERIAL, pixel);
    s.N = DecodeNormal(g_normal.xyz);
    s.roughness = g_material.x;
    s.specular = g_material.y;
    s.material_u = g_material.z;
    s.translucency = g_material.w;
    // the material table rows depend on the pixel only: read once for all lights
    s.mat0 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(s.material_u, 0.25));
    s.mat1 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(s.material_u, 0.75));
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
        l.shadow_cone = light_eval.eval[index].shadow_cone;
        l.scales = light_eval.eval[index].scales;
        l.info = light_eval.eval[index].info;
        g_light_index = index;
        vec3 diffuse;
        vec3 specular;
        if (EvaluateLight(l, v, s, (entry & 0x1000u) != 0u, gl_FragCoord.xy, diffuse, specular)) {
            diffuse_sum += diffuse;
            specular_sum += specular;
        }
    }
    out_diffuse = vec4(diffuse_sum, 1.0);
    out_specular = vec4(specular_sum, 1.0);
}
