#include "harness/harness_re_skeleton.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_guest_read.h"

REXCVAR_DEFINE_INT32(skate3_harness_re_ubt, 0, "Skate 3",
                     "RE-5 probe: sample every Nth skater StartJobs and log its UpdateBoneTransforms "
                     "parts and packs until the next one (0 = off; docs/re/characters.md)");

namespace skate3::harness::re_skeleton {
namespace {

thread_local uint32_t tl_owner = 0;
thread_local int tl_bracket = kNone;
thread_local float tl_pack_l2w[16] = {};  // the pack's localToWorld argument (r4) at entry
thread_local bool tl_pack_l2w_ok = false;
thread_local float tl_pack_in[16] = {};
thread_local bool tl_pack_in_ok = false;

std::atomic<uint32_t> g_starts{0};
std::atomic<uint32_t> g_sampled{0};  // the entity being sampled (0 = none)
std::atomic<uint32_t> g_tick{0};     // sampled tick number

constexpr uint32_t kInstStride = 0x28;  // cModelInstance
constexpr uint32_t kCtxStride = 0x50;   // Sk8::Render::MeshContext (TU3)

bool U32(uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x10000) return false;
  uint32_t raw = 0;
  if (!native_scene::GuestTryCopy(&raw, base + addr, 4)) return false;
  *out = __builtin_bswap32(raw);
  return true;
}

bool Floats(uint8_t* base, uint32_t addr, float* out, int n) {
  for (int i = 0; i < n; ++i) {
    uint32_t v = 0;
    if (!U32(base, addr + 4 * i, &v)) return false;
    std::memcpy(&out[i], &v, 4);
  }
  return true;
}

std::string FloatList(const float* f, int n) {
  std::string s;
  char b[24];
  for (int i = 0; i < n; ++i) {
    std::snprintf(b, sizeof(b), "%s%.9g", i ? "," : "", f[i]);
    s += b;
  }
  return s;
}

const char* BracketName(int b) {
  return b == kStartJobs ? "StartJobs" : b == kEndJobs ? "EndJobs" : b == kDriver ? "driver" : "-";
}

struct Persistent {
  uint32_t instance, slot, inst_addr, ctx, model, mesh, mats;
};

// The entity's current LOD: the same walk as PresentationEntity::
// BindConstants (sub_827A6658; native/skate3_native_entity.cpp).
std::vector<Persistent> EntityContexts(uint8_t* base, uint32_t entity) {
  std::vector<Persistent> out;
  uint32_t idx = 0, count = 0, arr = 0;
  if (!U32(base, entity + 16, &idx) || idx > 1024 || !U32(base, entity + (idx + 5) * 4, &count) ||
      !U32(base, entity + (idx + 8) * 4, &arr) || count == 0 || count > 32) {
    return out;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t rec = arr + i * kInstStride;
    uint32_t model = 0, cb = 0, ce = 0, mats = 0;
    if (!U32(base, rec, &model) || !U32(base, rec + 4, &cb) || !U32(base, rec + 8, &ce) ||
        ce <= cb || (ce - cb) % kCtxStride != 0 || (ce - cb) / kCtxStride > 64) {
      continue;
    }
    U32(base, rec + 0x14, &mats);
    for (uint32_t k = 0; k < (ce - cb) / kCtxStride; ++k) {
      uint32_t mesh = 0;
      U32(base, cb + k * kCtxStride, &mesh);
      out.push_back({i, k, rec, cb + k * kCtxStride, model, mesh, mats});
    }
  }
  return out;
}

bool Sampling() {
  return REXCVAR_GET(skate3_harness_re_ubt) > 0 && tl_owner != 0 &&
         tl_owner == g_sampled.load(std::memory_order_relaxed);
}

}  // namespace

uint64_t EnterOwner(uint8_t* /*base*/, uint32_t entity, Bracket bracket) {
  const uint64_t saved = (uint64_t(uint32_t(tl_bracket)) << 32) | tl_owner;
  tl_owner = entity;
  tl_bracket = bracket;
  const int32_t every = REXCVAR_GET(skate3_harness_re_ubt);
  if (every > 0 && bracket == kStartJobs) {
    // A new tick for this entity: sample it, or stop sampling it.
    const uint32_t n = g_starts.fetch_add(1, std::memory_order_relaxed);
    if (n % uint32_t(every) == 0) {
      g_sampled.store(entity, std::memory_order_relaxed);
      const uint32_t t = g_tick.fetch_add(1, std::memory_order_relaxed) + 1;
      REXLOG_INFO("re-tick: {} entity {:08X} (StartJobs)", t, entity);
    } else if (g_sampled.load(std::memory_order_relaxed) == entity) {
      g_sampled.store(0, std::memory_order_relaxed);
    }
  }
  return saved;
}

void RestoreOwner(uint64_t saved) {
  tl_owner = uint32_t(saved);
  tl_bracket = int(saved >> 32);
}

void OnBoneTransforms(uint8_t* base, uint32_t parts, uint32_t count, uint32_t src_palette,
                      uint32_t remaps) {
  if (!Sampling() || parts == 0 || count == 0 || count > 64) return;

  const std::vector<Persistent> persist = EntityContexts(base, tl_owner);
  std::string lines;
  int by_mesh_unique = 0, by_model = 0, same_mats = 0;
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t p = parts + k * kInstStride;
    uint32_t model = 0, cb = 0, ce = 0, mats = 0, matcount = 0, mesh = 0, remap = 0;
    U32(base, p, &model);
    U32(base, p + 4, &cb);
    U32(base, p + 8, &ce);
    U32(base, p + 0x14, &mats);
    U32(base, p + 0x18, &matcount);
    U32(base, remaps + 4 * k, &remap);
    if (cb) U32(base, cb, &mesh);
    std::string mesh_hits, model_hits;
    int mesh_n = 0;
    uint32_t inst_mats = 0, inst_addr = 0;
    for (const Persistent& q : persist) {
      char b[32];
      std::snprintf(b, sizeof(b), " %u.%u", q.instance, q.slot);
      if (mesh && q.mesh == mesh) {
        mesh_hits += b;
        ++mesh_n;
        inst_mats = q.mats;
        inst_addr = q.inst_addr;
      }
      if (model && q.model == model) model_hits += b;
    }
    by_mesh_unique += mesh_n == 1;
    by_model += !model_hits.empty();
    same_mats += mats != 0 && mats == inst_mats;
    float m0[16] = {};
    const bool have = mats && Floats(base, mats, m0, 16);
    char line[640];
    std::snprintf(line, sizeof(line),
                  "\n  part %2u model %08X ctx %08X..%08X mesh %08X bones %u remap %08X mats %08X | "
                  "same mesh:%s | same model:%s | instance %08X mats %08X%s | m0 %s",
                  k, model, cb, ce, mesh, matcount, remap, mats,
                  mesh_hits.empty() ? " -" : mesh_hits.c_str(),
                  model_hits.empty() ? " -" : model_hits.c_str(), inst_addr, inst_mats,
                  mats == inst_mats ? " (same buffer)" : "", have ? FloatList(m0, 16).c_str() : "?");
    lines += line;
  }
  std::string plist;
  for (const Persistent& q : persist) {
    char b[64];
    std::snprintf(b, sizeof(b), " %u.%u=%08X/%08X", q.instance, q.slot, q.model, q.mesh);
    plist += b;
  }
  REXLOG_INFO(
      "re-ubt: tick {} entity {:08X} [{}] parts {} at {:08X} src {:08X} remaps {:08X}: {} matched one "
      "persistent ctx by mesh, {} share a model, {} write the persistent instance's matrix buffer; "
      "persistent (inst.slot=model/mesh):{}{}",
      g_tick.load(), tl_owner, BracketName(tl_bracket), count, parts, src_palette, remaps,
      by_mesh_unique, by_model, same_mats, plist, lines);
}

void OnPackEnter(uint8_t* base, uint32_t instance, uint32_t local_to_world) {
  tl_pack_in_ok = false;
  tl_pack_l2w_ok = false;
  if (!Sampling()) return;
  uint32_t mats = 0;
  tl_pack_in_ok = U32(base, instance + 0x14, &mats) && mats && Floats(base, mats, tl_pack_in, 16);
  tl_pack_l2w_ok = Floats(base, local_to_world, tl_pack_l2w, 16);
}

void OnPackExit(uint8_t* base, uint32_t instance) {
  if (!Sampling()) return;
  // Which LOD instance of the owner this is.
  int lod_index = -1;
  for (const Persistent& q : EntityContexts(base, tl_owner)) {
    if (q.inst_addr == instance && q.slot == 0) {
      lod_index = int(q.instance);
      break;
    }
  }
  uint32_t mats = 0, rows = 0;
  U32(base, instance + 0x14, &mats);
  U32(base, instance + 0x18, &rows);
  float out[16] = {}, ltow[16] = {}, ltowt[16] = {};
  const bool have_out = mats && Floats(base, mats, out, 16);
  // m_MatLtoW (+0x160) and m_MatLtoWTrans (+0x1A0) of the SkaterPresEntity,
  // next to the pack's own localToWorld argument.
  const bool have_ltow = Floats(base, tl_owner + 0x160, ltow, 16);
  const bool have_ltowt = Floats(base, tl_owner + 0x1A0, ltowt, 16);
  REXLOG_INFO("re-pack: tick {} entity {:08X} [{}] instance {:08X} lod {} mats {:08X} rows {} | in {} | "
              "out {} | arg {} | ltow {} | ltowt {}",
              g_tick.load(), tl_owner, BracketName(tl_bracket), instance, lod_index, mats, rows,
              tl_pack_in_ok ? FloatList(tl_pack_in, 16) : std::string("?"),
              have_out ? FloatList(out, 16) : std::string("?"),
              tl_pack_l2w_ok ? FloatList(tl_pack_l2w, 16) : std::string("?"),
              have_ltow ? FloatList(ltow, 16) : std::string("?"),
              have_ltowt ? FloatList(ltowt, 16) : std::string("?"));
}

}  // namespace skate3::harness::re_skeleton
