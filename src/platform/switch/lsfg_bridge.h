#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace pt::vk {
class Context;
}

// LSFG frame generation (third_party/lsfg-vk, GPL-3.0-or-later) on the console's Vulkan device. The shaders come from the user's own
// Lossless.dll (sdmc:/switch/pt/lsfg/Lossless.dll); nothing of it is shipped.
// SelfTest: loads the DLL, builds the pipelines, opens a context of the given size and times the generation passes; logs everything.
void pt_switch_lsfg_selftest(pt::vk::Context& ctx, uint32_t width, uint32_t height, bool performance_mode);

// In-engine use: the renderer copies each finished scene image into the generator and records its passes into the frame's command buffer.
// Creates the instance (the DLL is read on first use) and the context for this size; a size change reopens it. false: not available.
bool pt_switch_lsfg_active(pt::vk::Context& ctx, uint32_t width, uint32_t height, bool performance_mode);
void pt_switch_lsfg_images(VkImage& source_for_this_frame, VkImage& generated);
void pt_switch_lsfg_record(VkCommandBuffer cmd);

// Presentation support (Renderer): the generated frame's image view (valid after pt_switch_lsfg_active), whether a generated frame exists yet
// (two source frames recorded), the last error, the motion flow factor (higher = cheaper, coarser; default 2.0).
VkImageView pt_switch_lsfg_generated_view();
bool pt_switch_lsfg_ready();
const char* pt_switch_lsfg_error();
void pt_switch_lsfg_set_flow(float flow);
