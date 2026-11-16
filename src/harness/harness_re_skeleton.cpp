#include "harness/harness_re_skeleton.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_guest_read.h"

REXCVAR_DEFINE_INT32(skate3_harness_re_ubt, 0, "Skate 3",
                     "RE-5 probe: every Nth UpdateBoneTransforms call in a skater's StartJobs, log "
                     "how its part records link to the entity's persistent MeshContexts (0 = off)");

namespace skate3::harness::re_skeleton {
namespace {

thread_local uint32_t tl_owner = 0;
std::atomic<uint32_t> g_calls{0};

constexpr uint32_t kInstStride = 0x28;  // cModelInstance
constexpr uint32_t kCtxStride = 0x50;   // Sk8::Render::MeshContext (TU3)

bool U32(uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x10000) return false;
  uint32_t raw = 0;
  if (!native_scene::GuestTryCopy(&raw, base + addr, 4)) return false;
  *out = __builtin_bswap32(raw);
  return true;
}

struct Persistent {
  uint32_t instance, slot, ctx, model, mesh;
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
    uint32_t model = 0, cb = 0, ce = 0;
    if (!U32(base, rec, &model) || !U32(base, rec + 4, &cb) || !U32(base, rec + 8, &ce) ||
        ce <= cb || (ce - cb) % kCtxStride != 0 || (ce - cb) / kCtxStride > 64) {
      continue;
    }
    for (uint32_t k = 0; k < (ce - cb) / kCtxStride; ++k) {
      uint32_t mesh = 0;
      U32(base, cb + k * kCtxStride, &mesh);
      out.push_back({i, k, cb + k * kCtxStride, model, mesh});
    }
  }
  return out;
}

}  // namespace

void SetOwner(uint32_t entity) { tl_owner = entity; }

void OnBoneTransforms(uint8_t* base, uint32_t parts, uint32_t count) {
  const int32_t every = REXCVAR_GET(skate3_harness_re_ubt);
  if (every <= 0 || tl_owner == 0 || parts == 0 || count == 0 || count > 64) return;
  const uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed);
  if (n % uint32_t(every) != 0) return;

  const std::vector<Persistent> persist = EntityContexts(base, tl_owner);
  std::string lines;
  int by_mesh_unique = 0, by_model = 0;
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t p = parts + k * kInstStride;
    uint32_t model = 0, cb = 0, ce = 0, mats = 0, matcount = 0, mesh = 0;
    U32(base, p, &model);
    U32(base, p + 4, &cb);
    U32(base, p + 8, &ce);
    U32(base, p + 0x14, &mats);
    U32(base, p + 0x18, &matcount);
    if (cb) U32(base, cb, &mesh);
    std::string mesh_hits, model_hits;
    int mesh_n = 0;
    for (const Persistent& q : persist) {
      char b[32];
      std::snprintf(b, sizeof(b), " %u.%u", q.instance, q.slot);
      if (mesh && q.mesh == mesh) {
        mesh_hits += b;
        ++mesh_n;
      }
      if (model && q.model == model) model_hits += b;
    }
    by_mesh_unique += mesh_n == 1;
    by_model += !model_hits.empty();
    char line[320];
    std::snprintf(line, sizeof(line),
                  "\n  part %2u model %08X ctx %08X..%08X mesh %08X bones %u | same mesh:%s | "
                  "same model:%s",
                  k, model, cb, ce, mesh, matcount, mesh_hits.empty() ? " -" : mesh_hits.c_str(),
                  model_hits.empty() ? " -" : model_hits.c_str());
    lines += line;
  }
  std::string plist;
  for (const Persistent& q : persist) {
    char b[64];
    std::snprintf(b, sizeof(b), " %u.%u=%08X/%08X", q.instance, q.slot, q.model, q.mesh);
    plist += b;
  }
  REXLOG_INFO(
      "re-ubt: entity {:08X} parts {} at {:08X}: {} matched one persistent ctx by mesh, {} share a "
      "model; persistent (inst.slot=model/mesh):{}{}",
      tl_owner, count, parts, by_mesh_unique, by_model, plist, lines);
}

}  // namespace skate3::harness::re_skeleton
