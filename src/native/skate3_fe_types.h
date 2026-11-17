#pragma once
// The frontend's state objects as C++ types (RE-9; docs/re/ui.md,
// docs/re/types.md "FrontEndState").
//
// Every FE state derives from FrontEndState (the table at *(S+8), S =
// *(0x830CFE14) + 0x200). Its life: OpenScreen asks the screen manager for a
// Flash screen (phase 1), the screen's intro plays (2), the shown callback
// marks it ready and active (3), CloseScreen starts the outro (4).
// Offsets were read from the base methods: OpenScreen sub_82D093D0 (vtable
// +56), CloseScreen sub_82D09470 (+60), the shown callback sub_82D09480
// (+28) and the input dispatch sub_825EC0E8. Derived states keep their own
// fields from +60.
#include <cstddef>
#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/types.h>

namespace skate3::fe {

using rex::be;

enum class Phase : uint32_t {
  kIdle = 0,
  kOpening = 1,  // OpenScreen called, the screen is loading
  kIntro = 2,    // the screen's intro animation
  kActive = 3,   // shown; takes input
  kClosing = 4,  // CloseScreen called, the outro plays
};

struct FrontEndState {
  be<uint32_t> vtable;           // +0
  be<uint32_t> unknown_04;       // +4
  be<uint32_t> screen_handle;    // +8   the manager's handle of our screen
  be<uint32_t> unknown_0c;       // +12
  be<uint32_t> phase;            // +16  Phase
  be<uint32_t> unknown_14;       // +20
  be<uint32_t> callback_owner;   // +24  this; with +28..+36, the screen
  be<uint32_t> callbacks[3];     // +28  manager's callbacks into the state
  be<uint32_t> open_arg;         // +40  OpenScreen's second argument
  be<uint32_t> screen_id;        // +44  the Flash screen (docs/re/fe_screens.tsv)
  uint8_t close_arg;             // +48  CloseScreen's argument
  uint8_t shown;                 // +49  set by the shown callback
  uint8_t unknown_4a[2];         // +50
  be<uint32_t> input_blocked;    // +52  input is dropped while non-zero
  be<uint32_t> screen_manager;   // +56
};
static_assert(offsetof(FrontEndState, phase) == 16);
static_assert(offsetof(FrontEndState, screen_id) == 44);
static_assert(offsetof(FrontEndState, shown) == 49);
static_assert(offsetof(FrontEndState, input_blocked) == 52);
static_assert(sizeof(FrontEndState) == 60);

// Vtable slots (byte offsets).
constexpr uint32_t kVtOnEnter = 0x0C;
constexpr uint32_t kVtOnUpdate = 0x10;
constexpr uint32_t kVtOnLeave = 0x18;
constexpr uint32_t kVtOpenScreen = 56;   // (this, screen_id, open_arg)
constexpr uint32_t kVtCloseScreen = 60;  // (this, close_arg)

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

// A guest object by address. FE objects live on the guest heap
// (0x40000000-0x50000000), below the physical mirror, so base + addr is
// their host address on every platform.
template <typename T>
T& At(uint8_t* base, uint32_t addr) {
  return *reinterpret_cast<T*>(base + addr);
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

// this->OpenScreen(screen, 0) / this->CloseScreen(0).
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
