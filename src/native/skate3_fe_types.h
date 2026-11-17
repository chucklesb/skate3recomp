#pragma once
// The frontend's state objects as C++ types (RE-9; docs/re/ui.md,
// docs/re/types.md "FrontEndState").
//
// Every FE state derives from Sk8::FE::FrontEndStateBase (vtable 0x82301CE0;
// the objects are in the FE manager's table at *(S+8), S = *(0x830CFE14) +
// 0x200). Field names are Skate 2's (its PDB has the same layout up to
// +52; TU3 adds +56); each offset was confirmed from TU3's own methods
// (skate3_native_fe.cpp). Its screen's life: OpenScreen requests a Flash
// screen (kOpenRequested), the manager's next update asks the screen
// manager to load it (kLoading), the intro callback shows it (kShown, input
// allowed), CloseScreen plays the outro (kClosing) and the outro callback
// ends it (kNone). Derived states keep their own fields from +60.
#include <cstddef>
#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/types.h>

namespace skate3::fe {

using rex::be;

// Sk8::FE::eScreenState (the enumerator names aren't in the PDB).
enum class ScreenState : uint32_t {
  kNone = 0,           // no screen, or its outro has finished
  kOpenRequested = 1,  // OpenScreen called
  kLoading = 2,        // the screen manager is loading it; the intro plays
  kShown = 3,          // up; OnUpdate runs and input is taken
  kClosing = 4,        // CloseScreen called, the outro plays
};

// Sk8::FE::sScreenCallBack: the screen manager calls these, with the state
// (`object`) as the second argument.
struct ScreenCallBack {
  be<uint32_t> unknown_00;     // +0   Skate 2: mScreenId (int*); not seen used in TU3
  be<uint32_t> object;         // +4   mpObject: the state
  be<uint32_t> on_loaded;      // +8   mLoadedCallBack (0x82D09368)
  be<uint32_t> on_intro_done;  // +12  mIntroCallBack (0x82D09380)
  be<uint32_t> on_outro_done;  // +16  mOutroCallBack (0x82D093A8)
};
static_assert(sizeof(ScreenCallBack) == 20);

// Sk8::FE::sFEStateTransition: how a state was entered (vtable +48 stores
// it). State 48 opens the screen its sub_state names.
struct StateTransition {
  be<int32_t> sub_state;           // +0  mSubState
  uint8_t keep_previous;           // +4  mbKeepPrevious
  uint8_t keep_previous_visible;   // +5  mbKeepPreviousVisible
  uint8_t unknown_06[2];
  be<uint32_t> user_data;          // +8  mpUserData
};
static_assert(sizeof(StateTransition) == 12);

struct FrontEndState {
  be<uint32_t> vtable;            // +0
  be<uint32_t> unknown_04;        // +4   Skate 2: mCurrentTimer
  be<uint32_t> screen_id;         // +8   mScreenId: the loaded screen (set by on_loaded)
  be<uint32_t> transition;        // +12  mpFEStateTransition: StateTransition*
  be<uint32_t> screen_state;      // +16  mScreenState: ScreenState
  ScreenCallBack callback;        // +20  mScreenCallBack
  be<uint32_t> screen_layer;      // +40  mScreenLayer (const char*): OpenScreen's 2nd argument
  be<uint32_t> screen_type;       // +44  mScreenType: the Flash screen (docs/re/fe_screens.tsv)
  uint8_t destroy_screen;         // +48  mbScreenDestroy: CloseScreen's argument
  uint8_t allow_input;            // +49  mbAllowInput
  uint8_t unknown_4a[2];          // +50
  be<uint32_t> local_input_state; // +52  mLocalInputState: input is taken only at 0
  be<uint32_t> screen_manager;    // +56  (TU3) the screen manager
};
static_assert(offsetof(FrontEndState, transition) == 12);
static_assert(offsetof(FrontEndState, screen_state) == 16);
static_assert(offsetof(FrontEndState, callback) == 20);
static_assert(offsetof(FrontEndState, screen_type) == 44);
static_assert(offsetof(FrontEndState, allow_input) == 49);
static_assert(offsetof(FrontEndState, local_input_state) == 52);
static_assert(sizeof(FrontEndState) == 60);

// The base vtable (0x82301CE0, 34 slots) as far as callers name them, by
// byte offset. The base's own entries for the others are the binary's shared
// empty / return-0 / return-1 functions.
constexpr uint32_t kFrontEndStateVtable = 0x82301CE0;
constexpr uint32_t kVtDestroy = 0x00;         // (this, flags): flags & 1 frees
constexpr uint32_t kVtOnEnter = 0x0C;
constexpr uint32_t kVtOnUpdate = 0x10;        // (this, dt_ms): the manager passes 16
constexpr uint32_t kVtOnLeave = 0x18;
constexpr uint32_t kVtShow = 0x1C;            // (this, show, silent)
constexpr uint32_t kVtOnShown = 0x20;         // after Show(true) allows input
constexpr uint32_t kVtReceiveInput = 0x28;    // (this, code): base returns 0
constexpr uint32_t kVtSetTransition = 0x30;   // (this, transition)
constexpr uint32_t kVtOpenScreen = 0x38;      // (this, screen_type, screen_layer)
constexpr uint32_t kVtCloseScreen = 0x3C;     // (this, destroy_screen)
constexpr uint32_t kVtOnMenuSelect = 0x40;
constexpr uint32_t kVtOnHighlight = 0x44;     // (this, ?, row)
constexpr uint32_t kVtIsItemEnabled = 0x48;   // (this, ?, row): base returns 1
constexpr uint32_t kVtOnIntroDone = 0x68;
constexpr uint32_t kVtOnScreenLoaded = 0x6C;
constexpr uint32_t kVtOnOutroDone = 0x70;

// The screen manager (FrontEndState::screen_manager) vtable slots the base
// class calls.
constexpr uint32_t kScreenMgrLoadScreen = 0x04;   // (mgr, screen_type, ScreenCallBack*, layer)
constexpr uint32_t kScreenMgrShowScreen = 0x10;   // (mgr, screen_id, show)
constexpr uint32_t kScreenMgrStateShown = 0x18;   // (mgr, state)

// State 5 (the EA logo) and states 6-8: a timer and a done flag the boot
// flow polls.
struct TimedScreenState : FrontEndState {
  be<uint32_t> elapsed_ms;  // +60
  uint8_t done;             // +64
};
static_assert(offsetof(TimedScreenState, done) == 64);

// State 22, the title ("Press Start").
struct TitleState : FrontEndState {
  uint8_t confirming;          // +60  Start accepted, leaving
  uint8_t unknown_3d[3];
  be<uint32_t> unknown_40;     // +64
  be<uint32_t> attract_mode;   // +68  non-zero: entered from the attract loop, no screen
  uint8_t prompt_ready;        // +72  the prompt delay has passed
  uint8_t unknown_49[3];
  be<uint32_t> idle_ms;        // +76  replays the intro movie at 30,000
  uint8_t start_armed;         // +80  Start pressed, waiting for its release
};
static_assert(offsetof(TitleState, attract_mode) == 68);
static_assert(offsetof(TitleState, idle_ms) == 76);
static_assert(offsetof(TitleState, start_armed) == 80);

// State 47, the language select.
struct LanguageSelectState : FrontEndState {
  be<uint32_t> idle_ms;  // +60  keeps the current language at 10,000
};

// State 45, the Options screens (FrontEndState_Options, vtable 0x823034B0).
struct OptionsState : FrontEndState {
  uint8_t unknown_3c[8];
  be<uint32_t> page;             // +68  index into kScreenLayouts
  be<uint32_t> highlighted_row;  // +72
};
static_assert(offsetof(OptionsState, page) == 68);
static_assert(offsetof(OptionsState, highlighted_row) == 72);

// A guest object by address (REX_PHYS_HOST_OFFSET: the physical mirror at
// 0xE0000000+ is shifted on some hosts). Include after generated/skate3_init.h.
template <typename T>
T& At(uint8_t* base, uint32_t addr) {
  return *reinterpret_cast<T*>(base + addr + REX_PHYS_HOST_OFFSET(addr));
}

// Calls this->vtable[slot] with r3 = self and the caller's r4/r5 as set,
// returning to `ret` (pass the current LR for a tail call).
inline void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t slot,
                        uint32_t ret) {
  const uint32_t fn = At<be<uint32_t>>(base, At<FrontEndState>(base, self).vtable + slot);
  ctx.r3.u64 = self;
  ctx.lr = ret;
  REX_CALL_INDIRECT_FUNC(fn);
}

// this->OpenScreen(screen_type, no layer) / this->CloseScreen(keep the screen).
inline void OpenScreen(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t screen,
                       uint32_t ret) {
  ctx.r4.u64 = screen;
  ctx.r5.u64 = 0;
  CallVirtual(ctx, base, self, kVtOpenScreen, ret);
}
inline void CloseScreen(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t ret) {
  ctx.r4.u64 = 0;
  CallVirtual(ctx, base, self, kVtCloseScreen, ret);
}

}  // namespace skate3::fe
