#include "engine/render/pipeline_cache.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/log.h"

namespace pt::vk {

VkPipelineCache g_pipeline_cache = VK_NULL_HANDLE;
std::string g_pipeline_cache_file;
uint64_t g_pipeline_cache_key = 0;

namespace {

// file: this header, then the driver's data (which starts with Vulkan's own header: size, version, vendor, device, pipelineCacheUUID)
struct FileHeader {
    char magic[4];
    uint32_t version;
    uint64_t size;
    uint64_t hash;  // FNV-1a 64 of the data: a file cut short by a crash or a full card is never handed to the driver
    uint64_t key;   // g_pipeline_cache_key of the program that wrote it
};
constexpr char kMagic[4] = {'P', 'T', 'P', 'C'};
constexpr uint32_t kVersion = 1;

std::string g_path;
size_t g_saved_size = 0;  // the data size loaded or last written: a bigger cache holds new pipelines
std::thread g_writer;
std::atomic<bool> g_writing{false};

uint64_t Fnv1a(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    FileHeader header{};
    bool ok = std::fread(&header, sizeof(header), 1, f) == 1 && std::memcmp(header.magic, kMagic, 4) == 0 && header.version == kVersion &&
              header.key == g_pipeline_cache_key && header.size > 0 && header.size < (256ull << 20);
    if (ok) {
        out.resize(static_cast<size_t>(header.size));
        ok = std::fread(out.data(), 1, out.size(), f) == out.size() && Fnv1a(out.data(), out.size()) == header.hash;
    }
    std::fclose(f);
    if (!ok) {
        out.clear();
    }
    return ok;
}

// the driver's data, or empty when it has nothing or fails
std::vector<uint8_t> CacheData(VkDevice device) {
    std::vector<uint8_t> data;
    size_t size = 0;
    if (vkGetPipelineCacheData(device, g_pipeline_cache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return data;
    }
    data.resize(size);
    if (vkGetPipelineCacheData(device, g_pipeline_cache, &size, data.data()) != VK_SUCCESS) {
        data.clear();
        return data;
    }
    data.resize(size);
    return data;
}

// writes <path>.tmp, then replaces <path> with it (the Switch's file system does not rename over an existing file)
bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        return false;
    }
    FileHeader header{};
    std::memcpy(header.magic, kMagic, 4);
    header.version = kVersion;
    header.size = data.size();
    header.hash = Fnv1a(data.data(), data.size());
    header.key = g_pipeline_cache_key;
    bool ok = std::fwrite(&header, sizeof(header), 1, f) == 1 && std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

void JoinWriter() {
    if (g_writer.joinable()) {
        g_writer.join();
    }
}

}  // namespace

void LoadPipelineCache(VkDevice device, const VkPhysicalDeviceProperties& properties) {
    g_path = g_pipeline_cache_file;
    if (g_path.empty()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<uint8_t> data;
    const char* source = "none";
    for (const std::string& candidate : {g_path, g_path + ".tmp"}) {
        if (ReadFile(candidate, data)) {
            source = candidate == g_path ? "saved" : "saved (.tmp)";
            break;
        }
    }
    // Vulkan's header at the start of the data: only this driver and device may read it (the driver checks it too)
    if (!data.empty()) {
        VkPipelineCacheHeaderVersionOne vk_header{};
        const bool match = data.size() >= sizeof(vk_header) && (std::memcpy(&vk_header, data.data(), sizeof(vk_header)), true) &&
                           vk_header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && vk_header.vendorID == properties.vendorID &&
                           vk_header.deviceID == properties.deviceID &&
                           std::memcmp(vk_header.pipelineCacheUUID, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
        if (!match) {
            data.clear();
            source = "saved, from another driver build (not used)";
        }
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device, &info, nullptr, &g_pipeline_cache) != VK_SUCCESS) {
        g_pipeline_cache = VK_NULL_HANDLE;
        LogInfo("pipeline cache: not available");
        return;
    }
    g_saved_size = data.size();
    char uuid[2 * VK_UUID_SIZE + 1] = {};
    for (uint32_t i = 0; i < VK_UUID_SIZE; ++i) std::snprintf(uuid + 2 * i, 3, "%02x", properties.pipelineCacheUUID[i]);
    LogInfo("pipeline cache: {} KB from {} ({}), read in {:.0f} ms; driver cache id {}, key {:016x}", data.size() / 1024, g_path, source,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), uuid, g_pipeline_cache_key);
}

void SavePipelineCacheAsync(VkDevice device) {
    if (g_pipeline_cache == VK_NULL_HANDLE || g_writing.load()) {
        return;
    }
    JoinWriter();
    g_writing = true;
    g_writer = std::thread([device] {
        const auto start = std::chrono::steady_clock::now();
        const std::vector<uint8_t> data = CacheData(device);
        if (!data.empty() && data.size() > g_saved_size) {
            const bool ok = WriteFile(g_path, data);
            LogInfo("pipeline cache: {} KB written {} in {:.0f} ms", data.size() / 1024, ok ? "" : "(failed)",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            if (ok) {
                g_saved_size = data.size();
            }
        }
        g_writing = false;
    });
}

void ClosePipelineCache(VkDevice device) {
    if (g_pipeline_cache == VK_NULL_HANDLE) {
        return;
    }
    JoinWriter();
    const std::vector<uint8_t> data = CacheData(device);
    if (!data.empty() && data.size() > g_saved_size) {
        const bool ok = WriteFile(g_path, data);
        LogInfo("pipeline cache: {} KB written at exit{}", data.size() / 1024, ok ? "" : " (failed)");
    }
    vkDestroyPipelineCache(device, g_pipeline_cache, nullptr);
    g_pipeline_cache = VK_NULL_HANDLE;
}

}  // namespace pt::vk
