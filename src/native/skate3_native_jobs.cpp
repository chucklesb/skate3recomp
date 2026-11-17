// EA::Jobs worker wake-ups without the thundering herd (see the header).

#include "native/skate3_native_jobs.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <tuple>
#include <vector>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "generated/skate3_init.h"
#include "native/skate3_native_guest_frame.h"
#include "native/skate3_native_replace.h"
#include "harness/harness.h"
#include "harness/harness_profile.h"

REXCVAR_DEFINE_STRING(skate3_native_job_wake, "on", "Skate 3",
                      "EA::Jobs WakeThreads (sub_8290B320): off = signal every worker on every "
                      "call (the game); on = signal only workers that may be waiting, once per "
                      "wait (no lost wakes: see skate3_native_jobs.h). Cuts wasted wake-ups")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_jobs_replace, true, "Skate 3",
                    "RE-7: native bodies for the EA::Jobs worker loop, WakeThreads, the "
                    "worker wait, the sleep-timeout getter/setter and the job spin-wait "
                    "(replace: j_* families); off = the recompiled functions. The worker "
                    "loop's mode is fixed when each job thread starts")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_wait_probe, false, "Skate 3",
                    "Probe: every 2 s, log the guest waits (WaitForSingleObjectEx) that took "
                    "over 5 ms, by caller and thread (wait-probe: lines)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_job_stats, false, "Skate 3",
                    "EA::Jobs counters: RunSingleJob calls/hits (workers, other threads and "
                    "their callers), worker waits, wake calls and signals; `jobs:` line every "
                    "120 frames")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_jobs {
namespace {

constexpr int kMaxWorkers = 32;

using rex::be;

// EA::Jobs, as far as these functions use it (docs/re/types.md "EA::Jobs").
// A worker slot in the backend, one per job thread.
struct JobSlot {
  uint8_t active;           // +0
  uint8_t unknown_01[35];
  be<uint32_t> wake_event;  // +36  the worker's event handle
  uint8_t unknown_28[8];
};
static_assert(sizeof(JobSlot) == 48);

// LocalBackend (Skate 2 name): runs jobs (vtable +0x60 RunSingleJob(mask)).
struct JobBackend {
  be<uint32_t> vtable;
  uint8_t unknown_04[36];
  JobSlot slots[32];              // +40
  uint8_t unknown_628[16];
  be<uint32_t> sleep_timeout_ms;  // +1592
  uint8_t unknown_63c;
  uint8_t caller_runs_jobs;       // +1597: no workers, the caller runs them
};
static_assert(offsetof(JobBackend, slots) == 40);
static_assert(offsetof(JobBackend, sleep_timeout_ms) == 1592);
static_assert(offsetof(JobBackend, caller_runs_jobs) == 1597);

// A job thread's state (the worker loop's argument).
struct JobWorker {
  be<uint32_t> unknown_00;
  be<uint32_t> backend;      // +4
  uint8_t unknown_08;
  uint8_t quit;              // +9
  uint8_t unknown_0a[22];
  be<uint32_t> thread_mask;  // +32  which jobs it may run
  uint8_t unknown_24[8];
  be<uint32_t> wake_event;   // +44
};
static_assert(offsetof(JobWorker, thread_mask) == 32);
static_assert(offsetof(JobWorker, wake_event) == 44);

// The 16-byte job handle the spin-wait polls (at +64 of its argument),
// passed by value to its owner's IsDone as two 64-bit halves.
struct JobHandle {
  be<uint64_t> id;      // +0
  be<uint32_t> owner;   // +8   answers IsDone
  be<uint16_t> serial;  // +12
  be<uint16_t> unknown_0e;
  bool Valid() const { return owner != 0 && (serial != 0 || id != 0); }
};
static_assert(sizeof(JobHandle) == 16);

// A guest object by address (REX_PHYS_HOST_OFFSET: the physical mirror at
// 0xE0000000+ is shifted on some hosts).
template <typename T>
T& At(uint8_t* base, uint32_t addr) {
  return *reinterpret_cast<T*>(base + addr + REX_PHYS_HOST_OFFSET(addr));
}

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
namespace native_replace = skate3::native_replace;

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

// --- Native bodies (RE-7 Body families, skate3_native_jobs_replace) --------
// Each follows the disassembly call for call: the same guest calls with the
// same arguments, the same stack frame size (callees may use the frame
// above their incoming r1), the same return addresses in LR (the caller
// histogram reads them) and the same r3 on return. Non-volatile guest
// registers are left untouched (the originals save and restore the ones
// they use).

namespace {

using skate3::native_guest::CallMethod;

// The original's stack frame: the back chain only, popped on exit.
struct GuestFrame {
  PPCContext& ctx;
  uint64_t saved;
  GuestFrame(PPCContext& c, uint8_t* base, uint32_t size) : ctx(c), saved(c.r1.u64) {
    const uint32_t sp = c.r1.u32 - size;
    REX_STORE_U32(sp, c.r1.u32);
    ctx.r1.u64 = sp;
  }
  ~GuestFrame() { ctx.r1.u64 = saved; }
};

constexpr uint32_t kVtRunSingleJob = 0x60;     // backend vtable
constexpr uint32_t kVtSleepTimeout = 0x64;     // backend vtable: GetJobThreadSleepTimeoutMS
constexpr uint32_t kVtIsDone = 0x30;           // job-handle owner vtable
constexpr uint32_t kJobManagerGlobal = 0x830CFE04;

// GetJobThreadSleepTimeoutMS (sub_82909738).
void BodyGetTimeout(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = At<JobBackend>(base, ctx.r3.u32).sleep_timeout_ms;
}

// SetJobThreadSleepTimeoutMS (sub_82909740).
void BodySetTimeout(PPCContext& ctx, uint8_t* base) {
  At<JobBackend>(base, ctx.r3.u32).sleep_timeout_ms = ctx.r4.u32;
}

// sub_82EDFEC0: WaitForSingleObject(handle, ms) = WaitForSingleObjectEx(handle,
// ms, not alertable), a tail call.
void BodyWait(PPCContext& ctx, uint8_t* base) {
  ctx.r5.u64 = 0;
  sub_82EE7880(ctx, base);
}

// sub_82EEEA98(r3 out, r4 ms): the kernel timeout for a Win32 one. -1
// (INFINITE) returns 0 (no timeout); otherwise *(s64*)out = u32(ms) *
// -10000 (relative, 100 ns units) and returns out (r3 unchanged).
void BodyTimeout(PPCContext& ctx, uint8_t* base) {
  if (ctx.r4.s32 == -1) {
    ctx.r3.u64 = 0;
    return;
  }
  At<be<uint64_t>>(base, ctx.r3.u32) = uint64_t(ctx.r4.u32) * uint64_t(-10000ll);
}

// sub_82EE6260(status): SetLastError from an NTSTATUS, frame 96. Converts it
// (RtlNtStatusToDosError), stores it as the current thread's last error
// unless the processor block's flag at +336 suppresses that, and returns it.
// r13 is the processor control region (Xbox 360 kernel): +256 the current
// thread, whose +352 is its last error.
constexpr uint32_t kPcrCurrentThread = 256;
constexpr uint32_t kPcrSuppressLastError = 336;  // purpose inferred from this use
constexpr uint32_t kThreadLastError = 352;
void BodySetLastError(PPCContext& ctx, uint8_t* base) {
  GuestFrame frame(ctx, base, 96);
  ctx.lr = 0x82EE6270;
  __imp__RtlNtStatusToDosError(ctx, base);  // r3 = the DOS error
  // Kernel blocks may sit above the physical mirror: the guest accessors.
  const uint32_t pcr = ctx.r13.u32;
  if (REX_LOAD_U32(pcr + kPcrSuppressLastError) == 0) {
    const uint32_t thread = REX_LOAD_U32(pcr + kPcrCurrentThread);
    REX_STORE_U32(thread + kThreadLastError, ctx.r3.u32);
  }
}

// WaitForSingleObjectEx (sub_82EE7880: r3 handle, r4 timeout ms, r5
// alertable), frame 128: sub_82EEEA98(sp+80, ms) makes the kernel timeout
// (its pointer, or 0 for INFINITE); then NtWaitForSingleObjectEx (import
// sub_82F9D134: handle, mode 1, alertable & 0xFF, timeout), again while
// alertable and the status is 257 (STATUS_ALERTED); a negative status sets
// the last error (sub_82EE6260) and returns -1, otherwise the status.
void BodyWaitEx(PPCContext& ctx, uint8_t* base) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t alertable = ctx.r5.u32;
  GuestFrame frame(ctx, base, 128);
  ctx.r3.u64 = ctx.r1.u32 + 80;  // r4 (ms) passes through
  ctx.lr = 0x82EE789C;
  sub_82EEEA98(ctx, base);
  const uint32_t timeout = ctx.r3.u32;
  for (;;) {
    ctx.r3.u64 = handle;
    ctx.r4.u64 = 1;
    ctx.r5.u64 = alertable & 0xFF;
    ctx.r6.u64 = timeout;
    ctx.lr = 0x82EE78B8;
    __imp__NtWaitForSingleObjectEx(ctx, base);
    const int32_t status = ctx.r3.s32;
    if (status < 0) {
      ctx.lr = 0x82EE78D8;
      sub_82EE6260(ctx, base);
      ctx.r3.s64 = -1;
      return;
    }
    if (alertable == 0 || status != 257) return;
  }
}

// The worker loop sub_8290B750(worker), frame 96.
void BodyWorker(PPCContext& ctx, uint8_t* base) {
  // Run jobs until none is left, then sleep on the wake event (with the
  // backend's timeout); until told to quit.
  const auto& worker = At<JobWorker>(base, ctx.r3.u32);
  GuestFrame frame(ctx, base, 96);
  while (worker.quit == 0) {
    ctx.r4.u64 = worker.thread_mask;
    CallMethod(ctx, base, worker.backend, kVtRunSingleJob, 0x8290B788);
    if ((ctx.r3.u32 & 0xFF) != 0) continue;  // ran one: look for more
    CallMethod(ctx, base, worker.backend, kVtSleepTimeout, 0x8290B7A8);
    ctx.r4.u64 = ctx.r3.u64;  // timeout ms
    ctx.r3.u64 = worker.wake_event;
    ctx.lr = 0x8290B7B4;
    sub_82EDFEC0(ctx, base);  // WaitForSingleObject
  }
  ctx.r3.u64 = 0;
}

// The spin-wait sub_824741F0(?, handle, mode), frame 112: mode 2 runs jobs
// with mask 4, mode 0 with mask 1, until the handle is done; other modes
// return at once.
void BodySpinWait(PPCContext& ctx, uint8_t* base) {
  const uint32_t h = ctx.r4.u32;
  const int32_t mode = ctx.r5.s32;
  if (mode != 0 && mode != 2) return;
  const uint32_t mask = mode == 2 ? 4 : 1;
  const uint32_t ret_done = mode == 2 ? 0x82474274 : 0x82474308;
  const uint32_t ret_run = mode == 2 ? 0x824742A8 : 0x8247433C;
  GuestFrame frame(ctx, base, 112);
  const auto& handle = At<JobHandle>(base, h + 64);
  for (;;) {
    // Done? An empty handle counts as done; otherwise ask its owner, passing
    // the handle by value (two 64-bit halves).
    if (handle.Valid()) {
      ctx.r4.u64 = handle.id;
      ctx.r5.u64 = At<be<uint64_t>>(base, h + 72);  // owner, serial, unknown_0e
      CallMethod(ctx, base, handle.owner, kVtIsDone, ret_done);
    } else {
      ctx.r3.u64 = 1;
    }
    if ((ctx.r3.u32 & 0xFF) != 0) return;
    // Not yet: run a job meanwhile, on the main backend.
    const uint32_t holder = At<be<uint32_t>>(base, At<be<uint32_t>>(base, kJobManagerGlobal) + 8);
    ctx.r4.u64 = mask;
    CallMethod(ctx, base, At<be<uint32_t>>(base, holder), kVtRunSingleJob, ret_run);
  }
}

bool StatsOn() { return g_stats.load(std::memory_order_relaxed); }

// WakeThreads sub_8290B320(backend), frame 112. backend+1597 set: run jobs
// on the caller (RunSingleJob mask 63) until none is left. Otherwise
// SetEvent each active worker slot's event; with skate3_native_job_wake on,
// only armed workers (see the header).
void BodyWake(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  GuestFrame frame(ctx, base, 112);
  if (At<JobBackend>(base, self).caller_runs_jobs != 0) {
    if (StatsOn()) Add(g_s.self_run);
    do {
      ctx.r4.u64 = 63;  // any job
      CallMethod(ctx, base, self, kVtRunSingleJob, 0x8290B35C);
    } while ((ctx.r3.u32 & 0xFF) != 0);
    return;
  }
  const bool selective = g_on.load(std::memory_order_relaxed);
  if (selective) {
    // The caller's job is published (guest atomics); order it before the
    // arm-flag reads.
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  for (const JobSlot& slot : At<JobBackend>(base, self).slots) {
    if (slot.active == 0) continue;
    const uint32_t event = slot.wake_event;
    if (selective) {
      Worker* w = FindWorker(event);
      if (w && !w->armed.exchange(false, std::memory_order_seq_cst)) {
        if (StatsOn()) Add(g_s.skipped);
        continue;
      }
    }
    if (StatsOn()) Add(g_s.signals);
    ctx.r3.u64 = event;
    ctx.lr = 0x8290B388;
    sub_82EE1DE8(ctx, base);  // SetEvent
  }
}

int BodyMode() { return REXCVAR_GET(skate3_native_jobs_replace) ? 2 : 0; }

native_replace::Family g_f_get_timeout{"j_gettmo", &BodyMode, nullptr, &BodyGetTimeout};
native_replace::Family g_f_set_timeout{"j_settmo", &BodyMode, nullptr, &BodySetTimeout};
native_replace::Family g_f_wait{"j_wait", &BodyMode, nullptr, &BodyWait};
native_replace::Family g_f_wait_ex{"j_waitex", &BodyMode, nullptr, &BodyWaitEx};
native_replace::Family g_f_timeout{"k_timeout", &BodyMode, nullptr, &BodyTimeout};
native_replace::Family g_f_last_error{"k_lasterr", &BodyMode, nullptr, &BodySetLastError};
native_replace::Family g_f_worker{"j_worker", &BodyMode, nullptr, &BodyWorker};
native_replace::Family g_f_wake{"j_wake", &BodyMode, nullptr, &BodyWake};
native_replace::Family g_f_spin{"j_spin", &BodyMode, nullptr, &BodySpinWait};

// The recompiled WakeThreads, with the selective signalling when the body is
// off (the earlier hook; kept so off = the game exactly when job_wake is off).
void WakeRecompiled(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  if (!g_on.load(std::memory_order_relaxed) || At<JobBackend>(base, self).caller_runs_jobs != 0) {
    if (StatsOn() && At<JobBackend>(base, self).caller_runs_jobs != 0) Add(g_s.self_run);
    __imp__sub_8290B320(ctx, base);
    return;
  }
  BodyWake(ctx, base);
}

}  // namespace

extern "C" REX_FUNC(sub_82909738) {
  native_replace::Run(g_f_get_timeout, ctx, base, &__imp__sub_82909738);
}

extern "C" REX_FUNC(sub_82909740) {
  native_replace::Run(g_f_set_timeout, ctx, base, &__imp__sub_82909740);
}

// The worker loop, sub_8290B750(worker): registers the thread as a worker
// (keyed by its event, worker+0x2C). It runs for the thread's life, so the
// mode is fixed when the thread starts.
extern "C" REX_FUNC(sub_8290B750) {
  RegisterWorker(At<JobWorker>(base, ctx.r3.u32).wake_event);
  native_replace::Run(g_f_worker, ctx, base, &__imp__sub_8290B750);
}

// sub_82EDFEC0(handle, timeout_ms): WaitForSingleObjectEx(handle, timeout,
// FALSE). 105 callers; only the workers' waits are counted.
extern "C" REX_FUNC(sub_82EDFEC0) {
  if (!t_worker || !StatsOn()) {
    native_replace::Run(g_f_wait, ctx, base, &__imp__sub_82EDFEC0);
    if (t_worker) t_last_wait = ctx.r3.u32 == 0 ? 1 : ctx.r3.u32 == 0x102 ? 2 : 0;
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  native_replace::Run(g_f_wait, ctx, base, &__imp__sub_82EDFEC0);
  Add(g_s.wait_us, std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count());
  Add(g_s.waits);
  const uint32_t r = ctx.r3.u32;
  t_last_wait = r == 0 ? 1 : r == 0x102 ? 2 : 0;
  if (r == 0) Add(g_s.signalled);
  else if (r == 0x102) Add(g_s.timeouts);
}

// The Win32 timeout and last-error helpers WaitForSingleObjectEx uses.
extern "C" REX_FUNC(sub_82EEEA98) {
  native_replace::Run(g_f_timeout, ctx, base, &__imp__sub_82EEEA98);
}

extern "C" REX_FUNC(sub_82EE6260) {
  native_replace::Run(g_f_last_error, ctx, base, &__imp__sub_82EE6260);
}

// Probe (skate3_native_wait_probe): long waits by caller and thread.
namespace {
struct WaitKey {
  uint32_t lr, thread, handle;
  bool operator<(const WaitKey& o) const {
    return std::tie(lr, thread, handle) < std::tie(o.lr, o.thread, o.handle);
  }
};
struct WaitSum {
  uint32_t count = 0;
  double ms = 0;
};
std::mutex g_wait_mu;
std::map<WaitKey, WaitSum> g_waits;
std::chrono::steady_clock::time_point g_wait_last{};

void NoteWait(uint32_t lr, uint32_t thread, uint32_t handle, double ms) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lk(g_wait_mu);
  if (ms > 5.0) {
    WaitSum& w = g_waits[{lr, thread, handle}];
    ++w.count;
    w.ms += ms;
  }
  if (now - g_wait_last < std::chrono::seconds(2)) return;
  g_wait_last = now;
  if (g_waits.empty()) return;
  std::vector<std::pair<WaitKey, WaitSum>> v(g_waits.begin(), g_waits.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.ms > b.second.ms; });
  std::string line;
  for (size_t i = 0; i < v.size() && i < 8; ++i) {
    line += fmt::format(" | lr={:08X} thr={:08X} h={:08X} n={} {:.0f}ms", v[i].first.lr,
                        v[i].first.thread, v[i].first.handle, v[i].second.count, v[i].second.ms);
  }
  REXLOG_INFO("wait-probe: last 2 s, waits over 5 ms:{}", line);
  g_waits.clear();
}
}  // namespace

// WaitForSingleObjectEx (callers: WaitForSingleObject sub_82EDFEC0 and one more).
extern "C" REX_FUNC(sub_82EE7880) {
  if (!REXCVAR_GET(skate3_native_wait_probe)) {
    native_replace::Run(g_f_wait_ex, ctx, base, &__imp__sub_82EE7880);
    return;
  }
  const uint32_t lr = ctx.lr, handle = ctx.r3.u32, thread = ctx.r13.u32;
  const auto t0 = std::chrono::steady_clock::now();
  native_replace::Run(g_f_wait_ex, ctx, base, &__imp__sub_82EE7880);
  NoteWait(lr, thread, handle,
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// EA::Jobs LocalBackend::WakeThreads(backend) (Skate 2 name, virtual).
extern "C" REX_FUNC(sub_8290B320) {
  if (StatsOn()) Add(g_s.wake_calls);
  native_replace::Run(g_f_wake, ctx, base, &WakeRecompiled);
}

// The spin-wait on a job (sub_824741F0).
extern "C" REX_FUNC(sub_824741F0) {
  native_replace::Run(g_f_spin, ctx, base, &__imp__sub_824741F0);
}
