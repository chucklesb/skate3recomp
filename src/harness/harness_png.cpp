#include "harness/harness_png.h"

#include <fstream>
#include <vector>

// Encode to memory only; files are written below with std::ofstream.
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "thirdparty/stb/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace skate3::harness {
namespace {

void AppendBytes(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

bool WritePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width,
              uint32_t height, std::string* error) {
  if (!rgba || width == 0 || height == 0) {
    *error = "empty image";
    return false;
  }
  std::vector<uint8_t> png;
  png.reserve(static_cast<size_t>(width) * height);
  // Fastest zlib level: bursts matter more than file size.
  stbi_write_png_compression_level = 1;
  if (!stbi_write_png_to_func(AppendBytes, &png, static_cast<int>(width),
                              static_cast<int>(height), 4, rgba, static_cast<int>(width) * 4)) {
    *error = "PNG encode failed";
    return false;
  }
  std::filesystem::path part = path;
  part += ".part";
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
      *error = "write failed";
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(part, path, ec);
  if (ec) {
    *error = ec.message();
    return false;
  }
  return true;
}

}  // namespace skate3::harness
