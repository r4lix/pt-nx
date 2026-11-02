#pragma once

#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <utility>
#include <vector>

#include "engine/render/vk_context.h"

struct SDL_Window;

namespace pt {

struct RendererSettings {
    bool validation = false;
    bool vsync = true;
    bool headless = false;
    uint32_t width = 1600;
    uint32_t height = 900;
};

struct XrTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};
};

struct XrFrame {
    const XrTarget* eye = nullptr;
    const XrTarget* hud = nullptr;
    bool overlay_on_frame = false;
};

class Renderer {
public:
    bool Init(SDL_Window* window, const RendererSettings& settings);
    void Shutdown();

    bool BeginFrame(bool present = true);
    void EndFrame(bool draw_ui);
    void Resize(uint32_t width, uint32_t height);
    void SetVsync(bool enabled);
    bool SaveScreenshot(const std::filesystem::path& path);

    vk::Context& Context() { return ctx_; }
    VkCommandBuffer Cmd() const { return frames_[frame_index_].cmd; }
    const vk::Image& SceneColor() const { return scene_color_; }
    VkExtent2D RenderExtent() const { return {scene_color_.extent.width, scene_color_.extent.height}; }
    uint32_t FrameIndex() const { return frame_index_; }
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    float exposure = 1.0f;
    float sharpen = -1.0f;  // sharpening of an upscaled scene in the final pass: < 0 the default (PT_SHARPEN, 0.6 on the Switch), else 0..1
    float output_brightness = 1.0f;
    float fade[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float grain[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float grain_offset[2] = {0.0f, 0.0f};
    std::function<void(VkCommandBuffer, VkImageView, VkExtent2D)> overlay;
    // the same overlay again for the second image of a frame-generation frame (records what overlay built, without stepping it again)
    std::function<void(VkCommandBuffer, VkImageView, VkExtent2D)> overlay_repeat;
    // Switch: LSFG frame generation (30 -> 60): each rendered frame is shown after a generated one, two presents of one VBlank each
    bool frame_generation = false;
    void SetFrameGeneration(bool on) { frame_generation = on; }  // BeginFrame then takes the late acquire path (the swapchain has 3 images)
    uint64_t presents = 0;            // images handed to the presentation engine, and how many of them were generated frames
    uint64_t generated_presents = 0;
    std::function<const vk::Image*(uint32_t)> hudless;
    // where a frame's time goes (filled every frame): CPU blocked on the frame fence, on the swapchain image, in present, and the
    // whole frame's GPU time (first to last command, scene + composite + UI), read back two frames later
    struct FrameTiming {
        float fence_ms = 0.0f;
        float acquire_ms = 0.0f;
        float present_ms = 0.0f;
        float gpu_ms = 0.0f;
        float gpu_start_after_submit_ms = 0.0f;  // GPU start of the frame minus the CPU time of its (last) submit; < 0: started earlier
        float gpu_idle_before_ms = 0.0f;         // GPU idle between the end of the previous frame and the start of this one
        bool calibrated = false;
    };
    const FrameTiming& Timing() const { return timing_; }
    uint32_t swap_interval = 0;  // Switch: VBlanks per presented image (2 = locked 30 fps); 0 leaves the WSI's choice

    void SetGrainNoise(VkImageView view);

    void SetRenderExtent(VkExtent2D extent) { render_extent_ = extent; }
    void SetXrFrame(const XrFrame& frame) { xr_frame_ = frame; xr_pending_ = true; }
    static constexpr VkExtent2D kHudExtent{1920, 1080};

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore scene_done = VK_NULL_HANDLE;  // late acquire: signalled by the scene submit so the driver kicks it off at once
        int64_t submit_ns = 0;                     // CPU time of the frame's final submit (frame timeline diagnostics)
        VkCommandBuffer present_cmd = VK_NULL_HANDLE;  // late acquire: the swapchain part of the frame, submitted after the scene
        VkCommandBuffer present_cmd2 = VK_NULL_HANDLE;  // frame generation: the second image of the frame
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkSemaphore image_available2 = VK_NULL_HANDLE;  // frame generation: the second swapchain image
        VkFence in_flight = VK_NULL_HANDLE;
    };

    bool CreateTargets(uint32_t width, uint32_t height);
    void DestroyTargets();
    bool CreateCompositePipeline(VkFormat output_format);
#ifdef PT_SWITCH
    bool SaveSceneScreenshot(const std::filesystem::path& path);
#endif
    bool InitImGui(SDL_Window* window);
    void Composite(VkCommandBuffer cmd, VkDescriptorSet set, float mode, VkExtent2D extent, VkOffset2D offset = {0, 0});
    void WriteCompositeSets();
    void WriteFgSet(VkImageView generated);
    void RecordXr(VkCommandBuffer cmd);
    void CopyToXr(VkCommandBuffer cmd, VkDescriptorSet set, const XrTarget& target, bool premultiplied);
    VkPipeline XrPipeline(VkFormat format);
    void DestroyXr();

    vk::Context ctx_;
    RendererSettings settings_;
    SDL_Window* window_ = nullptr;
    Frame frames_[kFramesInFlight];
    uint32_t frame_index_ = 0;
    uint32_t image_index_ = 0;
    bool swapchain_dirty_ = false;
    bool imgui_ready_ = false;
    bool output_ready_ = false;
    float brightness_override_ = 0.0f;

    // Switch: acquire the swapchain image only after the scene is submitted, and make only the final composite wait for it. With one
    // submission waiting on the acquire semaphore, the whole frame's GPU work waits until the display gives an image back, which with
    // a swap interval of 2 serialises CPU and GPU (measured: 27 ms of GPU work gave 45 ms frames)
    bool late_acquire_ = false;
    bool late_acquire_env_ = false;  // PT_LATE_ACQUIRE; frame generation also needs it (the swapchain has 3 images: the next scene must not wait for one)
    VkQueryPool frame_queries_ = VK_NULL_HANDLE;  // two timestamps per frame slot
    bool frame_queries_written_[kFramesInFlight] = {};
    float timestamp_ns_ = 1.0f;
    int64_t calib_offset_ns_ = 0;  // CLOCK_MONOTONIC ns minus device timestamp ns
    bool calib_ok_ = false;
    uint64_t last_gpu_end_ = 0;
    FrameTiming timing_;

    vk::Image scene_color_;
    vk::Image final_;
    vk::Image output_;
    VkFormat output_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    VkSampler wrap_sampler_ = VK_NULL_HANDLE;
    VkImageView grain_noise_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool composite_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet composite_set_ = VK_NULL_HANDLE;
    VkDescriptorSet final_set_ = VK_NULL_HANDLE;
    VkDescriptorSet fg_set_ = VK_NULL_HANDLE;  // the final pass reading the generated image
    VkImageView fg_view_ = VK_NULL_HANDLE;
    VkImageView fg_noise_ = VK_NULL_HANDLE;
    VkPipelineLayout composite_layout_ = VK_NULL_HANDLE;
    VkPipeline composite_pipeline_ = VK_NULL_HANDLE;

    VkExtent2D render_extent_{0, 0};
    bool presenting_ = false;
    XrFrame xr_frame_;
    bool xr_pending_ = false;
    vk::Image hud_;
    VkDescriptorPool xr_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet hud_set_ = VK_NULL_HANDLE;
    VkPipelineLayout xr_layout_ = VK_NULL_HANDLE;
    std::vector<std::pair<VkFormat, VkPipeline>> xr_pipelines_;
};

}
