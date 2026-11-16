#include "harness_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/func.h>

// The recompiled image's guest -> host function table (generated/skate3_init.cpp).
extern PPCFuncMapping skate3_PPCFuncMappings[];

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif
#endif

namespace skate3::harness::profile {
namespace {

constexpr int kMaxThreads = 16;
constexpr int kMaxDepth = 16;

struct Sample {
  uint32_t n = 0;
  uint64_t pc[kMaxDepth];
};

#if defined(__linux__)
constexpr uint32_t kRing = 1u << 14;
struct Ring {
  std::atomic<uint32_t> w{0};
  std::atomic<uint32_t> r{0};
  std::atomic<uint32_t> dropped{0};
  Sample s[kRing];
};
#endif

struct ThreadSlot {
  char name[32] = {};
  uint64_t samples = 0;  // stacks recorded
  uint64_t idle = 0;     // Windows: samples with no CPU used
  uint64_t ticks = 0;    // sampling attempts (Windows; the real rate)
#if defined(_WIN32)
  HANDLE handle = nullptr;
  uint64_t last_cycles = 0;
#elif defined(__linux__)
  pid_t tid = 0;
  clockid_t cpu_clock = 0;
  uintptr_t stack_lo = 0, stack_hi = 0;
  timer_t timer = nullptr;
  bool timer_on = false;
  Ring* ring = nullptr;
#endif
};

std::mutex g_mu;  // slots, aggregate, start/stop
ThreadSlot g_slots[kMaxThreads];
std::atomic<int> g_count{0};
std::atomic<bool> g_active{false};
std::atomic<bool> g_stop{false};
int g_hz = 1000;
std::thread g_worker;
std::chrono::steady_clock::time_point g_t0;
// Aggregated stacks: key = thread index byte + raw PCs.
std::unordered_map<std::string, uint32_t> g_stacks;
uint64_t g_dropped = 0;

thread_local bool t_noted = false;

void Add(int slot, const Sample& s) {
  if (s.n == 0) {
    return;
  }
  std::string key;
  key.resize(1 + s.n * sizeof(uint64_t));
  key[0] = static_cast<char>(slot);
  std::memcpy(&key[1], s.pc, s.n * sizeof(uint64_t));
  ++g_stacks[key];
  ++g_slots[slot].samples;
}

#if defined(_WIN32)

void SampleThread(int i) {
  ThreadSlot& t = g_slots[i];
  if (!t.handle) {
    return;
  }
  ULONG64 cycles = 0;
  QueryThreadCycleTime(t.handle, &cycles);
  // Suspending a waiting thread makes it run briefly (the suspend APC), so
  // a waiting thread's counter still moves a little each sample: count it
  // as idle unless it ran for over ~a quarter of the period (at ~2 GHz).
  const uint64_t min_cycles = 500'000'000ull / static_cast<uint64_t>(std::max(1, g_hz));
  const bool ran = cycles - t.last_cycles > min_cycles;
  t.last_cycles = cycles;
  ++t.ticks;
  if (!ran) {
    ++t.idle;
    return;
  }
  if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
    return;
  }
  CONTEXT ctx = {};
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  Sample s;
  if (GetThreadContext(t.handle, &ctx)) {
    // Walk with the image unwind data. No allocation while the thread is
    // suspended (it may hold the heap lock).
    for (int d = 0; d < kMaxDepth && ctx.Rip; ++d) {
      s.pc[s.n++] = ctx.Rip;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
      if (fn) {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                         &establisher, nullptr);
      } else {
        // Leaf without unwind data: the return address is at rsp.
        if (!ctx.Rsp) break;
        ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
      }
    }
  }
  ResumeThread(t.handle);
  Add(i, s);
}

void Worker() {
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  const LONGLONG period_100ns = -static_cast<LONGLONG>(10'000'000 / std::max(1, g_hz));
  while (!g_stop.load(std::memory_order_relaxed)) {
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = period_100ns;
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 100);
    } else {
      Sleep(1);
    }
    std::lock_guard lk(g_mu);
    const int n = g_count.load();
    for (int i = 0; i < n; ++i) {
      SampleThread(i);
    }
  }
  if (timer) CloseHandle(timer);
}

bool StartThread(int, std::string*) { return true; }
void StopThread(int) {}

#elif defined(__linux__)

// SIGPROF handler: runs on the sampled thread itself.
thread_local Ring* t_ring = nullptr;
thread_local uintptr_t t_stack_lo = 0, t_stack_hi = 0;

void OnProf(int, siginfo_t*, void* uctx) {
  Ring* ring = t_ring;
  if (!ring) {
    return;
  }
  const uint32_t w = ring->w.load(std::memory_order_relaxed);
  if (w - ring->r.load(std::memory_order_acquire) >= kRing) {
    ring->dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Sample& s = ring->s[w % kRing];
  s.n = 0;
  const auto* uc = static_cast<const ucontext_t*>(uctx);
  uintptr_t fp = 0;
#if defined(__aarch64__)
  s.pc[s.n++] = uc->uc_mcontext.pc;
  s.pc[s.n++] = uc->uc_mcontext.regs[30];  // LR: the caller in a leaf or prologue
  fp = uc->uc_mcontext.regs[29];
#elif defined(__x86_64__)
  s.pc[s.n++] = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
  fp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
#endif
  // Frame records {prev fp, return address}, only inside this thread's stack.
  while (s.n < kMaxDepth && fp >= t_stack_lo && fp + 16 <= t_stack_hi && (fp & 7) == 0) {
    const auto* rec = reinterpret_cast<const uintptr_t*>(fp);
    if (!rec[1]) break;
    s.pc[s.n++] = rec[1];
    if (rec[0] <= fp) break;
    fp = rec[0];
  }
  ring->w.store(w + 1, std::memory_order_release);
}

void Drain(int i) {
  ThreadSlot& t = g_slots[i];
  if (!t.ring) return;
  Ring& ring = *t.ring;
  const uint32_t w = ring.w.load(std::memory_order_acquire);
  uint32_t r = ring.r.load(std::memory_order_relaxed);
  for (; r != w; ++r) {
    Add(i, ring.s[r % kRing]);
  }
  ring.r.store(r, std::memory_order_release);
  g_dropped += ring.dropped.exchange(0);
}

bool StartThread(int i, std::string* error) {
  ThreadSlot& t = g_slots[i];
  if (t.timer_on) return true;
  sigevent sev = {};
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGPROF;
  sev.sigev_notify_thread_id = t.tid;
  if (timer_create(t.cpu_clock, &sev, &t.timer) != 0) {
    if (error) *error = std::string("timer_create: ") + std::strerror(errno);
    return false;
  }
  const long ns = 1'000'000'000L / std::max(1, g_hz);
  itimerspec its = {};
  its.it_interval.tv_sec = ns / 1'000'000'000L;
  its.it_interval.tv_nsec = ns % 1'000'000'000L;
  its.it_value = its.it_interval;
  timer_settime(t.timer, 0, &its, nullptr);
  t.timer_on = true;
  return true;
}

void StopThread(int i) {
  ThreadSlot& t = g_slots[i];
  if (t.timer_on) {
    timer_delete(t.timer);
    t.timer_on = false;
  }
}

void Worker() {
  while (!g_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::lock_guard lk(g_mu);
    for (int i = 0; i < g_count.load(); ++i) {
      Drain(i);
    }
  }
}

#else
bool StartThread(int, std::string*) { return true; }
void StopThread(int) {}
void Worker() {}
#endif

// Module name and offset for a code address.
struct ModRef {
  std::string name;
  uint64_t base = 0;
};

ModRef ModuleOf(uint64_t pc) {
  ModRef m;
#if defined(_WIN32)
  HMODULE h = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(pc), &h) &&
      h) {
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(h, path, MAX_PATH);
    std::string name;
    for (DWORD k = 0; k < len; ++k) name += static_cast<char>(path[k] < 128 ? path[k] : '_');
    m.name = name.substr(name.find_last_of("\\/") + 1);
    m.base = reinterpret_cast<uint64_t>(h);
  }
#elif defined(__linux__)
  Dl_info info = {};
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fbase) {
    std::string name = info.dli_fname ? info.dli_fname : "";
    m.name = name.substr(name.find_last_of('/') + 1);
    m.base = reinterpret_cast<uint64_t>(info.dli_fbase);
  }
#endif
  if (m.name.empty()) m.name = "?";
  return m;
}

}  // namespace

void NoteThread(const char* name) {
  if (t_noted) {
    return;
  }
  t_noted = true;
  std::lock_guard lk(g_mu);
  const int i = g_count.load();
  if (i >= kMaxThreads) {
    return;
  }
  ThreadSlot& t = g_slots[i];
  std::snprintf(t.name, sizeof(t.name), "%s", name);
#if defined(_WIN32)
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t.handle,
                  THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                  0);
#elif defined(__linux__)
  t.tid = static_cast<pid_t>(syscall(SYS_gettid));
  pthread_getcpuclockid(pthread_self(), &t.cpu_clock);
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* addr = nullptr;
    size_t size = 0;
    if (pthread_attr_getstack(&attr, &addr, &size) == 0) {
      t.stack_lo = reinterpret_cast<uintptr_t>(addr);
      t.stack_hi = t.stack_lo + size;
    }
    pthread_attr_destroy(&attr);
  }
  t.ring = new Ring();
  t_ring = t.ring;
  t_stack_lo = t.stack_lo;
  t_stack_hi = t.stack_hi;
#endif
  g_count.store(i + 1);
  REXLOG_INFO("harness: profile thread {} '{}' registered", i, name);
  if (g_active.load()) {
    StartThread(i, nullptr);
  }
}

bool Active() {
  return g_active.load(std::memory_order_relaxed);
}

bool Start(int hz, std::string* error) {
  std::lock_guard lk(g_mu);
  if (g_active.load()) {
    *error = "the profiler is already running";
    return false;
  }
  g_hz = std::clamp(hz, 10, 10000);
  g_stacks.clear();
  g_dropped = 0;
  for (int i = 0; i < g_count.load(); ++i) {
    g_slots[i].samples = g_slots[i].idle = g_slots[i].ticks = 0;
#if defined(_WIN32)
    if (g_slots[i].handle) QueryThreadCycleTime(g_slots[i].handle, &g_slots[i].last_cycles);
#endif
  }
#if defined(__linux__)
  static bool installed = false;
  if (!installed) {
    struct sigaction sa = {};
    sa.sa_sigaction = OnProf;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, nullptr) != 0) {
      *error = std::string("sigaction(SIGPROF): ") + std::strerror(errno);
      return false;
    }
    installed = true;
  }
#endif
  for (int i = 0; i < g_count.load(); ++i) {
    if (!StartThread(i, error)) {
      for (int j = 0; j < i; ++j) StopThread(j);
      return false;
    }
  }
  g_stop.store(false);
  g_active.store(true);
  g_t0 = std::chrono::steady_clock::now();
  g_worker = std::thread(Worker);
  REXLOG_INFO("harness: profile started at {} Hz ({} thread(s))", g_hz, g_count.load());
  return true;
}

std::string Stop(const std::filesystem::path& dir, uint64_t frames) {
  if (!g_active.exchange(false)) {
    return {};
  }
  {
    std::lock_guard lk(g_mu);
    for (int i = 0; i < g_count.load(); ++i) StopThread(i);
  }
  g_stop.store(true);
  if (g_worker.joinable()) g_worker.join();
  std::lock_guard lk(g_mu);
#if defined(__linux__)
  for (int i = 0; i < g_count.load(); ++i) Drain(i);
#endif
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();

  // Module per distinct PC (cached by PC; modules don't move).
  std::map<uint64_t, std::pair<int, uint64_t>> pc_ref;  // pc -> (module index, offset)
  std::vector<std::string> modules;
  std::map<std::string, int> module_index;
#if defined(_WIN32)
  const uint64_t exe_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
#endif
  int exe_index = -1;
  auto ref = [&](uint64_t pc) -> std::pair<int, uint64_t> {
    auto it = pc_ref.find(pc);
    if (it != pc_ref.end()) return it->second;
    const ModRef m = ModuleOf(pc);
    auto [mi, fresh] = module_index.emplace(m.name, static_cast<int>(modules.size()));
    if (fresh) modules.push_back(m.name);
    uint64_t off = m.base ? pc - m.base : pc;
#if defined(_WIN32)
    // The exe: the function's start from its unwind data, so the report
    // names recompiled functions from the guest table below, with no PDB.
    if (m.base == exe_base) {
      exe_index = mi->second;
      DWORD64 image_base = 0;
      if (PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &image_base, nullptr)) {
        off = fn->BeginAddress;
      }
    }
#endif
    const std::pair<int, uint64_t> r{mi->second, off};
    pc_ref.emplace(pc, r);
    return r;
  };

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(dir / "profile.txt", std::ios::binary | std::ios::trunc);
  uint64_t total = 0;
  out << "# skate3 guest profile: hz " << g_hz << " seconds " << seconds << " frames " << frames
      << " dropped " << g_dropped << "\n";
  for (int i = 0; i < g_count.load(); ++i) {
    out << "thread " << i << " " << g_slots[i].name << " samples " << g_slots[i].samples
        << " idle " << g_slots[i].idle << " ticks " << g_slots[i].ticks << "\n";
    total += g_slots[i].samples;
  }
  // Stacks first (they fill the module table), then the module lines.
  std::string body;
  char buf[48];
  for (const auto& [key, count] : g_stacks) {
    const int slot = static_cast<unsigned char>(key[0]);
    const size_t n = (key.size() - 1) / sizeof(uint64_t);
    body += "s " + std::to_string(count) + " " + std::to_string(slot);
    for (size_t k = 0; k < n; ++k) {
      uint64_t pc;
      std::memcpy(&pc, &key[1 + k * sizeof(uint64_t)], sizeof(pc));
      const auto [mi, off] = ref(pc);
      std::snprintf(buf, sizeof(buf), " %d+%llx", mi, static_cast<unsigned long long>(off));
      body += buf;
    }
    body += "\n";
  }
  for (size_t m = 0; m < modules.size(); ++m) {
    out << "module " << m << " " << modules[m] << "\n";
  }
#if defined(_WIN32)
  if (exe_index >= 0) {
    out << "exe " << exe_index << "\n";
    for (const PPCFuncMapping* fm = skate3_PPCFuncMappings; fm->host; ++fm) {
      const uint64_t host = reinterpret_cast<uint64_t>(fm->host);
      std::snprintf(buf, sizeof(buf), "guest %llx %08X\n",
                    static_cast<unsigned long long>(host - exe_base),
                    static_cast<unsigned>(fm->guest));
      out << buf;
    }
  }
#endif
  out << body;
  out.close();
  REXLOG_INFO("harness: profile stopped: {} samples over {:.1f} s, {} distinct stacks, {} dropped",
              total, seconds, g_stacks.size(), g_dropped);
  char json[256];
  std::snprintf(json, sizeof(json),
                "\"profile\":{\"file\":\"profile.txt\",\"hz\":%d,\"seconds\":%.2f,\"frames\":%llu,"
                "\"samples\":%llu,\"stacks\":%zu,\"dropped\":%llu}",
                g_hz, seconds, static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(total), g_stacks.size(),
                static_cast<unsigned long long>(g_dropped));
  g_stacks.clear();
  return json;
}

}  // namespace skate3::harness::profile
