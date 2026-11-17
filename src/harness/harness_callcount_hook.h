#pragma once
// Force-included into the recompiled sources when the build option
// SKATE3_COUNT_GUEST_CALLS is on (CMakeLists.txt; default off). The
// generated code defines DEFINE_REX_FUNC only #ifndef, so this version wins:
// each guest function's weak entry point counts its calls, then tail-calls
// the recompiled body (__imp__). A hook (a strong definition in src/)
// replaces the counting entry point, so hooked functions aren't counted.
//
// A diagnostic for ranking which functions to reverse engineer next
// (tools/re/leaves.py rank); it replaces nothing. The harness `callcount`
// step resets and dumps the counts (harness_callcount.h).
#include <atomic>
#include <cstdint>

namespace skate3::callcount {

struct Counter {
  const char* name;
  std::atomic<uint64_t> calls{0};
  Counter* next;
  explicit Counter(const char* n);
};

}  // namespace skate3::callcount

#define DEFINE_REX_FUNC(name)                                                    \
  extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*);               \
  static ::skate3::callcount::Counter name##_calls{#name};                       \
  __attribute__((weak, noinline)) extern "C" void name(PPCContext& __restrict ctx, \
                                                       uint8_t* base) {          \
    name##_calls.calls.fetch_add(1, std::memory_order_relaxed);                 \
    [[clang::musttail]] return __imp__##name(ctx, base);                         \
  }                                                                              \
  extern "C" void __imp__##name([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
