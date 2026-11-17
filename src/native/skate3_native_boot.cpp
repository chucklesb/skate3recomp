// The boot flow's frontend screens, native (see the header).

#include "native/skate3_native_boot.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "generated/skate3_init.h"
#include "native/skate3_native_replace.h"
#include "skate3_demo_path.h"

REXCVAR_DEFINE_BOOL(skate3_native_boot_replace, true, "Skate 3",
                    "RE-8: the boot logo screen's update (sub_82608E38) as native code "
                    "(replace: boot_logo); off runs the recompiled code, which can't be "
                    "skipped")
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

}  // namespace

extern "C" REX_FUNC(sub_82608E38) {
  skate3::native_replace::Run(g_logo, ctx, base, &__imp__sub_82608E38);
}
