// EA::Jobs worker wake-ups without the thundering herd (see the header).

#include "native/skate3_native_jobs.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "generated/skate3_init.h"
#include "harness/harness.h"
#include "harness/harness_profile.h"

REXCVAR_DEFINE_STRING(skate3_native_job_wake, "on", "Skate 3",
                      "EA::Jobs WakeThreads (sub_8290B320): off = signal every worker on every "
                      "call (the game); on = signal only workers that may be waiting, once per "
                      "wait (no lost wakes: see skate3_native_jobs.h). Cuts wasted wake-ups")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_job_stats, false, "Skate 3",
                    "EA::Jobs counters: RunSingleJob calls/hits (workers, other threads and "
                    "their callers), worker waits, wake calls and signals; `jobs:` line every "
                    "120 frames")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_jobs {
namespace {

constexpr int kMaxWorkers = 32;
constexpr uint32_t kWorkerEvent = 0x2C;     // worker: its wake event (handle)
constexpr uint32_t kBackendSelfRun = 1597;  // backend: u8, the caller runs jobs itself
constexpr uint32_t kBackendSlots = 76;      // backend: slot i event at +76 + 48 i
constexpr uint32_t kSlotStride = 48;
constexpr uint32_t kSlotActive = 36;        // slot event - 36: u8 active

struct Worker {
  std::atomic<uint32_t> event{0};
  std::atomic<bool> armed{false};
};
Worker g_workers[kMaxWorkers];
std::atomic<int> g_worker_count{0};
std::mutex g_register_mu;

thread_local Worker* t_worker = nullptr;
thread_local int t_last_wait = 0;  // 1 signalled, 2 timed out

std::atomic<bool> g_on{false};
std::atomic<bool> g_stats{false};

struct Stats {
  std::atomic<uint64_t> rsj_calls[2], rsj_hits[2];  // [0] other threads, [1] workers
  std::atomic<uint64_t> waits, wait_us, signalled, timeouts;
  std::atomic<uint64_t> hit_after_signal, hit_after_timeout, after_signal, after_timeout;
  std::atomic<uint64_t> wake_calls, signals, skipped, self_run;
  std::atomic<uint32_t> caller_lr[8];
  std::atomic<uint64_t> caller_n[8];
};
Stats g_s;
uint64_t g_frames = 0;

inline void Add(std::atomic<uint64_t>& a, uint64_t n = 1) {
  a.fetch_add(n, std::memory_order_relaxed);
}

inline uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t raw;
  std::memcpy(&raw, base + addr, 4);
  return __builtin_bswap32(raw);
}

Worker* FindWorker(uint32_t event) {
  const int n = g_worker_count.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    if (g_workers[i].event.load(std::memory_order_relaxed) == event) return &g_workers[i];
  }
  return nullptr;
}

void RegisterWorker(uint32_t event) {
  std::lock_guard lk(g_register_mu);
  if (Worker* w = FindWorker(event)) {
    t_worker = w;
    return;
  }
  const int n = g_worker_count.load(std::memory_order_relaxed);
  if (n >= kMaxWorkers) return;
  g_workers[n].event.store(event, std::memory_order_relaxed);
  g_workers[n].armed.store(false, std::memory_order_relaxed);
  g_worker_count.store(n + 1, std::memory_order_release);
  t_worker = &g_workers[n];
  REXLOG_INFO("jobs: worker {} event {:08X}", n, event);
}

void NoteCaller(uint32_t lr) {
  for (int i = 0; i < 8; ++i) {
    uint32_t cur = g_s.caller_lr[i].load(std::memory_order_relaxed);
    if (cur == 0 && g_s.caller_lr[i].compare_exchange_strong(cur, lr)) cur = lr;
    if (cur == lr) {
      Add(g_s.caller_n[i]);
      return;
    }
  }
}

}  // namespace

void OnFrameBuilt() {
  g_on.store(REXCVAR_GET(skate3_native_job_wake) == "on", std::memory_order_relaxed);
  g_stats.store(REXCVAR_GET(skate3_native_job_stats), std::memory_order_relaxed);
  if (++g_frames % 120 != 0 || !g_stats.load(std::memory_order_relaxed)) return;
  const auto x = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
  std::string callers;
  for (int i = 0; i < 8; ++i) {
    const uint32_t lr = g_s.caller_lr[i].load(std::memory_order_relaxed);
    if (lr) callers += fmt::format(" {:08X}:{}", lr, x(g_s.caller_n[i]));
  }
  REXLOG_INFO(
      "jobs: frame {} wake={} wake_calls={} signals={} skipped={} self_run={} "
      "worker_rsj={}/{} waits={} wait_ms={:.1f} signalled={} (hit {}/{}) timeouts={} "
      "(late {}/{}) other_rsj={}/{} callers{}",
      g_frames, g_on.load() ? "on" : "off", x(g_s.wake_calls), x(g_s.signals), x(g_s.skipped),
      x(g_s.self_run), x(g_s.rsj_hits[1]), x(g_s.rsj_calls[1]), x(g_s.waits),
      x(g_s.wait_us) / 1000.0, x(g_s.signalled), x(g_s.hit_after_signal), x(g_s.after_signal),
      x(g_s.timeouts), x(g_s.hit_after_timeout), x(g_s.after_timeout), x(g_s.rsj_hits[0]),
      x(g_s.rsj_calls[0]), callers);
}

}  // namespace skate3::native_jobs

using namespace skate3::native_jobs;

// EA::Jobs LocalBackend::RunSingleJob(backend, thread mask) (Skate 2 name):
// workers and the main thread run jobs through it; returns nonzero (u8) when
// it ran one. Registers the job threads with the harness profiler.
extern "C" REX_FUNC(sub_8290AFA0) {
  if (skate3::harness::Enabled()) {
    skate3::harness::profile::NoteThread("job");
  }
  Worker* w = t_worker;
  if (w) {
    // Arm before the scan (see the header): a job added after the scan
    // started finds the worker armed.
    w->armed.store(true, std::memory_order_seq_cst);
  }
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_8290AFA0(ctx, base);
  const bool hit = (ctx.r3.u32 & 0xFF) != 0;
  if (g_stats.load(std::memory_order_relaxed)) {
    const int k = w ? 1 : 0;
    Add(g_s.rsj_calls[k]);
    if (hit) Add(g_s.rsj_hits[k]);
    if (!w) NoteCaller(lr);
    if (t_last_wait) {
      Add(t_last_wait == 1 ? g_s.after_signal : g_s.after_timeout);
      if (hit) Add(t_last_wait == 1 ? g_s.hit_after_signal : g_s.hit_after_timeout);
    }
  }
  t_last_wait = 0;
}

// The worker loop, sub_8290B750(worker): registers the thread as a worker
// (keyed by its event, worker+0x2C).
extern "C" REX_FUNC(sub_8290B750) {
  RegisterWorker(Load32(base, ctx.r3.u32 + kWorkerEvent));
  __imp__sub_8290B750(ctx, base);
}

// sub_82EDFEC0(handle, timeout_ms): WaitForSingleObjectEx(handle, timeout,
// FALSE). 105 callers; only the workers' waits are counted.
extern "C" REX_FUNC(sub_82EDFEC0) {
  if (!t_worker || !g_stats.load(std::memory_order_relaxed)) {
    __imp__sub_82EDFEC0(ctx, base);
    if (t_worker) t_last_wait = ctx.r3.u32 == 0 ? 1 : ctx.r3.u32 == 0x102 ? 2 : 0;
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_82EDFEC0(ctx, base);
  Add(g_s.wait_us, std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count());
  Add(g_s.waits);
  const uint32_t r = ctx.r3.u32;
  t_last_wait = r == 0 ? 1 : r == 0x102 ? 2 : 0;
  if (r == 0) Add(g_s.signalled);
  else if (r == 0x102) Add(g_s.timeouts);
}

// EA::Jobs LocalBackend::WakeThreads(backend) (Skate 2 name, virtual).
extern "C" REX_FUNC(sub_8290B320) {
  const bool stats = g_stats.load(std::memory_order_relaxed);
  if (stats) Add(g_s.wake_calls);
  const uint32_t self = ctx.r3.u32;
  if (!g_on.load(std::memory_order_relaxed) || base[self + kBackendSelfRun] != 0) {
    if (stats && base[self + kBackendSelfRun] != 0) Add(g_s.self_run);
    __imp__sub_8290B320(ctx, base);
    return;
  }
  // The caller's job is published (guest atomics); order it before the
  // arm-flag reads.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  for (uint32_t i = 0; i < 32; ++i) {
    const uint32_t slot = self + kBackendSlots + kSlotStride * i;
    if (base[slot - kSlotActive] == 0) continue;
    const uint32_t event = Load32(base, slot);
    Worker* w = FindWorker(event);
    if (w && !w->armed.exchange(false, std::memory_order_seq_cst)) {
      if (stats) Add(g_s.skipped);
      continue;
    }
    if (stats) Add(g_s.signals);
    ctx.r3.u64 = event;
    sub_82EE1DE8(ctx, base);  // SetEvent
  }
}
