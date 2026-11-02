// Inert implementations of the desktop only subsystems for the Nintendo Switch build: the vendor upscalers and frame generation,
// NVIDIA Streamline, OpenXR, virtual pads, the microphone and voice recognizer, HTTP, LiveSplit and the process helpers.
// Every one reports "not available", which the engine already handles for machines without that hardware or SDK.
#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/platform/http.h"
#include "engine/platform/livesplit.h"
#include "engine/platform/os.h"
#include "engine/platform/virtual_pad.h"
#include "engine/render/upscale/frame_generation.h"
#include "engine/render/upscale/streamline.h"
#include "engine/render/upscale/upscale.h"
#include "engine/render/upscale/upscale_platform.h"
#include "engine/voice/microphone.h"
#include "engine/voice/voice_recognizer.h"
#include "engine/xr/xr_host.h"

// newlib's getentropy (reached through arc4random_buf in libexpat) needs this hook, which libnx does not supply: the console's CSRNG
extern "C" int _getentropy_r(struct _reent*, void* buf, size_t len) {
    if (len > 256) return -1;
    randomGet(buf, len);
    return 0;
}

namespace pt {

// upscalers ------------------------------------------------------------------------------------------------------------

std::unique_ptr<UpscaleBackend> CreateFsrBackend(vk::Context&, int) { return nullptr; }
std::unique_ptr<UpscaleBackend> CreateDlssBackend(vk::Context&) { return nullptr; }
std::unique_ptr<UpscaleBackend> CreateStreamlineDlssBackend(vk::Context&) { return nullptr; }
std::unique_ptr<UpscaleBackend> CreateXessBackend(vk::Context&) { return nullptr; }
void XessDeviceRequirements(bool, VkInstance, VkPhysicalDevice, DeviceFeatureSet&) {}
void DlssInstanceExtensions(bool, std::vector<std::string>&) {}
void DlssDeviceExtensions(bool, VkInstance, VkPhysicalDevice, std::vector<std::string>&) {}
bool DlssFrameGenRequirements(VkInstance, VkPhysicalDevice, uint32_t& unsupported, std::string& detail) {
    unsupported = 1;
    detail = "not available on this platform";
    return false;
}
uint32_t DlssPresetHint(DlssModel) { return 0; }
bool DlssAutoExposure() { return true; }
std::unique_ptr<FrameGeneration> CreateFrameGeneration(vk::Context&, const FrameGenQueues&) { return nullptr; }
std::unique_ptr<FrameGeneration> CreateDlssFrameGeneration(vk::Context&) { return nullptr; }

std::string NarrowText(const wchar_t* text) {
    std::string out;
    for (; text && *text; ++text) out.push_back(*text < 128 ? static_cast<char>(*text) : '?');
    return out;
}
int HardwareGpuScheduling(const uint8_t (&)[8]) { return -1; }

namespace streamline {

bool Start(const std::filesystem::path&, std::string& reason) {
    reason = "not available on this platform";
    return false;
}
bool Active() { return false; }
PFN_vkGetInstanceProcAddr InstanceProcAddr() { return nullptr; }
void DeviceCreated(VkInstance, VkPhysicalDevice, VkDevice) {}
void Shutdown() {}
bool DlssSupported() { return false; }
bool FrameGenSupported(std::string* reason) {
    if (reason) *reason = "not available on this platform";
    return false;
}
bool FrameGenNeedsVsyncOff() { return false; }
void BeginFrame() {}
void SetMarker(Marker) {}
void SetConstants(const FrameConstants&) {}
bool ConstantsSet() { return false; }
void SetFrameLimit(uint32_t) {}
const void* FrameToken() { return nullptr; }

}  // namespace streamline

// OpenXR ---------------------------------------------------------------------------------------------------------------

namespace xr {

struct Host::Impl {};

Host::Host() = default;
Host::~Host() = default;
bool Host::Init(const std::string&) {
    error_ = "VR is not available on this platform";
    return false;
}
bool Host::Ready() const { return false; }
VkResult Host::CreateInstance(const VkInstanceCreateInfo&, VkInstance&) { return VK_ERROR_INITIALIZATION_FAILED; }
VkPhysicalDevice Host::PhysicalDevice(VkInstance) { return VK_NULL_HANDLE; }
VkResult Host::CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo&, VkDevice&) { return VK_ERROR_INITIALIZATION_FAILED; }
bool Host::StartSession(vk::Context&, float) { return false; }
void Host::Shutdown() {}
void Host::PollEvents() {}
bool Host::SessionRunning() const { return false; }
bool Host::ExitRequested() const { return false; }
bool Host::Focused() const { return false; }
bool Host::FocusLost() const { return false; }
bool Host::WaitFrame() { return false; }
bool Host::ShouldRender() const { return false; }
bool Host::BeginFrame() { return false; }
void Host::LocateViews() {}
void Host::SyncActions() {}
void Host::EndFrame(const FrameLayers&) {}
void Host::Haptic(int, float, float) {}
bool Host::Acquire(Swapchain&) { return false; }
void Host::Release(Swapchain&) {}
bool Available() { return false; }

}  // namespace xr

// input and voice ------------------------------------------------------------------------------------------------------

VirtualPads::~VirtualPads() = default;
void VirtualPads::UseOnlyVirtualDevices() {}
bool VirtualPads::Attach(int, std::string_view) { return false; }
bool VirtualPads::Detach(int) { return false; }
void VirtualPads::DetachAll() {}
bool VirtualPads::SetButton(int, std::string_view, bool) { return false; }
bool VirtualPads::SetAxis(int, std::string_view, float) { return false; }
bool VirtualPads::SetTouch(int, bool, float, float) { return false; }
bool VirtualPads::Attached(int) const { return false; }
uint32_t VirtualPads::Id(int) const { return 0; }
VirtualRumble VirtualPads::TakeRumble(int) { return {}; }
VirtualPads::Slot* VirtualPads::Get(int) { return nullptr; }

bool Microphone::Open(int, const std::string&) { return false; }
void Microphone::Close() {}
size_t Microphone::Read(std::vector<int16_t>&) { return 0; }
bool Microphone::SetMonitor(bool) { return false; }

VoiceRecognizer::VoiceRecognizer() = default;
VoiceRecognizer::~VoiceRecognizer() = default;
bool VoiceRecognizer::Init(const std::filesystem::path&, const std::string& keyword) {
    keyword_ = keyword;
    state_ = State::Failed;
    return false;
}
void VoiceRecognizer::Shutdown() { state_ = State::Off; }
void VoiceRecognizer::Reset() {}
bool VoiceRecognizer::Feed(std::span<const int16_t>) { return false; }
bool VoiceRecognizer::Finish() { return false; }
bool VoiceRecognizer::Drain() { return false; }
std::string VoiceRecognizer::LastHypothesis() const { return {}; }
std::vector<VoiceRecognizer::Result> VoiceRecognizer::TakeResults() { return {}; }
bool VoiceRecognizer::MatchesKeyword(std::string_view, int, int* word_count) {
    if (word_count) *word_count = 0;
    return false;
}

// network and process helpers ------------------------------------------------------------------------------------------

namespace http {

std::optional<Response> Get(const Request&) { return std::nullopt; }
std::optional<int64_t> ParseHttpDate(const std::string&) { return std::nullopt; }

}  // namespace http

LiveSplitClient::~LiveSplitClient() = default;
void LiveSplitClient::Configure(bool, std::string, int) {}
void LiveSplitClient::Send(std::string_view) {}
void LiveSplitClient::Poll() {}
void LiveSplitClient::Close() {}
void LiveSplitClient::StartConnect() {}

namespace os {

FILE* OpenFile(const std::filesystem::path& path, const char* mode) { return std::fopen(path.string().c_str(), mode); }
int SeekFile(FILE* file, int64_t offset, int origin) { return fseeko(file, static_cast<off_t>(offset), origin); }
std::string GetEnv(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : std::string();
}
uint32_t ProcessId() { return 1; }
ProcessResult RunProcess(const std::filesystem::path&, const std::vector<std::string>&, const std::filesystem::path&, const std::filesystem::path&,
                         const std::atomic<bool>&, std::chrono::milliseconds) {
    return {};  // a console application cannot start another one
}
FileLock::FileLock(const std::filesystem::path&) : held_(true) {}
FileLock::~FileLock() = default;

}  // namespace os

}  // namespace pt
