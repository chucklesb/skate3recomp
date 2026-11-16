// Character skeleton store (see the header).

#include "native/skate3_native_skeleton.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_entity.h"
#include "native/skate3_native_guest_read.h"
#include "skate3_native_scene.h"

REXCVAR_DEFINE_STRING(skate3_native_render_scene_skeleton, "on", "Skate 3",
                      "Character skeleton store (RE-5): off | shadow | on. Captures one pose "
                      "per skater per tick at UpdateBoneTransforms and rebuilds every piece's "
                      "palette from it. shadow compares it with the bank capture and the "
                      "palette serve, changing nothing; on serves skater pieces from it and "
                      "resets their pose smoothing per character (skel: lines; "
                      "docs/re/characters.md). Default on since 2026-11-16; off = the old "
                      "per-piece path, for A/B")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_skeleton_log_interval, 120, "Skate 3",
                     "Frames between skel: stats lines while the skeleton store is on "
                     "(0 = off)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_scene_skeleton_lw, "on", "Skate 3",
                      "Skeleton store for LivingWorld entities (pedestrians, RE-5b): off | "
                      "shadow | on. Their fused pack writer (sub_827C1D38) gathers from one "
                      "pose array (*(entity+0x4B0)) with the skaters' formula; shadow builds "
                      "and compares their pieces (skel-lw: lines), on serves them (default since "
                      "2026-11-16). Needs skate3_native_render_scene_skeleton on or shadow")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_skeleton_log_jumps, false, "Skate 3",
                    "DEBUG: for 4 frames after a character's jump (skel: jump), log every "
                    "item of that character as drawn (skel-jf: lines)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_skeleton {
namespace {

using skate3::native_scene::DrawItem;
using skate3::native_scene::FrameScene;
using skate3::native_scene::GuestTryCopy;

constexpr uint32_t kInstStride = 0x28;  // cModelInstance
constexpr uint32_t kMaxParts = 32;
constexpr uint32_t kMaxRows = 96;        // DrawItem palettes top out here
constexpr uint32_t kMaxModelBones = 80;  // UBT skips models above this
constexpr uint32_t kMaxSkelBones = 512;
// A root step the motion does not explain (a jump): the character gate the
// pose smoothing uses for one tick (1.5 m), against the position the last
// two ticks predict.
constexpr float kJumpM = 1.5f;
constexpr double kGapS = 0.1;

// 0 = off, 1 = shadow, 2 = on. Refreshed once per built frame (string cvar).
std::atomic<int> g_mode{-1};

int ReadMode() {
  const std::string v = REXCVAR_GET(skate3_native_render_scene_skeleton);
  return v == "on" ? 2 : v == "shadow" ? 1 : 0;
}

// LivingWorld entities (RE-5b): 0 off, 1 shadow, 2 on; refreshed per frame.
std::atomic<int> g_lw_mode{0};
int LwMode() { return g_lw_mode.load(std::memory_order_relaxed); }

int Mode() {
  int m = g_mode.load(std::memory_order_relaxed);
  if (m < 0) {
    m = ReadMode();
    g_mode.store(m, std::memory_order_relaxed);
  }
  return m;
}

double NowS() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool U32(uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x10000) return false;
  uint32_t raw = 0;
  if (!GuestTryCopy(&raw, base + addr, 4)) return false;
  *out = __builtin_bswap32(raw);
  return true;
}

bool U16(uint8_t* base, uint32_t addr, uint16_t* out) {
  if (addr < 0x10000) return false;
  uint16_t raw = 0;
  if (!GuestTryCopy(&raw, base + addr, 2)) return false;
  *out = __builtin_bswap16(raw);
  return true;
}

// One UBT part = one LOD instance: where its gather list sits in Tick::idx.
struct Part {
  uint32_t inst = 0;
  uint32_t idx_ofs = 0;
  uint16_t rows = 0;        // palette rows the gather writes (all meshes)
  uint16_t pack_count = 0;  // the instance's +0x18 row count
  bool built = false;       // false: UBT skipped it (bones > 80) or unreadable
};

// One tick of one character: the pose rows the gather references (host-order
// Matrix44s, row-vector, translation in row 3), every part's gather list and
// the pack's localToWorld.
struct Tick {
  uint32_t seq = 0;
  uint32_t epoch = 0;
  uint64_t frame = 0;
  double t = 0.0;  // steady-clock seconds at the UBT
  uint32_t parts = 0;
  uint32_t count = 0;
  std::vector<Part> part;
  std::vector<uint16_t> idx;  // skeleton bone per palette row
  std::vector<float> pose;    // 16 floats per skeleton bone
  float ltow[16] = {};
  bool have_ltow = false;
  uint32_t packed_mask = 0;

  const Part* Find(uint32_t inst) const {
    if (inst < parts || (inst - parts) % kInstStride != 0) return nullptr;
    const uint32_t k = (inst - parts) / kInstStride;
    return k < part.size() ? &part[k] : nullptr;
  }
};

struct Entity {
  Tick cur, prev;  // cur may still wait for its pack; prev is always complete
  bool cur_valid = false;
  bool prev_valid = false;
  uint64_t last_frame = 0;
  uint32_t epoch = 1;
  // The last two complete ticks' roots, for the jump test.
  float root[2][3] = {};
  double root_t[2] = {};
  int roots = 0;
  // Newest complete tick first.
  const Tick* Newest() const {
    return cur_valid && cur.have_ltow ? &cur : prev_valid ? &prev : nullptr;
  }
  const Tick* Older() const { return cur_valid && cur.have_ltow && prev_valid ? &prev : nullptr; }
};

std::mutex g_mu;
std::unordered_map<uint32_t, Entity> g_ents;
uint64_t g_frames = 0;
std::atomic<uint32_t> g_seq{0};

// ---- counters (window; exchanged to 0 at each stats line) ------------------
std::atomic<uint64_t> c_ticks{0}, c_unpacked{0}, c_ubt_unowned{0}, c_part_skipped{0},
    c_part_bad{0}, c_pose_unread{0}, c_ltow_var{0}, c_pack_foreign{0};
// Jumps by cause.
std::atomic<uint64_t> c_jump_root{0}, c_jump_gap{0}, c_jump_parts{0};
// Items of skater-family entities, how many the store could build, and why
// not.
std::atomic<uint64_t> c_items{0}, c_built{0}, c_miss_noent{0}, c_miss_notick{0},
    c_miss_nopart{0}, c_miss_skipped{0}, c_unmapped{0}, c_served{0}, c_served_tail{0};
// [0] newest tick exact, [1] older tick exact, [2] close, [3] divergent.
std::atomic<uint64_t> c_bank[4], c_srv[4];
std::atomic<uint64_t> c_srv_unserved{0}, c_bank_none{0};
// The same for LivingWorld entities (RE-5b; the skel-lw: line).
std::atomic<uint64_t> c_lw_items{0}, c_lw_built{0}, c_lw_miss{0}, c_lw_served{0},
    c_lw_ticks{0}, c_lw_bad{0}, c_lw_bank_none{0};
std::atomic<uint64_t> c_lw_bank[4];
// Gap-filled pedestrian items (dbg_src 9) in ServeRange: served, not
// servable, no store entry / tick / part, not a mapped LW entity.
std::atomic<uint64_t> c_lw_tail_served{0}, c_lw_tail_unservable{0}, c_lw_tail_nobuild{0},
    c_lw_tail_notlw{0};
std::atomic<uint64_t> g_log_div{0}, g_log_jump{0};

struct Worst {
  float trans = -1.0f;
  float rot = 0.0f;
  uint32_t entity = 0, mesh = 0, k = 0;
  const char* vs = "";
};
Worst g_worst;  // g_mu

void BuildRow(const float* p, const float* l, float* o) {
  float w[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      w[r * 4 + c] = p[r * 4 + 0] * l[c] + p[r * 4 + 1] * l[4 + c] + p[r * 4 + 2] * l[8 + c] +
                     p[r * 4 + 3] * l[12 + c];
    }
  }
  // Row-vector 4x4 -> the column-vector [R|t] rows the shader reads.
  o[0] = w[0]; o[1] = w[4]; o[2] = w[8];   o[3] = w[12];
  o[4] = w[1]; o[5] = w[5]; o[6] = w[9];   o[7] = w[13];
  o[8] = w[2]; o[9] = w[6]; o[10] = w[10]; o[11] = w[14];
}

// Rows of part `p` of tick `t`, 12 floats each, n of them.
void BuildPiece(const Tick& t, const Part& p, uint32_t n, float* out) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t b = t.idx[p.idx_ofs + i];
    BuildRow(t.pose.data() + size_t(b) * 16, t.ltow, out + size_t(i) * 12);
  }
}

struct Err {
  float rot = 0.0f, trans = 0.0f;
};

Err Compare(const float* a, const float* b, uint32_t n) {
  Err e;
  for (uint32_t i = 0; i < n * 12; ++i) {
    const float d = std::fabs(a[i] - b[i]);
    float& slot = (i % 4 == 3) ? e.trans : e.rot;
    slot = std::max(slot, std::isfinite(d) ? d : 1e30f);
  }
  return e;
}

bool Exact(const Err& e) { return e.rot <= 2e-4f && e.trans <= 2e-3f; }
bool Close(const Err& e) { return e.rot <= 0.05f && e.trans <= 0.05f; }

// Per-item scratch (render thread only; BeforeServe -> AfterServe).
struct Shadow {
  size_t item = 0;
  uint32_t ctx = 0, mesh = 0, entity = 0, k = 0, n = 0;
  uint32_t ofs_new = 0, ofs_old = 0;
  uint32_t seq = 0, epoch = 0;
  bool have_old = false;
  bool lw = false;  // a LivingWorld entity (RE-5b)
};
std::vector<Shadow> g_shadow;
std::vector<float> g_rows;

int Classify(const Shadow& s, const float* other, Err* worst) {
  const Err a = Compare(g_rows.data() + s.ofs_new, other, s.n);
  Err b;
  if (s.have_old) b = Compare(g_rows.data() + s.ofs_old, other, s.n);
  if (Exact(a)) return 0;
  if (s.have_old && Exact(b)) return 1;
  const Err& best = (s.have_old && b.trans < a.trans) ? b : a;
  *worst = best;
  return Close(a) || (s.have_old && Close(b)) ? 2 : 3;
}

void NoteDiv(const Shadow& s, const Err& e, const char* vs, const DrawItem& item) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (e.trans > g_worst.trans) g_worst = {e.trans, e.rot, s.entity, s.mesh, s.k, vs};
  }
  const uint64_t n = g_log_div.fetch_add(1, std::memory_order_relaxed);
  if (n < 16 || n % 4096 == 0) {
    REXLOG_INFO(
        "skel: divergent vs {} entity {:08X} piece {} mesh {:08X} ctx {:08X} rows {} "
        "err rot {:.4f} trans {:.4f} | item src {} seq {} ent {:08X}",
        vs, s.entity, s.k, s.mesh, s.ctx, s.n, e.rot, e.trans, item.dbg_src, item.pose_seq,
        item.pose_entity);
  }
}

uint64_t X(std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); }

// A newly complete tick: does it continue the entity's last one? g_mu held.
void CheckContinuity(uint32_t owner, Entity& e, const Tick& t) {
  const float r[3] = {t.ltow[12], t.ltow[13], t.ltow[14]};
  const char* why = nullptr;
  float miss = 0.0f;
  if (e.roots > 0) {
    if (t.t - e.root_t[0] > kGapS) {
      why = "gap";
      c_jump_gap.fetch_add(1, std::memory_order_relaxed);
    } else if (e.prev_valid && (e.prev.parts != t.parts || e.prev.count != t.count)) {
      why = "parts";
      c_jump_parts.fetch_add(1, std::memory_order_relaxed);
    } else {
      // Predicted from the last two roots (a hitch can hold several ticks of
      // real travel, and speed glitches move metres every tick).
      float pred[3] = {e.root[0][0], e.root[0][1], e.root[0][2]};
      if (e.roots > 1 && e.root_t[0] - e.root_t[1] > 1e-4) {
        const double k = (t.t - e.root_t[0]) / (e.root_t[0] - e.root_t[1]);
        for (int i = 0; i < 3; ++i) pred[i] += float((e.root[0][i] - e.root[1][i]) * k);
      }
      const float dx = r[0] - pred[0], dy = r[1] - pred[1], dz = r[2] - pred[2];
      miss = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (miss > kJumpM) {
        why = "root";
        c_jump_root.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  if (why != nullptr) {
    ++e.epoch;
    e.roots = 0;
    const uint64_t n = g_log_jump.fetch_add(1, std::memory_order_relaxed);
    if (n < 32 || n % 1024 == 0) {
      REXLOG_INFO("skel: jump entity {:08X} epoch {} ({}, root off by {:.2f} m) at ({:.2f},{:.2f},{:.2f})",
                  owner, e.epoch, why, miss, r[0], r[1], r[2]);
    }
  }
  std::memcpy(e.root[1], e.root[0], sizeof(e.root[0]));
  e.root_t[1] = e.root_t[0];
  std::memcpy(e.root[0], r, sizeof(r));
  e.root_t[0] = t.t;
  e.roots = std::min(e.roots + 1, 2);
}

// Look up, build (and in "on" mode serve) one item. Returns true when built;
// fills s. g_rows grows by the rows built.
enum class Built { kNo, kYes, kNotSkater };
Built BuildItem(const DrawItem& item, size_t i, Shadow* s, bool count) {
  skate3::native_entity::CtxInfo ident;
  if (!skate3::native_entity::LookupCtx(item.ctx, &ident) || ident.entity == 0) {
    if (count) c_unmapped.fetch_add(1, std::memory_order_relaxed);
    return Built::kNotSkater;
  }
  using skate3::native_entity::EntClass;
  const bool lw = ident.cls == EntClass::kLivingWorld;
  if (lw ? LwMode() == 0
         : (ident.cls != EntClass::kSkater && ident.cls != EntClass::kColorized &&
            ident.cls != EntClass::kCac && ident.cls != EntClass::kSkaterAux)) {
    return Built::kNotSkater;
  }
  if (count) (lw ? c_lw_items : c_items).fetch_add(1, std::memory_order_relaxed);
  s->lw = lw;
  s->item = i;
  s->ctx = item.ctx;
  s->mesh = item.mesh;
  s->entity = ident.entity;
  std::lock_guard<std::mutex> lock(g_mu);
  const auto it = g_ents.find(ident.entity);
  const auto miss = [&](std::atomic<uint64_t>& c) {
    if (count) (lw ? c_lw_miss : c).fetch_add(1, std::memory_order_relaxed);
    return Built::kNo;
  };
  if (it == g_ents.end()) return miss(c_miss_noent);
  const Tick* nt = it->second.Newest();
  if (nt == nullptr) return miss(c_miss_notick);
  const Part* p = nt->Find(ident.instance);
  if (p == nullptr) return miss(c_miss_nopart);
  if (!p->built) return miss(c_miss_skipped);
  uint32_t n = std::min<uint32_t>(uint32_t(item.bones.size() / 12), p->rows);
  if (p->pack_count >= 1 && p->pack_count <= kMaxRows) n = std::min<uint32_t>(n, p->pack_count);
  s->k = (ident.instance - nt->parts) / kInstStride;
  s->n = n;
  s->seq = nt->seq;
  s->epoch = nt->epoch;
  s->ofs_new = uint32_t(g_rows.size());
  g_rows.resize(g_rows.size() + size_t(n) * 12);
  BuildPiece(*nt, *p, n, g_rows.data() + s->ofs_new);
  s->have_old = false;
  const Tick* ot = it->second.Older();
  const Part* op = ot ? ot->Find(ident.instance) : nullptr;
  if (op != nullptr && op->built && op->rows >= n) {
    s->have_old = true;
    s->ofs_old = uint32_t(g_rows.size());
    g_rows.resize(g_rows.size() + size_t(n) * 12);
    BuildPiece(*ot, *op, n, g_rows.data() + s->ofs_old);
  }
  if (count) (lw ? c_lw_built : c_built).fetch_add(1, std::memory_order_relaxed);
  return Built::kYes;
}

void Serve(DrawItem& item, const Shadow& s) {
  std::memcpy(item.bones.data(), g_rows.data() + s.ofs_new, size_t(s.n) * 12 * sizeof(float));
  // Rows past the game's own count are never indexed by the mesh.
  std::fill(item.bones.begin() + size_t(s.n) * 12, item.bones.end(), 0.0f);
  item.dbg_src = kDbgSrc;
  item.caster_bank = false;
  item.pose_entity = s.entity;
  item.pose_seq = s.seq;
  item.pose_part = uint16_t(s.k);
  item.pose_set = 0;
  item.serve_div = -1.0f;
  item.skel_epoch = s.epoch;
}

bool Servable(const DrawItem& item) {
  return item.skinned && item.ctx != 0 && item.bones.size() >= 12 && !item.pending &&
         item.dbg_src != 10;
}

}  // namespace

bool Active() { return Mode() != 0; }

void OnBoneTransforms(uint8_t* base, uint32_t owner, uint32_t parts, uint32_t count,
                      uint32_t src_palette, uint32_t remaps) {
  if (!Active() || parts == 0 || count == 0) return;
  if (owner == 0) {
    c_ubt_unowned.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Tick t;
  t.t = NowS();
  t.parts = parts;
  t.count = std::min(count, kMaxParts);
  t.part.resize(t.count);
  uint32_t max_bone = 0;
  // Mirrors the gather (sub_827A52C8): for part k, out rows continue across
  // the model's meshes; row = srcPalette[remap_k[u16 (mesh+0x4C)[i]]].
  for (uint32_t k = 0; k < t.count; ++k) {
    Part& p = t.part[k];
    p.inst = parts + k * kInstStride;
    p.idx_ofs = uint32_t(t.idx.size());
    uint32_t model = 0, remap = 0, mesh_table = 0, pack_count = 0;
    uint16_t nb = 0, nm = 0;
    if (!U32(base, p.inst, &model) || !U32(base, remaps + 4 * k, &remap) ||
        !U16(base, model + 0x34, &nb) || !U16(base, model + 0x32, &nm) ||
        !U32(base, model + 0x24, &mesh_table)) {
      c_part_bad.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    U32(base, p.inst + 0x18, &pack_count);
    p.pack_count = uint16_t(std::min<uint32_t>(pack_count, 0xFFFF));
    if (nb > kMaxModelBones) {
      c_part_skipped.fetch_add(1, std::memory_order_relaxed);
      continue;  // the game leaves this part's matrices alone too
    }
    uint32_t remap_raw[kMaxModelBones];
    if (nb != 0 && !GuestTryCopy(remap_raw, base + remap, size_t(nb) * 4)) {
      c_part_bad.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    bool ok = true;
    uint32_t rows = 0;
    for (uint32_t m = 0; m < nm && ok; ++m) {
      uint32_t mesh = 0, cnt = 0, list = 0;
      if (!U32(base, mesh_table + 8 * m, &mesh) || !U32(base, mesh + 0x48, &cnt) ||
          !U32(base, mesh + 0x4C, &list) || rows + cnt > kMaxRows) {
        ok = false;
        break;
      }
      uint16_t lr[kMaxRows];
      if (cnt != 0 && !GuestTryCopy(lr, base + list, size_t(cnt) * 2)) {
        ok = false;
        break;
      }
      for (uint32_t i = 0; i < cnt; ++i) {
        const uint16_t mb = __builtin_bswap16(lr[i]);
        if (mb >= nb) {
          ok = false;
          break;
        }
        const uint32_t sb = __builtin_bswap32(remap_raw[mb]);
        if (sb >= kMaxSkelBones) {
          ok = false;
          break;
        }
        max_bone = std::max(max_bone, sb);
        t.idx.push_back(uint16_t(sb));
      }
      rows += cnt;
    }
    if (!ok) {
      t.idx.resize(p.idx_ofs);
      c_part_bad.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    p.rows = uint16_t(rows);
    p.built = rows != 0;
  }
  // The pose rows the gather references (contiguous Matrix44s).
  const uint32_t nbones = max_bone + 1;
  std::vector<uint32_t> raw(size_t(nbones) * 16);
  if (t.idx.empty() || !GuestTryCopy(raw.data(), base + src_palette, raw.size() * 4)) {
    c_pose_unread.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  t.pose.resize(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    t.pose[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
  }
  t.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  std::lock_guard<std::mutex> lock(g_mu);
  t.frame = g_frames;
  Entity& e = g_ents[owner];
  if (e.cur_valid) {
    if (e.cur.have_ltow) {
      e.prev = std::move(e.cur);
      e.prev_valid = true;
    } else {
      c_unpacked.fetch_add(1, std::memory_order_relaxed);  // UBT twice, no pack between
    }
  }
  e.cur = std::move(t);
  e.cur_valid = true;
  e.last_frame = g_frames;
  c_ticks.fetch_add(1, std::memory_order_relaxed);
}

void OnPack(uint8_t* base, uint32_t owner, uint32_t instance, uint32_t local_to_world) {
  if (!Active() || owner == 0 || instance == 0) return;
  uint32_t raw[16];
  if (local_to_world < 0x10000 || !GuestTryCopy(raw, base + local_to_world, sizeof(raw))) {
    return;
  }
  float l[16];
  for (int i = 0; i < 16; ++i) l[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
  std::lock_guard<std::mutex> lock(g_mu);
  const auto it = g_ents.find(owner);
  if (it == g_ents.end() || !it->second.cur_valid) return;
  Entity& e = it->second;
  Tick& t = e.cur;
  const Part* p = t.Find(instance);
  if (p == nullptr) {
    c_pack_foreign.fetch_add(1, std::memory_order_relaxed);  // not this tick's parts
    return;
  }
  if (!t.have_ltow) {
    std::memcpy(t.ltow, l, sizeof(l));
    t.have_ltow = true;
    CheckContinuity(owner, e, t);
    t.epoch = e.epoch;
  } else if (std::memcmp(t.ltow, l, sizeof(l)) != 0) {
    c_ltow_var.fetch_add(1, std::memory_order_relaxed);  // one tick, two localToWorlds
  }
  const uint32_t k = (instance - t.parts) / kInstStride;
  if (k < 32) t.packed_mask |= 1u << k;
}

void BeforeServe(uint8_t* base, FrameScene& scene) {
  (void)base;
  g_shadow.clear();
  g_rows.clear();
  const int mode = Mode();
  if (mode == 0) return;
  for (size_t i = 0; i < scene.items.size(); ++i) {
    DrawItem& item = scene.items[i];
    if (!Servable(item)) continue;
    Shadow s;
    if (BuildItem(item, i, &s, true) != Built::kYes) continue;
    // The bank capture: the game's own upload for this draw.
    if ((item.dbg_src == 1 || item.dbg_src == 2) && !item.caster_bank) {
      Err e;
      const int c = Classify(s, item.bones.data(), &e);
      (s.lw ? c_lw_bank : c_bank)[c].fetch_add(1, std::memory_order_relaxed);
      if (c == 3) NoteDiv(s, e, s.lw ? "lw-bank" : "bank", item);
    } else {
      (s.lw ? c_lw_bank_none : c_bank_none).fetch_add(1, std::memory_order_relaxed);
    }
    if (mode == 2 && (!s.lw || LwMode() == 2)) {
      Serve(item, s);
      (s.lw ? c_lw_served : c_served).fetch_add(1, std::memory_order_relaxed);
    } else if (!s.lw) {
      g_shadow.push_back(s);
    }
  }
}

void AfterServe(const FrameScene& scene) {
  if (Mode() != 1) return;
  for (const Shadow& s : g_shadow) {
    if (s.item >= scene.items.size()) continue;
    const DrawItem& item = scene.items[s.item];
    if (s.lw || item.ctx != s.ctx || item.bones.size() < size_t(s.n) * 12) continue;
    if (item.dbg_src != 11) {
      c_srv_unserved.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    Err e;
    const int c = Classify(s, item.bones.data(), &e);
    c_srv[c].fetch_add(1, std::memory_order_relaxed);
    if (c == 3) NoteDiv(s, e, "serve", item);
  }
}

uint32_t ServeRange(uint8_t* base, FrameScene& scene, size_t first) {
  (void)base;
  if (Mode() != 2) return 0;
  uint32_t n = 0;
  for (size_t i = first; i < scene.items.size(); ++i) {
    DrawItem& item = scene.items[i];
    const bool gap = item.dbg_src == 9;  // LW gap fill: why it isn't served
    if (!Servable(item)) {
      if (gap) c_lw_tail_unservable.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    Shadow s;
    g_rows.clear();
    const Built b = BuildItem(item, i, &s, false);
    if (b != Built::kYes) {
      if (gap) {
        (b == Built::kNo ? c_lw_tail_nobuild : c_lw_tail_notlw)
            .fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    if (s.lw && LwMode() != 2) continue;
    Serve(item, s);
    if (gap) c_lw_tail_served.fetch_add(1, std::memory_order_relaxed);
    ++n;
  }
  c_served_tail.fetch_add(n, std::memory_order_relaxed);
  return n;
}

void OnLwPack(uint8_t* base, uint32_t entity) {
  if (!Active() || LwMode() == 0 || entity == 0) return;
  // The LivingWorld batch pack writer (sub_827C1D38) is UpdateBoneTransforms
  // and PackAndMultiply fused, with the same tables: instances at
  // entity[(lod + 8) * 4] (count entity[(lod + 5) * 4], lod at +0x10), the
  // pose array from the vtable +0x60 getter (*(entity + 0x4B0)), one remap
  // table per instance at entity + 0x270 + 4k, and localToWorld = m_MatLtoW
  // (+0x160). So it feeds the store like a skater tick.
  uint32_t lod = 0, count = 0, parts = 0, src = 0;
  if (!U32(base, entity + 0x10, &lod) || lod > 8 ||
      !U32(base, entity + (lod + 5) * 4, &count) ||
      !U32(base, entity + (lod + 8) * 4, &parts) || !U32(base, entity + 0x4B0, &src) ||
      count == 0 || count > kMaxParts) {
    c_lw_bad.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  OnBoneTransforms(base, entity, parts, count, src, entity + 0x270);
  for (uint32_t k = 0; k < count; ++k) {
    OnPack(base, entity, parts + k * kInstStride, entity + 0x160);
  }
  c_lw_ticks.fetch_add(1, std::memory_order_relaxed);
}

void LogJumpFrames(uint8_t* base, const FrameScene& scene, bool smoothed) {
  if (Mode() == 0 || !REXCVAR_GET(skate3_native_render_scene_skeleton_log_jumps)) return;
  // entity -> {last epoch seen, frames left to log}; render thread only.
  struct Watch {
    uint32_t epoch = 0;
    int left = 0;
    int nth = 0;
  };
  static std::unordered_map<uint32_t, Watch> s_watch;
  // Skater pieces drawn without a store entry for their entity (a character
  // on its first frames): log 4 frames of every character item near the
  // camera, with each piece's palette source.
  static int s_unstored_left = 0;
  static int s_unstored_nth = 0;
  {
    bool unstored = false;
    for (const DrawItem& it : scene.items) {
      if (!it.skinned || it.ctx == 0 || it.char_family == 0 || it.char_family > 5) continue;
      skate3::native_entity::CtxInfo ident;
      if (!skate3::native_entity::LookupCtx(it.ctx, &ident) || ident.entity == 0) {
        unstored = true;
        break;
      }
      using skate3::native_entity::EntClass;
      if (ident.cls != EntClass::kSkater && ident.cls != EntClass::kColorized &&
          ident.cls != EntClass::kCac && ident.cls != EntClass::kSkaterAux) {
        continue;  // pedestrians are not in the store
      }
      std::lock_guard<std::mutex> lock(g_mu);
      const auto e = g_ents.find(ident.entity);
      if (e == g_ents.end() || e->second.Newest() == nullptr) {
        unstored = true;
        break;
      }
    }
    static int s_triggers = 0;
    if (unstored && s_unstored_left == 0 && s_triggers < 12) {
      ++s_triggers;
      s_unstored_left = 4;
      s_unstored_nth = 0;
    }
  }
  if (s_unstored_left > 0) {
    --s_unstored_left;
    ++s_unstored_nth;
    for (size_t i = 0; i < scene.items.size(); ++i) {
      const DrawItem& it = scene.items[i];
      if (it.char_family == 0 && !it.ropa) continue;
      const bool skn = it.skinned && it.bones.size() >= 12;
      const float tx = skn ? it.bones[3] : it.world[12];
      const float ty = skn ? it.bones[7] : it.world[13];
      const float tz = skn ? it.bones[11] : it.world[14];
      const float rx = tx - scene.cam_pos[0], ry = ty - scene.cam_pos[1],
                  rz = tz - scene.cam_pos[2];
      if (rx * rx + ry * ry + rz * rz > 100.0f) continue;
      skate3::native_entity::CtxInfo ident;
      const bool mapped = it.ctx != 0 && skate3::native_entity::LookupCtx(it.ctx, &ident);
      bool stored = false;
      if (mapped) {
        std::lock_guard<std::mutex> lock(g_mu);
        const auto e = g_ents.find(ident.entity);
        stored = e != g_ents.end() && e->second.Newest() != nullptr;
      }
      REXLOG_INFO(
          "skel-new: f {} smoothed {} +{} item {} owner {:08X} stored {} fam {} ctx {:08X} "
          "mesh {:08X} ropa {} hair {} skinned {} src {} mode {} pose ent {:08X} seq {} part {} "
          "prevpack {} pframe {} t ({:.3f},{:.3f},{:.3f})",
          g_frames, smoothed ? 1 : 0, s_unstored_nth, i, mapped ? ident.entity : 0u,
          stored ? 1 : 0, it.char_family, it.ctx, it.mesh, it.ropa ? 1 : 0, it.hair ? 1 : 0,
          it.skinned ? 1 : 0, it.dbg_src, it.draw_mode, it.pose_entity, it.pose_seq,
          it.pose_part, it.pose_set, it.pose_frame, tx, ty, tz);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const auto& [ent, e] : g_ents) {
      Watch& w = s_watch[ent];
      if (w.epoch != e.epoch) {  // a jump, or a character appearing (epoch 0)
        w.left = 4;
        w.nth = 0;
      }
      w.epoch = e.epoch;
    }
  }
  for (auto& [ent, w] : s_watch) {
    if (w.left <= 0) continue;
    --w.left;
    ++w.nth;
    float er[12];
    // The character's root: its entity world, through any of its items.
    float root[3] = {};
    bool have_root = false;
    for (const DrawItem& it : scene.items) {
      skate3::native_entity::CtxInfo ident;
      if (it.ctx != 0 && skate3::native_entity::LookupCtx(it.ctx, &ident) &&
          ident.entity == ent && skate3::native_entity::ReadEntityWorldRows(base, it.ctx, er)) {
        root[0] = er[3];
        root[1] = er[7];
        root[2] = er[11];
        have_root = true;
        break;
      }
    }
    if (!have_root) {
      // Its pieces aren't mapped to it this frame: log around the camera.
      root[0] = scene.cam_pos[0];
      root[1] = scene.cam_pos[1];
      root[2] = scene.cam_pos[2];
      REXLOG_INFO("skel-jf: ent {:08X} epoch {} +{} has no mapped item; logging near the camera",
                  ent, w.epoch, w.nth);
    }
    const float reach2 = have_root ? 9.0f : 64.0f;
    // Every item within 3 m of it, whichever entity it maps to (or none).
    for (size_t i = 0; i < scene.items.size(); ++i) {
      const DrawItem& it = scene.items[i];
      const bool skn = it.skinned && it.bones.size() >= 12;
      const float tx = skn ? it.bones[3] : it.world[12];
      const float ty = skn ? it.bones[7] : it.world[13];
      const float tz = skn ? it.bones[11] : it.world[14];
      const float rx = tx - root[0], ry = ty - root[1], rz = tz - root[2];
      if (rx * rx + ry * ry + rz * rz > reach2) continue;
      if (!have_root && it.char_family == 0 && !it.ropa) continue;
      skate3::native_entity::CtxInfo ident;
      const bool mapped = it.ctx != 0 && skate3::native_entity::LookupCtx(it.ctx, &ident);
      // A rigid cloth piece's drape as the guest VB holds it now: the
      // centroid of 32 sample verts in model space, and placed by the world.
      float cm[3] = {}, cw[3] = {};
      if (it.ropa && !skn) {
        skate3::native_scene::SkinSampleVert sv[32];
        if (skate3::native_scene::ReadSkinSamplesGuest(base, it, 32, sv)) {
          for (const auto& v : sv) {
            for (int k = 0; k < 3; ++k) cm[k] += v.p[k] / 32.0f;
          }
          for (int c = 0; c < 3; ++c) {
            cw[c] = cm[0] * it.world[c] + cm[1] * it.world[4 + c] + cm[2] * it.world[8 + c] +
                    it.world[12 + c];
          }
        }
      }
      const bool eok = it.ctx != 0 && skate3::native_entity::ReadEntityWorldRows(base, it.ctx, er);
      REXLOG_INFO(
          "skel-jf: f {} smoothed {} ent {:08X} epoch {} +{} item {} owner {:08X} cls {} lw {:08X} fam {} "
          "ctx {:08X} mesh {:08X} ropa {} hair {} "
          "skinned {} rows {} src {} mode {} lag {:.0f}ms ret {} cb {} pend {} vb {:08X} "
          "shapes {} t ({:.3f},{:.3f},{:.3f}) ent ({:.3f},{:.3f},{:.3f}) d {:.3f} "
          "rot0 ({:.3f},{:.3f},{:.3f}) flag {:.6f} vbage {}ms vbaddr {:08X} fp {:016X} "
          "drape model ({:.3f},{:.3f},{:.3f}) world ({:.3f},{:.3f},{:.3f})",
          g_frames, smoothed ? 1 : 0, ent, w.epoch, w.nth, i, mapped ? ident.entity : 0u, mapped ? int(ident.cls) : -1,
          it.lw_entity, it.char_family, it.ctx, it.mesh, it.ropa ? 1 : 0, it.hair ? 1 : 0,
          it.skinned ? 1 : 0, it.bones.size() / 12, it.dbg_src, it.draw_mode, it.draw_lag_ms,
          it.retained ? 1 : 0, it.caster_bank ? 1 : 0, it.pending ? 1 : 0, it.vb_obj,
          it.shape_count, tx, ty, tz, eok ? er[3] : 0.0f, eok ? er[7] : 0.0f,
          eok ? er[11] : 0.0f,
          eok ? std::sqrt((tx - er[3]) * (tx - er[3]) + (ty - er[7]) * (ty - er[7]) +
                          (tz - er[11]) * (tz - er[11]))
              : -1.0f,
          skn ? it.bones[0] : it.world[0], skn ? it.bones[1] : it.world[1],
          skn ? it.bones[2] : it.world[2], it.ropa_flag,
          it.ropa ? skate3::native_entity::RopaVbAgeMs(it.vb_obj) : int64_t(-2), it.vb_addr,
          it.fingerprint, cm[0], cm[1], cm[2], cw[0], cw[1], cw[2]);
    }
  }
  if (s_watch.size() > 256) s_watch.clear();
}

void OnFrameBuilt() {
  g_mode.store(ReadMode(), std::memory_order_relaxed);
  {
    const std::string v = REXCVAR_GET(skate3_native_render_scene_skeleton_lw);
    g_lw_mode.store(v == "on" ? 2 : v == "shadow" ? 1 : 0, std::memory_order_relaxed);
  }
  size_t ents = 0;
  Worst worst;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    ++g_frames;
    for (auto it = g_ents.begin(); it != g_ents.end();) {
      it = g_frames - it->second.last_frame > 300 ? g_ents.erase(it) : std::next(it);
    }
    if (g_mode.load(std::memory_order_relaxed) == 0) g_ents.clear();
    ents = g_ents.size();
    worst = g_worst;
  }
  const int mode = Mode();
  const int32_t every = REXCVAR_GET(skate3_native_render_scene_skeleton_log_interval);
  if (mode == 0 || every <= 0 || g_frames % uint64_t(every) != 0) return;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_worst = Worst{};
  }
  REXLOG_INFO(
      "skel: mode {} frame {} items {} built {} served {} tail {} miss[noent={} notick={} "
      "nopart={} skipped={}] unmapped {} bank[new={} old={} close={} div={} none={}] "
      "serve[new={} old={} close={} div={} unserved={}] store[ents={} ticks={} unpacked={} "
      "unowned={} foreign={} ltow_var={} part_skip={} part_bad={} pose_unread={}] "
      "jump[root={} gap={} parts={}] worst[{} ent={:08X} piece={} mesh={:08X} trans={:.4f} "
      "rot={:.4f}]",
      mode == 2 ? "on" : "shadow", g_frames, X(c_items), X(c_built), X(c_served),
      X(c_served_tail), X(c_miss_noent), X(c_miss_notick), X(c_miss_nopart), X(c_miss_skipped),
      X(c_unmapped), X(c_bank[0]), X(c_bank[1]), X(c_bank[2]), X(c_bank[3]), X(c_bank_none),
      X(c_srv[0]), X(c_srv[1]), X(c_srv[2]), X(c_srv[3]), X(c_srv_unserved), ents, X(c_ticks),
      X(c_unpacked), X(c_ubt_unowned), X(c_pack_foreign), X(c_ltow_var), X(c_part_skipped),
      X(c_part_bad), X(c_pose_unread), X(c_jump_root), X(c_jump_gap), X(c_jump_parts),
      worst.trans < 0 ? "-" : worst.vs, worst.entity, worst.k, worst.mesh,
      std::max(worst.trans, 0.0f), worst.rot);
  if (LwMode() != 0) {
    REXLOG_INFO(
        "skel-lw: mode {} frame {} items {} built {} served {} miss {} bank[new={} old={} "
        "close={} div={} none={}] ticks {} bad {} gapfill[served={} unservable={} "
        "nobuild={} notlw={}]",
        LwMode() == 2 ? "on" : "shadow", g_frames, X(c_lw_items), X(c_lw_built),
        X(c_lw_served), X(c_lw_miss), X(c_lw_bank[0]), X(c_lw_bank[1]), X(c_lw_bank[2]),
        X(c_lw_bank[3]), X(c_lw_bank_none), X(c_lw_ticks), X(c_lw_bad),
        X(c_lw_tail_served), X(c_lw_tail_unservable), X(c_lw_tail_nobuild),
        X(c_lw_tail_notlw));
  }
}

}  // namespace skate3::native_skeleton
