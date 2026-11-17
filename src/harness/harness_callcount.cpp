// Guest call counts: the registry the counting entry points join
// (harness_callcount_hook.h) and the harness step's reset/dump.
#include "harness/harness_callcount.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <utility>
#include <vector>

#include "harness/harness_callcount_hook.h"

namespace skate3::callcount {
namespace {

// Pushed to by static constructors in every recompiled source, so it must be
// constant-initialized (no constructor of its own to run first).
constinit std::atomic<Counter*> g_head{nullptr};

}  // namespace

Counter::Counter(const char* n) : name(n), next(nullptr) {
  Counter* h = g_head.load(std::memory_order_relaxed);
  do {
    next = h;
  } while (!g_head.compare_exchange_weak(h, this, std::memory_order_release,
                                         std::memory_order_relaxed));
}

bool Enabled() {
#if defined(SKATE3_COUNT_GUEST_CALLS)
  return true;
#else
  return false;
#endif
}

void Reset() {
  for (Counter* c = g_head.load(std::memory_order_acquire); c; c = c->next) {
    c->calls.store(0, std::memory_order_relaxed);
  }
}

std::string Dump(const std::filesystem::path& dir, std::string* error) {
  if (!Enabled()) {
    *error = "this build doesn't count calls (configure with -DSKATE3_COUNT_GUEST_CALLS=ON)";
    return {};
  }
  std::vector<std::pair<uint64_t, const char*>> rows;
  uint64_t total = 0;
  for (Counter* c = g_head.load(std::memory_order_acquire); c; c = c->next) {
    const uint64_t n = c->calls.load(std::memory_order_relaxed);
    if (n) {
      rows.emplace_back(n, c->name);
      total += n;
    }
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::ofstream out(dir / "call_counts.tsv", std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "can't write call_counts.tsv";
    return {};
  }
  out << "# function\tcalls\n";
  for (const auto& [n, name] : rows) {
    out << name << '\t' << n << '\n';
  }
  char buf[160];
  std::snprintf(buf, sizeof(buf), "\"file\":\"call_counts.tsv\",\"functions\":%zu,\"calls\":%llu",
                rows.size(), static_cast<unsigned long long>(total));
  return buf;
}

}  // namespace skate3::callcount
