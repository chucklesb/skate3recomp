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

// The original's stack frame: stwu r1,-size(r1) with the back chain, popped
// on exit.
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

inline void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot_off,
                        uint32_t ret) {
  const uint32_t fn = REX_LOAD_U32(REX_LOAD_U32(obj) + slot_off);
  ctx.ctr.u64 = fn;
  ctx.lr = ret;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

constexpr uint32_t kVtRunSingleJob = 0x60;     // backend vtable
constexpr uint32_t kVtSleepTimeout = 0x64;     // backend vtable: GetJobThreadSleepTimeoutMS
constexpr uint32_t kVtIsDone = 0x30;           // job-handle owner vtable
constexpr uint32_t kSleepTimeoutField = 1592;  // backend: u32 ms
constexpr uint32_t kJobManagerGlobal = 0x830CFE04;

// GetJobThreadSleepTimeoutMS (sub_82909738): lwz r3,1592(r3).
void BodyGetTimeout(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = REX_LOAD_U32(ctx.r3.u32 + kSleepTimeoutField);
}

// SetJobThreadSleepTimeoutMS (sub_82909740): stw r4,1592(r3).
void BodySetTimeout(PPCContext& ctx, uint8_t* base) {
  REX_STORE_U32(ctx.r3.u32 + kSleepTimeoutField, ctx.r4.u32);
}

// sub_82EDFEC0: li r5,0; b WaitForSingleObjectEx (a tail call: LR kept).
void BodyWait(PPCContext& ctx, uint8_t* base) {
  ctx.r5.u64 = 0;
  sub_82EE7880(ctx, base);
}

// The worker loop sub_8290B750(worker), frame 96.
void BodyWorker(PPCContext& ctx, uint8_t* base) {
  const uint32_t worker = ctx.r3.u32;
  GuestFrame frame(ctx, base, 96);
  while (REX_LOAD_U8(worker + 9) == 0) {
    ctx.r3.u64 = REX_LOAD_U32(worker + 4);
    ctx.r4.u64 = REX_LOAD_U32(worker + 32);
    CallVirtual(ctx, base, ctx.r3.u32, kVtRunSingleJob, 0x8290B788);
    if ((ctx.r3.u32 & 0xFF) != 0) continue;
    ctx.r3.u64 = REX_LOAD_U32(worker + 4);
    CallVirtual(ctx, base, ctx.r3.u32, kVtSleepTimeout, 0x8290B7A8);
    ctx.r4.u64 = ctx.r3.u64;
    ctx.r3.u64 = REX_LOAD_U32(worker + 44);
    ctx.lr = 0x8290B7B4;
    sub_82EDFEC0(ctx, base);
  }
  ctx.r3.s64 = 0;
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
  for (;;) {
    const uint32_t owner = REX_LOAD_U32(h + 72);
    const bool valid = owner != 0 && (REX_LOAD_U16(h + 76) != 0 || REX_LOAD_U64(h + 64) != 0);
    if (valid) {
      ctx.r3.u64 = owner;
      ctx.r4.u64 = REX_LOAD_U64(h + 64);
      ctx.r5.u64 = REX_LOAD_U64(h + 72);
      CallVirtual(ctx, base, owner, kVtIsDone, ret_done);
    } else {
      ctx.r3.u64 = 1;
    }
    if ((ctx.r3.u32 & 0xFF) != 0) return;
    const uint32_t mgr = REX_LOAD_U32(REX_LOAD_U32(kJobManagerGlobal) + 8);
    ctx.r3.u64 = REX_LOAD_U32(mgr);
    ctx.r4.u64 = mask;
    CallVirtual(ctx, base, ctx.r3.u32, kVtRunSingleJob, ret_run);
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
  if (REX_LOAD_U8(self + kBackendSelfRun) != 0) {
    if (StatsOn()) Add(g_s.self_run);
    do {
      ctx.r3.u64 = self;
      ctx.r4.u64 = 63;
      CallVirtual(ctx, base, self, kVtRunSingleJob, 0x8290B35C);
    } while ((ctx.r3.u32 & 0xFF) != 0);
    return;
  }
  const bool selective = g_on.load(std::memory_order_relaxed);
  if (selective) {
    // The caller's job is published (guest atomics); order it before the
    // arm-flag reads.
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  for (uint32_t i = 0; i < 32; ++i) {
    const uint32_t slot = self + kBackendSlots + kSlotStride * i;
    if (REX_LOAD_U8(slot - kSlotActive) == 0) continue;
    const uint32_t event = REX_LOAD_U32(slot);
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
native_replace::Family g_f_worker{"j_worker", &BodyMode, nullptr, &BodyWorker};
native_replace::Family g_f_wake{"j_wake", &BodyMode, nullptr, &BodyWake};
native_replace::Family g_f_spin{"j_spin", &BodyMode, nullptr, &BodySpinWait};

// The recompiled WakeThreads, with the selective signalling when the body is
// off (the earlier hook; kept so off = the game exactly when job_wake is off).
void WakeRecompiled(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  if (!g_on.load(std::memory_order_relaxed) || REX_LOAD_U8(self + kBackendSelfRun) != 0) {
    if (StatsOn() && REX_LOAD_U8(self + kBackendSelfRun) != 0) Add(g_s.self_run);
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
  RegisterWorker(REX_LOAD_U32(ctx.r3.u32 + kWorkerEvent));
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

// EA::Jobs LocalBackend::WakeThreads(backend) (Skate 2 name, virtual).
extern "C" REX_FUNC(sub_8290B320) {
  if (StatsOn()) Add(g_s.wake_calls);
  native_replace::Run(g_f_wake, ctx, base, &WakeRecompiled);
}

// The spin-wait on a job (sub_824741F0).
extern "C" REX_FUNC(sub_824741F0) {
  native_replace::Run(g_f_spin, ctx, base, &__imp__sub_824741F0);
}
