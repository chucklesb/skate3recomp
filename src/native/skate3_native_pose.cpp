// Native replacements for the character pose functions (see the header).
//
// Written over the render types in skate3_render_types.h (ModelInstance,
// ModelData, MeshData, LivingWorldEntity). The skinning math keeps the
// guest's floating-point operation order (a multiply, then three fused
// multiply-adds per row) because the result must be bit-exact; everything
// around it is plain C++ over named fields.

#include "native/skate3_native_pose.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

#include <simde/x86/fma.h>
#include <simde/x86/sse.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/skate3_init.h"
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_replace.h"
#include "native/skate3_render_types.h"
#include "skate3_native_scene.h"

REXCVAR_DEFINE_STRING(skate3_native_pose_replace, "on", "Skate 3",
                      "Native replacements for the character pose functions (RE-7): off | "
                      "shadow | on. PackAndMultiplyMatricesForUpload (sub_827E5B30) and the "
                      "LivingWorld batch pack writer (sub_827C1D38) computed natively, bit for "
                      "bit; shadow compares with the recompiled functions (pose: lines), on "
                      "replaces them (default since 2026-11-16; off runs the recompiled code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_native_lw_rows_replace, "on", "Skate 3",
                      "RE-7: the LivingWorld-derived BindConstants' copy of the 160-byte "
                      "lighting rows (sub_827816C0): off | shadow (compared; replace: lwrows) "
                      "| on")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_pose {
namespace {

using namespace skate3::render;
using skate3::native_replace::Writes;

constexpr uint32_t kMaxParts = 64;   // refuse beyond: the game's counts sit far below
constexpr uint32_t kMaxBones = 256;  // per mesh, likewise

std::atomic<int> g_mode{-1};

int ReadMode() {
  const std::string v = REXCVAR_GET(skate3_native_pose_replace);
  return v == "on" ? 2 : v == "shadow" ? 1 : 0;
}

// A matrix as the guest's vector loads read it, in host floats.
bool LoadMatrix(uint8_t* base, uint32_t addr, float out[16]) {
  const Matrix44* m = Find<Matrix44>(base, VectorAligned(addr));
  if (!m) return false;
  for (int i = 0; i < 16; ++i) out[i] = m->m[i];
  return true;
}

bool LoadRows(uint8_t* base, uint32_t addr, simde__m128 rows[4]) {
  float f[16];
  if (!LoadMatrix(base, addr, f)) return false;
  for (int j = 0; j < 4; ++j) rows[j] = simde_mm_loadu_ps(f + 4 * j);
  return true;
}

// One bone's palette entry: bone x local_to_world, transposed, first three
// rows. Each row k of the product is computed as the guest does (bone[k].x
// * L0, then fused multiply-adds of .y * L1, .z * L2, .w * L3).
void SkinBone(const float bone[16], const simde__m128 local_to_world[4], uint32_t out_be[12]) {
  alignas(16) float product[4][4];
  for (int k = 0; k < 4; ++k) {
    simde__m128 acc = simde_mm_mul_ps(local_to_world[0], simde_mm_set1_ps(bone[4 * k + 0]));
    acc = simde_mm_fmadd_ps(local_to_world[1], simde_mm_set1_ps(bone[4 * k + 1]), acc);
    acc = simde_mm_fmadd_ps(local_to_world[2], simde_mm_set1_ps(bone[4 * k + 2]), acc);
    acc = simde_mm_fmadd_ps(local_to_world[3], simde_mm_set1_ps(bone[4 * k + 3]), acc);
    simde_mm_store_ps(product[k], acc);
  }
  for (int row = 0; row < 3; ++row) {
    for (int k = 0; k < 4; ++k) {
      out_be[row * 4 + k] = __builtin_bswap32(std::bit_cast<uint32_t>(product[k][row]));
    }
  }
}

// Every mesh of a skinned model, in order: calls fn(mesh) for each, after
// the checks the game makes. Returns false (refuse) on an unreadable
// pointer; true when done or when the game would skip the model.
template <typename Fn>
bool ForEachSkinnedMesh(uint8_t* base, const ModelData& model, Fn&& fn) {
  if (model.num_bones > kMaxSkinnedModelBones) return true;  // the game skips it
  for (uint32_t i = 0; i < model.num_meshes; ++i) {
    const auto* entry = Find<MeshTableEntry>(base, model.mesh_table + i * sizeof(MeshTableEntry));
    const MeshData* mesh = entry ? Find<MeshData>(base, entry->mesh) : nullptr;
    if (!mesh) return false;
    if (mesh->bone_count == 0) continue;
    if (mesh->bone_count > kMaxBones) return false;
    if (!fn(*mesh)) return false;
  }
  return true;
}

// The model bone a mesh bone maps to, then the pose bone through the part's
// remap table.
bool PoseBone(uint8_t* base, const MeshData& mesh, uint32_t j, uint32_t remap, uint32_t* out) {
  const auto* model_bone = Find<be<uint16_t>>(base, mesh.bone_map + 2 * j);
  const auto* pose_bone = model_bone ? Find<be<uint32_t>>(base, remap + 4u * *model_bone) : nullptr;
  if (!pose_bone) return false;
  *out = *pose_bone;
  return true;
}

// Sk8::UpdateBoneTransforms (sub_827A52C8): a gather. For each part and each
// mesh of its model, copy each bone's pose matrix into the part's palette
// (m_matrices), one 64-byte Matrix44 after another.
bool ComputeUbt(uint8_t* base, uint32_t parts, uint32_t count, uint32_t pose, uint32_t remaps,
                Writes* e) {
  e->row_words = 16;
  e->addr.clear();
  e->words.clear();
  if (count > kMaxParts) return false;
  for (uint32_t k = 0; k < count; ++k) {
    const auto* part = Find<ModelInstance>(base, parts + k * sizeof(ModelInstance));
    const auto* remap = Find<be<uint32_t>>(base, remaps + 4 * k);
    const ModelData* model = part ? Find<ModelData>(base, part->model) : nullptr;
    if (!part || !remap || !model) return false;
    uint32_t out = part->matrices;
    const bool ok = ForEachSkinnedMesh(base, *model, [&](const MeshData& mesh) {
      for (uint32_t j = 0; j < mesh.bone_count; ++j) {
        uint32_t bone = 0;
        if (!PoseBone(base, mesh, j, *remap, &bone)) return false;
        const auto* from = Find<Matrix44>(base, VectorAligned(pose + bone * sizeof(Matrix44)));
        if (!from) return false;
        const size_t at = e->words.size();
        e->words.resize(at + 16);
        std::memcpy(e->words.data() + at, from, sizeof(Matrix44));  // stays big-endian
        e->addr.push_back(VectorAligned(out));
        out += sizeof(Matrix44);
      }
      return true;
    });
    if (!ok) return false;
  }
  return true;
}

// Sk8::PackAndMultiplyMatricesForUpload (sub_827E5B30): in place, each of
// the instance's matrix_count Matrix44s becomes its 48-byte palette entry
// (entry i is written after matrix i is read, and 48(i+1) <= 64(i+1), so a
// later read is never overwritten first).
bool ComputePack(uint8_t* base, uint32_t instance, uint32_t local_to_world, Writes* e) {
  const auto* inst = Find<ModelInstance>(base, instance);
  if (!inst || inst->matrix_count > kMaxBones) return false;
  const uint32_t count = inst->matrix_count;
  const uint32_t palette = inst->matrices;
  simde__m128 ltw[4];
  if (count != 0 && (!InGuestRange(palette, 1) || !LoadRows(base, local_to_world, ltw))) {
    return false;
  }
  e->row_words = 12;
  e->addr.clear();
  e->words.resize(size_t(count) * 12);
  for (uint32_t i = 0; i < count; ++i) {
    float bone[16];
    if (!LoadMatrix(base, palette + sizeof(Matrix44) * i, bone)) return false;
    SkinBone(bone, ltw, e->words.data() + size_t(i) * 12);
    e->addr.push_back(VectorAligned(palette) + sizeof(PaletteRows) * i);
  }
  return true;
}

// The LivingWorld batch pack writer (sub_827C1D38): UpdateBoneTransforms and
// PackAndMultiply fused, for each part of the entity's current LOD: every
// bone's pose matrix, skinned by the entity's local_to_world, appended to the
// part's palette.
// The first refusal of each kind is logged (the game's code runs instead).
bool RefuseLw(const char* why, uint32_t entity, uint32_t value) {
  static std::atomic<uint32_t> s_seen{0};
  const uint32_t bit = 1u << (uint32_t(why[0]) & 31);
  if ((s_seen.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
    REXLOG_WARN("pose: lw refused ({}) entity {:08X} value {}", why, entity, value);
  }
  return false;
}

bool ComputeLw(uint8_t* base, uint32_t entity_addr, Writes* e) {
  const auto* entity = Find<LivingWorldEntity>(base, entity_addr);
  if (!entity) return RefuseLw("address", entity_addr, 0);
  if (entity->lod >= kLodCount) return RefuseLw("lod", entity_addr, entity->lod);
  const uint32_t count = entity->lod_part_count[entity->lod];
  const uint32_t parts = entity->lod_instances[entity->lod];
  e->row_words = 12;
  e->addr.clear();
  e->words.clear();
  if (count == 0) return true;
  // The pose comes from a virtual getter; only the classes whose getter is
  // the plain `return pose` are handled (another class: the game's code).
  const auto* getter = Find<be<uint32_t>>(base, entity->vtable + 0x60);
  if (!getter || *getter != kLivingWorldPoseGetter) {
    return RefuseLw("getter (another class)", entity_addr, getter ? uint32_t(*getter) : 0);
  }
  if (count > kMaxLivingWorldParts) return RefuseLw("parts", entity_addr, count);
  simde__m128 ltw[4];
  if (!LoadRows(base, entity_addr + offsetof(LivingWorldEntity, local_to_world), ltw)) {
    return false;
  }
  for (uint32_t k = 0; k < count; ++k) {
    const auto* part = Find<ModelInstance>(base, parts + k * sizeof(ModelInstance));
    const ModelData* model = part ? Find<ModelData>(base, part->model) : nullptr;
    if (!model) return false;
    const uint32_t remap = entity->part_bone_remap[k];
    uint32_t out = part->matrices;
    const bool ok = ForEachSkinnedMesh(base, *model, [&](const MeshData& mesh) {
      for (uint32_t j = 0; j < mesh.bone_count; ++j) {
        uint32_t bone = 0;
        float m[16];
        if (!PoseBone(base, mesh, j, remap, &bone) ||
            !LoadMatrix(base, entity->pose + bone * sizeof(Matrix44), m)) {
          return false;
        }
        const size_t at = e->words.size();
        e->words.resize(at + 12);
        SkinBone(m, ltw, e->words.data() + at);
        e->addr.push_back(VectorAligned(out));
        out += sizeof(PaletteRows);
      }
      return true;
    });
    if (!ok) return false;
  }
  return true;
}

bool PackCompute(PPCContext& ctx, uint8_t* base, Writes* w) {
  ctx.fpscr.enableFlushMode();  // the guest's vector float mode (the function sets it too)
  // Computed in full before writing; see ComputePack for why in place is safe.
  return ComputePack(base, ctx.r3.u32, ctx.r4.u32, w);
}

bool LwCompute(PPCContext& ctx, uint8_t* base, Writes* w) {
  ctx.fpscr.enableFlushMode();
  return ComputeLw(base, ctx.r3.u32, w);
}

std::atomic<int> g_rows_mode{-1};

int ReadRowsMode() {
  const std::string v = REXCVAR_GET(skate3_native_lw_rows_replace);
  return v == "on" ? 2 : v == "shadow" ? 1 : 0;
}

int RowsMode() {
  int m = g_rows_mode.load(std::memory_order_relaxed);
  if (m < 0) {
    m = ReadRowsMode();
    g_rows_mode.store(m, std::memory_order_relaxed);
  }
  return m;
}

// The lighting rows a LivingWorld-derived BindConstants (sub_827C25D8) and
// sub_82812040 take from the global lighting object: 9 vector rows and 4
// words, 160 bytes.
struct LightingRows {
  be<float> rows[9][4];   // +0, copied with vector loads (16-byte aligned)
  be<uint32_t> words[4];  // +144
};
static_assert(sizeof(LightingRows) == 160);

// sub_827816C0(dst, src): copies the lighting rows.
bool LwRowsCompute(PPCContext& ctx, uint8_t* base, Writes* w) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  w->row_words = 4;
  w->addr.clear();
  w->words.clear();
  // The game copies row by row; copying all first is the same unless the
  // blocks overlap (never seen; refuse).
  const uint32_t gap = dst > src ? dst - src : src - dst;
  if (gap != 0 && gap < sizeof(LightingRows) + 16) return false;
  for (uint32_t k = 0; k < 9; ++k) {
    const uint32_t row = offsetof(LightingRows, rows) + 16 * k;
    const auto* from = Find<be<float>[4]>(base, VectorAligned(src + row));
    if (!from) return false;
    std::memcpy(w->Add(VectorAligned(dst + row)), from, 16);
  }
  const auto* words = Find<be<uint32_t>[4]>(base, src + offsetof(LightingRows, words));
  if (!words) return false;
  std::memcpy(w->Add(dst + offsetof(LightingRows, words)), words, 16);
  return true;
}

bool UbtCompute(PPCContext& ctx, uint8_t* base, Writes* w) {
  // A plain copy: no floating-point mode involved.
  return ComputeUbt(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, w);
}

}  // namespace

int Mode() {
  int m = g_mode.load(std::memory_order_relaxed);
  if (m < 0) {
    m = ReadMode();
    g_mode.store(m, std::memory_order_relaxed);
  }
  return m;
}

native_replace::Family g_pack{"pack", &Mode, &PackCompute};
native_replace::Family g_lw{"lw", &Mode, &LwCompute};
native_replace::Family g_ubt{"ubt", &Mode, &UbtCompute};
native_replace::Family g_lw_rows{"lwrows", &RowsMode, &LwRowsCompute};

void OnFrameBuilt() {
  g_mode.store(ReadMode(), std::memory_order_relaxed);
  g_rows_mode.store(ReadRowsMode(), std::memory_order_relaxed);
}

}  // namespace skate3::native_pose

extern "C" REX_FUNC(sub_827816C0) {
  skate3::native_replace::Run(skate3::native_pose::g_lw_rows, ctx, base, &__imp__sub_827816C0);
}
