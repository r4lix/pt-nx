#include "platform/switch/lsfg_bridge.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <optional>

#include <lsfg-vk-backend/lsfgvk.hpp>
#include <lsfg-vk-common/vulkan/command_buffer.hpp>
#include <lsfg-vk-common/vulkan/vulkan.hpp>

#include "engine/core/log.h"
#include "engine/render/vk_context.h"

namespace {
constexpr const char* kDllPath = "sdmc:/switch/pt/lsfg/Lossless.dll";
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }
}  // namespace

void pt_switch_lsfg_selftest(pt::vk::Context& ctx, uint32_t width, uint32_t height, bool performance_mode) {
    lsfgvk::backend::BorrowedDevice device{};
    device.instance = ctx.instance;
    device.physicalDevice = ctx.physical;
    device.device = ctx.device;
    device.queueFamilyIndex = ctx.queue_family;
    device.queue = ctx.queue;
    device.getInstanceProcAddr = ctx.loader ? ctx.loader : vkGetInstanceProcAddr;
    try {
        pt::LogInfo("lsfg: loading {}", kDllPath);
        auto started = Clock::now();
        lsfgvk::backend::Instance instance(device, std::filesystem::path(kDllPath), true);
        pt::LogInfo("lsfg: instance ready in {:.0f} ms (shaders extracted from the DLL)", Ms(started));
        const auto& backend_vulkan = instance.vulkan();
        const uint32_t sizes[][2] = {{width * 2 / 3, height * 2 / 3}, {width, height}, {width * 4 / 3, height * 4 / 3}};
        for (const auto& size : sizes) {
            started = Clock::now();
            static const float flow = std::getenv("PT_LSFG_FLOW") ? static_cast<float>(std::atof(std::getenv("PT_LSFG_FLOW"))) : 1.0f;
            auto& context = instance.openLocalContext(size[0], size[1], false, flow, performance_mode, 1);
            pt::LogInfo("lsfg: context {}x{} opened in {:.0f} ms ({} mode, flow {})", size[0], size[1], Ms(started), performance_mode ? "performance" : "quality", flow);
            vk::CommandBuffer commands(backend_vulkan);
            double total = 0.0;
            double worst = 0.0;
            constexpr int kRuns = 14;
            for (int i = 0; i < kRuns; ++i) {
                started = Clock::now();
                commands.begin(backend_vulkan);
                instance.recordFrame(context, commands);
                commands.end(backend_vulkan);
                commands.submit(backend_vulkan);  // waits for completion
                const double ms = Ms(started);
                if (i >= 4) {  // the first runs include pipeline warm-up
                    total += ms;
                    worst = std::max(worst, ms);
                }
                if (i < 3) pt::LogInfo("lsfg: run {}: {:.2f} ms", i, ms);
            }
            pt::LogInfo("lsfg: {}x{} average generation time {:.2f} ms, worst {:.2f} ms", size[0], size[1], total / (kRuns - 4), worst);
            instance.closeContext(context);
        }
        pt::LogInfo("lsfg: self test finished");
    } catch (const std::exception& e) {
        pt::LogError("lsfg: self test failed: {}", e.what());
    }
}

namespace {
std::unique_ptr<lsfgvk::backend::Instance> g_instance;
lsfgvk::backend::Context* g_context = nullptr;
uint32_t g_width = 0, g_height = 0;
size_t g_frame = 0;
bool g_failed = false;
std::string g_error;
VkImageView g_view = VK_NULL_HANDLE;
float g_flow = -1.0f;
}  // namespace

bool pt_switch_lsfg_active(pt::vk::Context& ctx, uint32_t width, uint32_t height, bool performance_mode) {
    if (g_failed) return false;
    try {
        if (!g_instance) {
            lsfgvk::backend::BorrowedDevice device{};
            device.instance = ctx.instance;
            device.physicalDevice = ctx.physical;
            device.device = ctx.device;
            device.queueFamilyIndex = ctx.queue_family;
            device.queue = ctx.queue;
            device.getInstanceProcAddr = ctx.loader ? ctx.loader : vkGetInstanceProcAddr;
            const auto started = Clock::now();
            g_instance = std::make_unique<lsfgvk::backend::Instance>(device, std::filesystem::path(kDllPath), true);
            pt::LogInfo("lsfg: instance ready in {:.0f} ms", Ms(started));
        }
        if (!g_context || g_width != width || g_height != height) {
            vkDeviceWaitIdle(ctx.device);
            if (g_view) {
                vkDestroyImageView(ctx.device, g_view, nullptr);
                g_view = VK_NULL_HANDLE;
            }
            if (g_context) g_instance->closeContext(*g_context);
            if (g_flow < 0.0f) g_flow = std::getenv("PT_LSFG_FLOW") ? static_cast<float>(std::atof(std::getenv("PT_LSFG_FLOW"))) : 2.0f;
            const float flow = g_flow;
            const auto started = Clock::now();
            g_context = &g_instance->openLocalContext(width, height, false, flow, performance_mode, 1);
            g_width = width;
            g_height = height;
            g_frame = 0;
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = g_instance->destinationImage(*g_context, 0);
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = VK_FORMAT_R8G8B8A8_UNORM;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(ctx.device, &view, nullptr, &g_view) != VK_SUCCESS) throw std::runtime_error("no view for the generated image");
            pt::LogInfo("lsfg: context {}x{} ({} mode, flow {}) opened in {:.0f} ms", width, height, performance_mode ? "performance" : "quality", flow, Ms(started));
        }
        return true;
    } catch (const std::exception& e) {
        pt::LogError("lsfg: unavailable: {}", e.what());
        g_error = e.what();
        g_failed = true;
        return false;
    }
}

void pt_switch_lsfg_images(VkImage& source_for_this_frame, VkImage& generated) {
    source_for_this_frame = g_instance->sourceImage(*g_context, g_frame & 1);
    generated = g_instance->destinationImage(*g_context, 0);
}

void pt_switch_lsfg_record(VkCommandBuffer cmd) {
    try {
        const vk::CommandBuffer wrapper(cmd);
        g_instance->recordFrame(*g_context, wrapper);
        ++g_frame;
    } catch (const std::exception& e) {
        pt::LogError("lsfg: record failed: {}", e.what());
        g_failed = true;
    }
}

VkImageView pt_switch_lsfg_generated_view() { return g_view; }
bool pt_switch_lsfg_ready() { return g_context && g_frame >= 2; }
const char* pt_switch_lsfg_error() { return g_failed ? g_error.c_str() : ""; }
void pt_switch_lsfg_set_flow(float flow) { g_flow = flow; }
