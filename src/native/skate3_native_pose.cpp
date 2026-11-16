// Native replacements for the character pose functions (see the header).

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

#include "native/skate3_native_guest_read.h"
#include "skate3_native_scene.h"

REXCVAR_DEFINE_STRING(skate3_native_pose_replace, "on", "Skate 3",
                      "Native replacements for the character pose functions (RE-7): off | "
                      "shadow | on. PackAndMultiplyMatricesForUpload (sub_827E5B30) and the "
                      "LivingWorld batch pack writer (sub_827C1D38) computed natively, bit for "
                      "bit; shadow compares with the recompiled functions (pose: lines), on "
                      "replaces them (default since 2026-11-16; off runs the recompiled code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_pose {
namespace {

using skate3::native_scene::GuestTryCopy;

constexpr uint32_t kInstStride = 0x28;  // cModelInstance
constexpr uint32_t kMaxBones = 256;     // bounds; the game's counts sit far below
constexpr uint32_t kLwPoseGetter = 0x827B8228;  // vtable +0x60: return *(this+0x4B0)

std::atomic<int> g_mode{-1};

int ReadMode() {
  const std::string v = REXCVAR_GET(skate3_native_pose_replace);
  return v == "on" ? 2 : v == "shadow" ? 1 : 0;
}

// Direct guest reads, as the recompiled functions do. These replacements
// only touch the addresses the guest code itself would touch on the same
// call, so a plain load is as safe as the original; the fault-guarded
// GuestTryCopy (a non-inlined SEH / sigsetjmp call per read) made the
// native path slower than the recompiled one. The address checks keep
// garbage pointers (null-ish values) out.
inline bool Readable(uint32_t addr, uint32_t n) {
  return addr >= 0x10000 && uint64_t(addr) + n <= 0x100000000ull;
}

inline bool Copy(void* dst, uint8_t* base, uint32_t addr, uint32_t n) {
  if (!Readable(addr, n)) return false;
  std::memcpy(dst, base + addr, n);
  return true;
}

inline bool U32(uint8_t* base, uint32_t addr, uint32_t* out) {
  uint32_t raw = 0;
  if (!Copy(&raw, base, addr, 4)) return false;
  *out = __builtin_bswap32(raw);
  return true;
}

inline bool U16(uint8_t* base, uint32_t addr, uint16_t* out) {
  uint16_t raw = 0;
  if (!Copy(&raw, base, addr, 2)) return false;
  *out = __builtin_bswap16(raw);
  return true;
}

// lvx / stvx ignore the low 4 address bits.
constexpr uint32_t Vec(uint32_t addr) { return addr & ~15u; }

// 16 big-endian floats (one Matrix44) from guest memory, as lvx reads them.
bool Load16(uint8_t* base, uint32_t addr, float out[16]) {
  uint32_t raw[16];
  if (!Copy(raw, base, Vec(addr), sizeof(raw))) return false;
  for (int i = 0; i < 16; ++i) out[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
  return true;
}

// One bone: rows k of M x L as the guest computes them (m.x * L0, then FMAs
// of m.y * L1, m.z * L2, m.w * L3), then the first three rows of the
// transpose, as big-endian words.
void PackBone(const float m[16], const simde__m128 l[4], uint32_t out_be[12]) {
  alignas(16) float r[4][4];
  for (int k = 0; k < 4; ++k) {
    simde__m128 acc = simde_mm_mul_ps(l[0], simde_mm_set1_ps(m[4 * k + 0]));
    acc = simde_mm_fmadd_ps(l[1], simde_mm_set1_ps(m[4 * k + 1]), acc);
    acc = simde_mm_fmadd_ps(l[2], simde_mm_set1_ps(m[4 * k + 2]), acc);
    acc = simde_mm_fmadd_ps(l[3], simde_mm_set1_ps(m[4 * k + 3]), acc);
    simde_mm_store_ps(r[k], acc);
  }
  for (int c = 0; c < 3; ++c) {
    for (int k = 0; k < 4; ++k) {
      out_be[c * 4 + k] = __builtin_bswap32(std::bit_cast<uint32_t>(r[k][c]));
    }
  }
}

bool LoadL(uint8_t* base, uint32_t addr, simde__m128 l[4]) {
  float f[16];
  if (!Load16(base, addr, f)) return false;
  for (int j = 0; j < 4; ++j) l[j] = simde_mm_loadu_ps(f + 4 * j);
  return true;
}

// ---- counters (window; exchanged to 0 at each stats line) -----------------
std::atomic<uint64_t> c_pack_calls{0}, c_pack_exact{0}, c_pack_diff{0}, c_pack_rows{0},
    c_pack_bad{0}, c_pack_native{0};
std::atomic<uint64_t> c_lw_calls{0}, c_lw_exact{0}, c_lw_diff{0}, c_lw_rows{0}, c_lw_bad{0},
    c_lw_native{0}, c_lw_getter{0};
std::atomic<uint64_t> c_ubt_calls{0}, c_ubt_exact{0}, c_ubt_diff{0}, c_ubt_rows{0},
    c_ubt_bad{0}, c_ubt_native{0};
std::atomic<uint64_t> g_log_diff{0};
uint64_t g_frames = 0;

// A pending shadow computation, per thread (the hooks run on job threads).
struct Expect {
  bool armed = false;
  uint32_t row_words = 12;      // 12 (packed 3x4 rows) or 16 (Matrix44 copies)
  std::vector<uint32_t> addr;   // guest address of each row
  std::vector<uint32_t> words;  // row_words big-endian words per row
};
thread_local Expect tl_pack, tl_lw, tl_ubt;

// Sk8::UpdateBoneTransforms (sub_827A52C8): a gather. For each part k
// (stride 0x28), each mesh of its model, each bone: copy the 64-byte
// Matrix44 srcPalette[remaps[k][u16 (mesh+0x4C)[j]]] to the part's
// m_matrices (+0x14), advancing 64. Models above 80 bones are skipped.
bool ComputeUbt(uint8_t* base, uint32_t parts, uint32_t count, uint32_t src, uint32_t remaps,
                Expect* e) {
  e->row_words = 16;
  e->addr.clear();
  e->words.clear();
  if (count > 64) return false;
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t part = parts + k * kInstStride;
    uint32_t model = 0, out = 0, remap = 0, mesh_table = 0;
    uint16_t nb = 0, nm = 0;
    if (!U32(base, part, &model) || !U32(base, part + 0x14, &out) ||
        !U32(base, remaps + 4 * k, &remap) || !U16(base, model + 0x34, &nb)) {
      return false;
    }
    if (nb > 80) continue;
    if (!U16(base, model + 0x32, &nm)) return false;
    if (nm == 0) continue;
    if (!U32(base, model + 0x24, &mesh_table)) return false;
    for (uint32_t mi = 0; mi < nm; ++mi) {
      uint32_t mesh = 0, cnt = 0, list = 0;
      if (!U32(base, mesh_table + 8 * mi, &mesh) || !U32(base, mesh + 0x48, &cnt)) return false;
      if (cnt == 0) continue;
      if (cnt > kMaxBones || !U32(base, mesh + 0x4C, &list)) return false;
      for (uint32_t j = 0; j < cnt; ++j) {
        uint16_t idx = 0;
        uint32_t sb = 0;
        if (!U16(base, list + 2 * j, &idx) || !U32(base, remap + 4u * idx, &sb)) return false;
        const uint32_t from = Vec(src + (sb << 6));
        const size_t at = e->words.size();
        e->words.resize(at + 16);
        if (!Copy(e->words.data() + at, base, from, 64)) return false;
        e->addr.push_back(Vec(out));
        out += 64;
      }
    }
  }
  return true;
}

// Pack: count at +0x18, rows in place at m_matrices (+0x14): bone i reads
// 64 bytes at +64i and writes 48 at +48i (never ahead of a later read).
bool ComputePack(uint8_t* base, uint32_t inst, uint32_t ltow, Expect* e) {
  uint32_t count = 0, mat = 0;
  simde__m128 l[4];
  if (!U32(base, inst + 0x18, &count) || !U32(base, inst + 0x14, &mat) || count > kMaxBones ||
      (count != 0 && (mat < 0x10000 || !LoadL(base, ltow, l)))) {
    return false;
  }
  e->row_words = 12;
  e->addr.clear();
  e->words.resize(size_t(count) * 12);
  for (uint32_t i = 0; i < count; ++i) {
    float m[16];
    if (!Load16(base, mat + 64 * i, m)) return false;
    PackBone(m, l, e->words.data() + size_t(i) * 12);
    e->addr.push_back(Vec(mat) + 48 * i);
  }
  return true;
}

// The LivingWorld writer (sub_827C1D38): see docs/re/symbols.toml.
bool ComputeLw(uint8_t* base, uint32_t entity, Expect* e) {
  uint32_t lod = 0, count = 0, parts = 0, vtbl = 0, getter = 0, src = 0;
  if (!U32(base, entity + 0x10, &lod) || lod > 8 || !U32(base, entity + (lod + 5) * 4, &count) ||
      !U32(base, entity + (lod + 8) * 4, &parts) || !U32(base, entity, &vtbl) ||
      !U32(base, vtbl + 0x60, &getter)) {
    return false;
  }
  e->row_words = 12;
  e->addr.clear();
  e->words.clear();
  if (count == 0) return true;
  if (getter != kLwPoseGetter) {
    c_lw_getter.fetch_add(1, std::memory_order_relaxed);  // another class: keep the guest's
    return false;
  }
  simde__m128 l[4];
  if (!U32(base, entity + 0x4B0, &src) || !LoadL(base, entity + 0x160, l)) return false;
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t inst = parts + k * kInstStride;
    uint32_t model = 0, remap = 0, out = 0, mesh_table = 0;
    uint16_t nb = 0, nm = 0;
    if (!U32(base, inst, &model) || !U32(base, inst + 0x14, &out) ||
        !U32(base, entity + 0x270 + 4 * k, &remap) || !U16(base, model + 0x34, &nb)) {
      return false;
    }
    if (nb > 80) continue;  // the guest skips these models
    if (!U16(base, model + 0x32, &nm)) return false;
    if (nm == 0) continue;
    if (!U32(base, model + 0x24, &mesh_table)) return false;
    for (uint32_t mi = 0; mi < nm; ++mi) {
      uint32_t mesh = 0, cnt = 0, list = 0;
      if (!U32(base, mesh_table + 8 * mi, &mesh) || !U32(base, mesh + 0x48, &cnt)) return false;
      if (cnt == 0) continue;
      if (cnt > kMaxBones || !U32(base, mesh + 0x4C, &list)) return false;
      for (uint32_t j = 0; j < cnt; ++j) {
        uint16_t idx = 0;
        uint32_t sb = 0;
        float m[16];
        if (!U16(base, list + 2 * j, &idx) || !U32(base, remap + 4u * idx, &sb) ||
            !Load16(base, src + (sb << 6), m)) {
          return false;
        }
        const size_t at = e->words.size();
        e->words.resize(at + 12);
        PackBone(m, l, e->words.data() + at);
        e->addr.push_back(Vec(out));
        out += 48;
      }
    }
  }
  return true;
}

void Compare(uint8_t* base, Expect& e, std::atomic<uint64_t>& rows, std::atomic<uint64_t>& exact,
             std::atomic<uint64_t>& diff, const char* what) {
  if (!e.armed) return;
  e.armed = false;
  bool same = true;
  const uint32_t rw = e.row_words;
  for (size_t r = 0; r < e.addr.size() && same; ++r) {
    uint32_t got[16];
    if (!GuestTryCopy(got, base + e.addr[r], rw * 4)) {
      same = false;
      break;
    }
    for (uint32_t w = 0; w < rw; ++w) {
      if (got[w] != e.words[r * rw + w]) {
        same = false;
        const uint64_t n = g_log_diff.fetch_add(1, std::memory_order_relaxed);
        if (n < 16) {
          REXLOG_WARN("pose: {} DIFF row {} at {:08X} word {}: guest {:08X} native {:08X}", what,
                      r, e.addr[r], w, __builtin_bswap32(got[w]),
                      __builtin_bswap32(e.words[r * rw + w]));
        }
        break;
      }
    }
  }
  rows.fetch_add(e.addr.size(), std::memory_order_relaxed);
  (same ? exact : diff).fetch_add(1, std::memory_order_relaxed);
}

void Write(uint8_t* base, const Expect& e) {
  for (size_t r = 0; r < e.addr.size(); ++r) {
    std::memcpy(base + e.addr[r], e.words.data() + r * e.row_words, e.row_words * 4);
  }
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

void PackBefore(PPCContext& ctx, uint8_t* base) {
  if (Mode() != 1) return;
  c_pack_calls.fetch_add(1, std::memory_order_relaxed);
  ctx.fpscr.enableFlushMode();  // the guest's VMX mode (the function sets it too)
  tl_pack.armed = ComputePack(base, ctx.r3.u32, ctx.r4.u32, &tl_pack);
  if (!tl_pack.armed) c_pack_bad.fetch_add(1, std::memory_order_relaxed);
}

void PackAfter(uint8_t* base) {
  Compare(base, tl_pack, c_pack_rows, c_pack_exact, c_pack_diff, "pack");
}

bool PackReplace(PPCContext& ctx, uint8_t* base) {
  if (Mode() != 2) return false;
  c_pack_calls.fetch_add(1, std::memory_order_relaxed);
  ctx.fpscr.enableFlushMode();
  // Computed in full before writing: in place, a bone's rows are read
  // before any later write reaches them (48i + 48 <= 64(i + 1)).
  if (!ComputePack(base, ctx.r3.u32, ctx.r4.u32, &tl_pack)) {
    c_pack_bad.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Write(base, tl_pack);
  c_pack_rows.fetch_add(tl_pack.addr.size(), std::memory_order_relaxed);
  c_pack_native.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void LwBefore(PPCContext& ctx, uint8_t* base) {
  if (Mode() != 1) return;
  c_lw_calls.fetch_add(1, std::memory_order_relaxed);
  ctx.fpscr.enableFlushMode();
  tl_lw.armed = ComputeLw(base, ctx.r3.u32, &tl_lw);
  if (!tl_lw.armed) c_lw_bad.fetch_add(1, std::memory_order_relaxed);
}

void LwAfter(uint8_t* base) {
  Compare(base, tl_lw, c_lw_rows, c_lw_exact, c_lw_diff, "lw");
}

bool LwReplace(PPCContext& ctx, uint8_t* base) {
  if (Mode() != 2) return false;
  c_lw_calls.fetch_add(1, std::memory_order_relaxed);
  ctx.fpscr.enableFlushMode();
  if (!ComputeLw(base, ctx.r3.u32, &tl_lw)) {
    c_lw_bad.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Write(base, tl_lw);
  c_lw_rows.fetch_add(tl_lw.addr.size(), std::memory_order_relaxed);
  c_lw_native.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void UbtBefore(PPCContext& ctx, uint8_t* base) {
  if (Mode() != 1) return;
  c_ubt_calls.fetch_add(1, std::memory_order_relaxed);
  tl_ubt.armed =
      ComputeUbt(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, &tl_ubt);
  if (!tl_ubt.armed) c_ubt_bad.fetch_add(1, std::memory_order_relaxed);
}

void UbtAfter(uint8_t* base) {
  Compare(base, tl_ubt, c_ubt_rows, c_ubt_exact, c_ubt_diff, "ubt");
}

bool UbtReplace(PPCContext& ctx, uint8_t* base) {
  if (Mode() != 2) return false;
  c_ubt_calls.fetch_add(1, std::memory_order_relaxed);
  // A plain copy (lvx/stvx): no floating-point mode involved.
  if (!ComputeUbt(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, &tl_ubt)) {
    c_ubt_bad.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Write(base, tl_ubt);
  c_ubt_rows.fetch_add(tl_ubt.addr.size(), std::memory_order_relaxed);
  c_ubt_native.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void OnFrameBuilt() {
  g_mode.store(ReadMode(), std::memory_order_relaxed);
  ++g_frames;
  if (g_mode.load(std::memory_order_relaxed) == 0 || g_frames % 120 != 0) return;
  const auto x = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
  REXLOG_INFO(
      "pose: mode {} frame {} pack[calls={} exact={} diff={} native={} bad={} rows={}] "
      "lw[calls={} exact={} diff={} native={} bad={} getter={} rows={}] "
      "ubt[calls={} exact={} diff={} native={} bad={} rows={}]",
      g_mode.load(std::memory_order_relaxed) == 2 ? "on" : "shadow", g_frames, x(c_pack_calls),
      x(c_pack_exact), x(c_pack_diff), x(c_pack_native), x(c_pack_bad), x(c_pack_rows),
      x(c_lw_calls), x(c_lw_exact), x(c_lw_diff), x(c_lw_native), x(c_lw_bad), x(c_lw_getter),
      x(c_lw_rows), x(c_ubt_calls), x(c_ubt_exact), x(c_ubt_diff), x(c_ubt_native),
      x(c_ubt_bad), x(c_ubt_rows));
}

}  // namespace skate3::native_pose
