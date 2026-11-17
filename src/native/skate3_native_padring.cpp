// Sk8AppMan's pad-sample rings, native (RE-8; docs/re/boot.md, "The game
// tick and its pad-sample rings").
//
// Sk8AppMan (vtable 0x8230C880) owns two rings (appman+128 -> [6], [7]): 30
// slots of 4 pad records (100 bytes each, at q + 4 + (slot*4 + pad)*100),
// read index q+12004, write index q+12008, pad count q+12012, source buffer
// q+12016 (double-buffered pad records, its current half at +804). The pad
// sampler (slot +0x48, sub_826D6098, the timer thread) pushes one sample per
// tick; the per-tick update (slot +0x44, sub_826D34B8, the main thread) runs
// the game tick only when it can pop one. Single producer, single consumer:
// the guest uses lwarx/stwcx for each index access, the native bodies use
// sequentially consistent host atomics on the same big-endian words.
//
// skate3_native_padring_replace (default on): Body families
//   ring_pop    sub_8296D1F0(q) -> 1 and advance the read index, or 0 if empty;
//   ring_push   sub_8296D0D0(q): copy up to 4 pad records, advance the write index;
//   ring_unpush sub_826991B8(holder, idx): step ring idx's write index back one.

#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "generated/skate3_init.h"
#include "native/skate3_native_replace.h"

REXCVAR_DEFINE_BOOL(skate3_native_padring_replace, true, "Skate 3",
                    "RE-8: Sk8AppMan's pad-sample ring push/pop/unpush (sub_8296D0D0, "
                    "sub_8296D1F0, sub_826991B8) as native code (replace: ring_*)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kSlots = 30;
constexpr uint32_t kRead = 12004, kWrite = 12008, kPadCount = 12012, kSource = 12016;
constexpr uint32_t kSourceHalf = 804;  // in the source buffer
constexpr uint32_t kRecord = 100;

// Ring memory is guest heap (below the 0xE0000000 physical mirror).
inline uint32_t* Word(uint8_t* base, uint32_t addr) {
  return reinterpret_cast<uint32_t*>(base + addr);
}
inline uint32_t AtomicLoad(uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(__atomic_load_n(Word(base, addr), __ATOMIC_SEQ_CST));
}
inline void AtomicStore(uint8_t* base, uint32_t addr, uint32_t v) {
  __atomic_store_n(Word(base, addr), __builtin_bswap32(v), __ATOMIC_SEQ_CST);
}

// sub_8296D1F0(q): next = (read + 1) % 30; empty if next == write; else
// read = next and return 1.
void BodyPop(PPCContext& ctx, uint8_t* base) {
  const uint32_t q = ctx.r3.u32;
  const uint32_t next = (AtomicLoad(base, q + kRead) + 1) % kSlots;
  if (next == AtomicLoad(base, q + kWrite)) {
    ctx.r3.u64 = 0;
    return;
  }
  AtomicStore(base, q + kRead, next);
  ctx.r3.u64 = 1;
}

// sub_8296D0D0(q): n = min(pad count, 4) (signed); for each pad i the record
// (2*i + source half) of the source buffer goes to slot `write`, pad i; then
// write = (write + 1) % 30. (The guest copies with its memcpy, sub_82AED480.)
void BodyPush(PPCContext& ctx, uint8_t* base) {
  const uint32_t q = ctx.r3.u32;
  int32_t n = static_cast<int32_t>(REX_LOAD_U32(q + kPadCount));
  if (n >= 4) n = 4;
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t write = AtomicLoad(base, q + kWrite);
    const uint32_t src = REX_LOAD_U32(q + kSource);
    const uint32_t half = AtomicLoad(base, src + kSourceHalf);
    const uint32_t from = src + (2 * static_cast<uint32_t>(i) + half) * kRecord + 4;
    const uint32_t to = q + (write * 4 + static_cast<uint32_t>(i)) * kRecord + 4;
    std::memcpy(base + to, base + from, kRecord);
  }
  AtomicStore(base, q + kWrite, (AtomicLoad(base, q + kWrite) + 1) % kSlots);
}

// sub_826991B8(holder, idx): ring = holder[6 + idx]; if any, write =
// (write + 29) % 30 (withdraws the last pushed sample).
void BodyUnpush(PPCContext& ctx, uint8_t* base) {
  const uint32_t q = REX_LOAD_U32(ctx.r3.u32 + 4 * (ctx.r4.u32 + 6));
  if (q == 0) {
    return;
  }
  AtomicStore(base, q + kWrite, (AtomicLoad(base, q + kWrite) + kSlots - 1) % kSlots);
}

int Mode() { return REXCVAR_GET(skate3_native_padring_replace) ? 2 : 0; }

skate3::native_replace::Family g_pop{"ring_pop", &Mode, nullptr, &BodyPop};
skate3::native_replace::Family g_push{"ring_push", &Mode, nullptr, &BodyPush};
skate3::native_replace::Family g_unpush{"ring_unpush", &Mode, nullptr, &BodyUnpush};

}  // namespace

extern "C" REX_FUNC(sub_8296D1F0) {
  skate3::native_replace::Run(g_pop, ctx, base, &__imp__sub_8296D1F0);
}
extern "C" REX_FUNC(sub_8296D0D0) {
  skate3::native_replace::Run(g_push, ctx, base, &__imp__sub_8296D0D0);
}
extern "C" REX_FUNC(sub_826991B8) {
  skate3::native_replace::Run(g_unpush, ctx, base, &__imp__sub_826991B8);
}
