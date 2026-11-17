// D3D fence waits without spinning (see the header).

#include "native/skate3_native_fence.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/gpu_writeback.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "generated/skate3_init.h"

REXCVAR_DEFINE_STRING(skate3_native_fence_wait, "on", "Skate 3",
                      "D3D fence waits (BlockOnFence's poll, sub_82B76080): off = the game's "
                      "spin loop; on = block until the emulated GPU's next memory write-back "
                      "when the fence word has not moved (2 ms timeout), then poll as the "
                      "game does. Saves the CPU the spin burns (fence: lines)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_fence_wait_timeout_us, 2000, "Skate 3",
                     "skate3_native_fence_wait: longest block before polling anyway (us)")
    .range(50, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_fence {
namespace {

constexpr uint32_t kDoneFencePtr = 10896;  // device: pointer to the GPU-written fence word

std::atomic<bool> g_on{false};
std::atomic<int> g_timeout_us{2000};
std::atomic<uint64_t> c_polls{0}, c_waits{0}, c_wakes{0}, c_timeouts{0}, c_moved{0};
// Timeouts after which the word HAD moved: a write-back nobody signalled.
std::atomic<uint64_t> c_missed{0};
std::atomic<uint64_t> c_wait_us{0};
uint64_t g_frames = 0;

// As the recompiled code reads it (REX_LOAD_U32): on Windows the physical
// mirror at 0xE0000000+ sits 0x1000 higher in the host mapping. The GPU's
// fence word is there (e.g. 0xFFCA3000).
inline bool Read32(const uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x10000 || addr > 0xFFFFFFFCu) return false;
#if defined(_WIN32)
  const uint32_t host_off = addr >= 0xE0000000u ? 0x1000u : 0u;
#else
  const uint32_t host_off = 0;
#endif
  uint32_t raw;
  std::memcpy(&raw, base + addr + host_off, 4);
  *out = __builtin_bswap32(raw);
  return true;
}

// state = r3 of the poll: +0 device, +8 the completed-fence value seen last.
// Blocks while the GPU has written nothing new.
void BeforePoll(const uint8_t* base, uint32_t state) {
  c_polls.fetch_add(1, std::memory_order_relaxed);
  uint32_t dev = 0, seen = 0, done_ptr = 0, done = 0;
  if (!Read32(base, state, &dev) || !Read32(base, state + 8, &seen) ||
      !Read32(base, dev + kDoneFencePtr, &done_ptr)) {
    return;
  }
  // Sequence first, then the word: a write-back in between changes the
  // sequence, so the wait returns at once.
  const uint32_t seq = rex::graphics::GpuWritebackSeq();
  if (!Read32(base, done_ptr, &done)) return;
  if (done != seen) {
    c_moved.fetch_add(1, std::memory_order_relaxed);  // progress: poll now
    return;
  }
  c_waits.fetch_add(1, std::memory_order_relaxed);
  const auto t0 = std::chrono::steady_clock::now();
  const bool woke = rex::graphics::WaitGpuWriteback(
      seq, std::chrono::microseconds(g_timeout_us.load(std::memory_order_relaxed)));
  c_wait_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count(),
                      std::memory_order_relaxed);
  (woke ? c_wakes : c_timeouts).fetch_add(1, std::memory_order_relaxed);
  uint32_t after = 0;
  if (!woke && Read32(base, done_ptr, &after) && after != seen) {
    c_missed.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace

void OnFrameBuilt() {
  g_on.store(REXCVAR_GET(skate3_native_fence_wait) == "on", std::memory_order_relaxed);
  g_timeout_us.store(REXCVAR_GET(skate3_native_fence_wait_timeout_us), std::memory_order_relaxed);
  if (++g_frames % 120 != 0 || !g_on.load(std::memory_order_relaxed)) return;
  const auto x = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
  REXLOG_INFO("fence: frame {} polls={} moved={} waits={} wakes={} timeouts={} missed={} "
              "wait_us={}",
              g_frames, x(c_polls), x(c_moved), x(c_waits), x(c_wakes), x(c_timeouts),
              x(c_missed), x(c_wait_us));
}

}  // namespace skate3::native_fence

// The D3D wait poll (one turn of BlockOnFence and the other D3D waits).
extern "C" REX_FUNC(sub_82B76080) {
  if (skate3::native_fence::g_on.load(std::memory_order_relaxed)) {
    skate3::native_fence::BeforePoll(base, ctx.r3.u32);
  }
  __imp__sub_82B76080(ctx, base);
}

// Probe (skate3_native_wait_probe): time spent in D3D::CDevice::BlockOnFence
// (sub_82B755C0) per thread, logged every 2 s ("fence-probe:"). Finds threads
// that block on the GPU (e.g. the main thread while the boot loads).
namespace {
std::atomic<uint64_t> g_bof_us_main{0}, g_bof_n_main{0}, g_bof_us_other{0}, g_bof_n_other{0};
std::atomic<uint32_t> g_bof_main_thread{0};
std::atomic<int64_t> g_bof_last_ms{0};
}  // namespace

extern "C" REX_FUNC(sub_82B755C0) {
  if (!rex::cvar::Query<bool>("skate3_native_wait_probe")) {
    __imp__sub_82B755C0(ctx, base);
    return;
  }
  const uint32_t thread = ctx.r13.u32;
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_82B755C0(ctx, base);
  const auto t1 = std::chrono::steady_clock::now();
  const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  uint32_t main = g_bof_main_thread.load();
  if (main == 0) {
    g_bof_main_thread.compare_exchange_strong(main, thread);
    main = g_bof_main_thread.load();
  }
  if (thread == main) {
    g_bof_us_main += us;
    ++g_bof_n_main;
  } else {
    g_bof_us_other += us;
    ++g_bof_n_other;
  }
  const int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1.time_since_epoch()).count();
  int64_t last = g_bof_last_ms.load();
  if (now_ms - last >= 2000 && g_bof_last_ms.compare_exchange_strong(last, now_ms)) {
    REXLOG_INFO("fence-probe: last 2 s BlockOnFence: thread {:08X} n={} {} ms; others n={} {} ms",
                main, g_bof_n_main.exchange(0), g_bof_us_main.exchange(0) / 1000,
                g_bof_n_other.exchange(0), g_bof_us_other.exchange(0) / 1000);
  }
}
