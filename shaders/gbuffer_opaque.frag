#version 460
// The G-buffer for opaque draws that cannot discard (no alpha test, no directive alpha, no fade-out tint): without a kill instruction the
// depth test runs before the shader (early Z), so hidden fragments are rejected instead of shaded. SceneRenderer::RecordGBuffer routes the draws.
#define GB_FORCE_NO_KILL 1
#include "gbuffer_main.glsl"
