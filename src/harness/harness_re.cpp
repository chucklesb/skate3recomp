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
                      "Harness RE probe: comma-separated message types to log on every post, "
                      "not just the first time: hex ids or name prefixes (cMsgReplay)")
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

// Message type names: the image holds {u32 type id, const char* "cMsg..."}
// pairs (TU3: 0x822EB000-0x822F2000; tools/re/messages.py exports them).
constexpr uint32_t kMsgTableLo = 0x822EB000;
constexpr uint32_t kMsgTableHi = 0x822F2000;
std::once_flag g_names_once;
std::vector<std::pair<uint32_t, std::string>> g_msg_names;  // sorted by id

void LoadMessageNames(uint8_t* base) {
  for (uint32_t a = kMsgTableLo; a + 8 <= kMsgTableHi; a += 4) {
    uint32_t id = 0, ptr = 0;
    if (!Read32(base, a, &id) || !Read32(base, a + 4, &ptr) || ptr < 0x82000000 ||
        ptr >= 0x84A10000) {
      continue;
    }
    char buf[72] = {};
    if (!native_scene::GuestTryCopy(buf, base + ptr, sizeof(buf) - 1) ||
        std::strncmp(buf, "cMsg", 4) != 0) {
      continue;
    }
    g_msg_names.emplace_back(id, std::string(buf, strnlen(buf, sizeof(buf) - 1)));
  }
  std::sort(g_msg_names.begin(), g_msg_names.end());
  g_msg_names.erase(std::unique(g_msg_names.begin(), g_msg_names.end(),
                                [](const auto& a, const auto& b) { return a.first == b.first; }),
                    g_msg_names.end());
  REXLOG_INFO("re-probe: {} message type names loaded", g_msg_names.size());
}

const std::string& MessageName(uint8_t* base, uint32_t type) {
  static const std::string kUnknown = "?";
  std::call_once(g_names_once, [base] { LoadMessageNames(base); });
  auto it = std::lower_bound(g_msg_names.begin(), g_msg_names.end(), type,
                             [](const auto& e, uint32_t t) { return e.first < t; });
  return it != g_msg_names.end() && it->first == type ? it->second : kUnknown;
}

void LogMessage(const char* how, uint8_t* base, uint32_t msg, uint32_t recipient_ptr,
                uint32_t lr) {
  if (!REXCVAR_GET(skate3_harness_re_probe)) {
    return;
  }
  uint32_t type = 0, size = 0, recipient = 0;
  Read32(base, msg, &type);
  Read32(base, msg + 8, &size);
  Read32(base, recipient_ptr, &recipient);
  // Watched types (by hex id or by name, e.g. "cMsgReplay") are logged on
  // every post.
  char hex[12];
  std::snprintf(hex, sizeof(hex), "%08X", type);
  const std::string& name = MessageName(base, type);
  const std::string watch = REXCVAR_GET(skate3_harness_re_watch_msgs);
  bool watched = !watch.empty() && watch.find(hex) != std::string::npos;
  if (!watched && !watch.empty() && name != "?") {
    // Comma-separated entries; a name entry matches as a prefix.
    size_t pos = 0;
    while (pos <= watch.size() && !watched) {
      const size_t comma = watch.find(',', pos);
      const std::string entry =
          watch.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      watched = entry.size() >= 4 && name.compare(0, entry.size(), entry) == 0;
      if (comma == std::string::npos) {
        break;
      }
      pos = comma + 1;
    }
  }
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
  REXLOG_INFO("re-probe: msg{} {} type={:08X} ({}) size={} to={:08X} lr={:08X} payload: {}",
              watched ? " (watched)" : "", how, type, name,
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

// Game-mode state machines: every transition (sub_82720B50(sm, state fn,
// param) and sub_82720FE0(sm, param)), and the replay states found by their
// name strings. Shows how the Skate Reel menu enters replay playback.
extern "C" REX_FUNC(sub_82720B50) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    uint32_t cur = 0;
    Read32(base, ctx.r3.u32 + 8, &cur);
    REXLOG_INFO("re-probe: state transition sm={:08X} from={:08X} to={:08X} param={:08X} lr={:08X}",
                ctx.r3.u32, cur, ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82720B50(ctx, base);
}

extern "C" REX_FUNC(sub_82720FE0) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    uint32_t cur = 0;
    Read32(base, ctx.r3.u32 + 8, &cur);
    REXLOG_INFO("re-probe: state transition (B) sm={:08X} from={:08X} param={:08X} lr={:08X}",
                ctx.r3.u32, cur, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82720FE0(ctx, base);
}

#define RE_PROBE_FN(addr, name)                                                                \
  extern "C" REX_FUNC(sub_##addr) {                                                            \
    if (REXCVAR_GET(skate3_harness_re_probe)) {                                                \
      REXLOG_INFO("re-probe: " name " r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X}",      \
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr)); \
    }                                                                                          \
    __imp__sub_##addr(ctx, base);                                                              \
  }
RE_PROBE_FN(8271C2D0, "StateReplay::LoadReplayData")
RE_PROBE_FN(8271C9E8, "StateReplay::LoadWorldStream")
RE_PROBE_FN(82706D18, "StateReplay (sub_82706D18)")
RE_PROBE_FN(8270DAE8, "StateMemcardReplayLoadFlow")
RE_PROBE_FN(8270DB88, "StateMemcardLoadAllReplayHeadersFlow")
#undef RE_PROBE_FN

// Reel playback request: the game-mode owner's event handler
// sub_8270B3D0(owner, ref<event>*). The event object is refcounted
// (+0 vtable, +4 refs, +8 type, then the payload); type 12 copies the ref to
// owner+40 and sets owner+604, which the free-skate state turns into
// sub_8270C0E0 -> StateReplay (FE 52 -> 12).
namespace {
int g_event_logs[64] = {};
int g_tc_calls = 0;
}  // namespace

extern "C" REX_FUNC(sub_8270B3D0) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    uint32_t ev = 0, type = 0xFFFFFFFF;
    Read32(base, ctx.r4.u32, &ev);
    Read32(base, ev + 8, &type);
    const bool interesting = type == 12;
    int* seen = type < 64 ? &g_event_logs[type] : nullptr;
    if (interesting || (seen && *seen < 3)) {
      if (seen) ++*seen;
      REXLOG_INFO("re-probe: mode event owner={:08X} ev={:08X} type={} lr={:08X} words: {}",
                  ctx.r3.u32, ev, static_cast<int32_t>(type), static_cast<uint32_t>(ctx.lr),
                  Words(base, ev, 24));
    }
  }
  __imp__sub_8270B3D0(ctx, base);
}

// StateReplay setup: sub_8271BF50(StateReplay, ref<request event>*).
extern "C" REX_FUNC(sub_8271BF50) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    uint32_t ev = 0;
    Read32(base, ctx.r4.u32, &ev);
    REXLOG_INFO("re-probe: StateReplay setup sr={:08X} req={:08X} lr={:08X} req words: {} | sr words: {}",
                ctx.r3.u32, ev, static_cast<uint32_t>(ctx.lr), Words(base, ev, 24),
                Words(base, ctx.r3.u32, 40));
  }
  __imp__sub_8271BF50(ctx, base);
}

// Posts cMsgManualTimeStartEndFrames(start, end) from the replay timeline
// *(*(0x83083C38)+112) (vtable slot 12 = start, slot 11 = end).
extern "C" REX_FUNC(sub_82837198) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    uint32_t mgr = 0, tl = 0;
    Read32(base, 0x83083C38, &mgr);
    Read32(base, mgr + 112, &tl);
    REXLOG_INFO("re-probe: timeline start/end r3={:08X} r4={:08X} mgr={:08X} lr={:08X} timeline {} words: {}",
                ctx.r3.u32, ctx.r4.u32, mgr, static_cast<uint32_t>(ctx.lr), ObjAt(base, mgr + 112, 16),
                Words(base, tl, 48));
  }
  __imp__sub_82837198(ctx, base);
}

// Time controller (caller chain of the start/end post); sampled 1 in 30.
extern "C" REX_FUNC(sub_828331F0) {
  if (REXCVAR_GET(skate3_harness_re_probe) && (g_tc_calls++ % 30) == 0) {
    REXLOG_INFO("re-probe: time controller #{} r3={:08X} r4={:08X} f1={} lr={:08X} r3 words: {}",
                g_tc_calls - 1, ctx.r3.u32, ctx.r4.u32, ctx.f1.f64, static_cast<uint32_t>(ctx.lr),
                Words(base, ctx.r3.u32, 32));
  }
  __imp__sub_828331F0(ctx, base);
}

// Guest call stack: lr, then the saved LR of each frame (back chain at
// *(r1), saved LR at frame-8).
namespace {
std::string Backtrace(PPCContext& ctx, uint8_t* base, int depth = 14) {
  char buf[12];
  std::snprintf(buf, sizeof(buf), "%08X", static_cast<uint32_t>(ctx.lr));
  std::string out = buf;
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < depth; ++i) {
    uint32_t prev = 0, lr = 0;
    if (!Read32(base, sp, &prev) || prev <= sp || prev - sp > 0x10000) break;
    if (!Read32(base, prev - 8, &lr)) break;
    std::snprintf(buf, sizeof(buf), " %08X", lr);
    out += buf;
    sp = prev;
  }
  return out;
}
}  // namespace

// Who asks for a replay body load: the memcard load-op constructors (op
// vtable 0x8232E9EC, run on the memcard worker thread) and the replay
// deserializer that builds the type-27 request (save-content vtable slot 60).
#define RE_PROBE_BT(addr, name)                                                                  \
  extern "C" REX_FUNC(sub_##addr) {                                                              \
    if (REXCVAR_GET(skate3_harness_re_probe)) {                                                  \
      REXLOG_INFO("re-probe: " name " r3={:08X} r4={:08X} r5={:08X} r6={:08X} stack: {}",        \
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, Backtrace(ctx, base));         \
    }                                                                                            \
    __imp__sub_##addr(ctx, base);                                                                \
  }
RE_PROBE_BT(82EB0EA8, "memcard load op ctor A")
RE_PROBE_BT(82EBAAB0, "memcard load op ctor B")
RE_PROBE_BT(8251C3C8, "replay deserialize -> request")
#undef RE_PROBE_BT

// Memcard UI state sub_8271D6B0(state, event, param): event 14 = load a
// replay; param is the replay save-content object (secondary base, primary at
// param-0x198). Logs who sends it and the object, to find the slot field.
extern "C" REX_FUNC(sub_8271D6B0) {
  if (REXCVAR_GET(skate3_harness_re_probe) && ctx.r4.u32 != 0 && ctx.r4.u32 != 1 && ctx.r4.u32 != 4) {
    const uint32_t obj = ctx.r5.u32 >= 0x198 ? ctx.r5.u32 - 0x198 : 0;
    REXLOG_INFO("re-probe: memcard state event={} param={:08X} state={:08X} stack: {}", ctx.r4.u32,
                ctx.r5.u32, ctx.r3.u32, Backtrace(ctx, base));
    if (ctx.r4.u32 == 14 && obj) {
      for (int i = 0; i < 0x220; i += 0x40) {
        REXLOG_INFO("re-probe:   content+{:03X}: {}", i, Words(base, obj + i, 16));
      }
    }
  }
  __imp__sub_8271D6B0(ctx, base);
}

// The memcard UI owner's event handler sub_8271A828(owner, ref<event>*):
// type 36 = memcard request {+12 memcard event id, +16 param}; event 14 with
// the replay save content loads REPLY_N (+0x1AC 'H' header / 'B' body).
extern "C" REX_FUNC(sub_8271A828) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    uint32_t ev = 0, type = 0, id = 0, param = 0;
    Read32(base, ctx.r4.u32, &ev);
    Read32(base, ev + 8, &type);
    Read32(base, ev + 12, &id);
    Read32(base, ev + 16, &param);
    if (type == 36) {
      REXLOG_INFO("re-probe: memcard request ev={:08X} id={} param={:08X} owner={:08X} stack: {}", ev,
                  id, param, ctx.r3.u32, Backtrace(ctx, base, 20));
    }
  }
  __imp__sub_8271A828(ctx, base);
}

// Game-mode event post sub_824FCAD8(gctx, ref<event>*) (queues on the world's
// event deque). Logs memcard requests (type 36) with the poster's backtrace
// and the request's param object.
extern "C" REX_FUNC(sub_824FCAD8) {
  if (REXCVAR_GET(skate3_harness_re_probe)) {
    uint32_t ev = 0, type = 0, id = 0, param = 0;
    Read32(base, ctx.r4.u32, &ev);
    Read32(base, ev + 8, &type);
    Read32(base, ev + 12, &id);
    Read32(base, ev + 16, &param);
    if (type == 36 || type == 27) {
      REXLOG_INFO("re-probe: post event type={} id={} param={:08X} ev={:08X} stack: {}", type, id,
                  param, ev, Backtrace(ctx, base, 20));
    }
  }
  __imp__sub_824FCAD8(ctx, base);
}

// Game save/memcard manager *(0x83084690) (vtable 0x82321AE0): every method,
// logged with a backtrace only while --skate3_harness_re_trace is on (set it
// live around one menu action; most slots are per-frame getters).
REXCVAR_DEFINE_BOOL(skate3_harness_re_trace, false, "Skate 3",
                    "Harness RE probe: log the save manager's method calls with backtraces")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#define RE_TRACE_SLOT(slot, addr)                                                                 \
  extern "C" REX_FUNC(sub_##addr) {                                                               \
    if (REXCVAR_GET(skate3_harness_re_trace)) {                                                   \
      REXLOG_INFO("re-trace: savemgr slot " #slot " sub_" #addr                                   \
                  " r3={:08X} r4={:08X} r5={:08X} r6={:08X} r4 words: {} stack: {}",              \
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, Words(base, ctx.r4.u32, 8),      \
                  Backtrace(ctx, base));                                                          \
    }                                                                                             \
    __imp__sub_##addr(ctx, base);                                                                 \
  }
RE_TRACE_SLOT(0, 82BF8610)
RE_TRACE_SLOT(3, 82BF51F8)
RE_TRACE_SLOT(4, 82BF5210)
RE_TRACE_SLOT(5, 82BF66A8)
RE_TRACE_SLOT(6, 82BF6710)
RE_TRACE_SLOT(7, 82BF8B18)
RE_TRACE_SLOT(9, 82BF6EC8)
RE_TRACE_SLOT(10, 82BF97D0)
RE_TRACE_SLOT(13, 82BF67F0)
RE_TRACE_SLOT(14, 82BF6858)
RE_TRACE_SLOT(15, 82BF95C8)
RE_TRACE_SLOT(16, 82BF6A38)
RE_TRACE_SLOT(17, 82BF6AF8)
RE_TRACE_SLOT(18, 82BF6CA8)
RE_TRACE_SLOT(19, 82BF9750)
RE_TRACE_SLOT(20, 82BF6E70)
RE_TRACE_SLOT(21, 82BF7190)
RE_TRACE_SLOT(22, 82BF7138)
RE_TRACE_SLOT(24, 82BF71B8)
RE_TRACE_SLOT(25, 82BF5858)
RE_TRACE_SLOT(26, 82BF5A60)
RE_TRACE_SLOT(27, 82BF5928)
RE_TRACE_SLOT(28, 82BF8690)
RE_TRACE_SLOT(29, 82BF8800)
RE_TRACE_SLOT(30, 82BF8880)
RE_TRACE_SLOT(31, 82BF7488)
RE_TRACE_SLOT(32, 82BF8BF8)
RE_TRACE_SLOT(33, 82BF50F0)
RE_TRACE_SLOT(34, 82BF5280)
RE_TRACE_SLOT(35, 82BF5288)
RE_TRACE_SLOT(36, 82BF5118)
RE_TRACE_SLOT(37, 82BF6E58)
RE_TRACE_SLOT(39, 82BF94A8)
RE_TRACE_SLOT(40, 82BF5188)
RE_TRACE_SLOT(41, 82BF9308)
RE_TRACE_SLOT(42, 82BF5D88)
RE_TRACE_SLOT(43, 82BF9668)
RE_TRACE_SLOT(44, 82BF5E30)
RE_TRACE_SLOT(45, 82BF8C18)
RE_TRACE_SLOT(46, 82BF8910)
RE_TRACE_SLOT(47, 82BF96E8)
RE_TRACE_SLOT(48, 82BF5290)
RE_TRACE_SLOT(49, 82BF6728)
RE_TRACE_SLOT(50, 82BF6D58)
RE_TRACE_SLOT(51, 82BF9828)
RE_TRACE_SLOT(52, 82BF5228)
RE_TRACE_SLOT(53, 82BF5268)
RE_TRACE_SLOT(54, 82BF56A8)
RE_TRACE_SLOT(55, 82BF8B88)
// Memcard flow state callback sub_82713938(event, flow): submits flow+48
// through slot 45 / 47.
RE_TRACE_SLOT(flow, 82713938)
#undef RE_TRACE_SLOT

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
