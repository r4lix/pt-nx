#include "engine/render/renderer.h"

#include <chrono>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "engine/core/log.h"
#include "engine/render/upscale/streamline.h"
#include "engine/render/pipeline_cache.h"
#ifdef PT_SWITCH
#include "platform/switch/lsfg_bridge.h"
#endif

#ifdef PT_SWITCH
extern "C" void pt_switch_set_swap_interval(unsigned interval);
#endif

namespace pt {
namespace {

VkFormat g_imgui_format = VK_FORMAT_UNDEFINED;

void ImGuiCheck(VkResult result) {
    if (result != VK_SUCCESS) {
        LogError("imgui vulkan: {}", vk::ResultName(result));
    }
}

}

bool Renderer::Init(SDL_Window* window, const RendererSettings& settings) {
    settings_ = settings;
    if (const char* brightness = std::getenv("PT_OUTPUT_BRIGHTNESS")) {
        brightness_override_ = static_cast<float>(std::atof(brightness));
    }
    window_ = settings.headless ? nullptr : window;
    if (!ctx_.Init(window_, settings.validation)) {
        return false;
    }
    uint32_t width = settings.width;
    uint32_t height = settings.height;
    {
        timestamp_ns_ = ctx_.properties.limits.timestampPeriod;  // corrected for the Tegra X1 timer in vk::Context
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = 2 * kFramesInFlight;
        vkCreateQueryPool(ctx_.device, &query, nullptr, &frame_queries_);
    }
#ifdef PT_SWITCH
    late_acquire_env_ = window_ != nullptr && std::getenv("PT_LATE_ACQUIRE") != nullptr;  // opt-in: measured no gain once timing was right
    late_acquire_ = late_acquire_env_;
#endif
    VkFormat output_format = VK_FORMAT_R8G8B8A8_UNORM;
    if (window_) {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        width = static_cast<uint32_t>(w);
        height = static_cast<uint32_t>(h);
        if (!ctx_.CreateSwapchain(width, height, settings.vsync)) {
            return false;
        }
        output_format = ctx_.swapchain.format;
        width = ctx_.swapchain.extent.width;
        height = ctx_.swapchain.extent.height;
    }
    output_format_ = output_format;
    for (Frame& frame : frames_) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = ctx_.queue_family;
        vkCreateCommandPool(ctx_.device, &pool_info, nullptr, &frame.pool);
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = frame.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx_.device, &alloc, &frame.cmd);
        vkAllocateCommandBuffers(ctx_.device, &alloc, &frame.present_cmd);
        vkAllocateCommandBuffers(ctx_.device, &alloc, &frame.present_cmd2);
        VkSemaphoreCreateInfo scene_sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_.device, &scene_sem, nullptr, &frame.scene_done);
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_.device, &semaphore_info, nullptr, &frame.image_available);
        vkCreateSemaphore(ctx_.device, &semaphore_info, nullptr, &frame.image_available2);
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx_.device, &fence_info, nullptr, &frame.in_flight);
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx_.device, &sampler_info, nullptr, &linear_sampler_);
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(ctx_.device, &sampler_info, nullptr, &wrap_sampler_);
    if (!CreateTargets(width, height) || !CreateCompositePipeline(output_format)) {
        return false;
    }
    if (window_ && !InitImGui(window_)) {
        return false;
    }
    return true;
}

bool Renderer::InitImGui(SDL_Window* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "pt_imgui.ini";
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForVulkan(window);
    g_imgui_format = ctx_.swapchain.format;
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = ctx_.instance;
    info.PhysicalDevice = ctx_.physical;
    info.Device = ctx_.device;
    info.QueueFamily = ctx_.queue_family;
    info.Queue = ctx_.queue;
    info.DescriptorPoolSize = 256;
    info.MinImageCount = ctx_.swapchain.min_image_count;
    info.ImageCount = static_cast<uint32_t>(ctx_.swapchain.images.size());
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_imgui_format;
    info.CheckVkResultFn = ImGuiCheck;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LogError("ImGui_ImplVulkan_Init failed");
        return false;
    }
    imgui_ready_ = true;
    return true;
}

bool Renderer::CreateTargets(uint32_t width, uint32_t height) {
    const VkExtent3D extent{width, height, 1};
    if (!ctx_.CreateImage(scene_color_, kSceneColorFormat, extent,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    if (settings_.headless &&
        !ctx_.CreateImage(output_, VK_FORMAT_R8G8B8A8_UNORM, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    output_ready_ = false;
    if (!settings_.headless) {
        const VkFormat format = ctx_.swapchain.format;
        const bool capture = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM ||
                             format == VK_FORMAT_R8G8B8A8_SRGB;
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!capture) {
            LogWarn("renderer: swapchain format {} has no 8-bit layout, windowed screenshots disabled", static_cast<int>(format));
        } else if (!ctx_.CreateImage(output_, format, extent, usage)) {
            LogWarn("renderer: no capture image, windowed screenshots disabled");
        }
    }
    if (!ctx_.CreateImage(final_, output_format_, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    WriteCompositeSets();
    return true;
}

void Renderer::WriteCompositeSets() {
    if (!composite_set_ || !final_set_) {
        return;
    }
    const VkImageView noise = grain_noise_ ? grain_noise_ : scene_color_.view;
    VkDescriptorImageInfo infos[4] = {{linear_sampler_, scene_color_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {linear_sampler_, final_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {wrap_sampler_, noise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {wrap_sampler_, noise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[4]{};
    for (int i = 0; i < 4; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = i % 2 == 0 ? composite_set_ : final_set_;
        writes[i].dstBinding = i < 2 ? 0 : 1;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(ctx_.device, 4, writes, 0, nullptr);
}

void Renderer::WriteFgSet(VkImageView generated) {
    if (!fg_set_ || !generated) {
        return;
    }
    const VkImageView noise = grain_noise_ ? grain_noise_ : scene_color_.view;
    if (generated == fg_view_ && noise == fg_noise_) {
        return;  // the set is in use by frames in flight: written only when something changes (the generator was reopened after an idle wait)
    }
    vkDeviceWaitIdle(ctx_.device);
    VkDescriptorImageInfo infos[2] = {{linear_sampler_, generated, VK_IMAGE_LAYOUT_GENERAL}, {wrap_sampler_, noise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = fg_set_;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(ctx_.device, 2, writes, 0, nullptr);
    fg_view_ = generated;
    fg_noise_ = noise;
}

void Renderer::SetGrainNoise(VkImageView view) {
    if (grain_noise_ == view) {
        return;
    }
    vkDeviceWaitIdle(ctx_.device);
    grain_noise_ = view;
    WriteCompositeSets();
}

void Renderer::DestroyTargets() {
    ctx_.DestroyImage(scene_color_);
    ctx_.DestroyImage(final_);
    ctx_.DestroyImage(output_);
}

bool Renderer::CreateCompositePipeline(VkFormat output_format) {
    VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx_.device, &set_info, nullptr, &composite_set_layout_);
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 3;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    vkCreateDescriptorPool(ctx_.device, &pool_info, nullptr, &composite_pool_);
    const VkDescriptorSetLayout layouts[3] = {composite_set_layout_, composite_set_layout_, composite_set_layout_};
    VkDescriptorSet sets[3] = {};
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = composite_pool_;
    alloc.descriptorSetCount = 3;
    alloc.pSetLayouts = layouts;
    vkAllocateDescriptorSets(ctx_.device, &alloc, sets);
    composite_set_ = sets[0];
    final_set_ = sets[1];
    fg_set_ = sets[2];
    WriteCompositeSets();

    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &composite_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    vkCreatePipelineLayout(ctx_.device, &layout_info, nullptr, &composite_layout_);

    VkShaderModule vert = vk::LoadShaderModule(ctx_.device, "fullscreen.vert");
    VkShaderModule frag = vk::LoadShaderModule(ctx_.device, "composite.frag");
    if (!vert || !frag) {
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &output_format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = composite_layout_;
    const bool ok = vk::Check(vkCreateGraphicsPipelines(ctx_.device, vk::g_pipeline_cache, 1, &info, nullptr, &composite_pipeline_), "composite pipeline");
    vkDestroyShaderModule(ctx_.device, vert, nullptr);
    vkDestroyShaderModule(ctx_.device, frag, nullptr);
    return ok;
}

void Renderer::Shutdown() {
    if (!ctx_.device) {
        return;
    }
    vkDeviceWaitIdle(ctx_.device);
    if (imgui_ready_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    DestroyXr();
    vkDestroyPipeline(ctx_.device, composite_pipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device, composite_layout_, nullptr);
    vkDestroyDescriptorPool(ctx_.device, composite_pool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device, composite_set_layout_, nullptr);
    vkDestroySampler(ctx_.device, linear_sampler_, nullptr);
    vkDestroySampler(ctx_.device, wrap_sampler_, nullptr);
    DestroyTargets();
    vkDestroyQueryPool(ctx_.device, frame_queries_, nullptr);
    for (Frame& frame : frames_) {
        vkDestroyFence(ctx_.device, frame.in_flight, nullptr);
        vkDestroySemaphore(ctx_.device, frame.image_available, nullptr);
        vkDestroySemaphore(ctx_.device, frame.image_available2, nullptr);
        vkDestroySemaphore(ctx_.device, frame.scene_done, nullptr);
        vkDestroyCommandPool(ctx_.device, frame.pool, nullptr);
    }
    ctx_.Shutdown();
}

void Renderer::SetVsync(bool enabled) {
    if(settings_.vsync == enabled) return;
    settings_.vsync = enabled;
    if(window_) swapchain_dirty_ = true;
    LogInfo("display: VSync {} (applied at next frame)", enabled ? "on" : "off");
}

void Renderer::Resize(uint32_t, uint32_t) {
    swapchain_dirty_ = true;
}

bool Renderer::BeginFrame(bool present) {
    Frame& frame = frames_[frame_index_];
    late_acquire_ = window_ != nullptr && (late_acquire_env_ || frame_generation);
    streamline::BeginFrame();
    const auto fence_start = std::chrono::steady_clock::now();
    ctx_.CheckDeviceLost(vkWaitForFences(ctx_.device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX), "frame fence wait");
    timing_.fence_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - fence_start).count();
    if (frame_queries_ && frame_queries_written_[frame_index_]) {
        uint64_t stamps[2] = {};
        if (vkGetQueryPoolResults(ctx_.device, frame_queries_, 2 * frame_index_, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && stamps[1] > stamps[0]) {
            timing_.gpu_ms = static_cast<float>(static_cast<double>(stamps[1] - stamps[0]) * timestamp_ns_ * 1e-6);
            static int calib_tick = 0;
            if (ctx_.calibrated_timestamps && vkGetCalibratedTimestampsKHR && (calib_tick++ % 60) == 0) {
                VkCalibratedTimestampInfoKHR infos[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR},
                                                         {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
                infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
                infos[1].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
                uint64_t values[2] = {};
                uint64_t deviation = 0;
                if (vkGetCalibratedTimestampsKHR(ctx_.device, 2, infos, values, &deviation) == VK_SUCCESS) {
                    calib_offset_ns_ = static_cast<int64_t>(values[1]) - static_cast<int64_t>(static_cast<double>(values[0]) * timestamp_ns_);
                    calib_ok_ = true;
                }
            }
            timing_.calibrated = calib_ok_;
            if (calib_ok_) {
                const int64_t start_ns = static_cast<int64_t>(static_cast<double>(stamps[0]) * timestamp_ns_) + calib_offset_ns_;
                timing_.gpu_start_after_submit_ms = static_cast<float>(static_cast<double>(start_ns - frame.submit_ns) * 1e-6);
            }
            if (last_gpu_end_ != 0 && stamps[0] > last_gpu_end_) {
                timing_.gpu_idle_before_ms = static_cast<float>(static_cast<double>(stamps[0] - last_gpu_end_) * timestamp_ns_ * 1e-6);
            } else {
                timing_.gpu_idle_before_ms = 0.0f;
            }
            last_gpu_end_ = stamps[1];
        }
    }
    grain[0] = 0.0f;
    if (render_extent_.width > 0 && render_extent_.height > 0 &&
        (scene_color_.extent.width != render_extent_.width || scene_color_.extent.height != render_extent_.height)) {
        vkDeviceWaitIdle(ctx_.device);
        DestroyTargets();
        if (!CreateTargets(render_extent_.width, render_extent_.height)) {
            return false;
        }
        LogInfo("renderer: render size {}x{} (VR)", render_extent_.width, render_extent_.height);
    }
    presenting_ = window_ && present;
    if (presenting_) {
        if (swapchain_dirty_) {
            int w = 0;
            int h = 0;
            SDL_GetWindowSizeInPixels(window_, &w, &h);
            if (w == 0 || h == 0) {
                return false;
            }
            if (!ctx_.CreateSwapchain(static_cast<uint32_t>(w), static_cast<uint32_t>(h), settings_.vsync)) {
                return false;
            }
            DestroyTargets();
            if (render_extent_.width > 0 && render_extent_.height > 0) {
                CreateTargets(render_extent_.width, render_extent_.height);
            } else {
                CreateTargets(ctx_.swapchain.extent.width, ctx_.swapchain.extent.height);
            }
            if (imgui_ready_) {
                ImGui_ImplVulkan_SetMinImageCount(ctx_.swapchain.min_image_count);
            }
            swapchain_dirty_ = false;
        }
        if (!late_acquire_) {
            const auto acquire_start = std::chrono::steady_clock::now();
            const VkResult acquire = ctx_.AcquireNextImage(frame.image_available, &image_index_);
            timing_.acquire_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - acquire_start).count();
            ctx_.CheckDeviceLost(acquire, "swapchain acquire");
            if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT) {
                return false;
            }
            if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
                swapchain_dirty_ = true;
                return false;
            }
            if (acquire == VK_SUBOPTIMAL_KHR) {
                swapchain_dirty_ = true;
            }
        }
    }
    vkResetFences(ctx_.device, 1, &frame.in_flight);
    vkResetCommandPool(ctx_.device, frame.pool, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.cmd, &begin);
    if (frame_queries_) {
        vkCmdResetQueryPool(frame.cmd, frame_queries_, 2 * frame_index_, 2);
    }

    vk::ImageBarrier(frame.cmd, scene_color_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = scene_color_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, RenderExtent()};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(frame.cmd, &rendering);
    vkCmdEndRendering(frame.cmd);
    if (frame_queries_) {
        // after a 3D operation: NVK then releases the stamp on the 3D engine after all earlier work (after the query reset it may go
        // through the copy engine, ahead of the previous frame's rendering)
        vkCmdWriteTimestamp2(frame.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frame_queries_, 2 * frame_index_);
    }
    streamline::SetMarker(streamline::Marker::SimulationEnd);
    streamline::SetMarker(streamline::Marker::RenderSubmitStart);
    return true;
}

void Renderer::Composite(VkCommandBuffer cmd, VkDescriptorSet set, float mode, VkExtent2D extent, VkOffset2D offset) {
    VkViewport viewport{static_cast<float>(offset.x), static_cast<float>(offset.y), static_cast<float>(extent.width), static_cast<float>(extent.height),
                        0.0f, 1.0f};
    VkRect2D scissor{offset, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_layout_, 0, 1, &set, 0, nullptr);
    const float brightness = std::clamp(brightness_override_ > 0.0f ? brightness_override_ : output_brightness, 0.1f, 4.0f);
    /* The grain tiles three times across the original 16:9 frame; scaling by the window's width in 16:9 frames keeps the grain texel size on ultrawide. */
    const float across = extent.height ? (static_cast<float>(extent.width) / static_cast<float>(extent.height)) / (16.0f / 9.0f) : 1.0f;
    // sharpening of an upscaled scene: only when the scene is drawn smaller than this output (the Switch's adaptive size); PT_SHARPEN=0..1
    static const float sharpen_setting = [] {
        const char* v = std::getenv("PT_SHARPEN");
#ifdef PT_SWITCH
        return v ? static_cast<float>(std::atof(v)) : 0.6f;
#else
        return v ? static_cast<float>(std::atof(v)) : 0.0f;
#endif
    }();
    const bool upscaled = scene_color_.extent.width < extent.width || scene_color_.extent.height < extent.height;
    const float push[16] = {exposure, brightness, mode, upscaled ? (sharpen >= 0.0f ? sharpen : sharpen_setting) : 0.0f, fade[0], fade[1], fade[2], fade[3],
                            grain[0], grain[1], grain[2], grain[3], grain_offset[0], grain_offset[1], across, 0.0f};
    vkCmdPushConstants(cmd, composite_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void Renderer::EndFrame(bool draw_ui) {
    Frame& frame = frames_[frame_index_];
    VkCommandBuffer cmd = frame.cmd;
    const bool late = late_acquire_ && presenting_;
    vk::ImageBarrier(cmd, scene_color_.image, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkImage target_image = window_ ? (presenting_ && !late ? ctx_.swapchain.images[image_index_] : VK_NULL_HANDLE) : output_.image;
    VkImageView target_view = window_ ? (presenting_ && !late ? ctx_.swapchain.views[image_index_] : VK_NULL_HANDLE) : output_.view;
    const VkExtent2D extent = RenderExtent();
    const bool xr = std::exchange(xr_pending_, false);
    vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = final_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    const vk::Image* hud = presenting_ && !late && hudless && !xr ? hudless(image_index_) : nullptr;
    const VkExtent2D window_extent = presenting_ ? ctx_.swapchain.extent : extent;
    const bool fitted = window_extent.width != extent.width || window_extent.height != extent.height;
#ifdef PT_SWITCH
    // the scene renders at a reduced size on the console: the game's own UI is drawn after the scaling, at the window's size, so it stays sharp
    const bool ui_on_window = fitted && overlay && !hud && !xr && (target_image != VK_NULL_HANDLE || late);
#else
    const bool ui_on_window = false;
#endif
    vkCmdBeginRendering(cmd, &rendering);
    Composite(cmd, composite_set_, 0.0f, extent);
    if (overlay && !hud && !ui_on_window && (!xr || xr_frame_.overlay_on_frame)) {
        overlay(cmd, final_.view, extent);
    }
    vkCmdEndRendering(cmd);
    vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (hud) {
        vk::ImageBarrier(cmd, hud->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        color.imageView = hud->view;
        vkCmdBeginRendering(cmd, &rendering);
        Composite(cmd, final_set_, 1.0f, extent);
        vkCmdEndRendering(cmd);
        vk::ImageBarrier(cmd, hud->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (overlay) {
            vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            color.imageView = final_.view;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            vkCmdBeginRendering(cmd, &rendering);
            overlay(cmd, final_.view, extent);
            vkCmdEndRendering(cmd);
            color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    VkDescriptorSet first_set = final_set_;  // what the first presented image shows (the generated frame in a frame-generation frame)
    bool fg_follow = false;  // the real frame follows on a second image, after this one (with the generated frame) is presented
#ifdef PT_SWITCH
    if (frame_generation && presenting_ && !hud && !xr && ui_on_window && overlay_repeat &&
        pt_switch_lsfg_active(ctx_, extent.width, extent.height, true)) {
        // The finished scene (final_: the scene composited without the UI) goes into the generator, whose passes are recorded here; the frame is
        // then shown as two images: the one generated between the previous scene and this one, then this one. Both are rendered by this submit.
        VkImage source = VK_NULL_HANDLE, generated = VK_NULL_HANDLE;
        pt_switch_lsfg_images(source, generated);
        const int32_t w = static_cast<int32_t>(extent.width);
        const int32_t h = static_cast<int32_t>(extent.height);
        const VkImageBlit region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {w, h, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {w, h, 1}}};
        auto memory = [&](VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = src_stage;
            barrier.srcAccessMask = src_access;
            barrier.dstStageMask = dst_stage;
            barrier.dstAccessMask = dst_access;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        };
        vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        memory(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        vkCmdBlitImage(cmd, final_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
        vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        memory(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        pt_switch_lsfg_record(cmd);
        (void)generated;
        memory(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        if (pt_switch_lsfg_ready()) {
            WriteFgSet(pt_switch_lsfg_generated_view());
            first_set = fg_set_;
            fg_follow = true;
        }
        static int entered = 0;
        if (entered++ < 3) LogInfo("frame generation: frame {} ready {} follow {}", entered, pt_switch_lsfg_ready(), fg_follow);
    }
#endif
    if (xr) {
        RecordXr(cmd);
    }
    if (late) {
        // the scene and the offscreen composite go to the GPU now, with nothing to wait for; then the image, then the short part
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo scene_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        scene_info.commandBuffer = cmd;
        VkSubmitInfo2 scene_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        scene_submit.commandBufferInfoCount = 1;
        scene_submit.pCommandBufferInfos = &scene_info;
        // nxvk's nvgpu backend only kicks work off to the GPU when a submit signals something: signal a semaphore so the scene starts now
        VkSemaphoreSubmitInfo scene_signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        scene_signal.semaphore = frame.scene_done;
        scene_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        scene_submit.signalSemaphoreInfoCount = 1;
        scene_submit.pSignalSemaphoreInfos = &scene_signal;
        frame.submit_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        ctx_.CheckDeviceLost(vkQueueSubmit2(ctx_.queue, 1, &scene_submit, VK_NULL_HANDLE), "scene submit");
        const auto acquire_start = std::chrono::steady_clock::now();
        const VkResult acquire = ctx_.AcquireNextImage(frame.image_available, &image_index_);
        timing_.acquire_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - acquire_start).count();
        ctx_.CheckDeviceLost(acquire, "swapchain acquire");
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            // no image this frame: still signal the frame fence (it was reset), skip the present
            if (acquire != VK_NOT_READY && acquire != VK_TIMEOUT) {
                swapchain_dirty_ = true;
            }
            VkSubmitInfo2 empty{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            VkSemaphoreSubmitInfo scene_wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            scene_wait.semaphore = frame.scene_done;
            scene_wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            empty.waitSemaphoreInfoCount = 1;
            empty.pWaitSemaphoreInfos = &scene_wait;
            ctx_.CheckDeviceLost(vkQueueSubmit2(ctx_.queue, 1, &empty, frame.in_flight), "frame fence submit");
            frame_index_ = (frame_index_ + 1) % kFramesInFlight;
            return;
        }
        if (acquire == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true;
        }
        target_image = ctx_.swapchain.images[image_index_];
        target_view = ctx_.swapchain.views[image_index_];
        cmd = frame.present_cmd;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
    }
    if (target_image) {
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        color.imageView = target_view;
        if (fitted) {
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            rendering.renderArea = {{0, 0}, window_extent};
        }
        vkCmdBeginRendering(cmd, &rendering);
        if (fitted) {
            const float scale = std::min(static_cast<float>(window_extent.width) / static_cast<float>(extent.width),
                                         static_cast<float>(window_extent.height) / static_cast<float>(extent.height));
            const VkExtent2D shown{std::max(1u, static_cast<uint32_t>(extent.width * scale)), std::max(1u, static_cast<uint32_t>(extent.height * scale))};
            Composite(cmd, first_set, 1.0f, shown,
                      {static_cast<int32_t>((window_extent.width - shown.width) / 2), static_cast<int32_t>((window_extent.height - shown.height) / 2)});
        } else {
            Composite(cmd, final_set_, 1.0f, extent);
        }
        if (ui_on_window) {
            overlay(cmd, target_view, window_extent);
        }
        if (draw_ui && imgui_ready_) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        }
        vkCmdEndRendering(cmd);
    }
    if (presenting_ && output_.image && !fitted) {
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk::ImageBarrier(cmd, output_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {extent.width, extent.height, 1};
        vkCmdCopyImage(cmd, target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        vk::ImageBarrier(cmd, output_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, ctx_.PresentLayout());
        output_ready_ = true;
    } else if (presenting_) {
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                         ctx_.PresentLayout());
    } else if (!window_) {
        output_ready_ = true;
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    if (frame_queries_) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frame_queries_, 2 * frame_index_ + 1);
        frame_queries_written_[frame_index_] = true;
    }
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSemaphoreSubmitInfo waits[3] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    waits[0].semaphore = frame.image_available;
    waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    waits[1].semaphore = frame.scene_done;  // binary semaphore signalled by the scene submit (late acquire) must be waited
    waits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signals[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    if (presenting_) {
        signals[0].semaphore = ctx_.swapchain.render_finished[image_index_];
        submit.waitSemaphoreInfoCount = late ? 2u : 1u;
        submit.pWaitSemaphoreInfos = waits;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = signals;
    }
    if (!late) {
        frame.submit_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    ctx_.CheckDeviceLost(vkQueueSubmit2(ctx_.queue, 1, &submit, fg_follow ? VK_NULL_HANDLE : frame.in_flight), "frame submit");
    streamline::SetMarker(streamline::Marker::RenderSubmitEnd);
    if (presenting_) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ctx_.swapchain.render_finished[image_index_];
        present.swapchainCount = 1;
        present.pSwapchains = &ctx_.swapchain.handle;
        present.pImageIndices = &image_index_;
        streamline::SetMarker(streamline::Marker::PresentStart);
#ifdef PT_SWITCH
        const uint32_t interval = fg_follow ? 1u : swap_interval;  // frame generation: each of the frame's two images stays one VBlank
        if (interval > 0) {
            pt_switch_set_swap_interval(interval);
        }
#endif
        const auto present_start = std::chrono::steady_clock::now();
        const VkResult result = ctx_.QueuePresent(present);
        ++presents;
        if (fg_follow) ++generated_presents;
        timing_.present_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - present_start).count();
        streamline::SetMarker(streamline::Marker::PresentEnd);
        ctx_.CheckDeviceLost(result, "present");
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true;
        }
    }
    if (fg_follow) {
        // frame generation, second half: the real frame goes on a second swapchain image (the first one, with the generated frame, is presented
        // already), with the same UI again. The surface has only 3 images, so this image is acquired after the first was presented.
        uint32_t index2 = 0;
        const auto acquire2_start = std::chrono::steady_clock::now();
        const VkResult acquire2 = ctx_.AcquireNextImage(frame.image_available2, &index2);
        // waiting for the second image is waiting for the display, not CPU work (the log's "cpu busy" counted it before)
        timing_.acquire_ms += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - acquire2_start).count();
        const bool have_image = acquire2 == VK_SUCCESS || acquire2 == VK_SUBOPTIMAL_KHR;
        VkSubmitInfo2 second{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        VkCommandBufferSubmitInfo second_cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        VkSemaphoreSubmitInfo second_wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        VkSemaphoreSubmitInfo second_signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        if (have_image) {
            if (acquire2 == VK_SUBOPTIMAL_KHR) {
                swapchain_dirty_ = true;
            }
            VkCommandBuffer cmd2 = frame.present_cmd2;
            const VkImage image2 = ctx_.swapchain.images[index2];
            const VkImageView view2 = ctx_.swapchain.views[index2];
            VkCommandBufferBeginInfo begin2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd2, &begin2);
            vk::ImageBarrier(cmd2, image2, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            color.imageView = view2;
            vkCmdBeginRendering(cmd2, &rendering);
            const float scale2 = std::min(static_cast<float>(window_extent.width) / static_cast<float>(extent.width),
                                          static_cast<float>(window_extent.height) / static_cast<float>(extent.height));
            const VkExtent2D shown2{std::max(1u, static_cast<uint32_t>(extent.width * scale2)), std::max(1u, static_cast<uint32_t>(extent.height * scale2))};
            Composite(cmd2, final_set_, 1.0f, shown2,
                      {static_cast<int32_t>((window_extent.width - shown2.width) / 2), static_cast<int32_t>((window_extent.height - shown2.height) / 2)});
            overlay_repeat(cmd2, view2, window_extent);
            if (draw_ui && imgui_ready_) {
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd2);
            }
            vkCmdEndRendering(cmd2);
            vk::ImageBarrier(cmd2, image2, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                             ctx_.PresentLayout());
            vkEndCommandBuffer(cmd2);
            second_cmd_info.commandBuffer = cmd2;
            second_wait.semaphore = frame.image_available2;
            second_wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            second_signal.semaphore = ctx_.swapchain.render_finished[index2];
            second_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            second.commandBufferInfoCount = 1;
            second.pCommandBufferInfos = &second_cmd_info;
            second.waitSemaphoreInfoCount = 1;
            second.pWaitSemaphoreInfos = &second_wait;
            second.signalSemaphoreInfoCount = 1;
            second.pSignalSemaphoreInfos = &second_signal;
        } else if (acquire2 != VK_NOT_READY && acquire2 != VK_TIMEOUT) {
            swapchain_dirty_ = true;
        }
        // this submit carries the frame's fence: the frame is done when its second half is (or at once when there is no second image)
        ctx_.CheckDeviceLost(vkQueueSubmit2(ctx_.queue, 1, &second, frame.in_flight), "frame generation submit");
        if (have_image) {
            VkPresentInfoKHR present2{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present2.waitSemaphoreCount = 1;
            present2.pWaitSemaphores = &ctx_.swapchain.render_finished[index2];
            present2.swapchainCount = 1;
            present2.pSwapchains = &ctx_.swapchain.handle;
            present2.pImageIndices = &index2;
#ifdef PT_SWITCH
            pt_switch_set_swap_interval(1);
#endif
            const auto present_start = std::chrono::steady_clock::now();
            const VkResult result = ctx_.QueuePresent(present2);
            timing_.present_ms += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - present_start).count();
            ++presents;
            ctx_.CheckDeviceLost(result, "present");
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
                swapchain_dirty_ = true;
            }
        }
    }
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

// Read-back pixels (4 bytes each, in the image's format) to an RGBA PNG.
static bool WriteScreenshotPng(const std::filesystem::path& path, uint8_t* pixels, uint32_t width, uint32_t height, VkFormat format) {
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0, n = size_t(width) * height; i < n; ++i) {
        uint8_t* p = pixels + i * 4;
        if (bgra) {
            std::swap(p[0], p[2]);
        }
        p[3] = 255;
    }
    const int ok = stbi_write_png(path.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4, pixels, static_cast<int>(width * 4));
    if (ok) {
        LogInfo("screenshot written to {}", path.string());
    } else {
        LogError("screenshot {} write failed", path.string());
    }
    return ok != 0;
}

#ifdef PT_SWITCH
// The console renders the scene smaller than the screen and scales it up while presenting, so the presented image is never copied into
// output_ (that copy needs both sizes equal) and the photo mode had nothing to save. The last frame's finished scene is still in final_
// (composited without the UI, which the console draws on the screen after the scaling): scale it to the screen's size as it was shown
// and save that.
bool Renderer::SaveSceneScreenshot(const std::filesystem::path& path) {
    vkDeviceWaitIdle(ctx_.device);
    const VkExtent2D scene{final_.extent.width, final_.extent.height};
    const VkExtent2D screen = ctx_.swapchain.extent;
    const float scale = std::min(static_cast<float>(screen.width) / static_cast<float>(scene.width),
                                 static_cast<float>(screen.height) / static_cast<float>(scene.height));
    const uint32_t width = std::max(1u, static_cast<uint32_t>(static_cast<float>(scene.width) * scale));
    const uint32_t height = std::max(1u, static_cast<uint32_t>(static_cast<float>(scene.height) * scale));
    vk::Image scaled;
    if (!ctx_.CreateImage(scaled, output_format_, {width, height, 1}, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        LogError("screenshot {} not written: no image to scale into", path.string());
        return false;
    }
    vk::Buffer readback;
    if (!ctx_.CreateBuffer(readback, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
        ctx_.DestroyImage(scaled);
        return false;
    }
    ctx_.Submit([&](VkCommandBuffer cmd) {
        vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk::ImageBarrier(cmd, scaled.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageBlit region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 {{0, 0, 0}, {static_cast<int32_t>(scene.width), static_cast<int32_t>(scene.height), 1}},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 {{0, 0, 0}, {static_cast<int32_t>(width), static_cast<int32_t>(height), 1}}};
        vkCmdBlitImage(cmd, final_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scaled.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                       VK_FILTER_LINEAR);
        vk::ImageBarrier(cmd, scaled.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, scaled.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    vmaInvalidateAllocation(ctx_.allocator, readback.allocation, 0, VK_WHOLE_SIZE);
    const bool ok = WriteScreenshotPng(path, static_cast<uint8_t*>(readback.mapped), width, height, output_format_);
    ctx_.DestroyBuffer(readback);
    ctx_.DestroyImage(scaled);
    return ok;
}
#endif

bool Renderer::SaveScreenshot(const std::filesystem::path& path) {
#ifdef PT_SWITCH
    if (window_ && !output_ready_ && final_.image) {
        return SaveSceneScreenshot(path);
    }
#endif
    if (!output_.image || !output_ready_) {
        LogError("screenshot {} not written: {}", path.string(), output_.image ? "no frame rendered yet" : "no capture image");
        return false;
    }
    vkDeviceWaitIdle(ctx_.device);
    const uint32_t width = output_.extent.width;
    const uint32_t height = output_.extent.height;
    vk::Buffer readback;
    if (!ctx_.CreateBuffer(readback, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
        return false;
    }
    ctx_.Submit([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, output_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    });
    vmaInvalidateAllocation(ctx_.allocator, readback.allocation, 0, VK_WHOLE_SIZE);
    const bool ok = WriteScreenshotPng(path, static_cast<uint8_t*>(readback.mapped), width, height, output_.format);
    ctx_.DestroyBuffer(readback);
    return ok;
}

/* XR_KHR_vulkan_enable2 hands the swapchain images over in colour attachment layout and wants them back the same way, hence no transitions here. */
void Renderer::RecordXr(VkCommandBuffer cmd) {
    if (!xr_layout_) {
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &composite_set_layout_;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        if (!vk::Check(vkCreatePipelineLayout(ctx_.device, &layout_info, nullptr, &xr_layout_), "xr copy layout")) {
            return;
        }
    }
    if (xr_frame_.hud) {
        if (!hud_.image) {
            if (!ctx_.CreateImage(hud_, output_format_, {kHudExtent.width, kHudExtent.height, 1},
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
                return;
            }
            VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
            VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool_info.maxSets = 1;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = &pool_size;
            vkCreateDescriptorPool(ctx_.device, &pool_info, nullptr, &xr_pool_);
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = xr_pool_;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &composite_set_layout_;
            vkAllocateDescriptorSets(ctx_.device, &alloc, &hud_set_);
            VkDescriptorImageInfo infos[2] = {{linear_sampler_, hud_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                              {linear_sampler_, hud_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet writes[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = hud_set_;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &infos[i];
            }
            vkUpdateDescriptorSets(ctx_.device, 2, writes, 0, nullptr);
        }
        vk::ImageBarrier(cmd, hud_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = hud_.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, kHudExtent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);
        if (overlay) {
            overlay(cmd, hud_.view, kHudExtent);
        }
        vkCmdEndRendering(cmd);
        vk::ImageBarrier(cmd, hud_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CopyToXr(cmd, hud_set_, *xr_frame_.hud, true);
    }
    if (xr_frame_.eye) {
        CopyToXr(cmd, final_set_, *xr_frame_.eye, false);
    }
}

void Renderer::CopyToXr(VkCommandBuffer cmd, VkDescriptorSet set, const XrTarget& target, bool premultiplied) {
    const VkPipeline pipeline = XrPipeline(target.format);
    if (!pipeline || !target.image) {
        return;
    }
    vk::ImageBarrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 1);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, target.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(target.extent.width), static_cast<float>(target.extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, xr_layout_, 0, 1, &set, 0, nullptr);
    const float brightness = std::clamp(brightness_override_ > 0.0f ? brightness_override_ : output_brightness, 0.1f, 4.0f);
    const float across = target.rect.w > 0.0f && target.extent.height
                             ? (static_cast<float>(target.extent.width) / static_cast<float>(target.extent.height)) / (16.0f / 9.0f)
                             : 1.0f;
    const float push[16] = {target.rect.x, target.rect.y, target.rect.z, target.rect.w,
                            premultiplied ? 1.0f : 0.0f, premultiplied ? 1.0f : brightness, 0.0f, 0.0f,
                            premultiplied ? 0.0f : grain[0], grain[1], grain[2], grain[3],
                            grain_offset[0], grain_offset[1], across, 0.0f};
    vkCmdPushConstants(cmd, xr_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

VkPipeline Renderer::XrPipeline(VkFormat format) {
    for (const auto& [f, pipeline] : xr_pipelines_) {
        if (f == format) {
            return pipeline;
        }
    }
    VkShaderModule vert = vk::LoadShaderModule(ctx_.device, "fullscreen.vert");
    VkShaderModule frag = vk::LoadShaderModule(ctx_.device, "xr_copy.frag");
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vert && frag) {
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = xr_layout_;
        if (!vk::Check(vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "xr copy pipeline")) {
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (vert) vkDestroyShaderModule(ctx_.device, vert, nullptr);
    if (frag) vkDestroyShaderModule(ctx_.device, frag, nullptr);
    xr_pipelines_.emplace_back(format, pipeline);
    return pipeline;
}

void Renderer::DestroyXr() {
    for (const auto& [format, pipeline] : xr_pipelines_) {
        if (pipeline) vkDestroyPipeline(ctx_.device, pipeline, nullptr);
    }
    xr_pipelines_.clear();
    if (xr_layout_) vkDestroyPipelineLayout(ctx_.device, xr_layout_, nullptr);
    xr_layout_ = VK_NULL_HANDLE;
    if (xr_pool_) vkDestroyDescriptorPool(ctx_.device, xr_pool_, nullptr);
    xr_pool_ = VK_NULL_HANDLE;
    hud_set_ = VK_NULL_HANDLE;
    ctx_.DestroyImage(hud_);
}

}
