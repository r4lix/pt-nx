// Glue for the SMAA reference code (smaa/SMAA.glsl) on this engine's descriptors: a "texture" is an image index with the sampler number
// in bits 8 and up, and the combined sampler is built at each use (glslang cannot pass a constructed sampler as an argument).
#define SMAA_CUSTOM_SL
#define SMAATexture2D(tex) uint tex
#define SMAATexturePass2D(tex) tex
#define SMAA_TEX(tex) sampler2D(images[(tex) & 255u], samplers[(tex) >> 8])
#define SMAASampleLevelZero(tex, coord) textureLod(SMAA_TEX(tex), coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(SMAA_TEX(tex), coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(SMAA_TEX(tex), coord, 0.0, offset)
#define SMAASample(tex, coord) textureLod(SMAA_TEX(tex), coord, 0.0)
#define SMAASamplePoint(tex, coord) textureLod(SMAA_TEX(tex), coord, 0.0)
#define SMAASampleOffset(tex, coord, offset) textureLodOffset(SMAA_TEX(tex), coord, 0.0, offset)
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) ((a) * (b) + (c))
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
#define SMAA_FLATTEN
#define SMAA_BRANCH
vec4 g_metrics;
#define SMAA_RT_METRICS g_metrics
#define SMAA_PRESET_HIGH
#include "SMAA.glsl"
#define SMAA_IMG_POINT(index) ((index) | (SMP_POINT_CLAMP << 8))
#define SMAA_IMG_LINEAR(index) ((index) | (SMP_LINEAR_CLAMP << 8))
