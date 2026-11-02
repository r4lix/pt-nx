#pragma once

#include <volk.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/fs/qar.h"

namespace pt {

struct FtexTexture {
    uint16_t pixel_format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t mip_count = 0;
    uint32_t faces = 1;
    uint32_t flags = 0;
    uint8_t filter = 0;
    uint16_t address_mode = 0;
    std::vector<std::vector<uint8_t>> mips;

    bool Srgb() const { return (flags & 2) != 0; }
    bool Cube() const { return (flags & 4) != 0; }
    bool NormalMap() const { return (flags & 8) != 0; }
    VkFormat Format() const;
    uint32_t MipWidth(uint32_t level) const { return std::max(1u, width >> level); }
    uint32_t MipHeight(uint32_t level) const { return std::max(1u, height >> level); }
};

std::string FtexStem(std::string_view path);

// A cache of decompressed textures on disk (the Switch build: the archive keeps them zlib-compressed and inflating them on the console's
// CPU is most of a stage's texture load time). LoadFtex reads a cached texture instead of the archive and writes one after decoding.
// Empty directory: off.
void SetDecodedTextureCache(const std::filesystem::path& dir);
struct DecodedTextureCacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    double read_ms = 0.0;
};
DecodedTextureCacheStats GetDecodedTextureCacheStats();

// Largest side of a 2D texture that is loaded: bigger ones start at a lower mip (the top mips are never read or inflated). 0: full size.
// The console reads the archive at about 50 MB/s, so the top mip of a 2K texture (three quarters of its bytes) is most of the load time.
void SetMaxTextureSide(uint32_t side);
std::optional<std::vector<uint8_t>> ReadTextureFile(const QarArchive& qar, const std::string& name);
bool LoadFtex(const QarArchive& qar, std::string_view path, FtexTexture& out);
bool DecodeFtexLevel(const FtexTexture& ftex, uint32_t level, std::vector<uint8_t>& rgba);
void EncodeBc1Block(const uint8_t* rgba, uint8_t* out);
void EncodeBc3AlphaBlock(const uint8_t* rgba, uint8_t* out);

}
