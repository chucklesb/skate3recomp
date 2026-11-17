// Small frontend-state methods, native (RE-7 / RE-9; docs/re/ui.md).
//
// Every FE state object (FrontEndStateBase; the table at *(S+8), S =
// *(0x830CFE14) + 0x200) has OnEnter at vtable +0x0C and OnLeave at +0x18.
// Many are a few instructions: set a field or two, then tail call
// this->OpenScreen(id, 0) (vtable +56) or this->CloseScreen(0) (+60). The
// boot screens' ones live in skate3_native_boot.cpp; these are the others,
// found by listing every FE state's vtable slots (2026-11-17). Each body
// follows its disassembly: the same stores, the same guest call with the
// same arguments, the caller's LR passed through (tail calls).
//
// skate3_native_fe_replace (default on): one Body family per function,
// named after the FE state, so the replace counters show which ones a run
// reaches.

#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "generated/skate3_init.h"
#include "native/skate3_native_replace.h"

REXCVAR_DEFINE_BOOL(skate3_native_fe_replace, true, "Skate 3",
                    "RE-9: small frontend-state OnEnter/OnLeave methods as native code "
                    "(replace: fe_s<state>_enter / _leave); off runs the recompiled code")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kVtOpenScreen = 56;   // (this, screen, 0)
constexpr uint32_t kVtCloseScreen = 60;  // (this, 0)

// Tail call through `obj`'s vtable slot, keeping the caller's LR.
void TailVirtual(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot) {
  const uint32_t vt = REX_LOAD_U32(obj);
  const uint32_t fn = REX_LOAD_U32(vt + slot);
  ctx.ctr.u64 = fn;
  REX_CALL_INDIRECT_FUNC(fn);
}

void OpenScreen(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t screen) {
  ctx.r5.u64 = 0;
  ctx.r4.u64 = screen;
  TailVirtual(ctx, base, self, kVtOpenScreen);
}

void CloseScreen(PPCContext& ctx, uint8_t* base, uint32_t self) {
  ctx.r4.u64 = 0;
  TailVirtual(ctx, base, self, kVtCloseScreen);
}

// sub_8260E868, state 51 OnEnter: +60 = 0; OpenScreen(91).
void BodyS51Enter(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  ctx.r11.u64 = 0;
  REX_STORE_U32(self + 60, 0);
  OpenScreen(ctx, base, self, 91);
}

// sub_82634A28, state 40 OnEnter: +64 = 10000 (a ms timer), +60 = the float
// at 0x821161A0; OpenScreen(82).
void BodyS40Enter(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  constexpr uint32_t kFloatConst = 0x821161A0;
  ctx.r9.u64 = 10000;
  REX_STORE_U32(self + 64, 10000);
  const uint32_t bits = REX_LOAD_U32(kFloatConst);
  float f;
  std::memcpy(&f, &bits, 4);
  ctx.f0.f64 = f;  // lfs f0
  REX_STORE_U32(self + 60, bits);
  OpenScreen(ctx, base, self, 82);
}

// sub_825F20B0, state 48 OnEnter: OpenScreen(**(this+12)).
void BodyS48Enter(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t p = REX_LOAD_U32(self + 12);
  ctx.r10.u64 = p;
  OpenScreen(ctx, base, self, REX_LOAD_U32(p));
}

// sub_82639690, state 46 OnEnter: marks itself active without a screen:
// byte +49 = 1, +16 = 3 (the state the input dispatch requires), byte +1614
// = 1.
void BodyS46Enter(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  ctx.r11.u64 = 1;
  ctx.r10.u64 = 3;
  REX_STORE_U8(self + 49, 1);
  REX_STORE_U32(self + 16, 3);
  REX_STORE_U8(self + 1614, 1);
}

// sub_8260A4B8, states 34 and 62 OnLeave: +52 = 0; CloseScreen(0).
void BodyS34Leave(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  ctx.r11.u64 = 0;
  REX_STORE_U32(self + 52, 0);
  CloseScreen(ctx, base, self);
}

// sub_825EC980, state 23 OnLeave: +60 = this + 5048, +68 = 0;
// CloseScreen(0).
void BodyS23Leave(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  ctx.r11.u64 = self + 5048;
  ctx.r10.u64 = 0;
  REX_STORE_U32(self + 60, self + 5048);
  REX_STORE_U32(self + 68, 0);
  CloseScreen(ctx, base, self);
}

// sub_82638620, state 43 OnLeave: the global byte 0x83083566 = 0;
// CloseScreen(0). (State 43's update, sub_826386A0, checks that byte.)
void BodyS43Leave(PPCContext& ctx, uint8_t* base) {
  ctx.r11.u64 = 0;
  REX_STORE_U8(0x83083566, 0);
  CloseScreen(ctx, base, ctx.r3.u32);
}

int Mode() { return REXCVAR_GET(skate3_native_fe_replace) ? 2 : 0; }

using skate3::native_replace::Family;
Family g_s51{"fe_s51_enter", &Mode, nullptr, &BodyS51Enter};
Family g_s40{"fe_s40_enter", &Mode, nullptr, &BodyS40Enter};
Family g_s48{"fe_s48_enter", &Mode, nullptr, &BodyS48Enter};
Family g_s46{"fe_s46_enter", &Mode, nullptr, &BodyS46Enter};
Family g_s34{"fe_s34_leave", &Mode, nullptr, &BodyS34Leave};
Family g_s23{"fe_s23_leave", &Mode, nullptr, &BodyS23Leave};
Family g_s43{"fe_s43_leave", &Mode, nullptr, &BodyS43Leave};

}  // namespace

extern "C" REX_FUNC(sub_8260E868) { skate3::native_replace::Run(g_s51, ctx, base, &__imp__sub_8260E868); }
extern "C" REX_FUNC(sub_82634A28) { skate3::native_replace::Run(g_s40, ctx, base, &__imp__sub_82634A28); }
extern "C" REX_FUNC(sub_825F20B0) { skate3::native_replace::Run(g_s48, ctx, base, &__imp__sub_825F20B0); }
extern "C" REX_FUNC(sub_82639690) { skate3::native_replace::Run(g_s46, ctx, base, &__imp__sub_82639690); }
extern "C" REX_FUNC(sub_8260A4B8) { skate3::native_replace::Run(g_s34, ctx, base, &__imp__sub_8260A4B8); }
extern "C" REX_FUNC(sub_825EC980) { skate3::native_replace::Run(g_s23, ctx, base, &__imp__sub_825EC980); }
extern "C" REX_FUNC(sub_82638620) { skate3::native_replace::Run(g_s43, ctx, base, &__imp__sub_82638620); }
