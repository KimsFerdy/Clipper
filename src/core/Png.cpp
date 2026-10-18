#include "core/Png.h"

#include <cstring>

namespace clipper {

bool pngSize(const std::string& png, uint32_t* width, uint32_t* height) {
    // 8 byte signature, then the first chunk is always IHDR: u32 length, "IHDR", u32 width, u32 height (big endian)
    static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 24 || std::memcmp(png.data(), kSignature, 8) != 0 || png.compare(12, 4, "IHDR") != 0)
        return false;
    auto be32 = [&](size_t at) {
        const auto* p = reinterpret_cast<const unsigned char*>(png.data() + at);
        return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
    };
    *width = be32(16);
    *height = be32(20);
    return true;
}

}  // namespace clipper
