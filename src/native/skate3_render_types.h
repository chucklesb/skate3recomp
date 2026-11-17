#pragma once
// The character render objects as C++ types (RE-5 / RE-7; docs/re/types.md,
// docs/re/characters.md). Names follow the Skate 2 PDB where the field kept
// its meaning in TU3; every offset here was confirmed against TU3 (the
// static_asserts pin them).
//
// Include after generated/skate3_init.h (REX_PHYS_HOST_OFFSET).
#include <cstddef>
#include <cstdint>

#include <rex/types.h>

namespace skate3::render {

using rex::be;

// A 4x4 float matrix, row-major, 64 bytes (Matrix44).
struct Matrix44 {
  be<float> m[16];
};
static_assert(sizeof(Matrix44) == 64);

// The skinning palette entry the vertex shader reads: the first three rows
// of a bone's (transposed) world matrix, 48 bytes.
struct PaletteRows {
  be<float> row[3][4];
};
static_assert(sizeof(PaletteRows) == 48);

// pegasus::tRModelData::tMeshData, as far as skinning reads it. TU3's layout
// differs from Skate 2's here.
struct MeshData {
  uint8_t unknown_00[0x48];
  be<uint32_t> bone_count;  // +0x48  palette entries this mesh uses
  be<uint32_t> bone_map;    // +0x4C  be<u16>[bone_count]: mesh bone -> model bone
};
static_assert(offsetof(MeshData, bone_count) == 0x48);
static_assert(offsetof(MeshData, bone_map) == 0x4C);

// One entry of the model's mesh table (8 bytes: the game steps by 8).
struct MeshTableEntry {
  be<uint32_t> mesh;  // MeshData*
  be<uint32_t> unknown_04;
};
static_assert(sizeof(MeshTableEntry) == 8);

// pegasus::tRModelData (the shared model).
struct ModelData {
  uint8_t unknown_00[0x24];
  be<uint32_t> mesh_table;  // +0x24  MeshTableEntry[num_meshes]
  uint8_t unknown_28[0x0A];
  be<uint16_t> num_meshes;  // +0x32
  be<uint16_t> num_bones;   // +0x34
};
static_assert(offsetof(ModelData, mesh_table) == 0x24);
static_assert(offsetof(ModelData, num_meshes) == 0x32);
static_assert(offsetof(ModelData, num_bones) == 0x34);

// Skinning skips models with more bones than this (UpdateBoneTransforms and
// the LivingWorld pack writer both check it).
constexpr uint16_t kMaxSkinnedModelBones = 80;

// Sk8::Render::MeshContext: one drawn mesh of an instance (TU3 grew it to
// 0x50 from Skate 2's 0x34).
constexpr uint32_t kMeshContextSize = 0x50;

// Sk8::Render::cModelInstance: one placed model with its packed palette.
struct ModelInstance {
  be<uint32_t> model;                // +0x00  ModelData*
  be<uint32_t> mesh_contexts_begin;  // +0x04  MeshContext[] (xvector begin)
  be<uint32_t> mesh_contexts_end;    // +0x08
  uint8_t unknown_0c[8];
  be<uint32_t> matrices;             // +0x14  the palette: Matrix44s before packing, PaletteRows after
  be<uint32_t> matrix_count;         // +0x18
  be<uint32_t> mesh_remap_depth;     // +0x1C
  be<uint32_t> mesh_remap_tables;    // +0x20
  uint8_t skinned;                   // +0x24
  uint8_t multi_parted;              // +0x25
  uint8_t unknown_26[2];
};
static_assert(sizeof(ModelInstance) == 0x28);
static_assert(offsetof(ModelInstance, matrices) == 0x14);
static_assert(offsetof(ModelInstance, matrix_count) == 0x18);

// Sk8::cLivingWorldPresEntity (pedestrians and other LivingWorld characters;
// TU3 grew it past Skate 2's 0x240).
constexpr uint32_t kLodCount = 3;
// Parts per LOD: the per-part remap table runs from +0x270 to the pose
// field (9 parts seen on the Odin 2, 2026-11-17).
constexpr uint32_t kMaxLivingWorldParts = (0x4B0 - 0x270) / 4;
struct LivingWorldEntity {
  be<uint32_t> vtable;
  uint8_t unknown_04[0x0C];
  be<uint32_t> lod;                        // +0x10  current LOD level
  be<uint32_t> lod_part_count[kLodCount];  // +0x14
  be<uint32_t> lod_instances[kLodCount];   // +0x20  ModelInstance[part_count]
  uint8_t unknown_2c[0x160 - 0x2C];
  Matrix44 local_to_world;                 // +0x160 m_MatLtoW
  uint8_t unknown_1a0[0x210 - 0x1A0];
  be<float> opacity[4];                    // +0x210 x = the alpha the shaders output
  uint8_t unknown_220[0x270 - 0x220];
  be<uint32_t> part_bone_remap[kMaxLivingWorldParts];  // +0x270 per part: be<u32>[] model bone -> pose bone
  be<uint32_t> pose;                       // +0x4B0 Matrix44 per skeleton bone (vtable +0x60 returns it)
};
static_assert(offsetof(LivingWorldEntity, lod) == 0x10);
static_assert(offsetof(LivingWorldEntity, lod_instances) == 0x20);
static_assert(offsetof(LivingWorldEntity, local_to_world) == 0x160);
static_assert(offsetof(LivingWorldEntity, opacity) == 0x210);
static_assert(offsetof(LivingWorldEntity, part_bone_remap) == 0x270);
static_assert(offsetof(LivingWorldEntity, pose) == 0x4B0);

// The LivingWorld vtable slot +0x60 implementation that returns `pose`; the
// pack writer's gather source for the classes we replace.
constexpr uint32_t kLivingWorldPoseGetter = 0x827B8228;

// A guest object by address, or nullptr outside the guest's address range
// (null-ish pointers). For code that reads only what the game itself is
// reading on the same call, so a fault means the game would fault too.
inline bool InGuestRange(uint32_t addr, uint32_t size) {
  return addr >= 0x10000 && uint64_t(addr) + size <= 0x100000000ull;
}
template <typename T>
const T* Find(uint8_t* base, uint32_t addr) {
  if (!InGuestRange(addr, sizeof(T))) return nullptr;
  return reinterpret_cast<const T*>(base + addr + REX_PHYS_HOST_OFFSET(addr));
}
template <typename T>
T* FindMutable(uint8_t* base, uint32_t addr) {
  return const_cast<T*>(Find<T>(base, addr));
}

// The game's vector loads ignore the low 4 address bits; matrices are read
// the way it reads them.
constexpr uint32_t VectorAligned(uint32_t addr) { return addr & ~15u; }

}  // namespace skate3::render
