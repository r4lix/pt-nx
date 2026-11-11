#version 460
#include "common.glsl"

// Temporal anti-aliasing on the tonemapped image (the slot FXAA runs in). The scene is drawn with a sub-pixel jitter (SceneRenderer::PrepareFrame);
// the previous resolved image (history, ids.y) is looked up where this pixel was last frame (camera motion rebuilt from the depth and the
// previous view-projection, as mb_velocity.frag does), clamped to the colours around the pixel in the current frame (so a stale colour cannot
// survive a change) and blended with the current pixel. Pixels of moving objects (object velocity, ids.z) lean on the current frame.
layout(push_constant) uniform PassPush {
    uvec4 ids;  // x current image, y history image, z object velocity image, w view
    vec4 f0;    // x history weight (0..1), y 1 = no history (first frame, cut, size change)
    vec4 f1;
    vec4 f2;
    mat4 m;     // previous frame's view-projection (not jittered)
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    View v = frame.views[pass.ids.w];
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 size = ivec2(v.viewport.xy);
    vec4 centre = ImgFetch(pass.ids.x, p);
    if (pass.f0.y > 0.5) {
        out_color = centre;
        return;
    }

    // colours around the pixel in the current frame
    vec3 mean = vec3(0.0);
    vec3 mean2 = vec3(0.0);
    vec3 lo = vec3(1.0e9);
    vec3 hi = vec3(-1.0e9);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 c = ImgFetch(pass.ids.x, clamp(p + ivec2(x, y), ivec2(0), size - 1)).rgb;
            mean += c;
            mean2 += c * c;
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }
    mean *= 1.0 / 9.0;
    vec3 sigma = sqrt(max(mean2 * (1.0 / 9.0) - mean * mean, vec3(0.0)));
    vec3 box_lo = max(lo, mean - 1.25 * sigma);
    vec3 box_hi = min(hi, mean + 1.25 * sigma);

    // where this pixel was in the previous frame: the nearest depth around it (keeps object edges sharp), its world position, the previous projection
    float depth = 0.0;
    for (int i = 0; i < 5; ++i) {
        ivec2 o = i == 0 ? ivec2(0) : (i == 1 ? ivec2(1, 0) : (i == 2 ? ivec2(-1, 0) : (i == 3 ? ivec2(0, 1) : ivec2(0, -1))));
        depth = max(depth, ImgFetch(IMG_DEPTH, clamp(p + o, ivec2(0), size - 1)).x);
    }
    float z = ViewZ(v, depth);
    vec3 world = (v.inv_view * vec4(PixelNdc(v, vec2(p) + 0.5) * v.projection_param.xy * z, z, 1.0)).xyz;
    vec4 previous = pass.m * vec4(world, 1.0);
    vec2 prev_uv = previous.xy / previous.w * 0.5 + 0.5;
    bool inside = previous.w > 0.0 && all(greaterThan(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)));

    vec3 history = Img(pass.ids.y, SMP_LINEAR_CLAMP, prev_uv).rgb;
    history = clamp(history, box_lo, box_hi);

    float weight = inside ? pass.f0.x : 0.0;
    float motion_px = length((vec2(p) + 0.5 - prev_uv * vec2(size)));
    weight *= 1.0 - clamp(motion_px * 0.04, 0.0, 0.5);  // fast pans: less history
    if (ImgFetch(pass.ids.z, p).x > 0.5) {
        weight *= 0.4;  // a moving object (not in the camera motion)
    }
    out_color = vec4(mix(centre.rgb, history, weight), centre.a);
}
