// Frontend state methods, native (RE-9; docs/re/ui.md).
//
// Written over the FE types in skate3_fe_types.h: FrontEndState (the base
// every FE state derives from) and the states we've mapped. Each method is
// what the state does, in our words; registers appear only at the call
// boundary (arguments in, r3 out, guest calls we haven't replaced).
// The boot screens' methods are in skate3_native_boot.cpp.
//
// skate3_native_fe_replace (default on): one Body family per method, so the
// replace counters show which ones a run reaches.

#include <cstddef>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "generated/skate3_init.h"
#include "native/skate3_fe_types.h"
#include "native/skate3_native_fe.h"
#include "native/skate3_native_guest_frame.h"
#include "native/skate3_native_replace.h"

REXCVAR_DEFINE_BOOL(skate3_native_fe_replace, true, "Skate 3",
                    "RE-9: FrontEndState methods as native code (OpenScreen, CloseScreen, "
                    "the enter/leave methods of several states, the Options screen's "
                    "IsItemEnabled and highlight); off runs the recompiled code")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using namespace skate3::fe;
using skate3::native_guest::CallMethod;
using skate3::native_guest::GuestFrame;

uint32_t CallerLr(const PPCContext& ctx) { return static_cast<uint32_t>(ctx.lr); }

// ---- FrontEndState (the base class) ----------------------------------------

// The screen manager's callbacks into a state, installed by OpenScreen.
constexpr uint32_t kOnScreenLoaded = 0x82D09368;
constexpr uint32_t kOnScreenIntroDone = 0x82D09380;
constexpr uint32_t kOnScreenOutroDone = 0x82D093A8;

// sub_82D093D0, vtable +56: OpenScreen(screen_type, screen_layer). Records
// the request and the callbacks; the FE manager's next update hands it to
// the screen manager (RequestScreen).
void BodyOpenScreenBase(PPCContext& ctx, uint8_t* base) {
  auto& self = At<FrontEndState>(base, ctx.r3.u32);
  self.screen_layer = ctx.r5.u32;
  self.callback.object = ctx.r3.u32;
  self.screen_type = ctx.r4.u32;
  self.callback.on_loaded = kOnScreenLoaded;
  self.callback.on_intro_done = kOnScreenIntroDone;
  self.callback.on_outro_done = kOnScreenOutroDone;
  self.screen_state = static_cast<uint32_t>(ScreenState::kOpenRequested);
}

// sub_82D09470, vtable +60: CloseScreen(destroy_screen). Starts the outro.
void BodyCloseScreenBase(PPCContext& ctx, uint8_t* base) {
  auto& self = At<FrontEndState>(base, ctx.r3.u32);
  self.destroy_screen = static_cast<uint8_t>(ctx.r4.u32);
  self.screen_state = static_cast<uint32_t>(ScreenState::kClosing);
}

// sub_82D09410: RequestScreen, from the FE manager's update before each
// state's OnUpdate. A requested screen goes to the screen manager to load
// (with our callbacks and layer); the state then waits for it.
void BodyRequestScreen(PPCContext& ctx, uint8_t* base) {
  const uint32_t self_addr = ctx.r3.u32;
  auto& self = At<FrontEndState>(base, self_addr);
  if (self.screen_state != static_cast<uint32_t>(ScreenState::kOpenRequested)) {
    return;
  }
  {
    GuestFrame frame(ctx, base, 96, /*save_r31=*/true);
    ctx.r4.u64 = self.screen_type;
    ctx.r5.u64 = self_addr + offsetof(FrontEndState, callback);
    ctx.r6.u64 = self.screen_layer;
    CallMethod(ctx, base, self.screen_manager, kScreenMgrLoadScreen, 0x82D09450);
  }
  At<FrontEndState>(base, self_addr).screen_state = static_cast<uint32_t>(ScreenState::kLoading);
}

// sub_82D09480, vtable +28: Show(show, silent). Showing allows input and
// marks the screen up, then (unless silent) runs OnIntroDone as if the intro
// had played; either way the screen manager shows or hides our screen. Once
// shown, input is unblocked, OnShown runs and the manager is told.
void BodyShow(PPCContext& ctx, uint8_t* base) {
  const uint32_t self_addr = ctx.r3.u32;
  const uint32_t show_arg = ctx.r4.u32;  // passed on whole, compared as a byte
  const bool show = (show_arg & 0xFF) == 1;
  const bool silent = (ctx.r5.u32 & 0xFF) != 0;
  GuestFrame frame(ctx, base, 112, /*save_r31=*/true);
  if (show) {
    auto& self = At<FrontEndState>(base, self_addr);
    self.allow_input = 1;
    self.screen_state = static_cast<uint32_t>(ScreenState::kShown);
    if (!silent) {
      CallMethod(ctx, base, self_addr, kVtOnIntroDone, 0x82D094CC);
    }
  }
  {
    const auto& self = At<FrontEndState>(base, self_addr);
    ctx.r4.u64 = self.screen_id;
    ctx.r5.u64 = show_arg;
    CallMethod(ctx, base, self.screen_manager, kScreenMgrShowScreen, 0x82D094E8);
  }
  if (show) {
    At<FrontEndState>(base, self_addr).local_input_state = 0;
    CallMethod(ctx, base, self_addr, kVtOnShown, 0x82D0950C);
    ctx.r4.u64 = self_addr;
    CallMethod(ctx, base, At<FrontEndState>(base, self_addr).screen_manager, kScreenMgrStateShown,
               0x82D09524);
  }
}

// The screen manager's callbacks (screen_id, state); each ends in the
// state's own handler, as a tail call.
// sub_82D09368: the screen has loaded: remember its id.
void BodyOnScreenLoaded(PPCContext& ctx, uint8_t* base) {
  const uint32_t self_addr = ctx.r4.u32;
  At<FrontEndState>(base, self_addr).screen_id = ctx.r3.u32;
  CallMethod(ctx, base, self_addr, kVtOnScreenLoaded, CallerLr(ctx));
}
// sub_82D09380: the intro has played: the screen is up and takes input.
void BodyOnScreenIntroDone(PPCContext& ctx, uint8_t* base) {
  const uint32_t self_addr = ctx.r4.u32;
  auto& self = At<FrontEndState>(base, self_addr);
  self.allow_input = 1;
  self.screen_state = static_cast<uint32_t>(ScreenState::kShown);
  CallMethod(ctx, base, self_addr, kVtOnIntroDone, CallerLr(ctx));
}
// sub_82D093A8: the outro has played: a closing screen is gone.
void BodyOnScreenOutroDone(PPCContext& ctx, uint8_t* base) {
  const uint32_t self_addr = ctx.r4.u32;
  auto& self = At<FrontEndState>(base, self_addr);
  if (self.screen_state == static_cast<uint32_t>(ScreenState::kClosing)) {
    self.screen_state = static_cast<uint32_t>(ScreenState::kNone);
  }
  CallMethod(ctx, base, self_addr, kVtOnOutroDone, CallerLr(ctx));
}

// sub_82639388, vtable +0: the base destructor (this, flags). Derived
// destructors end the same way (their own copies). flags & 1: free the
// object through the default allocator (ICoreAllocator::Free, vtable +12).
// Returns this.
constexpr uint32_t kAllocatorFree = 0x0C;
void BodyDestroy(PPCContext& ctx, uint8_t* base) {
  const uint32_t self_addr = ctx.r3.u32;
  const bool free_it = (ctx.r4.u32 & 1) != 0;
  At<FrontEndState>(base, self_addr).vtable = kFrontEndStateVtable;
  if (free_it) {
    GuestFrame frame(ctx, base, 96, /*save_r31=*/true);
    ctx.lr = 0x826393B8;
    sub_828AAF88(ctx, base);  // EA::Allocator::ICoreAllocator::GetDefaultAllocator
    const uint32_t allocator = ctx.r3.u32;
    ctx.r4.u64 = self_addr;
    ctx.r5.u64 = 0;
    CallMethod(ctx, base, allocator, kAllocatorFree, 0x826393D0);
  }
  ctx.r3.u64 = self_addr;
}

// ---- Single states ----------------------------------------------------------

// State 51 (screen 91): +60 cleared on entry (not yet named).
struct State51 : FrontEndState {
  be<uint32_t> unknown_3c;  // +60
};
void BodyS51Enter(PPCContext& ctx, uint8_t* base) {
  At<State51>(base, ctx.r3.u32).unknown_3c = 0;
  OpenScreen(ctx, base, ctx.r3.u32, 91, CallerLr(ctx));
}

// State 40, Billboard5 (screen 82): a billboard that counts down. Its
// update (sub_82634A58) runs the float countdown and the ms timer.
struct BillboardState : FrontEndState {
  be<float> countdown_s;  // +60
  be<int32_t> timer_ms;   // +64
};
constexpr uint32_t kBillboardCountdownStart = 0x821161A0;  // a float constant in .rdata
void BodyS40Enter(PPCContext& ctx, uint8_t* base) {
  auto& self = At<BillboardState>(base, ctx.r3.u32);
  self.timer_ms = 10000;
  self.countdown_s = At<be<float>>(base, kBillboardCountdownStart).get();
  OpenScreen(ctx, base, ctx.r3.u32, 82, CallerLr(ctx));
}

// State 48: opens the screen its transition's sub-state names.
void BodyS48Enter(PPCContext& ctx, uint8_t* base) {
  const uint32_t transition = At<FrontEndState>(base, ctx.r3.u32).transition;
  OpenScreen(ctx, base, ctx.r3.u32, At<StateTransition>(base, transition).sub_state,
             CallerLr(ctx));
}

// State 46: has no screen of its own; it marks itself shown and active at
// once and asks its update (sub_82639710) for a refresh.
struct State46 : FrontEndState {
  uint8_t unknown_3c[1614 - 60];
  uint8_t refresh_pending;  // +1614
};
static_assert(offsetof(State46, refresh_pending) == 1614);
void BodyS46Enter(PPCContext& ctx, uint8_t* base) {
  auto& self = At<State46>(base, ctx.r3.u32);
  self.allow_input = 1;
  self.screen_state = static_cast<uint32_t>(ScreenState::kShown);
  self.refresh_pending = 1;
}

// States 34 and 62 OnLeave: unblock input, close the screen.
void BodyS34Leave(PPCContext& ctx, uint8_t* base) {
  At<FrontEndState>(base, ctx.r3.u32).local_input_state = 0;
  CloseScreen(ctx, base, ctx.r3.u32, CallerLr(ctx));
}

// State 23, ChallengeMap (screen 23) OnLeave: rewind its list (a cursor into
// its own inline buffer at +5048, and the count), close the screen.
struct ChallengeMapState : FrontEndState {
  be<uint32_t> cursor;  // +60
  be<uint32_t> unknown_40;
  be<uint32_t> count;   // +68
};
constexpr uint32_t kChallengeMapBuffer = 5048;
void BodyS23Leave(PPCContext& ctx, uint8_t* base) {
  auto& self = At<ChallengeMapState>(base, ctx.r3.u32);
  self.cursor = ctx.r3.u32 + kChallengeMapBuffer;
  self.count = 0;
  CloseScreen(ctx, base, ctx.r3.u32, CallerLr(ctx));
}

// State 43, Marketing ("coming soon", screen 85) OnLeave: clear the global
// flag its update (sub_826386A0) watches, close the screen.
constexpr uint32_t kMarketingFlag = 0x83083566;
void BodyS43Leave(PPCContext& ctx, uint8_t* base) {
  At<uint8_t>(base, kMarketingFlag) = 0;
  CloseScreen(ctx, base, ctx.r3.u32, CallerLr(ctx));
}

// ---- Options (state 45) ------------------------------------------------------

// kScreenLayouts: the option on each row of each Options page.
constexpr uint32_t kScreenLayouts = 0x83026FE8;  // be<int32_t>[13][16]
constexpr uint32_t kRowsPerPage = 16;

// Option ids (kOptionTable, 0x83026EC8: each one's title string id).
enum OptionId : int32_t {
  kOptionDifficultySettings = 2,   // ID_GAMESETTINGS_DIFFICULTY_SETTINGS
  kOptionSfxPack = 10,             // ID_GAMESETTINGS_SFXPACK
  kOptionSkateFeedSettings = 35,   // ID_GAMESETTINGS_SKATEFEED_SETTINGS
};

// The world state block (scenario-harness.md: +40 current map, +48
// requested); its first word is a mode. Difficulty can't change in modes 4
// and 5 (not yet named).
constexpr uint32_t kWorldStateBlock = 0x830B7AE8;

// sub_8260D648, vtable +72: IsItemEnabled(row). Every row is enabled except:
// Difficulty Settings in world modes 4 and 5; the SFX pack selector unless
// there is more than one pack (sub_82486760 counts the "default" attrib
// collection); skate.Feed settings while sub_8245A2D0 says it's unavailable
// (an online object's state == 11). The two calls get a stack frame of the
// original's size and its return addresses.
void BodyOptionsItemEnabled(PPCContext& ctx, uint8_t* base) {
  const auto& self = At<OptionsState>(base, ctx.r3.u32);
  const uint32_t row = ctx.r5.u32;
  const int32_t option =
      At<be<int32_t>>(base, kScreenLayouts + 4 * (self.page * kRowsPerPage + row));
  bool enabled = true;
  if (option == kOptionDifficultySettings) {
    const int32_t mode = At<be<int32_t>>(base, kWorldStateBlock);
    enabled = mode != 4 && mode != 5;
  } else if (option == kOptionSkateFeedSettings) {
    GuestFrame frame(ctx, base, 96, false);
    ctx.lr = 0x8260D6C4;
    sub_8245A2D0(ctx, base);
    enabled = (ctx.r3.u32 & 0xFF) == 0;
  } else if (option == kOptionSfxPack) {
    GuestFrame frame(ctx, base, 96, false);
    ctx.lr = 0x8260D6F0;
    sub_82486760(ctx, base);
    enabled = ctx.r3.s32 > 1;
  }
  ctx.r3.u64 = enabled ? 1 : 0;
}

// sub_8260D710, vtable +68: OnHighlight(row): remembers the highlighted row.
void BodyOptionsHighlight(PPCContext& ctx, uint8_t* base) {
  At<OptionsState>(base, ctx.r3.u32).highlighted_row = ctx.r5.u32;
}

int Mode() { return REXCVAR_GET(skate3_native_fe_replace) ? 2 : 0; }

using skate3::native_replace::Family;
Family g_open_base{"fe_openscreen", &Mode, nullptr, &BodyOpenScreenBase};
Family g_close_base{"fe_closescreen_base", &Mode, nullptr, &BodyCloseScreenBase};
Family g_request{"fe_request_screen", &Mode, nullptr, &BodyRequestScreen};
Family g_show{"fe_show", &Mode, nullptr, &BodyShow};
Family g_cb_loaded{"fe_cb_loaded", &Mode, nullptr, &BodyOnScreenLoaded};
Family g_cb_intro{"fe_cb_intro", &Mode, nullptr, &BodyOnScreenIntroDone};
Family g_cb_outro{"fe_cb_outro", &Mode, nullptr, &BodyOnScreenOutroDone};
Family g_destroy{"fe_destroy", &Mode, nullptr, &BodyDestroy};
Family g_s51{"fe_s51_enter", &Mode, nullptr, &BodyS51Enter};
Family g_s40{"fe_s40_enter", &Mode, nullptr, &BodyS40Enter};
Family g_s48{"fe_s48_enter", &Mode, nullptr, &BodyS48Enter};
Family g_s46{"fe_s46_enter", &Mode, nullptr, &BodyS46Enter};
Family g_s34{"fe_s34_leave", &Mode, nullptr, &BodyS34Leave};
Family g_s23{"fe_s23_leave", &Mode, nullptr, &BodyS23Leave};
Family g_s43{"fe_s43_leave", &Mode, nullptr, &BodyS43Leave};
Family g_opt_enabled{"fe_options_enabled", &Mode, nullptr, &BodyOptionsItemEnabled};
Family g_opt_highlight{"fe_options_highlight", &Mode, nullptr, &BodyOptionsHighlight};

}  // namespace

namespace skate3::native_fe {
// skate3_options_menu.cpp hooks sub_8260D648 for its own pages and calls
// this for the game's rows.
void OptionsItemEnabled(PPCContext& ctx, uint8_t* base) {
  skate3::native_replace::Run(g_opt_enabled, ctx, base, &__imp__sub_8260D648);
}
}  // namespace skate3::native_fe

using skate3::native_replace::Run;
extern "C" REX_FUNC(sub_82D093D0) { Run(g_open_base, ctx, base, &__imp__sub_82D093D0); }
extern "C" REX_FUNC(sub_82D09470) { Run(g_close_base, ctx, base, &__imp__sub_82D09470); }
extern "C" REX_FUNC(sub_82D09410) { Run(g_request, ctx, base, &__imp__sub_82D09410); }
extern "C" REX_FUNC(sub_82D09480) { Run(g_show, ctx, base, &__imp__sub_82D09480); }
extern "C" REX_FUNC(sub_82D09368) { Run(g_cb_loaded, ctx, base, &__imp__sub_82D09368); }
extern "C" REX_FUNC(sub_82D09380) { Run(g_cb_intro, ctx, base, &__imp__sub_82D09380); }
extern "C" REX_FUNC(sub_82D093A8) { Run(g_cb_outro, ctx, base, &__imp__sub_82D093A8); }
extern "C" REX_FUNC(sub_82639388) { Run(g_destroy, ctx, base, &__imp__sub_82639388); }
extern "C" REX_FUNC(sub_8260E868) { Run(g_s51, ctx, base, &__imp__sub_8260E868); }
extern "C" REX_FUNC(sub_82634A28) { Run(g_s40, ctx, base, &__imp__sub_82634A28); }
extern "C" REX_FUNC(sub_825F20B0) { Run(g_s48, ctx, base, &__imp__sub_825F20B0); }
extern "C" REX_FUNC(sub_82639690) { Run(g_s46, ctx, base, &__imp__sub_82639690); }
extern "C" REX_FUNC(sub_8260A4B8) { Run(g_s34, ctx, base, &__imp__sub_8260A4B8); }
extern "C" REX_FUNC(sub_825EC980) { Run(g_s23, ctx, base, &__imp__sub_825EC980); }
extern "C" REX_FUNC(sub_82638620) { Run(g_s43, ctx, base, &__imp__sub_82638620); }
extern "C" REX_FUNC(sub_8260D710) { Run(g_opt_highlight, ctx, base, &__imp__sub_8260D710); }
