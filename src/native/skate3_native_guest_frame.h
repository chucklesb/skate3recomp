#pragma once
// Helpers for native bodies that call back into guest code (RE-7 / RE-9).
//
// A replaced function that calls other guest functions must leave the guest
// stack as the original would: callees save their LR into the caller's frame
// and may walk the back chain. These rebuild the original's frame and pass
// the original's return addresses.
//
// Include after generated/skate3_init.h (the REX_ memory macros need the
// global namespace).
#include <cstdint>

#include <rex/ppc/context.h>

namespace skate3::native_guest {

// The original's stack frame: back chain, the saved LR at -8 and (when the
// original saves it) r31 at -16 of the caller's r1. The destructor pops it
// and restores LR, as the original's epilogue does.
struct GuestFrame {
  PPCContext& ctx;
  uint64_t saved_r1;
  uint64_t saved_lr;
  GuestFrame(PPCContext& c, uint8_t* base, uint32_t size, bool save_r31)
      : ctx(c), saved_r1(c.r1.u64), saved_lr(c.lr) {
    const uint32_t entry = c.r1.u32;
    REX_STORE_U32(entry - 8, static_cast<uint32_t>(c.lr));
    if (save_r31) {
      REX_STORE_U64(entry - 16, c.r31.u64);
    }
    const uint32_t sp = entry - size;
    REX_STORE_U32(sp, entry);
    ctx.r1.u64 = sp;
  }
  ~GuestFrame() {
    ctx.r1.u64 = saved_r1;
    ctx.lr = saved_lr;
  }
};

}  // namespace skate3::native_guest
