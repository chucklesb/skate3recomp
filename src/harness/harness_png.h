#pragma once
// PNG writing for the scenario harness (stb_image_write, every platform).

#include <cstdint>
#include <filesystem>
#include <string>

namespace skate3::harness {

// Writes tightly packed RGBA8 as a PNG (written to <path>.part, then
// renamed, so a reader never sees a half-written file).
bool WritePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width,
              uint32_t height, std::string* error);

}  // namespace skate3::harness
