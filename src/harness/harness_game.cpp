#include "harness/harness_game.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include <cstdlib>

REXCVAR_DEFINE_STRING(skate3_harness_re_dump, "", "Skate 3",
                      "Harness RE: log guest words once, 'ADDR:COUNT[,ADDR:COUNT...]' (hex addr, "
                      "or *ADDR+OFF to follow a pointer); cleared once logged")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_harness_re_poke, "", "Skate 3",
                      "Harness RE: write guest words once, 'ADDR=VALUE[,ADDR=VALUE...]' (hex); "
                      "cleared once written")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#include "native/skate3_native_guest_read.h"

// The guest functions called below (recompiled; TU3 image addresses).
REX_EXTERN(sub_82910CE8);  // registry find(map*, u32 key) -> entry* (node + 20) or 0
REX_EXTERN(sub_82706F50);  // teleport sequencer(teleporter, matrix*, id, flag, b, b, b) -> bool
REX_EXTERN(sub_8271A828);  // memcard owner event handler(owner, ref<event>*) -> 1 handled / 2 not
REX_EXTERN(sub_82B938D8);  // post message(msg*, recipient*, priority) (queued)
REX_EXTERN(__imp__RtlEnterCriticalSection);  // 0x82F9CB44
REX_EXTERN(__imp__RtlLeaveCriticalSection);  // 0x82F9CB54
REX_EXTERN(sub_824FCAD8);  // game-mode event post(gctx, ref<event>*)
REX_EXTERN(sub_82506BE0);  // editor timeline key position(E, index) -> f1 (0..1)
REX_EXTERN(sub_824AD240);  // FE manager singleton getter
REX_EXTERN(sub_82627128);  // My Replays "view replay" action(this)
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
  bool use_pos = false;  // TeleportToPos: pos/yaw instead of a locator
  float pos[3] = {};
  float yaw_deg = 0;
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

// CallGuest for functions that return a float (f1).
double CallGuestF(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*),
                  std::initializer_list<uint32_t> args) {
  const PPCContext saved = ctx;
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9};
  size_t i = 0;
  for (uint32_t a : args) {
    regs[i++]->u64 = a;
  }
  fn(ctx, base);
  const double ret = ctx.f1.f64;
  ctx = saved;
  return ret;
}

// The objects every teleport needs, checked: false (with res.error) when the
// game is not in a state that can teleport.
struct TeleportObjects {
  uint32_t registry = 0, teleporter = 0, mode = 0;
};
bool GetTeleportObjects(uint8_t* base, TeleportObjects* o, TeleportResult* res) {
  uint32_t world = 0, owner = 0;
  if (!Read32(base, kWorldPtr, &world) || !world) {
    res->error = "no world object yet";
    res->busy = true;
    return false;
  }
  if (!Read32(base, world + kWorldRegistry, &o->registry) || !o->registry ||
      !Read32(base, world + kWorldModeOwner, &owner) || !owner ||
      !Read32(base, owner + kOwnerTeleporter, &o->teleporter) || !o->teleporter) {
    res->error = "the teleporter is not set up (not in free skate?)";
    return false;
  }
  uint32_t mode_vt = 0;
  uint8_t allowed = 0;
  if (!Read32(base, owner + kOwnerTeleportMode, &o->mode) || !o->mode ||
      !Read32(base, o->mode, &mode_vt) || mode_vt != kTeleportModeVtable) {
    res->error = "the teleport mode component is missing (not in free skate?)";
    return false;
  }
  if (!Read8(base, o->mode + kTeleportModeAllowed, &allowed) || !allowed) {
    res->error = "the game does not allow teleporting right now";
    res->busy = true;  // e.g. a teleport or respawn still finishing
    return false;
  }
  return true;
}

// The sequencer path with a given transform (`matrix`: guest address of a
// 16-byte-aligned 4x4 world matrix, a locator's layout). Returns whether the
// teleporting state was entered (the destination is streamed in, then the
// player moved).
bool SequencerTeleport(PPCContext& ctx, uint8_t* base, const TeleportObjects& o,
                       uint32_t matrix, uint32_t hash, uint32_t stack_top) {
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
                {o.teleporter, matrix, hash, flag_byte ? 1u : 0u, 0, 0, 0}, stack_top) &
      0xFF;
  t_force_streamed_teleport = false;
  if (needs_streaming) {
    CallGuest(ctx, base, sub_82720B50, {o.mode, kStateTeleporting, o.teleporter}, stack_top);
  }
  return needs_streaming != 0;
}

TeleportResult DoTeleportPos(PPCContext& ctx, uint8_t* base, const float pos[3],
                             float yaw_deg) {
  TeleportResult res;
  TeleportObjects o;
  if (!GetTeleportObjects(base, &o, &res)) {
    return res;
  }
  // A yaw-only transform in the locators' layout: rows 0..2 the basis, row 3
  // the translation. The skater entity's matrix (what SkaterYawDeg reads) is
  // turned 90 degrees from the transform the teleport takes (measured: a
  // teleport built with angle a reads back as a + 98.9 standing still; the
  // other ~9 degrees are the idle pose), so the angle is yaw - 90.
  const float a = (yaw_deg - 90.0f) * 3.14159265358979f / 180.0f;
  const float c = std::cos(a), s = std::sin(a);
  const float m[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, pos[0], pos[1], pos[2], 1};
  // On the guest stack below the hook's frame, 16-byte aligned (the game
  // copies matrices with 128-bit vector loads).
  const uint32_t data = (ctx.r1.u32 - 0x100) & ~0xFu;
  for (int k = 0; k < 16; ++k) {
    uint32_t bits;
    std::memcpy(&bits, &m[k], 4);
    Write32(base, data + 4 * k, bits);
  }
  for (int k = 0; k < 3; ++k) {
    res.target[k] = pos[k];
  }
  res.streamed = SequencerTeleport(ctx, base, o, data, 0, data - 0x80);
  res.ok = true;
  return res;
}

TeleportResult DoTeleport(PPCContext& ctx, uint8_t* base, const std::string& location,
                          uint64_t map_key_override) {
  TeleportResult res;
  res.hash = NameHash(location);
  TeleportObjects o;
  if (!GetTeleportObjects(base, &o, &res)) {
    return res;
  }
  const uint32_t registry = o.registry;

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
  res.streamed = SequencerTeleport(ctx, base, o, locator, res.hash, 0);
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

namespace {
struct Spot {
  const char* name;
  float x, y, z;
};
constexpr Spot kSpots[] = {
#include "harness/teleport_spots.inc"
};
}  // namespace

std::string NearestSpot(const float pos[3], float* distance) {
  const Spot* best = nullptr;
  float best_d2 = 0.0f;
  for (const Spot& s : kSpots) {
    const float dx = s.x - pos[0], dy = s.y - pos[1], dz = s.z - pos[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (!best || d2 < best_d2) {
      best = &s;
      best_d2 = d2;
    }
  }
  if (distance) {
    *distance = best ? std::sqrt(best_d2) : 0.0f;
  }
  return best ? best->name : std::string();
}

float SkaterYawDeg(const float m[16]) {
  return std::atan2(m[8], m[10]) * 180.0f / 3.14159265358979f;
}

namespace {
TeleportResult RunRequest(Request& req, double timeout_s);
}  // namespace

TeleportResult Teleport(const std::string& location, double timeout_s, uint64_t map_key) {
  Request req;
  req.location = location;
  req.map_key = map_key;
  return RunRequest(req, timeout_s);
}

TeleportResult TeleportToPos(const float pos[3], float yaw_deg, double timeout_s) {
  Request req;
  req.use_pos = true;
  std::memcpy(req.pos, pos, sizeof(req.pos));
  req.yaw_deg = yaw_deg;
  return RunRequest(req, timeout_s);
}

namespace {
TeleportResult RunRequest(Request& req, double timeout_s) {
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
}  // namespace

namespace {

// Posts a message to the simulation, the way the presentation side posts
// cMsgPauseSim (sub_82477A68): queue = *(world+144)+24, its lock at +16, the
// recipient word at +4; sub_82B938D8(&msg, &recipient, 1). A message is
// {type, 0, size, 0, payload words...}.
uint32_t g_msg_block = 0;

bool PostSimMessage(PPCContext& ctx, uint8_t* base, uint32_t type,
                    std::initializer_list<uint32_t> payload) {
  uint32_t world = 0, q = 0, recipient = 0;
  if (!Read32(base, kWorldPtr, &world) || !Read32(base, world + 144, &q) || !q) {
    return false;
  }
  q += 24;
  if (!g_msg_block) {
    g_msg_block = rex::system::kernel_memory()->SystemHeapAlloc(128);
  }
  const uint32_t msg = g_msg_block, recip = g_msg_block + 96;
  std::memset(base + msg, 0, 128);
  Write32(base, msg + 0, type);
  Write32(base, msg + 8, 16 + 4 * static_cast<uint32_t>(payload.size()));
  uint32_t off = 16;
  for (uint32_t w : payload) {
    Write32(base, msg + off, w);
    off += 4;
  }
  CallGuest(ctx, base, __imp__RtlEnterCriticalSection, {q + 16});
  Read32(base, q + 4, &recipient);
  Write32(base, recip, recipient);
  CallGuest(ctx, base, sub_82B938D8, {msg, recip, 1});
  CallGuest(ctx, base, __imp__RtlLeaveCriticalSection, {q + 16});
  return true;
}

// The game's own "view replay" action (My Replays, FE 59): sub_82627128(this)
// reads the selected index from browser+80, browser = *(*(sub_824AD240()+8)
// + 236), sets the save manager's current replay, queues op type 8 for entry
// [index] of the content manager *(0x830670A0) (list +384, count +392, filled
// from the replay headers) and kicks its op queue. `this` only receives the
// op id at +8, so a scratch block stands in for the FE screen.
uint32_t g_view_this = 0;
uint32_t g_view_handler = 0;     // stand-in screen handler object
uint32_t g_view_handler_vt = 0;  // its vtable (all no-ops)
uint32_t g_view_browser = 0;     // browser whose handler slot holds the stand-in

void DoViewReel(PPCContext& ctx, uint8_t* base, int n) {
  uint32_t mgr = 0, count = 0;
  Read32(base, 0x830670A0, &mgr);
  Read32(base, mgr + 392, &count);
  const uint32_t fe = CallGuest(ctx, base, sub_824AD240, {});
  uint32_t fe_data = 0, browser = 0;
  Read32(base, fe + 8, &fe_data);
  Read32(base, fe_data + 236, &browser);
  REXLOG_INFO("harness: view reel {}: content mgr {:08X} entries {} browser {:08X}", n, mgr, count,
              browser);
  if (!mgr || !browser || n < 1 || static_cast<uint32_t>(n) > count) {
    REXLOG_INFO("harness: view reel {}: not possible (no list / browser, or slot out of range)", n);
    return;
  }
  if (!g_view_this) {
    auto* memory = rex::system::kernel_memory();
    g_view_this = memory->SystemHeapAlloc(64);
    std::memset(base + g_view_this, 0, 64);
    // Stand-in for the My Replays screen handler: the load's completion
    // listener (sub_82620A98 / sub_82620AF0) calls *(browser+5352) slot 13
    // (done) / 14 (failed). Every slot is a bare blr (sub_82B61BB8).
    g_view_handler_vt = memory->SystemHeapAlloc(4 * 64);
    for (int i = 0; i < 64; ++i) {
      Write32(base, g_view_handler_vt + 4 * i, 0x82B61BB8);
    }
    g_view_handler = memory->SystemHeapAlloc(16);
    std::memset(base + g_view_handler, 0, 16);
    Write32(base, g_view_handler, g_view_handler_vt);
  }
  uint32_t handler = 0;
  Read32(base, browser + 5352, &handler);
  if (!handler) {
    Write32(base, browser + 5352, g_view_handler);
    g_view_browser = browser;
  }
  Write32(base, browser + 80, static_cast<uint32_t>(n - 1));
  CallGuest(ctx, base, sub_82627128, {g_view_this});
  uint32_t op = 0;
  Read32(base, g_view_this + 8, &op);
  REXLOG_INFO("harness: view reel {}: queued op {} (screen handler {})", n, op,
              handler ? "live" : "stand-in");
}

// Remove the stand-in handler once the load has completed (the listener
// clears browser+126, the busy flag).
int g_view_frames = 0;
bool g_view_seen_busy = false;

void MaybeRestoreViewHandler(uint8_t* base) {
  if (!g_view_browser) {
    g_view_frames = 0;
    g_view_seen_busy = false;
    return;
  }
  uint8_t busy = 0;
  uint32_t handler = 0;
  Read8(base, g_view_browser + 126, &busy);
  Read32(base, g_view_browser + 5352, &handler);
  g_view_seen_busy |= busy != 0;
  ++g_view_frames;
  if ((g_view_seen_busy && !busy) || g_view_frames > 600 || handler != g_view_handler) {
    if (handler == g_view_handler) {
      Write32(base, g_view_browser + 5352, 0);
    }
    REXLOG_INFO("harness: view reel: stand-in screen handler removed");
    g_view_browser = 0;
  }
}

// ---- play_reel ------------------------------------------------------------
// Load (main thread):
//   1. cMsgPauseSim{1, 11}, as the pause menu does (a reel loaded from live
//      free skate hangs or crashes on playback);
//   2. My Replays' own view action for the slot (DoViewReel).
//   StateReplay loads it and opens the replay editor (FE 12), paused on the
//   last frame.
// Play (render thread, sub_82C7EA50 hook): the editor's time object
//   E = **0x8303740C has a state at E+10696 (8 = paused, 5 = playing) and a
//   normalized position at E+4; while paused it pins the replay track's time
//   to E+4 * duration (duration = *(*E + 0xC), seconds). We keep it paused and
//   set E+4 = t / duration each editor frame, t += the frame's dt (f1): our
//   own 1x clock, no editor input.
// UI: the whole 2D pass is skipped while a reel is active (SuppressUi).
// Exit (main thread): the editor's Finished > Exit action: flags on the
//   editor data x = *(*(sub_824AD240()+8)+48) (x+52 = 1, x+140 = 1, byte
//   *(*(0x83067060+36)+12) = 0) and a type-145 game event; StateReplay then
//   restreams free skate. Then cMsgPauseSim{0, 11}.
constexpr uint32_t kModeStateFreeSkate = 0x8270C890;
constexpr uint32_t kEditorTimePtr = 0x8303740C;

std::atomic<int> g_reel_phase{static_cast<int>(ReelPhase::kIdle)};
std::atomic<int> g_reel_request{0};
std::atomic<uint32_t> g_reel_e{0};
std::atomic<float> g_reel_t{0.0f};
std::atomic<float> g_reel_duration{0.0f};
std::atomic<float> g_reel_speed{1.0f};  // replay clock rate (play_reel speed)
std::atomic<uint64_t> g_reel_frames{0};
// Options for the current reel (set by StartReel before the request).
bool g_reel_from_marker = false;
std::atomic<float> g_reel_start{0.0f};   // where playback started (s)
std::atomic<float> g_reel_marker{-1.0f};  // the reel's marker (s), -1 if none
// Stills: the render-thread clock stops exactly at each time and holds there
// until the harness thread has taken its screenshot (ReleaseReelHold).
constexpr int kMaxStills = 32;
float g_still_t[kMaxStills] = {};
std::atomic<int> g_still_count{0};
std::atomic<int> g_still_next{0};
std::atomic<bool> g_reel_hold{false};
std::mutex g_reel_mu;
std::string g_reel_error;
int g_reel_slot = 0;
int g_reel_wait = 0;
int g_reel_ready_frames = 0;
bool g_reel_exit_posted = false;
uint32_t g_event_block = 0;
// The game's loading screen (FE 52) shows as in the menu flow: the 2D pass
// runs while it is the top FE screen. Between it and the first played frame
// (the editor opens on the reel's last frame), and from the exit until the
// loading screen is up, a black curtain covers the picture (harness overlay).
constexpr uint32_t kFeLoadingScreen = 52;
constexpr int kReelPrerollFrames = 4;  // editor frames at the start position before the clock runs
std::atomic<bool> g_reel_loading_screen{false};
std::atomic<bool> g_reel_curtain{false};
std::atomic<int> g_reel_preroll{0};
bool g_reel_seen_loading = false;

// Top screen of the FE stack (FrontEndManager *(0x830CFE14): records of 20
// bytes between +0x210 and +0x214, screen id first), 0xFFFFFFFF if none.
uint32_t FeTop(uint8_t* base) {
  uint32_t mgr = 0, beg = 0, end = 0, id = 0xFFFFFFFF;
  if (Read32(base, 0x830CFE14, &mgr) && mgr && Read32(base, mgr + 0x210, &beg) &&
      Read32(base, mgr + 0x214, &end) && end >= beg + 20 && end - beg < 20 * 64) {
    Read32(base, end - 20, &id);
  }
  return id;
}

void SetReelPhase(ReelPhase p, const std::string& error = {}) {
  if (!error.empty()) {
    std::lock_guard lk(g_reel_mu);
    g_reel_error = error;
    REXLOG_INFO("harness: reel {}: {}", g_reel_slot, error);
  }
  g_reel_phase.store(static_cast<int>(p));
}

ReelPhase Phase() { return static_cast<ReelPhase>(g_reel_phase.load()); }

uint32_t GameCtx(uint8_t* base) {
  uint32_t world = 0, gctx = 0;
  Read32(base, kWorldPtr, &world);
  Read32(base, world + kWorldModeOwner, &gctx);
  return gctx;
}

uint32_t ModeState(uint8_t* base) {
  uint32_t owner = 0, state = 0;
  Read32(base, GameCtx(base) + 0x28, &owner);
  Read32(base, owner + 8, &state);
  return state;
}

// Posts a refcounted game event {vtable, refs, type} through the game-mode
// event queue (sub_824FCAD8(gctx, ref*)). The block is persistent and its
// refcount never reaches 0.
bool PostGameEvent(PPCContext& ctx, uint8_t* base, uint32_t type) {
  const uint32_t gctx = GameCtx(base);
  if (!gctx) {
    return false;
  }
  if (!g_event_block) {
    g_event_block = rex::system::kernel_memory()->SystemHeapAlloc(64);
  }
  const uint32_t ev = g_event_block, ref = g_event_block + 32;
  std::memset(base + ev, 0, 64);
  Write32(base, ev + 0, 0x8230C738);
  Write32(base, ev + 4, 0x10000);
  Write32(base, ev + 8, type);
  Write32(base, ref, ev);
  CallGuest(ctx, base, sub_824FCAD8, {gctx, ref});
  return true;
}

void PostReelExit(PPCContext& ctx, uint8_t* base) {
  const uint32_t fe = CallGuest(ctx, base, sub_824AD240, {});
  uint32_t fe_data = 0, editor = 0, flags_obj = 0;
  Read32(base, fe + 8, &fe_data);
  Read32(base, fe_data + 48, &editor);
  Read32(base, 0x83067060 + 36, &flags_obj);
  if (flags_obj) {
    base[flags_obj + 12] = 0;
  }
  if (editor) {
    Write32(base, editor + 52, 1);
    base[editor + 140] = 1;
  }
  PostGameEvent(ctx, base, 145);
}

void ReelTick(PPCContext& ctx, uint8_t* base) {
  if (const int slot = g_reel_request.exchange(0); slot > 0) {
    g_reel_slot = slot;
    g_reel_wait = 0;
    g_reel_ready_frames = 0;
    g_reel_exit_posted = false;
    g_reel_seen_loading = false;
    g_reel_loading_screen.store(false);
    g_reel_curtain.store(false);
    g_reel_frames.store(0);
    g_reel_t.store(0.0f);
    g_reel_e.store(0);
    {
      std::lock_guard lk(g_reel_mu);
      g_reel_error.clear();
    }
    uint32_t mgr = 0, count = 0;
    Read32(base, 0x830670A0, &mgr);
    Read32(base, mgr + 392, &count);
    if (!mgr || static_cast<uint32_t>(slot) > count) {
      SetReelPhase(ReelPhase::kFailed, "no saved replay " + std::to_string(slot) + " (" +
                                           std::to_string(count) + " saved)");
      return;
    }
    if (ModeState(base) != kModeStateFreeSkate) {
      SetReelPhase(ReelPhase::kFailed, "the game is not in free skate");
      return;
    }
    PostSimMessage(ctx, base, 0xDDC8C41A, {1u, 11u});  // cMsgPauseSim(1)
    DoViewReel(ctx, base, slot);
    SetReelPhase(ReelPhase::kLoading);
    return;
  }
  const bool on_loading_screen = FeTop(base) == kFeLoadingScreen;
  g_reel_loading_screen.store(on_loading_screen);
  switch (Phase()) {
    case ReelPhase::kLoading: {
      ++g_reel_wait;
      g_reel_seen_loading |= on_loading_screen;
      g_reel_curtain.store(g_reel_seen_loading && !on_loading_screen);
      uint32_t holder = 0, e = 0, state = 0, scale_obj = 0, pos = 0, dur_bits = 0;
      Read32(base, kEditorTimePtr, &holder);
      Read32(base, holder, &e);
      Read32(base, e + 10696, &state);
      Read32(base, e + 4, &pos);
      Read32(base, e, &scale_obj);
      Read32(base, scale_obj + 0xC, &dur_bits);
      float dur = 0, p = 0;
      std::memcpy(&dur, &dur_bits, 4);
      std::memcpy(&p, &pos, 4);
      // Ready: out of free skate (the mode owner's current state is a replay
      // sub-state, not kModeStateReplay itself), the editor paused (state 8) on the last frame
      // (position 1.0) with a sane duration, for half a second.
      const bool ready = ModeState(base) != kModeStateFreeSkate && e && state == 8 && p > 0.999f &&
                         dur > 0.1f && dur < 3600.0f;
      g_reel_ready_frames = ready ? g_reel_ready_frames + 1 : 0;
      // The editor's timeline keys (the marker) arrive about half a second
      // after it settles: wait until channel 0 has a key (count >= 2), or 2 s
      // for a reel without one.
      uint32_t keys_word = 0;
      Read32(base, e + 556, &keys_word);
      const bool keys = (keys_word & 0xFFFF) >= 2;
      // Playing from the start needs no marker: go as soon as the editor has
      // settled (the marker is then reported only if its key is already in).
      const bool keys_needed = g_reel_from_marker;
      if (g_reel_ready_frames >= 5 &&
          (keys || !keys_needed || g_reel_ready_frames >= 75)) {
        // The marker: the first key of the editor's current timeline channel,
        // as the D-pad jump reads it: sub_82506BE0(E, 1) = key 1's normalized
        // position (index 0 = start 0.0; past the last key = end 1.0). Keys
        // are u16/65535 in the list at E + 556 + channel * 16.
        const float m = static_cast<float>(CallGuestF(ctx, base, sub_82506BE0, {e, 1}));
        const bool has_marker = m > 0.0f && m < 1.0f;
        g_reel_marker.store(has_marker ? m * dur : -1.0f);
        float start = 0.0f;
        if (g_reel_from_marker) {
          if (has_marker) {
            start = m * dur;
          } else {
            REXLOG_INFO("harness: reel {}: no marker; playing from the start", g_reel_slot);
          }
        }
        // Stills before the start are skipped.
        int next = 0;
        while (next < g_still_count.load() && g_still_t[next] < start) {
          ++next;
        }
        g_still_next.store(next);
        g_reel_hold.store(false);
        g_reel_duration.store(dur);
        g_reel_start.store(start);
        g_reel_t.store(start);
        uint32_t pbits = 0;
        const float p0 = start / dur;
        std::memcpy(&pbits, &p0, 4);
        Write32(base, e + 4, pbits);
        g_reel_preroll.store(kReelPrerollFrames);
        g_reel_e.store(e);
        REXLOG_INFO("harness: reel {}: playing from {:.2f} s ({:.2f} s long, marker {:.2f} s, editor "
                    "time object {:08X})", g_reel_slot, start, dur, g_reel_marker.load(), e);
        SetReelPhase(ReelPhase::kPlaying);
      } else if (g_reel_wait > 30 * 90) {
        SetReelPhase(ReelPhase::kFailed, "the replay did not load within 90 s");
      }
      break;
    }
    case ReelPhase::kPlaying:
      g_reel_curtain.store(g_reel_preroll.load() > 0);
      if (!g_reel_hold.load() && g_reel_t.load() >= g_reel_duration.load()) {
        g_reel_e.store(0);  // stop driving
        g_reel_wait = 0;
        SetReelPhase(ReelPhase::kExiting);
      }
      break;
    case ReelPhase::kExiting:
      ++g_reel_wait;
      if (!g_reel_exit_posted && g_reel_wait > 15) {  // hold the last frame for 0.5 s
        PostReelExit(ctx, base);
        g_reel_exit_posted = true;
        g_reel_seen_loading = false;
        REXLOG_INFO("harness: reel {}: exit posted", g_reel_slot);
      }
      if (g_reel_exit_posted) {
        // Black from the exit until the game's loading screen is up.
        g_reel_seen_loading |= on_loading_screen;
        g_reel_curtain.store(!g_reel_seen_loading);
      }
      if (g_reel_exit_posted && ModeState(base) == kModeStateFreeSkate) {
        g_reel_curtain.store(false);
        PostSimMessage(ctx, base, 0xDDC8C41A, {0u, 11u});  // cMsgPauseSim(0)
        REXLOG_INFO("harness: reel {}: back in free skate", g_reel_slot);
        SetReelPhase(ReelPhase::kDone);
      } else if (g_reel_wait > 30 * 120) {
        SetReelPhase(ReelPhase::kFailed, "the game did not return to free skate within 120 s");
      }
      break;
    default:
      break;
  }
}
}  // namespace

bool StartReel(int slot, const ReelOptions& options, std::string* error) {
  const ReelPhase p = Phase();
  if (p == ReelPhase::kLoading || p == ReelPhase::kPlaying || p == ReelPhase::kExiting ||
      g_reel_request.load()) {
    *error = "a reel is already playing";
    return false;
  }
  if (options.stills_s.size() > static_cast<size_t>(kMaxStills)) {
    *error = "at most " + std::to_string(kMaxStills) + " stills per reel";
    return false;
  }
  g_reel_from_marker = options.from_marker;
  g_reel_speed.store(options.speed > 0.0f ? options.speed : 1.0f);
  std::vector<float> stills = options.stills_s;
  std::sort(stills.begin(), stills.end());
  for (size_t i = 0; i < stills.size(); ++i) {
    g_still_t[i] = stills[i];
  }
  g_still_count.store(static_cast<int>(stills.size()));
  g_still_next.store(0);
  g_reel_hold.store(false);
  g_reel_phase.store(static_cast<int>(ReelPhase::kIdle));
  g_reel_request.store(slot);
  return true;
}

void ReleaseReelHold() {
  g_reel_hold.store(false);
}

ReelStatus GetReelStatus() {
  ReelStatus s;
  s.phase = Phase();
  s.slot = g_reel_slot;
  s.duration_s = g_reel_duration.load();
  s.t_s = g_reel_t.load();
  s.frames = g_reel_frames.load();
  s.start_s = g_reel_start.load();
  s.marker_s = g_reel_marker.load();
  s.holding = g_reel_hold.load();
  s.hold_index = g_still_next.load() - 1;
  std::lock_guard lk(g_reel_mu);
  s.error = g_reel_error;
  return s;
}

void DoReDump(uint8_t* base, const std::string& spec) {
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) end = spec.size();
    std::string item = spec.substr(pos, end - pos);
    pos = end + 1;
    const size_t colon = item.find(':');
    const int count = colon == std::string::npos ? 16 : std::atoi(item.c_str() + colon + 1);
    std::string a = item.substr(0, colon);
    uint32_t addr = 0;
    if (!a.empty() && a[0] == '*') {  // *PTR+OFF: follow PTR, add OFF
      const size_t plus = a.find('+');
      uint32_t p = static_cast<uint32_t>(std::strtoul(a.substr(1, plus - 1).c_str(), nullptr, 16));
      Read32(base, p, &addr);
      if (plus != std::string::npos) addr += static_cast<uint32_t>(std::strtoul(a.c_str() + plus + 1, nullptr, 16));
    } else {
      addr = static_cast<uint32_t>(std::strtoul(a.c_str(), nullptr, 16));
    }
    std::string words;
    char buf[12];
    for (int i = 0; i < count && i < 256; ++i) {
      uint32_t v = 0xDEADDEAD;
      Read32(base, addr + 4 * i, &v);
      std::snprintf(buf, sizeof(buf), "%s%08X", (i % 8) ? " " : (i ? " | " : ""), v);
      words += buf;
    }
    REXLOG_INFO("harness: re dump {} @{:08X}: {}", item, addr, words);
  }
}

void DoRePoke(uint8_t* base) {
  if (const std::string p = REXCVAR_GET(skate3_harness_re_poke); !p.empty()) {
    rex::cvar::SetFlagByName("skate3_harness_re_poke", "");
    size_t pos = 0;
    while (pos < p.size()) {
      size_t end = p.find(',', pos);
      if (end == std::string::npos) end = p.size();
      const std::string item = p.substr(pos, end - pos);
      pos = end + 1;
      const size_t eq = item.find('=');
      if (eq == std::string::npos) continue;
      const uint32_t a = static_cast<uint32_t>(std::strtoul(item.substr(0, eq).c_str(), nullptr, 16));
      const uint32_t v = static_cast<uint32_t>(std::strtoul(item.c_str() + eq + 1, nullptr, 16));
      Write32(base, a, v);
      REXLOG_INFO("harness: re poke {:08X} = {:08X}", a, v);
    }
  }
}

bool SuppressUi() {
  const ReelPhase p = Phase();
  if (p == ReelPhase::kPlaying) {
    return true;
  }
  // Loading and exiting: the game's loading screen shows, nothing else.
  return (p == ReelPhase::kLoading || p == ReelPhase::kExiting) && !g_reel_loading_screen.load();
}

bool ReelCurtain() {
  const ReelPhase p = Phase();
  return (p == ReelPhase::kLoading || p == ReelPhase::kPlaying || p == ReelPhase::kExiting) &&
         g_reel_curtain.load();
}

void OnGuestFrame(PPCContext& ctx, uint8_t* base) {  // main (simulation) thread
  ReelTick(ctx, base);
  DoRePoke(base);
  if (const std::string d = REXCVAR_GET(skate3_harness_re_dump); !d.empty()) {
    rex::cvar::SetFlagByName("skate3_harness_re_dump", "");
    DoReDump(base, d);
  }
  MaybeRestoreViewHandler(base);
  Request* req;
  {
    std::lock_guard lk(g_mu);
    req = g_pending;
    if (!req || req->result) {
      return;
    }
  }
  if (req->use_pos) {
    TeleportResult r = DoTeleportPos(ctx, base, req->pos, req->yaw_deg);
    REXLOG_INFO("harness: teleport to [{:.2f}, {:.2f}, {:.2f}] yaw {:.1f} -> {}{}", req->pos[0],
                req->pos[1], req->pos[2], req->yaw_deg,
                !r.ok ? "failed: " : "issued via the sequencer", r.error);
    {
      std::lock_guard lk(g_mu);
      req->result = std::move(r);
    }
    g_cv.notify_all();
    return;
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

// The simulation loop's per-frame game-state tick (main thread; called from
// sub_82965C90). Harness game requests run here, after the tick: game logic
// must not run on the render thread (the D3D Swap hook), because the game's
// allocators pick heaps per thread (a reel loaded from the render thread
// allocated from RenderHeap and later hung or crashed on playback).
extern "C" REX_FUNC(sub_826D34B8) {
  __imp__sub_826D34B8(ctx, base);
  skate3::harness::game::OnGuestFrame(ctx, base);
}

// Replay editor time update (render thread): sub_82C7EA50(this, f1 = frame
// dt); E = *this. While play_reel plays, set the editor's normalized position
// before the original pins the replay time to it (see play_reel above).
extern "C" REX_FUNC(sub_82C7EA50) {
  using namespace skate3::harness::game;
  const uint32_t e_playing = g_reel_e.load();
  if (e_playing) {
    uint32_t e = 0;
    Read32(base, ctx.r3.u32, &e);
    if (e == e_playing) {
      const float dur = g_reel_duration.load();
      const float t = g_reel_t.load();
      float pos = dur > 0 ? t / dur : 1.0f;
      pos = pos < 0 ? 0 : (pos > 1 ? 1 : pos);
      uint32_t bits = 0;
      std::memcpy(&bits, &pos, 4);
      Write32(base, e + 4, bits);
      if (g_reel_preroll.load() > 0) {
        g_reel_preroll.fetch_sub(1);  // the start frame is being drawn behind the curtain
      } else if (!g_reel_hold.load()) {
        float nt = t + static_cast<float>(ctx.f1.f64) * g_reel_speed.load();
        const int next = g_still_next.load();
        if (next < g_still_count.load() && nt >= g_still_t[next]) {
          nt = g_still_t[next];  // stop exactly at the still and hold there
          g_still_next.store(next + 1);
          g_reel_hold.store(true);
        }
        g_reel_t.store(nt);
        g_reel_frames.fetch_add(1);
      }
    }
  }
  __imp__sub_82C7EA50(ctx, base);
}
