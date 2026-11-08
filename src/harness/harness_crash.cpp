#include "harness/harness_crash.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
// windows.h first.
#include <dbghelp.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <rex/exception_handler.h>
#endif

#if defined(_WIN32)
#include <algorithm>
#include <utility>
#include <vector>

#include <rex/ppc/func.h>

// The recompiled image's guest -> host function table (generated/skate3_init.cpp).
extern PPCFuncMapping skate3_PPCFuncMappings[];
#endif

namespace skate3::harness::crash {
namespace {

std::atomic<bool> g_guest_fault{false};

// A runtime-computed address the optimizer can't see through, in the unmapped
// first page on every target.
volatile uintptr_t g_bad_address = 0x10;

#if defined(_WIN32)

wchar_t g_txt_path[MAX_PATH];
wchar_t g_dmp_path[MAX_PATH];
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_write_dump = nullptr;
std::atomic<bool> g_crashed{false};
// Host entry point -> guest address of every recompiled function, sorted by
// host address (built at Install: a crash is no time to allocate).
std::vector<std::pair<uint64_t, uint32_t>> g_host_to_guest;

void Put(HANDLE f, const char* s) {
  DWORD written = 0;
  WriteFile(f, s, static_cast<DWORD>(std::strlen(s)), &written, nullptr);
}

// "<module>+0x<rva>" for a code address, or "0x<pc>".
void FormatPc(char* out, size_t n, uint64_t pc) {
  HMODULE mod = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &mod) &&
      mod) {
    char path[MAX_PATH];
    const DWORD len = GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = path;
    for (DWORD i = 0; i < len; ++i) {
      if (path[i] == '\\' || path[i] == '/') {
        name = path + i + 1;
      }
    }
    _snprintf_s(out, n, _TRUNCATE, "%s+0x%llx", len ? name : "?",
                static_cast<unsigned long long>(pc - reinterpret_cast<uint64_t>(mod)));
    // In the game image: name the recompiled guest function the frame is in
    // (exact match on the host function's start, from the unwind tables).
    if (mod == GetModuleHandleW(nullptr) && !g_host_to_guest.empty()) {
      DWORD64 image_base = 0;
      if (PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &image_base, nullptr)) {
        const uint64_t begin = image_base + fn->BeginAddress;
        auto it = std::lower_bound(g_host_to_guest.begin(), g_host_to_guest.end(),
                                   std::make_pair(begin, uint32_t{0}));
        if (it != g_host_to_guest.end() && it->first == begin) {
          const size_t used = std::strlen(out);
          _snprintf_s(out + used, n - used, _TRUNCATE, " (guest sub_%08X, host +0x%llx)",
                      it->second, static_cast<unsigned long long>(pc - begin));
        }
      }
    }
  } else {
    _snprintf_s(out, n, _TRUNCATE, "0x%llx", static_cast<unsigned long long>(pc));
  }
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
  if (g_crashed.exchange(true)) {
    return EXCEPTION_EXECUTE_HANDLER;  // A second thread: the first one reports.
  }
  HANDLE f = CreateFileW(g_txt_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    char line[512];
    char where[MAX_PATH + 32];
    FormatPc(where, sizeof(where), reinterpret_cast<uint64_t>(er->ExceptionAddress));
    _snprintf_s(line, sizeof(line), _TRUNCATE, "crash: exception 0x%08lx at %s (thread %lu)\n",
                er->ExceptionCode, where, GetCurrentThreadId());
    Put(f, line);
    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        er->NumberParameters >= 2) {
      const ULONG_PTR op = er->ExceptionInformation[0];
      _snprintf_s(line, sizeof(line), _TRUNCATE, "address: 0x%llx (%s)\n",
                  static_cast<unsigned long long>(er->ExceptionInformation[1]),
                  op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "execute" : "access");
      Put(f, line);
    }
    // Unwind from the faulting context with the image's own unwind tables.
    CONTEXT ctx = *ep->ContextRecord;
    for (int i = 0; i < 64 && ctx.Rip; ++i) {
      FormatPc(where, sizeof(where), ctx.Rip);
      _snprintf_s(line, sizeof(line), _TRUNCATE, "frame: %s\n", where);
      Put(f, line);
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
      if (!fn) {
        // Leaf function: the return address is on top of the stack.
        if (!ctx.Rsp) {
          break;
        }
        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
      } else {
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                         &establisher, nullptr);
      }
    }
    if (g_write_dump) {
      HANDLE d = CreateFileW(g_dmp_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
      if (d != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        const BOOL ok = g_write_dump(
            GetCurrentProcess(), GetCurrentProcessId(), d,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                       MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
            &mei, nullptr, nullptr);
        CloseHandle(d);
        Put(f, ok ? "dump: crash.dmp\n" : "dump: MiniDumpWriteDump failed\n");
      }
    }
    CloseHandle(f);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

#elif defined(__linux__)

char g_txt_path[4096];
int g_txt_fd = -1;

void Put(const char* s) {
  size_t n = std::strlen(s);
  while (n > 0) {
    const ssize_t w = write(g_txt_fd, s, n);
    if (w <= 0) {
      return;
    }
    s += w;
    n -= static_cast<size_t>(w);
  }
}

void PutHex(uint64_t v) {
  char buf[19];
  buf[0] = '0';
  buf[1] = 'x';
  int n = 0;
  char digits[16];
  do {
    digits[n++] = "0123456789abcdef"[v & 0xF];
    v >>= 4;
  } while (v && n < 16);
  for (int i = 0; i < n; ++i) {
    buf[2 + i] = digits[n - 1 - i];
  }
  buf[2 + n] = '\0';
  Put(buf);
}

void PutDec(uint64_t v) {
  char buf[21];
  int i = 20;
  buf[i] = '\0';
  do {
    buf[--i] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v);
  Put(buf + i);
}

void PutFrame(uint64_t pc) {
  // Module mapping is resolved on the host from the maps copy below.
  Put("frame: ");
  PutHex(pc);
  Put("\n");
}

// Runs inside the SDK's SIGSEGV/SIGILL handler (async-signal-safe only).
void OnUnhandled(int signal_number, void* signal_info, void* signal_context) {
  g_txt_fd = open(g_txt_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (g_txt_fd < 0) {
    return;
  }
  const auto* info = static_cast<const siginfo_t*>(signal_info);
  const auto* uc = static_cast<const ucontext_t*>(signal_context);
  Put("crash: signal ");
  PutDec(static_cast<uint64_t>(signal_number));
  Put(signal_number == SIGSEGV ? " (SIGSEGV)" : signal_number == SIGILL ? " (SIGILL)" : "");
  Put(" (thread ");
  PutDec(static_cast<uint64_t>(gettid()));
  Put(")\naddress: ");
  PutHex(reinterpret_cast<uint64_t>(info->si_addr));
  Put("\n");

#if defined(__aarch64__)
  const uint64_t pc = uc->uc_mcontext.pc;
  const uint64_t lr = uc->uc_mcontext.regs[30];
  const uint64_t sp = uc->uc_mcontext.sp;
  uint64_t fp = uc->uc_mcontext.regs[29];
  constexpr uint64_t kRetOffset = 8;  // frame record: [fp] = prev fp, [fp+8] = lr
#elif defined(__x86_64__)
  const uint64_t pc = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
  const uint64_t lr = 0;
  const uint64_t sp = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSP]);
  uint64_t fp = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBP]);
  constexpr uint64_t kRetOffset = 8;  // [rbp] = prev rbp, [rbp+8] = return address
#endif
  Put("pc: ");
  PutHex(pc);
  Put(" sp: ");
  PutHex(sp);
  Put(" fp: ");
  PutHex(fp);
  if (lr) {
    Put(" lr: ");
    PutHex(lr);
  }
  Put("\n");
  PutFrame(pc);
  // The faulting function may not have stored its frame record yet; the
  // link register then holds its caller. The runner drops a duplicate.
  if (lr) {
    PutFrame(lr);
  }
  // Frame-pointer walk, bounded to 8 MiB above sp: frame records live on
  // this thread's stack, and reading outside it would fault again inside
  // this handler.
  for (int i = 0; i < 64; ++i) {
    if (fp < sp || fp >= sp + (8u << 20) || (fp & 7) != 0) {
      break;
    }
    const uint64_t next = *reinterpret_cast<const uint64_t*>(fp);
    const uint64_t ret = *reinterpret_cast<const uint64_t*>(fp + kRetOffset);
    if (!ret) {
      break;
    }
    PutFrame(ret);
    if (next <= fp) {
      break;
    }
    fp = next;
  }

  Put("maps:\n");
  const int maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (maps >= 0) {
    char buf[4096];
    ssize_t n;
    while ((n = read(maps, buf, sizeof(buf))) > 0) {
      if (write(g_txt_fd, buf, static_cast<size_t>(n)) != n) {
        break;
      }
    }
    close(maps);
  }
  close(g_txt_fd);
}

#endif

}  // namespace

void Install(const std::filesystem::path& run_dir) {
#if defined(_WIN32)
  const std::wstring txt = (run_dir / "crash.txt").wstring();
  const std::wstring dmp = (run_dir / "crash.dmp").wstring();
  wcsncpy_s(g_txt_path, txt.c_str(), _TRUNCATE);
  wcsncpy_s(g_dmp_path, dmp.c_str(), _TRUNCATE);
  for (const PPCFuncMapping* m = skate3_PPCFuncMappings; m->host; ++m) {
    g_host_to_guest.emplace_back(reinterpret_cast<uint64_t>(m->host), static_cast<uint32_t>(m->guest));
  }
  std::sort(g_host_to_guest.begin(), g_host_to_guest.end());
  // Loaded now: a crash is a bad time to load libraries.
  if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
    g_write_dump =
        reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
  }
  // The SDK's vectored handler sees every exception first (guest MMIO and
  // memory watches); what it doesn't claim continues to this filter.
  SetUnhandledExceptionFilter(UnhandledFilter);
#elif defined(__linux__)
  const std::string txt = (run_dir / "crash.txt").string();
  std::strncpy(g_txt_path, txt.c_str(), sizeof(g_txt_path) - 1);
  rex::arch::ExceptionHandler::SetUnhandled(OnUnhandled);
#else
  (void)run_dir;
#endif
}

void FaultNow() {
  *reinterpret_cast<volatile uint32_t*>(g_bad_address) = 0x0BADC0DE;
}

void ArmGuestFault() {
  g_guest_fault.store(true, std::memory_order_release);
}

void MaybeGuestFault() {
  if (g_guest_fault.load(std::memory_order_relaxed) &&
      g_guest_fault.exchange(false, std::memory_order_acq_rel)) {
    FaultNow();
  }
}

}  // namespace skate3::harness::crash
