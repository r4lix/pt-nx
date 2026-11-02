#pragma once

#include <cstdint>
#include <string>

#include "engine/render/vk.h"

namespace pt::vk {

// The driver's compiled pipelines, kept between runs in one file. On the Switch NVK compiles every pipeline on the console's CPU: at start-up
// and, for the G-buffer's flag-specialised variants, during play (a run of slow frames when an area with new materials comes into view).
// VK_NULL_HANDLE when off: pipeline creation then works as without a cache.
extern VkPipelineCache g_pipeline_cache;
// set before the device is created (main): the file (empty = no cache) and a key of the running program (a new build must not get the
// old build's pipelines: the file is used only when its key matches)
extern std::string g_pipeline_cache_file;
extern uint64_t g_pipeline_cache_key;

// Creates g_pipeline_cache, filled from g_pipeline_cache_file when that file is intact (own checksum), has this key and was written by this
// driver and device.
void LoadPipelineCache(VkDevice device, const VkPhysicalDeviceProperties& properties);
// Writes the cache in the background when it grew since it was loaded or last written (call it now and then, e.g. every few seconds).
void SavePipelineCacheAsync(VkDevice device);
// At exit, before the device goes: waits for a background write, writes the cache if it grew, destroys it.
void ClosePipelineCache(VkDevice device);

}  // namespace pt::vk
