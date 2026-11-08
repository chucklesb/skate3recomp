// Reverse-engineering probes for the harness (off unless
// --skate3_harness_re_probe=true). They log what the game passes to the
// script-binding layer, so the state-setting and Reel verbs can be built on
// the game's own functions. Findings: scenarios/README.md "Notes for later
// work". Addresses are TU3 (3.0.3.0) image addresses.
//
// Script bindings are tables of {0, 2, const char* name, lua_CFunction}
// (16-byte entries). Each binding gets its context object from closure
// upvalue 2 through sub_82857548(lua_State*). Known context fields:
//   +16  message queue (critical section at +16 of it), used by SetSessionMarker
//   +24  player registry: vtable slot 6 = player object from player id
//   +28  local player source: vtable slot 1 = local player id
//   +40  teleport service: slot 8 = TeleportLocalPlayerToSessionMarker,
//        slot 9 = TeleportLocalToWorld(u64* name hash, handle*, bool, int)

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string.h>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_guest_read.h"

REXCVAR_DEFINE_STRING(skate3_harness_re_watch_msgs, "", "Skate 3",
                      "Harness RE probe: comma-separated message types (hex) to log on every "
                      "post, not just the first time")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_harness_re_probe, false, "Skate 3",
                    "Harness RE probes: log the script-binding context, session-marker and "
                    "highlight-reel calls (reverse-engineering aid; noisy)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::harness::re {
namespace {

bool Read32(uint8_t* base, uint32_t addr, uint32_t* out) {
  uint32_t raw = 0;
  if (addr == 0 || !native_scene::GuestTryCopy(&raw, base + addr, sizeof(raw))) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

std::string Words(uint8_t* base, uint32_t addr, int count) {
  std::string out;
  char buf[16];
  for (int i = 0; i < count; ++i) {
    uint32_t v = 0;
    std::snprintf(buf, sizeof(buf), "%s%08X", i ? " " : "", Read32(base, addr + 4 * i, &v) ? v : 0xDEADDEAD);
    out += buf;
  }
  return out;
}

// "obj vt=V [slot0 slot1 ...]" for an object pointer stored at addr.
std::string ObjAt(uint8_t* base, uint32_t field_addr, int slots) {
  uint32_t obj = 0, vt = 0;
  if (!Read32(base, field_addr, &obj) || !obj) {
    return "null";
  }
  char buf[48];
  if (!Read32(base, obj, &vt)) {
    std::snprintf(buf, sizeof(buf), "%08X (unreadable)", obj);
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "%08X vt=%08X [", obj, vt);
  return buf + Words(base, vt, slots) + "]";
}

std::mutex g_mu;
// Context -> the fields last logged (+16 .. +40); logged again when they change.
struct SeenContext {
  uint32_t ctx = 0;
  uint32_t fields[7] = {};
};
std::vector<SeenContext> g_contexts_seen;
int g_context_logs = 0;

}  // namespace
}  // namespace skate3::harness::re

using namespace skate3::harness::re;

// lua context getter: upvalue 2 of the binding closure (light userdata).
extern "C" REX_FUNC(sub_82857548) {
  __imp__sub_82857548(ctx, base);
  if (!REXCVAR_GET(skate3_harness_re_probe)) {
    return;
  }
  const uint32_t c = ctx.r3.u32;
  if (!c) {
    return;
  }
  {
    uint32_t fields[7] = {};
    for (int i = 0; i < 7; ++i) {
      Read32(base, c + 16 + 4 * i, &fields[i]);
    }
    std::lock_guard lk(g_mu);
    SeenContext* entry = nullptr;
    for (SeenContext& seen : g_contexts_seen) {
      if (seen.ctx == c) {
        entry = &seen;
      }
    }
    if (entry && std::memcmp(entry->fields, fields, sizeof(fields)) == 0) {
      return;
    }
    if (!entry) {
      if (g_contexts_seen.size() >= 16) {
        return;
      }
      entry = &g_contexts_seen.emplace_back();
      entry->ctx = c;
    }
    if (++g_context_logs > 64) {
      return;
    }
    std::memcpy(entry->fields, fields, sizeof(fields));
  }
  REXLOG_INFO("re-probe: binding context {:08X} (lr {:08X}) words[0..16]: {}", c,
              static_cast<uint32_t>(ctx.lr), Words(base, c, 16));
  for (uint32_t off : {16u, 24u, 28u, 40u}) {
    REXLOG_INFO("re-probe:   ctx+{}: {}", off, ObjAt(base, c + off, 12));
  }
}

// Teleport / session-marker script bindings (r3 = lua_State). Logged on
// entry; the context getter hook above logs the context they resolve.
#define RE_PROBE_BINDING(addr, name)                                                    \
  extern "C" REX_FUNC(sub_##addr) {                                                     \
    if (REXCVAR_GET(skate3_harness_re_probe)) {                                         \
      REXLOG_INFO("re-probe: binding " name " L={:08X} lr={:08X} stack: {}", ctx.r3.u32, \
                  static_cast<uint32_t>(ctx.lr), Words(base, ctx.r3.u32, 6));           \
    }                                                                                   \
    __imp__sub_##addr(ctx, base);                                                       \
  }
RE_PROBE_BINDING(82D5C808, "TeleportLocalPlayerToToFELocation")
RE_PROBE_BINDING(82D5CAD0, "TeleportLocalPlayerToLocationName")
RE_PROBE_BINDING(82D5CA98, "TeleportLocalPlayerToSessionMarker")
RE_PROBE_BINDING(82D5CBD8, "TeleportLocalPlayerToLocationID")
RE_PROBE_BINDING(82D5C948, "TeleportLocalToWorld")
RE_PROBE_BINDING(82839210, "TeleportAndStartChallenge")
RE_PROBE_BINDING(82D4E3D8, "SetSessionMarker")
RE_PROBE_BINDING(8284AA50, "ClearTeleports")
// The two teleport-start methods (senders of 523ED4CC, the first message of a
// menu teleport). A: (this, u64* map key, const char* location, r6, r7, r8),
// used across maps (Industrial); B: (this, r4, r5, r6), same map (Campus).
namespace skate3::harness::re {
namespace {
std::string GuestString(uint8_t* base, uint32_t addr) {
  char buf[96] = {};
  if (!addr || !native_scene::GuestTryCopy(buf, base + addr, sizeof(buf) - 1)) {
    return "?";
  }
  buf[sizeof(buf) - 1] = 0;
  return std::string(buf, strnlen(buf, sizeof(buf) - 1));
}
}  // namespace
}  // namespace skate3::harness::re

extern "C" REX_FUNC(sub_828646E0) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    uint32_t hi = 0, lo = 0;
    Read32(base, ctx.r4.u32, &hi);
    Read32(base, ctx.r4.u32 + 4, &lo);
    REXLOG_INFO(
        "re-probe: teleport-start A this={:08X} mapkey*={:08X} ({:08X}{:08X}) location='{}' "
        "r6={:08X} r7={:08X} r8={:08X} lr={:08X} this words: {}",
        ctx.r3.u32, ctx.r4.u32, hi, lo, GuestString(base, ctx.r5.u32), ctx.r6.u32, ctx.r7.u32,
        ctx.r8.u32, static_cast<uint32_t>(ctx.lr), Words(base, ctx.r3.u32, 8));
  }
  __imp__sub_828646E0(ctx, base);
}

extern "C" REX_FUNC(sub_82864918) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    REXLOG_INFO(
        "re-probe: teleport-start B this={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X} "
        "r4 words: {} this words: {}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr),
        Words(base, ctx.r4.u32, 16), Words(base, ctx.r3.u32, 8));
  }
  __imp__sub_82864918(ctx, base);
}
#undef RE_PROBE_BINDING

// Message census: the game posts typed messages (r3 = message: +0 type hash,
// +8 size, payload from +16; r4 = &recipient). Each new (type, caller) pair
// is logged once with the payload head, so actions done by hand (a menu
// teleport) show up as new types.
namespace skate3::harness::re {
namespace {
std::mutex g_msg_mu;
std::vector<std::pair<uint32_t, uint32_t>> g_msg_seen;

void LogMessage(const char* how, uint8_t* base, uint32_t msg, uint32_t recipient_ptr,
                uint32_t lr) {
  if (!REXCVAR_GET(skate3_harness_re_probe)) {
    return;
  }
  uint32_t type = 0, size = 0, recipient = 0;
  Read32(base, msg, &type);
  Read32(base, msg + 8, &size);
  Read32(base, recipient_ptr, &recipient);
  // Watched types are logged on every post.
  char hex[12];
  std::snprintf(hex, sizeof(hex), "%08X", type);
  const std::string watch = REXCVAR_GET(skate3_harness_re_watch_msgs);
  const bool watched = !watch.empty() && (watch.find(hex) != std::string::npos ||
                                          watch.find(std::string(hex).insert(0, "0x")) != std::string::npos);
  if (!watched) {
    std::lock_guard lk(g_msg_mu);
    for (const auto& [t, l] : g_msg_seen) {
      if (t == type && l == lr) {
        return;
      }
    }
    if (g_msg_seen.size() >= 2048) {
      return;
    }
    g_msg_seen.emplace_back(type, lr);
  }
  REXLOG_INFO("re-probe: msg{} {} type={:08X} size={} to={:08X} lr={:08X} payload: {}",
              watched ? " (watched)" : "", how, type,
              size, recipient, lr, Words(base, msg + 16, size >= 16 ? std::min<uint32_t>((size - 16) / 4, 24) : 0));
}
}  // namespace
}  // namespace skate3::harness::re

extern "C" REX_FUNC(sub_82B938D8) {  // post message (queued)
  LogMessage("post", base, ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  __imp__sub_82B938D8(ctx, base);
}

extern "C" REX_FUNC(sub_82B93960) {  // post message (immediate)
  LogMessage("send", base, ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  __imp__sub_82B93960(ctx, base);
}

// SetSessionMarker implementation (r3 = binding context).
extern "C" REX_FUNC(sub_82D55AE8) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    REXLOG_INFO("re-probe: SetSessionMarker ctx={:08X} lr={:08X}", ctx.r3.u32,
                static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82D55AE8(ctx, base);
}

// PlayHighlightReel implementation (r3 = highlight group, r4/r5 = flags).
extern "C" REX_FUNC(sub_82BCDDE8) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    REXLOG_INFO("re-probe: PlayHighlightReel group={:08X} a={} b={} lr={:08X} group words: {}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr),
                Words(base, ctx.r3.u32, 12));
  }
  __imp__sub_82BCDDE8(ctx, base);
}
