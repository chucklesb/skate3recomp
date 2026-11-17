// The boot flow's frontend screens, native (see the header).

#include "native/skate3_native_boot.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "generated/skate3_init.h"
#include "native/skate3_fe_types.h"
#include "native/skate3_native_guest_frame.h"
#include "native/skate3_native_replace.h"
#include "skate3_demo_path.h"

REXCVAR_DEFINE_BOOL(skate3_native_boot_replace, true, "Skate 3",
                    "RE-8: the boot screens' small FE-state methods as native code: the "
                    "logo's update (boot_logo), the one-line OnEnter of 13 FE states "
                    "(fe_open), the OnLeave of states 5-8 and 10/11/13 (fe_close, "
                    "fe_closescreen), the title's OnEnter/OnLeave (title_enter, "
                    "title_leave) and the language select's 10 s timer (lang_update). Off "
                    "runs the recompiled code (the logo then can't be skipped)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The REX_ memory macros need the global namespace (u32 and friends).
namespace {

using namespace skate3::fe;
using skate3::native_guest::GuestFrame;

constexpr uint32_t kLogoDurationMs = 4000;

// States 5-8 (the EA logo, ...) OnUpdate(this, dt_ms), vtable +0x10: the
// screen stays up for 4 s, then flags itself done for the boot flow. Our
// skip (any button, or the harness) ends it early.
void BodyLogoUpdate(PPCContext& ctx, uint8_t* base) {
  auto& self = At<TimedScreenState>(base, ctx.r3.u32);
  self.elapsed_ms = self.elapsed_ms + ctx.r4.u32;
  if (self.elapsed_ms >= kLogoDurationMs || skate3::demo_path::IntroSkipPressed() ||
      skate3::demo_path::ShouldForceIntroLogoComplete()) {
    self.done = 1;
  }
}

int Mode() { return REXCVAR_GET(skate3_native_boot_replace) ? 2 : 0; }

skate3::native_replace::Family g_logo{"boot_logo", &Mode, nullptr, &BodyLogoUpdate};

// ---- The FE states' enter/leave methods (2026-11-17; docs/re/ui.md).

// OnEnter (vtable +0x0C) of 13 states: show the state's screen. One family;
// each hook passes its screen id (the table below).
thread_local uint32_t t_open_screen = 0;
void BodyOpenScreen(PPCContext& ctx, uint8_t* base) {
  OpenScreen(ctx, base, ctx.r3.u32, t_open_screen, static_cast<uint32_t>(ctx.lr));
}

// sub_826504C8, OnLeave (vtable +0x18) of states 10, 11, 13: close the screen.
void BodyCloseScreenOnly(PPCContext& ctx, uint8_t* base) {
  CloseScreen(ctx, base, ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
}

// sub_82608E18, OnLeave of the timed screens (states 5-8): flag done (a
// skipped screen counts as finished), close the screen.
void BodyCloseScreen(PPCContext& ctx, uint8_t* base) {
  At<TimedScreenState>(base, ctx.r3.u32).done = 1;
  CloseScreen(ctx, base, ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
}

// sub_82608F18, the title's OnEnter: reset the idle timer and the Start
// prompt, block input until the screen is shown, then show the title screen
// (2) unless we came from the attract loop.
constexpr uint32_t kTitleScreen = 2;
void BodyTitleEnter(PPCContext& ctx, uint8_t* base) {
  auto& self = At<TitleState>(base, ctx.r3.u32);
  self.idle_ms = 0;
  self.input_blocked = 1;
  self.unknown_40 = 0;
  self.start_armed = 0;
  if (self.attract_mode != 0) {
    return;
  }
  OpenScreen(ctx, base, ctx.r3.u32, kTitleScreen, static_cast<uint32_t>(ctx.lr));
}

// sub_82608F58, the title's OnLeave: the leave sound, then close the screen.
// PlayFeSound (sub_825DFB88) takes the sound's 64-bit name hash.
constexpr uint64_t kTitleLeaveSound = 0xE3071166217C9661ull;
void BodyTitleLeave(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  GuestFrame frame(ctx, base, 96, /*save_r31=*/false);
  ctx.r3.u64 = kTitleLeaveSound;
  ctx.lr = 0x82608F84;
  sub_825DFB88(ctx, base);
  CloseScreen(ctx, base, self, 0x82608F9C);
}

// sub_82639400, the language select's OnUpdate(this, dt_ms): after 10 s
// without a choice, keep the current language and pop the state
// (FeManager() sub_824AD240, then its PopState sub_82D0AC88).
constexpr uint32_t kLanguageTimeoutMs = 10000;
void BodyLanguageUpdate(PPCContext& ctx, uint8_t* base) {
  auto& self = At<LanguageSelectState>(base, ctx.r3.u32);
  self.idle_ms = self.idle_ms + ctx.r4.u32;
  if (self.idle_ms < kLanguageTimeoutMs) {
    return;
  }
  self.idle_ms = 0;
  GuestFrame frame(ctx, base, 96, /*save_r31=*/false);
  ctx.lr = 0x8263942C;
  sub_824AD240(ctx, base);  // r3 = the FE manager
  ctx.lr = 0x82639430;
  sub_82D0AC88(ctx, base);  // pop the top state
}

skate3::native_replace::Family g_open{"fe_open", &Mode, nullptr, &BodyOpenScreen};
skate3::native_replace::Family g_close_only{"fe_closescreen", &Mode, nullptr, &BodyCloseScreenOnly};
skate3::native_replace::Family g_close{"fe_close", &Mode, nullptr, &BodyCloseScreen};
skate3::native_replace::Family g_title_enter{"title_enter", &Mode, nullptr, &BodyTitleEnter};
skate3::native_replace::Family g_title_leave{"title_leave", &Mode, nullptr, &BodyTitleLeave};
skate3::native_replace::Family g_lang{"lang_update", &Mode, nullptr, &BodyLanguageUpdate};

}  // namespace

extern "C" REX_FUNC(sub_82608E38) {
  skate3::native_replace::Run(g_logo, ctx, base, &__imp__sub_82608E38);
}
#define SKATE3_FE_OPEN(addr, screen)                                         extern "C" REX_FUNC(sub_##addr) {                                            t_open_screen = screen;                                                    skate3::native_replace::Run(g_open, ctx, base, &__imp__sub_##addr);      }
// FE state id -> the screen its OnEnter opens.
SKATE3_FE_OPEN(82608E00, 5)    // state 5, the boot logo
SKATE3_FE_OPEN(82608E58, 6)    // state 6
SKATE3_FE_OPEN(82608E70, 7)    // state 7
SKATE3_FE_OPEN(82608E88, 8)    // state 8
SKATE3_FE_OPEN(826098B0, 9)    // state 9
SKATE3_FE_OPEN(8260A270, 10)   // state 11
SKATE3_FE_OPEN(82634BB0, 23)   // state 41
SKATE3_FE_OPEN(825EF0F8, 60)   // state 28
SKATE3_FE_OPEN(825EC7E8, 61)   // state 70
SKATE3_FE_OPEN(8260FD88, 68)   // state 32
SKATE3_FE_OPEN(82638608, 85)   // state 43
SKATE3_FE_OPEN(82638E00, 86)   // state 44
SKATE3_FE_OPEN(826393E8, 94)   // state 47, the language select
#undef SKATE3_FE_OPEN
extern "C" REX_FUNC(sub_826504C8) {
  skate3::native_replace::Run(g_close_only, ctx, base, &__imp__sub_826504C8);
}
extern "C" REX_FUNC(sub_82608E18) {
  skate3::native_replace::Run(g_close, ctx, base, &__imp__sub_82608E18);
}
extern "C" REX_FUNC(sub_82608F18) {
  skate3::native_replace::Run(g_title_enter, ctx, base, &__imp__sub_82608F18);
}
extern "C" REX_FUNC(sub_82608F58) {
  skate3::native_replace::Run(g_title_leave, ctx, base, &__imp__sub_82608F58);
}
extern "C" REX_FUNC(sub_82639400) {
  skate3::native_replace::Run(g_lang, ctx, base, &__imp__sub_82639400);
}
