#version 460
#include "common.glsl"
// vfx_composite.frag for particles drawn at half resolution: the half-size premultiplied colour and transmittance are upsampled bilinearly
// (interpolating premultiplied values is exact), then applied to the scene exactly as in the full-size shader.
layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 scene = ImgFetch(IMG_HDR_COPY, pixel);
    vec2 uv = (vec2(pixel) + 0.5) / vec2(ImgSize(IMG_HDR_COPY));
    vec4 particles = ImgLod(IMG_PARTICLES_HALF, SMP_LINEAR_CLAMP, uv, 0.0);
    if (particles.a >= 0.9999 && dot(particles.rgb, particles.rgb) < 1.0e-12) {
        out_color = scene;
        return;
    }
    vec3 encoded = particles.rgb + particles.a * SrgbEncode(max(scene.rgb, vec3(0.0)));
    out_color = vec4(SrgbDecode(max(encoded, vec3(0.0))), pass.ids.x == 1u ? scene.a * particles.a : scene.a);
}
