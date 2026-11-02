#version 460
#include "common.glsl"
// Half-resolution depth for the particle effects: the nearest of each 2x2 block (reverse Z: the largest value), so a particle is hidden
// wherever any pixel of the block is covered by nearer geometry and never leaks over an edge.
void main() {
    ivec2 full = ivec2(ImgSize(IMG_DEPTH));
    ivec2 p = ivec2(gl_FragCoord.xy) * 2;
    float a = ImgFetch(IMG_DEPTH, min(p, full - 1)).x;
    float b = ImgFetch(IMG_DEPTH, min(p + ivec2(1, 0), full - 1)).x;
    float c = ImgFetch(IMG_DEPTH, min(p + ivec2(0, 1), full - 1)).x;
    float d = ImgFetch(IMG_DEPTH, min(p + ivec2(1, 1), full - 1)).x;
    gl_FragDepth = max(max(a, b), max(c, d));
}
