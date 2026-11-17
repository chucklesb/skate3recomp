// The boot flow's frontend screens, native (see the header).

#include "native/skate3_native_boot.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "generated/skate3_init.h"
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

constexpr uint32_t kLogoElapsedMs = 60;  // u32: dt accumulated since the state opened
constexpr uint32_t kLogoDone = 64;       // u8: the boot flow's handler moves on when set
constexpr uint32_t kLogoDurationMs = 4000;

// FrontEndState (id 5) OnUpdate(this, dt_ms), vtable +0x10: elapsed += dt;
// at 4000 ms set the done byte. A leaf function: no frame, no calls.
void BodyLogoUpdate(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t elapsed = REX_LOAD_U32(self + kLogoElapsedMs) + ctx.r4.u32;
  REX_STORE_U32(self + kLogoElapsedMs, elapsed);
  ctx.r11.u64 = elapsed;
  if (elapsed >= kLogoDurationMs || skate3::demo_path::IntroSkipPressed() ||
      skate3::demo_path::ShouldForceIntroLogoComplete()) {
    ctx.r11.u64 = 1;
    REX_STORE_U8(self + kLogoDone, 1);
  }
}

int Mode() { return REXCVAR_GET(skate3_native_boot_replace) ? 2 : 0; }

skate3::native_replace::Family g_logo{"boot_logo", &Mode, nullptr, &BodyLogoUpdate};

// ---- The FE states' other small methods (2026-11-17). Each follows its
// disassembly: the same guest calls with the same arguments, return
// addresses and stack frame. Virtual calls go through the state's vtable.
constexpr uint32_t kVtOpenScreen = 56;   // (this, screen, 0)
constexpr uint32_t kVtCloseScreen = 60;  // (this, 0)

// The original's stack frame: back chain, the saved LR at -8 and (when the
// original saves it) r31 at -16 of the caller's r1; popped on exit.
struct GuestFrame {
  PPCContext& ctx;
  uint64_t saved;
  GuestFrame(PPCContext& c, uint8_t* base, uint32_t size, bool save_r31) : ctx(c), saved(c.r1.u64) {
    const uint32_t entry = c.r1.u32;
    REX_STORE_U32(entry - 8, static_cast<uint32_t>(c.lr));
    if (save_r31) {
      REX_STORE_U64(entry - 16, c.r31.u64);
    }
    const uint32_t sp = entry - size;
    REX_STORE_U32(sp, entry);
    ctx.r1.u64 = sp;
  }
  ~GuestFrame() { ctx.r1.u64 = saved; }
};

// A virtual call through `obj`'s vtable slot with LR = `ret` (a tail call
// passes the caller's LR through).
void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot, uint32_t ret) {
  const uint32_t vt = REX_LOAD_U32(obj);
  const uint32_t fn = REX_LOAD_U32(vt + slot);
  ctx.r11.u64 = vt;
  ctx.r10.u64 = fn;
  ctx.ctr.u64 = fn;
  ctx.lr = ret;
  REX_CALL_INDIRECT_FUNC(fn);
}

// The OnEnter of 13 FE states (vtable +0x0C) is one line: tail call
// this->OpenScreen(N, 0) (lwz vtable; li r5,0; li r4,N; lwz +56; bctr).
// One family; each wrapper passes its N (found by matching that exact code
// in every FE state's vtable, 2026-11-17).
thread_local uint32_t t_open_screen = 0;
void BodyOpenScreen(PPCContext& ctx, uint8_t* base) {
  ctx.r5.u64 = 0;
  ctx.r4.u64 = t_open_screen;
  CallVirtual(ctx, base, ctx.r3.u32, kVtOpenScreen, static_cast<uint32_t>(ctx.lr));
}

// sub_826504C8: the OnLeave (vtable +0x18) of several FE states: tail call
// this->CloseScreen(0).
void BodyCloseScreenOnly(PPCContext& ctx, uint8_t* base) {
  ctx.r4.u64 = 0;
  CallVirtual(ctx, base, ctx.r3.u32, kVtCloseScreen, static_cast<uint32_t>(ctx.lr));
}

// sub_82608E18: their shared OnLeave (vtable +0x18): the done byte +64 = 1,
// tail call this->CloseScreen(0).
void BodyCloseScreen(PPCContext& ctx, uint8_t* base) {
  ctx.r11.u64 = 1;
  ctx.r4.u64 = 0;
  REX_STORE_U8(ctx.r3.u32 + kLogoDone, 1);
  CallVirtual(ctx, base, ctx.r3.u32, kVtCloseScreen, static_cast<uint32_t>(ctx.lr));
}

// sub_82608F18: the title's OnEnter (FE state 22): +76 = 0 (the idle timer),
// +52 = 1, +64 = 0, byte +80 = 0 (prompt not armed); unless mode +68 is set,
// tail call this->OpenScreen(2, 0).
void BodyTitleEnter(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t mode = REX_LOAD_U32(self + 68);
  REX_STORE_U32(self + 76, 0);
  REX_STORE_U32(self + 52, 1);
  REX_STORE_U32(self + 64, 0);
  REX_STORE_U8(self + 80, 0);
  ctx.r9.u64 = 1;
  ctx.r10.u64 = mode;
  ctx.r11.u64 = 0;
  if (mode != 0) {
    return;
  }
  ctx.r5.u64 = 0;
  ctx.r4.u64 = 2;
  CallVirtual(ctx, base, self, kVtOpenScreen, static_cast<uint32_t>(ctx.lr));
}

// sub_82608F58: the title's OnLeave: play the leave sound (sub_825DFB88 with
// its 64-bit FE sound hash), then this->CloseScreen(0); returns its r3.
constexpr uint64_t kTitleLeaveSound = 0xE3071166217C9661ull;
void BodyTitleLeave(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  GuestFrame frame(ctx, base, 96, /*save_r31=*/true);
  ctx.r3.u64 = kTitleLeaveSound;
  ctx.lr = 0x82608F84;
  sub_825DFB88(ctx, base);
  ctx.r4.u64 = 0;
  ctx.r3.u64 = self;
  CallVirtual(ctx, base, self, kVtCloseScreen, 0x82608F9C);
}

// sub_82639400: FrontEndState_LanguageSelect::OnUpdate(this, dt_ms): +60 +=
// dt; at 10000 ms reset it and pop the screen, keeping the current language
// (sub_82D0AC88 on the FE manager, sub_824AD240).
constexpr uint32_t kLanguageTimeoutMs = 10000;
void BodyLanguageUpdate(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  GuestFrame frame(ctx, base, 96, /*save_r31=*/false);
  const uint32_t elapsed = REX_LOAD_U32(self + 60) + ctx.r4.u32;
  REX_STORE_U32(self + 60, elapsed);
  ctx.r11.u64 = elapsed;
  if (elapsed < kLanguageTimeoutMs) {
    return;
  }
  ctx.r11.u64 = 0;
  REX_STORE_U32(self + 60, 0);
  ctx.lr = 0x8263942C;
  sub_824AD240(ctx, base);
  ctx.lr = 0x82639430;
  sub_82D0AC88(ctx, base);
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
