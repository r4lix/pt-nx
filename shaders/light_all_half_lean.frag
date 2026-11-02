#version 460
// light_all_half.frag for lights without a shadow and without a mask (most of them): no shadow or mask code, so far fewer registers
#define PT_LIGHT_LEAN
#include "light_half_main.glsl"
