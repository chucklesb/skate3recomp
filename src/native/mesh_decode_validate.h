#pragma once

// DecodeMesh input validation (KI-27). Header-only and templated on the item
// type so it can be unit-checked against a small fixture without the
// renderer (the fields used are the DrawItem vertex-layout members).
//
// DecodeMesh reads every vertex element at `v * stride + offset` straight
// out of a host copy of the guest payload. The offsets come from the guest
// vertex declaration (16-bit fields) while the stride is a separate byte, so
// a malformed or half-initialized declaration could place an element past
// the end of the vertex and the last vertex would read past the end of the
// host buffer. Validating "offset + bytes read <= stride" per element plus
// "num_verts * stride <= payload bytes" bounds every vertex read.

#include <cstddef>
#include <cstdint>

namespace skate3::native_scene {

// Sanity caps for one decoded mesh. The guest index format is u16, so real
// meshes stay far below these; they only stop a garbage count from turning
// into a multi-GB upload buffer or guest copy.
constexpr uint32_t kMaxDecodeVerts = 1u << 20;     // 56 MiB decoded VB
constexpr uint32_t kMaxDecodeIndices = 1u << 23;   // 16 MiB guest IB

// Bytes DecodeMesh reads for a POSITION element of xenos vertex format
// `fmt`; 0 = format DecodeMesh cannot decode.
constexpr uint32_t MeshPosReadBytes(uint32_t fmt) {
  switch (fmt) {
    case 57: return 12;  // k_32_32_32_FLOAT
    case 32: return 6;   // k_16_16_16_16_FLOAT (xyz)
    case 26: return 6;   // k_16_16_16_16 snorm (xyz)
    default: return 0;
  }
}

// Bytes DecodeMesh reads for a TEXCOORD element of format `fmt` (0 = the
// format is not decoded, nothing is read). These are the bytes actually
// consumed, not the format's nominal size: the synthetic cloth-quad item
// declares a float2 UV as k_32_32_32_32_FLOAT (38) at +16 of a 24-byte
// stride, and only xy is read. Half4 (32) reads all 8 bytes (uv0's second
// pair is the decal UV) and s16x4 (26) reads all 8 bytes (lm_norm in the
// water / world-family normal packs).
constexpr uint32_t MeshUvReadBytes(uint32_t fmt) {
  switch (fmt) {
    case 31: return 4;  // k_16_16_FLOAT
    case 32: return 8;  // k_16_16_16_16_FLOAT
    case 37: return 8;  // k_32_32_FLOAT
    case 38: return 8;  // k_32_32_32_32_FLOAT (xy only)
    case 57: return 8;  // k_32_32_32_FLOAT (xy only)
    case 25: return 4;  // k_16_16 snorm
    case 26: return 8;  // k_16_16_16_16 snorm
    default: return 0;
  }
}

// Validate a mesh's vertex layout and counts before DecodeMesh touches the
// payload. vb_avail / ib_avail are the bytes actually available behind the
// vertex / index payload pointers. Returns nullptr when the decode is safe,
// otherwise a short static reason for the rejection log.
template <typename Item>
const char* ValidateMeshDecodeInput(const Item& item, size_t vb_avail,
                                    size_t ib_avail) {
  const uint32_t stride = item.stride;
  if (stride == 0) {
    return "zero stride";
  }
  const uint32_t num_verts = item.vb_bytes / stride;
  if (num_verts == 0) {
    return "no vertices";
  }
  if (num_verts > kMaxDecodeVerts) {
    return "vertex count over cap";
  }
  if (item.ib_count == 0) {
    return "no indices";
  }
  if (item.ib_count > kMaxDecodeIndices) {
    return "index count over cap";
  }
  if (size_t(num_verts) * stride > vb_avail) {
    return "vertex payload shorter than count x stride";
  }
  if (!item.cloth_quads && size_t(item.ib_count) * 2 > ib_avail) {
    return "index payload shorter than count x 2";
  }
  // bytes == 0: the element is not decoded, so its offset is never read.
  const auto fits = [stride](uint32_t offset, uint32_t bytes) {
    return bytes == 0 || uint64_t(offset) + bytes <= stride;
  };
  const uint32_t pos_bytes = MeshPosReadBytes(item.pos_fmt);
  if (pos_bytes == 0) {
    return "unsupported position format";
  }
  if (!fits(item.pos_offset, pos_bytes)) {
    return "position element past stride";
  }
  if (!fits(item.uv_offset, MeshUvReadBytes(item.uv_fmt))) {
    return "texcoord0 element past stride";
  }
  if (!fits(item.uv2_offset, MeshUvReadBytes(item.uv2_fmt))) {
    return "texcoord1 element past stride";
  }
  if (item.skinned &&
      (!fits(item.bw_offset, 4) || !fits(item.bi_offset, 4))) {
    return "blend weight/index element past stride";
  }
  if (item.normal_fmt == 16 && !fits(item.normal_offset, 4)) {
    return "normal element past stride";
  }
  // tan_fmt marks a usage-6 tangent (water layout), tb_fmt a tangent +
  // binormal pair; both are k_10_11_11 words.
  if ((item.tan_fmt == 16 || item.tb_fmt == 16) &&
      !fits(item.tangent_offset, 4)) {
    return "tangent element past stride";
  }
  if (item.tb_fmt == 16 && !fits(item.binormal_offset, 4)) {
    return "binormal element past stride";
  }
  return nullptr;
}

}  // namespace skate3::native_scene
