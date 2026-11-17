#include "skate3_demo_path.h"

#include "generated/skate3_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_demo_path, false, "Skate 3",
                    "Probe and automate the boot path to gameplay");
REXCVAR_DEFINE_BOOL(skate3_demo_path_probe, false, "Skate 3",
                    "Log Skate 3 boot/frontend states used by the demo path");
REXCVAR_DEFINE_BOOL(skate3_demo_path_signed_in, false, "Skate 3",
                    "Demo path: keep the real signed-in profile (and its save) instead of "
                    "forcing a signed-out boot");
REXCVAR_DEFINE_STRING(skate3_demo_path_gameplay_inputs, "", "Skate 3",
                      "Demo path: comma-separated pad inputs injected once after gameplay "
                      "settles (tokens: a b x y start back lb rb lt rt up down left right l3 "
                      "r3; a ':ms' suffix overrides the delay AFTER that input, e.g. rt:1500). "
                      "Map switch to PCU Library: "
                      "start,a,rt:1500,rt:1500,down,down,a,down,down,down,down,down,a,a");
REXCVAR_DEFINE_INT32(skate3_demo_path_input_settle_ms, 2500, "Skate 3",
                     "Demo path: wait this long after gameplay is reached before injecting "
                     "skate3_demo_path_gameplay_inputs")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(skate3_demo_path_input_delay_ms, 600, "Skate 3",
                     "Demo path: delay between injected gameplay inputs")
    .range(50, 10000);
REXCVAR_DEFINE_STRING(skate3_demo_path_boot_screen_inputs, "", "Skate 3",
                      "Demo path: extra per-screen boot inputs, 'id:token|token...' entries "
                      "separated by commas (FE screen ids as logged by 'FE stack'). While that "
                      "screen is on top of the FE stack before gameplay, the tokens are tapped "
                      "in turn instead of the dialog A auto-tap. Takes precedence over the "
                      "built-in rule (screen 15, the new-profile skater editor: b|a)");
REXCVAR_DEFINE_BOOL(skate3_intro_movie_skip, true, "Skate 3",
                    "Skippable intros: during the boot (before the title screen) any "
                    "button skips the Skate 3 logo and the intro movie; later frontend "
                    "movies skip on A or Start (the default keyboard bindings make that "
                    "Space and Enter). The language screen and the title already take "
                    "input (docs/re/boot.md)");

REXCVAR_DEFINE_BOOL(skate3_title_quick_start, true, "Skate 3",
                    "Title screen: any button shows the \"Press Start\" prompt at once "
                    "instead of after its 1.5 s delay, and A works like Start on it "
                    "(docs/re/boot.md)");

namespace skate3::native_scene {
// Fault-guarded guest read (defined in skate3_native_scene.cpp).
bool GuestTryLoadU32(uint8_t* base, uint32_t addr, uint32_t* out);
}  // namespace skate3::native_scene

namespace skate3::demo_path {
namespace {

// Guest memory base, captured by the boot-flow hooks for the boot-assist
// worker's FE stack reads.
std::atomic<uint8_t*> g_guest_base{nullptr};

constexpr uint32_t kFrontEndStatePressStart = 24;
constexpr uint32_t kFrontEndStateLanguageSelect = 47;

std::atomic<uint32_t> g_last_requested_state{0};
std::atomic<bool> g_seen_language_update{false};
std::atomic<uint32_t> g_last_language_select_event{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_last_press_start_event{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_automation_stage{0};
std::atomic<bool> g_skip_intro_movie{false};
std::atomic<bool> g_logged_intro_movie_skip{false};
std::atomic<bool> g_f10_poll_was_down{false};
// The buttons player one's pad gave the game at its last poll (after any
// harness rewrite: XInputGetState is the game's only pad path), so the skip
// sees exactly what the game sees, scripted input included.
std::atomic<uint32_t> g_guest_pad_buttons{0};
// Set once the boot flow reaches the title screen (ShowPressStartMode):
// from then on only A / Start skip (in-game frontend movies).
std::atomic<bool> g_title_reached{false};
// Fresh presses since the current intro screen began: every guest pad poll
// adds its rising edges (NoteGuestPad), a screen's start clears them
// (ClearPendingPresses), a skip check consumes them (SkipEdge).
std::atomic<uint32_t> g_pending_presses{0};
std::atomic<uint32_t> g_ui_prev_buttons{0xFFFF};
// After a press revealed the title's prompt: Start / A presses don't arm it
// until every button is up again, so that press can't also confirm.
std::atomic<bool> g_title_swallow{false};
// Milliseconds to add to APT's next time step (sub_82E663D0): fast-forwards
// the title's intro animation (background fade, logo, prompt) to its end.
std::atomic<uint32_t> g_apt_fast_forward_ms{0};

bool SkipEdge(const char* what, uint16_t mask, bool needs_intro_skip);
void ClearPendingPresses();
uint16_t ButtonsDown();


bool ProbeEnabled() {
  return rex::cvar::Query<bool>("skate3_demo_path") ||
         rex::cvar::Query<bool>("skate3_demo_path_probe");
}

bool AutomationEnabled() {
  return rex::cvar::Query<bool>("skate3_demo_path");
}

const char* KnownFrontEndStateName(uint32_t state_id) {
  switch (state_id) {
    case kFrontEndStatePressStart:
      return "press-start";
    case kFrontEndStateLanguageSelect:
      return "language-select";
    default:
      return "unknown";
  }
}

void LogBootFlowEventChange(std::atomic<uint32_t>& last_event, const char* name, uint32_t event,
                            uint32_t state_this) {
  if (!ProbeEnabled()) {
    return;
  }

  uint32_t previous = last_event.load(std::memory_order_relaxed);
  while (previous != event) {
    if (last_event.compare_exchange_weak(previous, event, std::memory_order_relaxed)) {
      REXLOG_INFO("Skate 3 demo path: {} event={} this=0x{:08X}", name, event, state_this);
      return;
    }
  }
}

void QueueLanguageAcceptIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }

  uint32_t expected = 0;
  if (!g_automation_stage.compare_exchange_strong(expected, 1, std::memory_order_relaxed)) {
    return;
  }

  rex::kernel::xam::QueueSyntheticInput(rex::input::X_INPUT_GAMEPAD_A, 8);
  REXLOG_INFO("Skate 3 demo path: queued language-select A pulse");
}

void EnableTitleStartAutoTapIfNeeded(const char* reason) {
  if (!AutomationEnabled()) {
    return;
  }

  uint32_t stage = g_automation_stage.load(std::memory_order_relaxed);
  while (stage < 2) {
    if (g_automation_stage.compare_exchange_weak(stage, 2, std::memory_order_relaxed)) {
      rex::kernel::xam::SetSyntheticAutoTap(rex::input::X_INPUT_GAMEPAD_START, true);
      REXLOG_INFO("Skate 3 demo path: enabled press-start auto-tap ({})", reason);
      return;
    }
  }
}

void EnableIntroMovieSkipIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }

  bool expected = false;
  if (g_skip_intro_movie.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
    REXLOG_INFO("Skate 3 demo path: enabled intro movie completion override");
  }
}

void PollF10Marker() {
#if defined(_WIN32)
  if (!ProbeEnabled()) {
    return;
  }

  const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  const bool was_down = g_f10_poll_was_down.exchange(f10_down, std::memory_order_relaxed);
  if (f10_down && !was_down) {
    REXLOG_WARN("Skate 3 demo path: polled F10 milestone marker");
  }
#endif
}

// The guest addresses hooked below are Title Update 3 (3.0.3.0) image
// addresses; the retail image has other code there (KI-33).
static_assert(SKATE3_HAS_TITLE_UPDATE, "hooks target Title Update 3 addresses");

extern "C" REX_FUNC(Skate3DemoPath_SetFrontEndStateHook) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t state_id = ctx.r4.u32;
  const uint32_t mode = ctx.r5.u32;
  const uint32_t caller_lr = ctx.lr;

  if (ProbeEnabled()) {
    g_last_requested_state.store(state_id, std::memory_order_relaxed);
    REXLOG_INFO(
        "Skate 3 demo path: FE SetState state={} ({}) mode={} manager=0x{:08X} lr=0x{:08X}",
        state_id, KnownFrontEndStateName(state_id), mode, manager, caller_lr);
  }

  sub_82D0AFA0(ctx, base);
}

// Probe, once at the first FE push: the FrontEndManager's state table
// (*(mgr+8): one object per FE state id, docs/re/boot.md), each object's
// vtable and its first 24 slots, so each state's functions can be read.
void DumpFrontEndStateTable(uint8_t* base, uint32_t mgr) {
  static std::atomic<bool> s_dumped{false};
  if (s_dumped.exchange(true)) {
    return;
  }
  uint32_t table = 0;
  if (!skate3::native_scene::GuestTryLoadU32(base, mgr + 8, &table) || table == 0) {
    REXLOG_INFO("Skate 3 demo path: FE state table unreadable (mgr=0x{:08X})", mgr);
    return;
  }
  REXLOG_INFO("Skate 3 demo path: FE state table mgr=0x{:08X} table=0x{:08X}", mgr, table);
  for (uint32_t id = 0; id < 160; ++id) {
    uint32_t obj = 0, vt = 0;
    if (!skate3::native_scene::GuestTryLoadU32(base, table + 4 * id, &obj)) {
      break;
    }
    if (obj == 0 || !skate3::native_scene::GuestTryLoadU32(base, obj, &vt)) {
      continue;
    }
    std::string slots;
    for (uint32_t k = 0; k < 24; ++k) {
      uint32_t fn = 0;
      if (!skate3::native_scene::GuestTryLoadU32(base, vt + 4 * k, &fn)) {
        break;
      }
      slots += fmt::format(" {:08X}", fn);
    }
    REXLOG_INFO("Skate 3 demo path: FE state {} obj=0x{:08X} vt=0x{:08X}:{}", id, obj, vt,
                slots);
  }
}

// Probe: the boot flow's state handlers (Sk8::Flow::StateBootFlow, a chain
// of (this, event) handlers: 0 enter, 1 update, 3 exit; docs/re/boot.md).
// Logs each handler's enter/exit and, on every update, changes of the boot
// game module's load phase (module+68; the module is *(*(0x83083BCC)+8)
// when its vtable is 0x8230C748) and of the FE movie player's state
// (*(*(fe+264)+12), fe = *(0x830CFE14); 4 = done).
constexpr uint32_t kBootModuleHolder = 0x83083BCC;
constexpr uint32_t kBootModuleVtable = 0x8230C748;
constexpr uint32_t kFrontEndPtr = 0x830CFE14;

void BootHandlerProbe(const char* name, uint8_t* base, uint32_t event) {
  if (!ProbeEnabled()) {
    return;
  }
  if (event != 1) {
    REXLOG_INFO("Skate 3 boot: {} event={}", name, event);
  }
  static std::atomic<int64_t> s_phase{-2}, s_movie{-2}, s_failed{-2};
  uint32_t holder = 0, module = 0, vt = 0, phase = 0, failed = 0;
  int64_t p = -1, f = -1;
  if (skate3::native_scene::GuestTryLoadU32(base, kBootModuleHolder, &holder) && holder &&
      skate3::native_scene::GuestTryLoadU32(base, holder + 8, &module) && module &&
      skate3::native_scene::GuestTryLoadU32(base, module, &vt) && vt == kBootModuleVtable &&
      skate3::native_scene::GuestTryLoadU32(base, module + 68, &phase) &&
      skate3::native_scene::GuestTryLoadU32(base, module + 32, &failed)) {
    p = phase;
    f = failed >> 24;  // the byte at +32
  }
  uint32_t fe = 0, player = 0, movie = 0;
  int64_t m = -1;
  if (skate3::native_scene::GuestTryLoadU32(base, kFrontEndPtr, &fe) && fe &&
      skate3::native_scene::GuestTryLoadU32(base, fe + 264, &player) && player &&
      skate3::native_scene::GuestTryLoadU32(base, player + 12, &movie)) {
    m = movie;
  }
  const int64_t op = s_phase.exchange(p), om = s_movie.exchange(m), of = s_failed.exchange(f);
  if (op != p || om != m || of != f) {
    REXLOG_INFO("Skate 3 boot: [{}] module phase {} -> {} (+32 {}), movie state {} -> {}", name,
                op, p, f, om, m);
  }
}

// Also: each screen's enter (event 0) clears the presses collected so far,
// so a press made on the previous screen can't skip this one.
#define SKATE3_BOOT_HANDLER_PROBE(addr, label)       \
  extern "C" REX_FUNC(sub_##addr) {                  \
    if (ctx.r4.u32 == 0) {                           \
      ClearPendingPresses();                         \
    }                                                \
    BootHandlerProbe(label, base, ctx.r4.u32);       \
    __imp__sub_##addr(ctx, base);                    \
  }
// Probe: the title screen (FE state 22). Its ReceiveInput(this, ?, code)
// (vtable +0x28) with the FE input code, and in its OnUpdate(this, dt) the
// prompt flags: +72 ready (set once the global ms counter 0x8308355C passes
// 1500), +80 armed (input 0), +60 busy, +68 mode.
constexpr uint32_t kTitlePromptDelayMs = 0x8308355C;

void TitleProbe(const char* what, uint8_t* base, uint32_t self, uint32_t a, uint32_t b) {
  if (!ProbeEnabled()) {
    return;
  }
  uint32_t delay = 0, w60 = 0, w68 = 0, w72 = 0, w80 = 0;
  skate3::native_scene::GuestTryLoadU32(base, kTitlePromptDelayMs, &delay);
  skate3::native_scene::GuestTryLoadU32(base, self + 60, &w60);
  skate3::native_scene::GuestTryLoadU32(base, self + 68, &w68);
  skate3::native_scene::GuestTryLoadU32(base, self + 72, &w72);
  skate3::native_scene::GuestTryLoadU32(base, self + 80, &w80);
  const uint32_t ready = w72 >> 24, armed = w80 >> 24, busy = w60 >> 24;
  static std::atomic<uint32_t> s_last{0xFFFFFFFF};
  const uint32_t key = ready | (armed << 1) | (busy << 2) | ((delay > 1500) << 3) | (w68 << 4);
  const bool input = what[0] == 'i';
  if (input || s_last.exchange(key) != key) {
    REXLOG_INFO("Skate 3 title: {} r4={} r5={} delay={} ready={} armed={} busy={} mode={}", what,
                a, b, delay, ready, armed, busy, w68);
  }
}

// The title's FE input codes (measured): a button's press is its code, its
// release the code + 44. Start 0 / 44, Back 1, A 2 / 46, B 3, Y 4, X 5,
// LB 9. ReceiveInput sub_82609238 arms on 0 (+80) and confirms on 44 when
// the prompt is ready (+72), armed and not busy (+60).
constexpr uint32_t kTitleStartPress = 0, kTitleStartRelease = 44;
constexpr uint32_t kTitleAPress = 2, kTitleARelease = 46;
constexpr uint32_t kTitleReady = 72, kTitleArmed = 80, kTitleMode = 68;

extern "C" REX_FUNC(sub_82609238) {
  TitleProbe("input", base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (REXCVAR_GET(skate3_title_quick_start) && REX_LOAD_U32(ctx.r3.u32 + kTitleMode) == 0) {
    // A works like Start on the title.
    if (ctx.r5.u32 == kTitleAPress) {
      ctx.r5.u64 = kTitleStartPress;
    } else if (ctx.r5.u32 == kTitleARelease) {
      ctx.r5.u64 = kTitleStartRelease;
    }
    // The press that revealed the prompt doesn't arm it.
    if (ctx.r5.u32 == kTitleStartPress && g_title_swallow.load(std::memory_order_relaxed)) {
      REX_STORE_U32(ctx.r3.u32 + 76, 0);  // the idle timer, as any input resets it
      REX_STORE_U32(ctx.r3.u32 + 64, 0);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_82609238(ctx, base);
}

// Probe: APT's per-movie time step sub_82E663D0(elapsed_ms) on the current
// movie *(*(0x83085768)+24): logs each distinct movie with its elapsed ms
// and frame duration while the title is up, and the title state's movie
// wrapper words (state+56), to tie the two together.
std::atomic<uint32_t> g_probe_title_state{0};

extern "C" REX_FUNC(sub_82E663D0) {
  if (const uint32_t ff = g_apt_fast_forward_ms.exchange(0, std::memory_order_relaxed)) {
    REXLOG_INFO("Skate 3 title: APT fast-forward {} ms", ff);
    ctx.r3.u64 = ctx.r3.u32 + ff;
  }
  if (ProbeEnabled() && g_probe_title_state.load(std::memory_order_relaxed)) {
    static uint32_t s_seen[16] = {};
    static int s_count = 0;
    uint32_t ctxp = 0, movie = 0;
    skate3::native_scene::GuestTryLoadU32(base, 0x83085768, &ctxp);
    if (ctxp) skate3::native_scene::GuestTryLoadU32(base, ctxp + 24, &movie);
    bool seen = false;
    for (int i = 0; i < s_count; ++i) seen |= s_seen[i] == movie;
    if (!seen && s_count < 16) {
      s_seen[s_count++] = movie;
      uint32_t title = g_probe_title_state.load(), wrapper = 0;
      skate3::native_scene::GuestTryLoadU32(base, title + 56, &wrapper);
      std::string words;
      for (uint32_t k = 0; k < 16 && wrapper; ++k) {
        uint32_t v = 0;
        skate3::native_scene::GuestTryLoadU32(base, wrapper + 4 * k, &v);
        words += fmt::format(" {:08X}", v);
      }
      REXLOG_INFO("Skate 3 title: apt step movie=0x{:08X} ctx=0x{:08X} elapsed={} | title "
                  "wrapper 0x{:08X}:{}",
                  movie, ctxp, ctx.r3.u32, wrapper, words);
    }
  }
  __imp__sub_82E663D0(ctx, base);
}

// Before the title's OnUpdate(this, dt): while the prompt is still waiting
// for its delay (the ms counter kTitlePromptDelayMs <= 1500, +72 clear), a
// fresh press of any button ends the delay; the update then shows it.
extern "C" REX_FUNC(sub_82609050) {
  TitleProbe("update", base, ctx.r3.u32, ctx.r4.u32, 0);
  g_probe_title_state.store(ctx.r3.u32, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_title_quick_start)) {
    const uint32_t self = ctx.r3.u32;
    if (g_title_swallow.load(std::memory_order_relaxed) && ButtonsDown() == 0) {
      g_title_swallow.store(false, std::memory_order_relaxed);
    }
    if (REX_LOAD_U8(self + kTitleReady) == 0 && REX_LOAD_U32(kTitlePromptDelayMs) <= 1500 &&
        SkipEdge("title prompt delay", 0xF3FF, false)) {
      REX_STORE_U32(kTitlePromptDelayMs, 1501);
      // The prompt's own animation (about 1.8 s from the title's start)
      // runs on APT's clock, not this delay: play it to its end now.
      g_apt_fast_forward_ms.store(3000, std::memory_order_relaxed);
      REX_STORE_U8(self + kTitleArmed, 0);  // a Start press already dispatched won't confirm
      g_title_swallow.store(true, std::memory_order_relaxed);
    }
  }
  __imp__sub_82609050(ctx, base);
}

// Probe: every flow-state transition (sub_826DFB30(this, next_handler, ...),
// the HSM's "go to"), with the caller, so the state the game sits in during
// a wait can be found.
extern "C" REX_FUNC(sub_826DFB30) {
  if (ProbeEnabled()) {
    REXLOG_INFO("Skate 3 flow: this=0x{:08X} -> handler 0x{:08X} (r5={} r6={}) lr=0x{:08X}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_826DFB30(ctx, base);
}

SKATE3_BOOT_HANDLER_PROBE(826FDE48, "logo (FE 5)")
SKATE3_BOOT_HANDLER_PROBE(826FDEF0, "movie (FE 24)")
SKATE3_BOOT_HANDLER_PROBE(826FE058, "wait for load")
SKATE3_BOOT_HANDLER_PROBE(826FE138, "title (FE 22)")

// Probe: FE screen pushes (screen id in r4), logged with the caller so
// unknown boot screens can be traced to the code that opens them.
#define SKATE3_FE_PUSH_PROBE(addr)                                                    \
  extern "C" REX_FUNC(sub_##addr) {                                                   \
    if (ProbeEnabled()) {                                                             \
      REXLOG_INFO("Skate 3 demo path: FE push via sub_" #addr " id={} lr=0x{:08X}",    \
                  ctx.r4.u32, static_cast<uint32_t>(ctx.lr));                         \
      DumpFrontEndStateTable(base, ctx.r3.u32);                                       \
    }                                                                                 \
    __imp__sub_##addr(ctx, base);                                                     \
  }
SKATE3_FE_PUSH_PROBE(82D0ABE0)
SKATE3_FE_PUSH_PROBE(82D0AB30)
SKATE3_FE_PUSH_PROBE(82D0AEF0)

extern "C" REX_FUNC(Skate3DemoPath_LanguageSelectStateHook) {
  g_guest_base.store(base, std::memory_order_relaxed);
  PollF10Marker();
  LogBootFlowEventChange(g_last_language_select_event, "BootFlow LanguageSelectState",
                         ctx.r4.u32, ctx.r3.u32);
  if (ctx.r4.u32 == 4) {
    EnableIntroMovieSkipIfNeeded();
  }

  sub_826FDD70(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_ShowPressStartModeHook) {
  g_guest_base.store(base, std::memory_order_relaxed);
  PollF10Marker();
  LogBootFlowEventChange(g_last_press_start_event, "BootFlow ShowPressStartMode", ctx.r4.u32,
                         ctx.r3.u32);
  g_title_reached.store(true, std::memory_order_relaxed);
  if (ctx.r4.u32 == 1) {
    g_skip_intro_movie.store(false, std::memory_order_relaxed);
    EnableTitleStartAutoTapIfNeeded("press-start state");
  }

  sub_826FE1D8(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_LanguageSelectUpdateHook) {
  PollF10Marker();
  if (ProbeEnabled()) {
    bool expected = false;
    if (g_seen_language_update.compare_exchange_strong(expected, true,
                                                       std::memory_order_relaxed)) {
      REXLOG_INFO(
          "Skate 3 demo path: FrontEndState_LanguageSelect::OnUpdate first seen "
          "dt={} this=0x{:08X}",
          ctx.r4.u32, ctx.r3.u32);
      QueueLanguageAcceptIfNeeded();
    }
  }

  sub_82639400(ctx, base);
}

// Scripted post-gameplay input sequence (e.g. driving the pause-menu map
// switch that reproduces the sticky-slow-GPU state). Runs on its own thread
// with real-time pacing: waits for the gameplay presence context, lets the
// scene settle, then injects one pad input at a time.
struct GameplayInputToken {
  const char* name;
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
};

constexpr GameplayInputToken kGameplayInputTokens[] = {
    {"a", rex::input::X_INPUT_GAMEPAD_A, 0, 0},
    {"b", rex::input::X_INPUT_GAMEPAD_B, 0, 0},
    {"x", rex::input::X_INPUT_GAMEPAD_X, 0, 0},
    {"y", rex::input::X_INPUT_GAMEPAD_Y, 0, 0},
    {"start", rex::input::X_INPUT_GAMEPAD_START, 0, 0},
    {"back", rex::input::X_INPUT_GAMEPAD_BACK, 0, 0},
    {"lb", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, 0},
    {"rb", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0},
    {"lt", 0, 255, 0},
    {"rt", 0, 0, 255},
    {"up", rex::input::X_INPUT_GAMEPAD_DPAD_UP, 0, 0},
    {"down", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN, 0, 0},
    {"left", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT, 0, 0},
    {"right", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT, 0, 0},
    {"l3", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB, 0, 0},
    {"r3", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB, 0, 0},
};

const GameplayInputToken* FindGameplayInputToken(const std::string& name) {
  for (const GameplayInputToken& token : kGameplayInputTokens) {
    if (name == token.name) {
      return &token;
    }
  }
  return nullptr;
}

// Set once during app setup (before the guest boots), read from the guest
// thread by the movie-skip poll.
std::function<rex::input::InputSystem*()> g_ui_input_provider;

// User-facing movie skip: a fresh A / Start press while a frontend movie is
// updating completes it (the generated FEMoviePlayer::Update patch consults
// ShouldForceIntroMovieComplete every movie tick, demo path or not). The
// merged raw pad state is polled so the default keyboard bindings (Space = A,
// Enter = Start) work identically on every backend; rising-edge only, so a
// button held since before the movie doesn't blow straight through it.
// One rising-edge detector shared by every skippable intro screen (the logo
// update and the movie tick run on consecutive frames), so a button held
// from one screen into the next doesn't skip both. Starts "down": a button
// held since before the first intro screen does nothing.
uint16_t UiButtons() {
  if (g_ui_input_provider) {
    if (rex::input::InputSystem* input = g_ui_input_provider()) {
      rex::input::X_INPUT_GAMEPAD pad;
      if (input->GetUiGamepadState(&pad)) {
        return pad.buttons;
      }
    }
  }
  return 0;
}

uint16_t ButtonsDown() {
  return static_cast<uint16_t>(g_guest_pad_buttons.load(std::memory_order_relaxed) |
                               UiButtons());
}

void ClearPendingPresses() {
  g_pending_presses.store(0, std::memory_order_relaxed);
  g_ui_prev_buttons.store(UiButtons(), std::memory_order_relaxed);
}

// True when a button in `mask` was freshly pressed since the last check or
// screen start (guest pad edges collected on every poll, plus host UI pad
// edges). Consumes the pending presses. A button held since before the
// screen began never counts.
bool SkipEdge(const char* what, uint16_t mask, bool needs_intro_skip) {
  if (needs_intro_skip && !REXCVAR_GET(skate3_intro_movie_skip)) {
    return false;
  }
  const uint16_t ui = UiButtons();
  const uint16_t ui_prev =
      static_cast<uint16_t>(g_ui_prev_buttons.exchange(ui, std::memory_order_relaxed));
  const uint16_t presses = static_cast<uint16_t>(
      g_pending_presses.exchange(0, std::memory_order_relaxed) | (ui & ~ui_prev));
  if ((presses & mask) != 0) {
    REXLOG_INFO("Skate 3: {} skipped by user input (buttons 0x{:04X})", what, presses);
    return true;
  }
  return false;
}

// Any digital button during the boot (not Guide); A / Start afterwards.
uint16_t IntroSkipMask() {
  return g_title_reached.load(std::memory_order_relaxed)
             ? static_cast<uint16_t>(rex::input::X_INPUT_GAMEPAD_A |
                                     rex::input::X_INPUT_GAMEPAD_START)
             : static_cast<uint16_t>(0xF3FF);
}

bool UserRequestedMovieSkip() { return SkipEdge("frontend movie", IntroSkipMask(), true); }

std::atomic<bool> g_input_worker_quit{false};
std::thread g_input_worker;

void JoinGameplayInputWorker() {
  g_input_worker_quit.store(true, std::memory_order_relaxed);
  if (g_input_worker.joinable()) {
    g_input_worker.join();
  }
}

bool InterruptibleSleepMs(int64_t total_ms) {
  constexpr int64_t kSliceMs = 100;
  while (total_ms > 0) {
    if (g_input_worker_quit.load(std::memory_order_relaxed)) {
      return false;
    }
    int64_t slice = total_ms < kSliceMs ? total_ms : kSliceMs;
    std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    total_ms -= slice;
  }
  return !g_input_worker_quit.load(std::memory_order_relaxed);
}

void StartGameplayInputWorkerIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }
  const std::string sequence = REXCVAR_GET(skate3_demo_path_gameplay_inputs);
  if (sequence.empty()) {
    return;
  }

  const int32_t settle_ms = REXCVAR_GET(skate3_demo_path_input_settle_ms);
  const int32_t default_delay_ms = REXCVAR_GET(skate3_demo_path_input_delay_ms);

  struct SequenceEntry {
    const GameplayInputToken* token;
    int32_t delay_after_ms;
  };
  std::vector<SequenceEntry> tokens;
  std::stringstream stream(sequence);
  std::string raw_token;
  while (std::getline(stream, raw_token, ',')) {
    // Allow spaces around tokens.
    size_t begin = raw_token.find_first_not_of(" \t");
    size_t end = raw_token.find_last_not_of(" \t");
    if (begin == std::string::npos) {
      continue;
    }
    std::string name = raw_token.substr(begin, end - begin + 1);
    // Optional ':ms' suffix overriding the delay after this input (slower UI
    // transitions - the map-menu RT tabs - need more time to register).
    int32_t delay_after_ms = default_delay_ms;
    if (size_t colon = name.find(':'); colon != std::string::npos) {
      delay_after_ms = std::atoi(name.c_str() + colon + 1);
      if (delay_after_ms <= 0) {
        REXLOG_WARN("Skate 3 demo path: bad delay in gameplay input token '{}' - sequence "
                    "disabled",
                    name);
        return;
      }
      name.resize(colon);
    }
    const GameplayInputToken* token = FindGameplayInputToken(name);
    if (!token) {
      REXLOG_WARN("Skate 3 demo path: unknown gameplay input token '{}' - sequence disabled",
                  name);
      return;
    }
    tokens.push_back({token, delay_after_ms});
  }
  if (tokens.empty()) {
    return;
  }
  g_input_worker = std::thread([tokens, settle_ms] {
    // Wait for the gameplay presence context (0x8001 == 1).
    while (rex::kernel::guest_presence::GameplayContextValue() != 1) {
      if (!InterruptibleSleepMs(100)) {
        return;
      }
    }
    REXLOG_INFO("Skate 3 demo path: gameplay reached; injecting {} inputs after {} ms settle",
                tokens.size(), settle_ms);
    if (!InterruptibleSleepMs(settle_ms)) {
      return;
    }
    for (size_t i = 0; i < tokens.size(); ++i) {
      const GameplayInputToken* token = tokens[i].token;
      // ~8 polls at the game's 60 Hz input tick = a ~130 ms press. Triggers
      // are analog and debounced more heavily by the game's menus - hold them
      // for a human-tap-length ~270 ms.
      const bool is_trigger = token->left_trigger != 0 || token->right_trigger != 0;
      rex::kernel::xam::QueueSyntheticInput(token->buttons, token->left_trigger,
                                            token->right_trigger, is_trigger ? 16 : 8);
      REXLOG_INFO("Skate 3 demo path: injected gameplay input {}/{} '{}' (delay {} ms)", i + 1,
                  tokens.size(), token->name, tokens[i].delay_after_ms);
      if (!InterruptibleSleepMs(tokens[i].delay_after_ms)) {
        return;
      }
    }
    REXLOG_INFO("Skate 3 demo path: gameplay input sequence complete");
  });
  std::atexit(JoinGameplayInputWorker);
}

// ---- Boot assist: per-screen inputs between press-start and gameplay ------
//
// Between the press-start screen and gameplay the kernel's XGI handler keeps
// a dialog A auto-tap running (every 0x8001 = 0 presence update re-arms it),
// which accepts the boot dialogs of an existing profile. Screens that A alone
// can't leave get their own inputs here: a worker polls the FrontEndManager
// push-state stack and, while a screen with a rule is on top, turns the A
// auto-tap off and taps the rule's buttons in turn.

constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;  // TU3 image
constexpr uint32_t kFeStackBeginOffset = 0x210;
constexpr uint32_t kFeStackEndOffset = 0x214;
constexpr uint32_t kFeStackRecordSize = 20;  // screen id at +0
constexpr size_t kFeStackMaxDepth = 16;

struct BootScreenRule {
  uint32_t screen_id;
  std::vector<uint16_t> buttons;
  // Tried after kPrimaryRounds passes through `buttons` found nothing.
  std::vector<uint16_t> fallback;
};

const char* ButtonName(uint16_t buttons) {
  for (const GameplayInputToken& token : kGameplayInputTokens) {
    if (token.buttons == buttons && token.left_trigger == 0 && token.right_trigger == 0) {
      return token.name;
    }
  }
  return "?";
}

// Reads the FE push-state stack (bottom first). False if unreadable.
bool ReadFrontEndStack(uint8_t* base, std::vector<uint32_t>* ids) {
  ids->clear();
  uint32_t mgr = 0, beg = 0, end = 0;
  if (!skate3::native_scene::GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) || mgr == 0 ||
      !skate3::native_scene::GuestTryLoadU32(base, mgr + kFeStackBeginOffset, &beg) ||
      !skate3::native_scene::GuestTryLoadU32(base, mgr + kFeStackEndOffset, &end) ||
      beg > end || end - beg > kFeStackRecordSize * kFeStackMaxDepth) {
    return false;
  }
  for (uint32_t addr = beg; addr + kFeStackRecordSize <= end; addr += kFeStackRecordSize) {
    uint32_t id = 0;
    if (!skate3::native_scene::GuestTryLoadU32(base, addr, &id)) {
      return false;
    }
    ids->push_back(id);
  }
  return true;
}

std::string FormatIds(const std::vector<uint32_t>& ids) {
  std::string out;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) {
      out += ',';
    }
    out += std::to_string(ids[i]);
  }
  return out;
}

// Parses "id:token|token,id:token". Returns false (and logs) on a bad entry.
bool ParseBootScreenRules(const std::string& spec, std::vector<BootScreenRule>* rules) {
  std::stringstream stream(spec);
  std::string entry;
  while (std::getline(stream, entry, ',')) {
    size_t begin = entry.find_first_not_of(" \t");
    if (begin == std::string::npos) {
      continue;
    }
    entry = entry.substr(begin, entry.find_last_not_of(" \t") - begin + 1);
    const size_t colon = entry.find(':');
    if (colon == std::string::npos || colon == 0) {
      REXLOG_WARN("Skate 3 demo path: bad boot screen rule '{}'", entry);
      return false;
    }
    BootScreenRule rule{static_cast<uint32_t>(std::strtoul(entry.c_str(), nullptr, 10)), {}};
    std::stringstream tokens(entry.substr(colon + 1));
    std::string name;
    while (std::getline(tokens, name, '|')) {
      const GameplayInputToken* token = FindGameplayInputToken(name);
      if (!token || token->buttons == 0) {
        REXLOG_WARN("Skate 3 demo path: bad token '{}' in boot screen rule '{}'", name, entry);
        return false;
      }
      rule.buttons.push_back(token->buttons);
    }
    if (rule.buttons.empty()) {
      REXLOG_WARN("Skate 3 demo path: empty boot screen rule '{}'", entry);
      return false;
    }
    rules->push_back(std::move(rule));
  }
  return true;
}

std::atomic<bool> g_boot_assist_quit{false};
std::thread g_boot_assist;

void JoinBootAssist() {
  g_boot_assist_quit.store(true, std::memory_order_relaxed);
  if (g_boot_assist.joinable()) {
    g_boot_assist.join();
  }
}

void StartBootAssistIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }
  std::vector<BootScreenRule> rules;
  if (!ParseBootScreenRules(REXCVAR_GET(skate3_demo_path_boot_screen_inputs), &rules)) {
    rules.clear();
  }
  // Built-in rule (after any user rules, which win): a fresh profile's
  // startup create-a-skater editor (FE screen 15) ignores the A auto-tap
  // and Start; B asks to leave it and A accepts, which ends the boot flow.
  // That worked on one PC and not another (screen stayed put for 96 taps),
  // so the taps escalate: each pass holds the button longer (8, 24, 60
  // input polls), and after 4 passes other buttons are tried as well.
  // (The "welcome new team" and skater name prompts, screen 46, are keyboard
  // dialogs the SDK accepts by itself in demo-path mode.)
  rules.push_back({15,
                   {rex::input::X_INPUT_GAMEPAD_B, rex::input::X_INPUT_GAMEPAD_A},
                   {rex::input::X_INPUT_GAMEPAD_START, rex::input::X_INPUT_GAMEPAD_A,
                    rex::input::X_INPUT_GAMEPAD_Y, rex::input::X_INPUT_GAMEPAD_A,
                    rex::input::X_INPUT_GAMEPAD_B, rex::input::X_INPUT_GAMEPAD_A,
                    rex::input::X_INPUT_GAMEPAD_BACK, rex::input::X_INPUT_GAMEPAD_A}});
  g_boot_assist = std::thread([rules = std::move(rules)] {
    constexpr auto kPoll = std::chrono::milliseconds(50);
    // A confirm tap only lands if it follows the opening tap closely (1500 ms
    // apart never worked, 1000 ms or less did), and how soon the dialog is
    // up differs per machine, so the gap after the first tap of each pair
    // cycles through several values; pairs are 2 s apart.
    constexpr int64_t kPairGapsMs[] = {700, 400, 1000, 250, 1300, 550};
    constexpr int64_t kTapIntervalMs = 2000;
    constexpr uint32_t kHoldPolls[] = {8, 24, 60};
    constexpr uint32_t kPrimaryRounds = 4;
    constexpr int64_t kStatusIntervalMs = 15000;
    std::vector<uint32_t> ids;
    std::vector<uint32_t> last_ids;
    bool have_last = false;
    const BootScreenRule* active = nullptr;
    size_t next_button = 0;
    uint32_t round = 0;
    uint32_t taps = 0;
    uint16_t last_button = 0;
    uint32_t last_hold = 0;
    auto entered = std::chrono::steady_clock::now();
    auto next_status = entered;
    auto next_tap = std::chrono::steady_clock::now();
    while (!g_boot_assist_quit.load(std::memory_order_relaxed)) {
      if (rex::kernel::guest_presence::GameplayContextValue() == 1) {
        return;  // the XGI handler ends all boot automation at gameplay
      }
      uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
      if (base != nullptr && ReadFrontEndStack(base, &ids)) {
        if (!have_last || ids != last_ids) {
          REXLOG_INFO("Skate 3 demo path: FE stack [{}]", FormatIds(ids));
          last_ids = ids;
          have_last = true;
        }
        const BootScreenRule* rule = nullptr;
        if (!ids.empty()) {
          for (const BootScreenRule& candidate : rules) {
            if (candidate.screen_id == ids.back()) {
              rule = &candidate;
              break;
            }
          }
        }
        const auto now = std::chrono::steady_clock::now();
        if (rule != active) {
          if (active) {
            REXLOG_INFO(
                "Skate 3 demo path: boot screen {} left after {} taps in {} ms (last tap {} held "
                "{} polls, pass {}); FE stack now [{}]",
                active->screen_id, taps,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - entered).count(),
                ButtonName(last_button), last_hold, round, FormatIds(ids));
          }
          if (rule) {
            REXLOG_INFO("Skate 3 demo path: boot screen {} has its own inputs", rule->screen_id);
            next_button = 0;
            round = 0;
            taps = 0;
            entered = now;
            next_status = now + std::chrono::milliseconds(kStatusIntervalMs);
            next_tap = now + std::chrono::milliseconds(kTapIntervalMs);
          } else if (rex::kernel::guest_presence::GameplayContextValue() == 0) {
            rex::kernel::xam::SetSyntheticAutoTap(rex::input::X_INPUT_GAMEPAD_A, true);
          }
          active = rule;
        }
        if (active) {
          // The XGI handler re-arms the A auto-tap on every presence update.
          rex::kernel::xam::SetSyntheticAutoTap(rex::input::X_INPUT_GAMEPAD_A, false);
          if (now >= next_tap) {
            const std::vector<uint16_t>& seq =
                (round >= kPrimaryRounds && !active->fallback.empty()) ? active->fallback
                                                                       : active->buttons;
            if (next_button >= seq.size()) {
              next_button = 0;
            }
            const uint16_t buttons = seq[next_button];
            const uint32_t hold = kHoldPolls[round % std::size(kHoldPolls)];
            const bool first_of_pair = (next_button % 2) == 0;
            if (++next_button >= seq.size()) {
              next_button = 0;
              ++round;
            }
            rex::kernel::xam::QueueSyntheticInput(buttons, hold);
            ++taps;
            last_button = buttons;
            last_hold = hold;
            const int64_t gap_ms =
                first_of_pair ? kPairGapsMs[taps / 2 % std::size(kPairGapsMs)] : kTapIntervalMs;
            REXLOG_INFO(
                "Skate 3 demo path: boot screen {} tap {} (0x{:04X}) hold {} polls, pass {}, next "
                "in {} ms",
                active->screen_id, ButtonName(buttons), buttons, hold, round, gap_ms);
            next_tap = now + std::chrono::milliseconds(gap_ms);
          }
          if (now >= next_status) {
            REXLOG_INFO(
                "Skate 3 demo path: still on boot screen {} after {} taps ({} ms); presence "
                "context 0x8001={}; FE stack [{}]",
                active->screen_id, taps,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - entered).count(),
                rex::kernel::guest_presence::GameplayContextValue(), FormatIds(ids));
            next_status = now + std::chrono::milliseconds(kStatusIntervalMs);
          }
        }
      }
      std::this_thread::sleep_for(kPoll);
    }
  });
  std::atexit(JoinBootAssist);
}

}  // namespace

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher) {
  if (!dispatcher || !ProbeEnabled()) {
    return;
  }

  dispatcher->SetFunction(0x82D0AFA0, &Skate3DemoPath_SetFrontEndStateHook);
  dispatcher->SetFunction(0x826FDD70, &Skate3DemoPath_LanguageSelectStateHook);
  dispatcher->SetFunction(0x826FE1D8, &Skate3DemoPath_ShowPressStartModeHook);
  dispatcher->SetFunction(0x82639400, &Skate3DemoPath_LanguageSelectUpdateHook);
  REXLOG_INFO("Skate 3 demo path: frontend probe hooks installed");
  StartGameplayInputWorkerIfNeeded();
  StartBootAssistIfNeeded();
}

rex::input::InputSystem* GetUiInputSystem() {
  return g_ui_input_provider ? g_ui_input_provider() : nullptr;
}

void SetUiInputProvider(std::function<rex::input::InputSystem*()> provider) {
  g_ui_input_provider = std::move(provider);
}

void NoteGuestPad(uint8_t* base, uint32_t user, uint32_t state_addr, uint32_t result) {
  if ((user & 0xFF) != 0 || result != 0 || state_addr == 0) {
    return;
  }
  uint32_t word = 0;  // X_INPUT_STATE: packet (u32), then buttons (u16)
  if (skate3::native_scene::GuestTryLoadU32(base, state_addr + 4, &word)) {
    const uint32_t now = word >> 16;
    const uint32_t before = g_guest_pad_buttons.exchange(now, std::memory_order_relaxed);
    if (const uint32_t pressed = now & ~before) {
      g_pending_presses.fetch_or(pressed, std::memory_order_relaxed);
    }
  }
}

bool IntroSkipPressed() { return SkipEdge("intro screen", IntroSkipMask(), true); }

bool ShouldForceIntroMovieComplete() {
  if (AutomationEnabled() && g_skip_intro_movie.load(std::memory_order_relaxed)) {
    bool expected = false;
    if (g_logged_intro_movie_skip.compare_exchange_strong(expected, true,
                                                          std::memory_order_relaxed)) {
      REXLOG_INFO("Skate 3 demo path: forcing frontend intro movie complete");
    }
    return true;
  }
  return UserRequestedMovieSkip();
}

}  // namespace skate3::demo_path
