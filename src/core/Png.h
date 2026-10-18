#pragma once

#include <cstdint>
#include <string>

namespace clipper {

// width / height from the IHDR chunk, no decoding. false if it isn't a png
bool pngSize(const std::string& png, uint32_t* width, uint32_t* height);

}  // namespace clipper
