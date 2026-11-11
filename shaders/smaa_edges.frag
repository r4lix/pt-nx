#version 460
#include "common.glsl"

// SMAA 1x (Jimenez et al., MIT license: shaders/smaa/LICENSE-SMAA.txt), the reference code. Three passes: luma edges, blending weights,
// neighbourhood blending; the vertex stage's offsets are computed here per pixel.
layout(push_constant) uniform PassPush {
    uvec4 ids;  // x input image, w view
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

#include "smaa/smaa_glue.glsl"

void main() {
    View v = frame.views[pass.ids.w];
    g_metrics = vec4(v.viewport.zw, v.viewport.xy);
    vec2 texcoord = gl_FragCoord.xy * v.viewport.zw;
    vec4 offset[3];
    SMAAEdgeDetectionVS(texcoord, offset);
    out_color = vec4(SMAALumaEdgeDetectionPS(texcoord, offset, SMAA_IMG_POINT(pass.ids.x)), 0.0, 0.0);
}
