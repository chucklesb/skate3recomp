#include "harness/harness_game.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <condition_variable>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdio>

REXCVAR_DEFINE_INT32(skate3_harness_re_load_reel, 0, "Skate 3",
                     "Harness RE experiment: load saved replay N (REPLY_N) the way My Replays "
                     "does; reset to 0 once issued")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#include "native/skate3_native_guest_read.h"

// The guest functions called below (recompiled; TU3 image addresses).
REX_EXTERN(sub_82910CE8);  // registry find(map*, u32 key) -> entry* (node + 20) or 0
REX_EXTERN(sub_82706F50);  // teleport sequencer(teleporter, matrix*, id, flag, b, b, b) -> bool
REX_EXTERN(sub_8271A828);  // memcard owner event handler(owner, ref<event>*) -> 1 handled / 2 not
REX_EXTERN(sub_82720B50);  // state transition(component, target state fn, param)
// The pause map's entry point: TeleportToLocation(service, u64* map key,
// const char** name, a, b) (the menu passes a = 0, b = 1). It loads the
// destination's map when it differs from the current one, then teleports.
REX_EXTERN(sub_82864628);

namespace skate3::harness::game {
namespace {
// Set on the guest thread while the harness's own sequencer call runs.
thread_local bool t_force_streamed_teleport = false;
}  // namespace
}  // namespace skate3::harness::game

// sub_82864C40(matrix*, flag): does the destination need streaming? The
// sequencer moves the player at once when it says no, but in free skate that
// instant move is not applied (the player only accepts the transform message
// from the teleporting state). For the harness's calls it always says yes, so
// every teleport takes the teleporting-state path, as far destinations do.
extern "C" REX_FUNC(sub_82864C40) {
  __imp__sub_82864C40(ctx, base);
  if (skate3::harness::game::t_force_streamed_teleport) {
    ctx.r3.u64 = 1;
  }
}

namespace skate3::harness::game {
namespace {

constexpr uint32_t kWorldPtr = 0x83083BCC;     // world object
constexpr uint32_t kWorldRegistry = 168;       // object registry
constexpr uint32_t kWorldModeOwner = 156;      // game-mode owner
constexpr uint32_t kOwnerTeleporter = 116;     // teleporter
constexpr uint32_t kSettingsPtr = 0x83067068;  // *(this) + 120: the event's flag byte
// The game-mode component that handles the map's teleport-to-location event
// (HandleEvent sub_8270B3D0 at vtable slot 4). It is a small state machine:
// +8 current state handler, +44 "teleport allowed" byte. After the sequencer
// accepts a request, the handler enters the teleporting state with the
// teleporter as its parameter; that state drives the fade, streaming and move.
constexpr uint32_t kOwnerTeleportMode = 40;
constexpr uint32_t kTeleportModeVtable = 0x8230DF88;
constexpr uint32_t kTeleportModeAllowed = 44;
constexpr uint32_t kStateTeleporting = 0x8270C7D0;
// The teleport service the pause map calls: *(*(kMessageSystemPtr) + 0x108).
constexpr uint32_t kMessageSystemPtr = 0x830CFD94;
constexpr uint32_t kServiceOffset = 0x108;
constexpr uint32_t kServiceVtable = 0x82315370;
// Teleport state block: +40 current map key (u64).
constexpr uint32_t kTeleportStateBlock = 0x830B7AE8;

// Which district each locator is in (generated from the game's locator
// files), and the map keys the pause map passes per district (captured from
// the menu). Districts without a known key use the sequencer path, which
// cannot load another map.
struct LocatorDistrict {
  uint32_t hash;
  const char* district;
};
constexpr LocatorDistrict kLocatorDistricts[] = {
#include "harness/teleport_districts.inc"
};
struct DistrictKey {
  const char* district;
  uint64_t key;
};
constexpr DistrictKey kDistrictKeys[] = {
    {"DownTown", 0x4773016A41B2E05Eull},
    {"University", 0xCB473E68A2EAFABAull},
    {"Industrial", 0xA8B1F47090679026ull},
};

const char* DistrictOf(uint32_t hash) {
  const auto* end = std::end(kLocatorDistricts);
  const auto* it = std::lower_bound(std::begin(kLocatorDistricts), end, hash,
                                    [](const LocatorDistrict& d, uint32_t h) { return d.hash < h; });
  return it != end && it->hash == hash ? it->district : nullptr;
}

uint64_t DistrictKeyOf(const char* district) {
  for (const auto& k : kDistrictKeys) {
    if (district && std::strcmp(k.district, district) == 0) {
      return k.key;
    }
  }
  return 0;
}

void Write32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}

bool Read32(uint8_t* base, uint32_t addr, uint32_t* out) {
  uint32_t raw = 0;
  if (addr == 0 || !native_scene::GuestTryCopy(&raw, base + addr, sizeof(raw))) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

bool Read8(uint8_t* base, uint32_t addr, uint8_t* out) {
  return addr != 0 && native_scene::GuestTryCopy(out, base + addr, 1);
}

bool ReadF32(uint8_t* base, uint32_t addr, float* out) {
  uint32_t v = 0;
  if (!Read32(base, addr, &v)) {
    return false;
  }
  std::memcpy(out, &v, 4);
  return true;
}

struct Request {
  std::string location;
  uint64_t map_key = 0;
  std::optional<TeleportResult> result;
};

std::mutex g_mu;
std::condition_variable g_cv;
Request* g_pending = nullptr;  // owned by the waiting harness thread

// Calls guest function `fn` with r3..r9 on the current guest thread. The
// callee pushes its frame below the hook's r1; every register is restored
// afterwards, so the hooked function continues as if nothing ran.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*),
                   std::initializer_list<uint32_t> args, uint32_t stack_top = 0) {
  const PPCContext saved = ctx;
  if (stack_top) {
    ctx.r1.u64 = stack_top;  // the callee's frames go below this
  }
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9};
  size_t i = 0;
  for (uint32_t a : args) {
    regs[i++]->u64 = a;
  }
  fn(ctx, base);
  const uint32_t ret = ctx.r3.u32;
  ctx = saved;
  return ret;
}

TeleportResult DoTeleport(PPCContext& ctx, uint8_t* base, const std::string& location,
                          uint64_t map_key_override) {
  TeleportResult res;
  res.hash = NameHash(location);
  uint32_t world = 0, registry = 0, owner = 0, teleporter = 0;
  if (!Read32(base, kWorldPtr, &world) || !world) {
    res.error = "no world object yet";
    res.busy = true;
    return res;
  }
  if (!Read32(base, world + kWorldRegistry, &registry) || !registry ||
      !Read32(base, world + kWorldModeOwner, &owner) || !owner ||
      !Read32(base, owner + kOwnerTeleporter, &teleporter) || !teleporter) {
    res.error = "the teleporter is not set up (not in free skate?)";
    return res;
  }
  uint32_t mode = 0, mode_vt = 0;
  uint8_t allowed = 0;
  if (!Read32(base, owner + kOwnerTeleportMode, &mode) || !mode ||
      !Read32(base, mode, &mode_vt) || mode_vt != kTeleportModeVtable) {
    res.error = "the teleport mode component is missing (not in free skate?)";
    return res;
  }
  if (!Read8(base, mode + kTeleportModeAllowed, &allowed) || !allowed) {
    res.error = "the game does not allow teleporting right now";
    res.busy = true;  // e.g. a teleport or respawn still finishing
    return res;
  }

  const uint32_t entry = CallGuest(ctx, base, sub_82910CE8, {registry, res.hash});
  uint32_t locator = 0;
  if (!entry || !Read32(base, entry + 4, &locator) || !locator) {
    res.error = "unknown location '" + location + "' (not in the registry)";
    return res;
  }
  for (int k = 0; k < 3; ++k) {
    ReadF32(base, locator + 48 + 4 * k, &res.target[k]);
  }

  // The pause map's path, when the destination's map key is known: it loads
  // another map (Industrial, ...) first, then teleports.
  const char* district = DistrictOf(res.hash);
  res.district = district ? district : "";
  const uint64_t map_key = map_key_override == kForceSequencer ? 0
                           : map_key_override                  ? map_key_override
                                                               : DistrictKeyOf(district);
  uint32_t msgsys = 0, service = 0, service_vt = 0;
  if (map_key && Read32(base, kMessageSystemPtr, &msgsys) && msgsys &&
      Read32(base, msgsys + kServiceOffset, &service) && service &&
      Read32(base, service, &service_vt) && service_vt == kServiceVtable) {
    if (location.size() > 95) {
      res.error = "location name too long";
      return res;
    }
    uint32_t cur_hi = 0, cur_lo = 0;
    Read32(base, kTeleportStateBlock + 40, &cur_hi);
    Read32(base, kTeleportStateBlock + 44, &cur_lo);
    res.map_load = ((uint64_t(cur_hi) << 32) | cur_lo) != map_key;
    // Arguments on the guest stack, above a lowered r1: the u64 key, the name
    // pointer, the name.
    const uint32_t data = (ctx.r1.u32 - 0x100) & ~0xFu;
    Write32(base, data, uint32_t(map_key >> 32));
    Write32(base, data + 4, uint32_t(map_key));
    Write32(base, data + 8, data + 16);
    std::memcpy(base + data + 16, location.c_str(), location.size() + 1);
    CallGuest(ctx, base, sub_82864628, {service, data, data + 8, 0, 1}, data - 0x80);
    res.via_service = true;
    res.streamed = true;
    res.ok = true;
    return res;
  }
  uint32_t settings = 0;
  uint8_t flag_byte = 0;
  if (Read32(base, kSettingsPtr, &settings) && settings) {
    Read8(base, settings + 120, &flag_byte);
  }
  // Returns whether the destination must be streamed in first
  // (sub_82864C40). 0: the area is already loaded and the sequencer moved the
  // player right away (sub_827070C0), which free skate ignores. 1: it stored
  // the request; entering the teleporting state does the fade, streaming and
  // move, as the map does. The hook above makes it always 1 for this call.
  t_force_streamed_teleport = true;
  const uint32_t needs_streaming =
      CallGuest(ctx, base, sub_82706F50,
                {teleporter, locator, res.hash, flag_byte ? 1u : 0u, 0, 0, 0}) &
      0xFF;
  t_force_streamed_teleport = false;
  if (needs_streaming) {
    CallGuest(ctx, base, sub_82720B50, {mode, kStateTeleporting, teleporter});
  }
  res.streamed = needs_streaming != 0;
  res.ok = true;
  return res;
}

}  // namespace

uint32_t NameHash(std::string_view name) {
  uint32_t h = 0;
  for (char ch : name) {
    h = (h << 4) + static_cast<uint32_t>(static_cast<int32_t>(static_cast<signed char>(ch)));
    const uint32_t g = h & 0xF0000000u;
    if (g) {
      h ^= ((g >> 23) & 0x1FFu) ^ g;
    }
  }
  return h;
}

TeleportResult Teleport(const std::string& location, double timeout_s, uint64_t map_key) {
  Request req{location, map_key, std::nullopt};
  std::unique_lock lk(g_mu);
  if (g_pending) {
    TeleportResult r;
    r.error = "another game call is pending";
    return r;
  }
  g_pending = &req;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(timeout_s));
  if (!g_cv.wait_until(lk, deadline, [&] { return req.result.has_value(); })) {
    g_pending = nullptr;
    TeleportResult r;
    r.error = "the game did not reach a frame boundary in time";
    return r;
  }
  g_pending = nullptr;
  return *req.result;
}

// ---- skate.Reel load experiment (RE; see scenarios/README.md) ----------------
// "Load replay N" the way FE 59 (My Replays) does it: the replay save content
// is named REPLY_N with load type 'B' (body; 'H' = header), then a type-36
// memcard request {id 14, content} goes to the memcard owner's event handler.
// The load then deserializes into the type-27 request that starts StateReplay.
//   gctx    = *(*(0x83083BCC) + 156)   (table of the game-mode state objects)
//   owner   = *(gctx + 0x11C)          (memcard owner; handler sub_8271A828)
//   content = *(*(gctx + 0xE4) + 0x38) (replay save content, vtable 0x822FDE48)
//   request param = content + 0x198 (secondary base, vtable 0x822FDEC0)
namespace {
uint32_t g_reel_event = 0;  // persistent type-36 event object (system heap)
uint32_t g_reel_ref = 0;    // ref cell pointing at it

void DoLoadReelExperiment(PPCContext& ctx, uint8_t* base, int n) {
  uint32_t world = 0, gctx = 0, owner = 0, holder = 0, content = 0;
  if (!Read32(base, kWorldPtr, &world) || !Read32(base, world + kWorldModeOwner, &gctx) ||
      !Read32(base, gctx + 0x11C, &owner) || !Read32(base, gctx + 0xE4, &holder) ||
      !Read32(base, holder + 0x38, &content) || !owner || !content) {
    REXLOG_INFO("harness: reel load {}: objects missing (world={:08X} gctx={:08X} owner={:08X} "
                "content={:08X})", n, world, gctx, owner, content);
    return;
  }
  if (!g_reel_event) {
    auto* memory = rex::system::kernel_memory();
    g_reel_event = memory->SystemHeapAlloc(64);
    g_reel_ref = memory->SystemHeapAlloc(16);
    if (!g_reel_event || !g_reel_ref) {
      REXLOG_INFO("harness: reel load: guest alloc failed");
      return;
    }
    std::memset(base + g_reel_event, 0, 64);
  }
  // Name the content REPLY_N, body load.
  char name[16] = {};
  std::snprintf(name, sizeof(name), "REPLY_%d", n);
  std::memcpy(base + content + 0x1BC, name, sizeof(name));
  base[content + 0x1AC] = 'B';
  // Event: generic event vtable, a refcount that never reaches 0, type 36,
  // memcard event 14, param = the content's secondary base.
  Write32(base, g_reel_event + 0, 0x8230C738);
  Write32(base, g_reel_event + 4, 0x10000);
  Write32(base, g_reel_event + 8, 36);
  Write32(base, g_reel_event + 12, 14);
  Write32(base, g_reel_event + 16, content + 0x198);
  Write32(base, g_reel_ref, g_reel_event);
  const uint32_t r = CallGuest(ctx, base, sub_8271A828, {owner, g_reel_ref});
  REXLOG_INFO("harness: reel load {}: request sent to owner {:08X} (content {:08X}) -> {}", n,
              owner, content, r);
}
}  // namespace

void OnGuestFrame(PPCContext& ctx, uint8_t* base) {
  if (const int n = REXCVAR_GET(skate3_harness_re_load_reel); n > 0) {
    rex::cvar::SetFlagByName("skate3_harness_re_load_reel", "0");
    DoLoadReelExperiment(ctx, base, n);
  }
  Request* req;
  {
    std::lock_guard lk(g_mu);
    req = g_pending;
    if (!req || req->result) {
      return;
    }
  }
  TeleportResult r = DoTeleport(ctx, base, req->location, req->map_key);
  REXLOG_INFO("harness: teleport '{}' (hash {:08X}, district {}) -> {}{}", req->location, r.hash,
              r.district.empty() ? "?" : r.district,
              !r.ok             ? "failed: "
              : r.via_service   ? (r.map_load ? "issued via the map path (loading another map)"
                                              : "issued via the map path")
                                : "issued via the sequencer (no map key for this district)",
              r.error);
  {
    std::lock_guard lk(g_mu);
    req->result = std::move(r);
  }
  g_cv.notify_all();
}

}  // namespace skate3::harness::game
