#include "harness/harness_mem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include "native/skate3_native_guest_read.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace skate3::harness::mem {
namespace {

constexpr uint64_t kPage = 0x10000;

// Guest offsets [start, end) that are mapped readable, from the OS: probing
// unmapped pages would fault, and every fault is logged by the runtime's
// exception handler before the guarded copy catches it (tens of thousands
// of error lines per 4 GB scan, and slow). Copies still go through
// GuestTryCopy, for pages that change underneath.
std::vector<std::pair<uint64_t, uint64_t>> ReadableRanges(uint8_t* base) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  constexpr uint64_t kSpace = 0x100000000ull;
#if defined(_WIN32)
  uint64_t off = 0;
  while (off < kSpace) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(base + off, &info, sizeof(info)) == 0) break;
    const uint64_t start = reinterpret_cast<uint8_t*>(info.BaseAddress) - base;
    const uint64_t end = std::min<uint64_t>(start + info.RegionSize, kSpace);
    const bool readable =
        info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 &&
        (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
    if (readable) out.emplace_back(std::max(start, off), end);
    off = end;
  }
#else
  std::ifstream maps("/proc/self/maps");
  std::string line;
  const uint64_t lo = reinterpret_cast<uint64_t>(base), hi = lo + kSpace;
  bool any = false;
  while (std::getline(maps, line)) {
    any = true;
    unsigned long long s = 0, e = 0;
    char perms[8] = {};
    if (std::sscanf(line.c_str(), "%llx-%llx %7s", &s, &e, perms) != 3 || perms[0] != 'r') continue;
    if (e <= lo || s >= hi) continue;
    out.emplace_back(std::max<uint64_t>(s, lo) - lo, std::min<uint64_t>(e, hi) - lo);
  }
  if (!any) out.emplace_back(0, kSpace);  // no /proc (macOS): probe everything
#endif
  // Merge neighbours.
  std::vector<std::pair<uint64_t, uint64_t>> merged;
  for (const auto& r : out) {
    if (!merged.empty() && merged.back().second >= r.first) {
      merged.back().second = std::max(merged.back().second, r.second);
    } else {
      merged.push_back(r);
    }
  }
  return merged;
}

// Calls fn(page offset) for every 64 KB page in [lo, hi) the OS reports
// readable (GPU registers skipped).
template <typename Fn>
void ForEachPage(uint8_t* base, uint64_t lo, uint64_t hi, Fn&& fn) {
  for (const auto& [s, e] : ReadableRanges(base)) {
    const uint64_t from = std::max(lo, s) & ~(kPage - 1);
    const uint64_t to = std::min(hi, e);
    for (uint64_t a = from; a < to; a += kPage) {
      if (a >= 0x7FC00000 && a < 0x80000000) continue;
      fn(a);
    }
  }
}
constexpr size_t kListCap = 200000;            // candidates kept as an address list
constexpr uint64_t kSnapshotSpan = 0x40000000;  // 1 GB: an unknown-value find needs a range

unsigned Width(const std::string& t) {
  return t == "u8" ? 1 : t == "u16" ? 2 : t == "f64" ? 8 : 4;
}

uint64_t LoadBe(const uint8_t* p, unsigned w) {
  uint64_t v = 0;
  for (unsigned i = 0; i < w; ++i) {
    v = (v << 8) | p[i];
  }
  return v;
}

void StoreBe(uint8_t* p, unsigned w, uint64_t v) {
  for (unsigned i = 0; i < w; ++i) {
    p[w - 1 - i] = static_cast<uint8_t>(v >> (8 * i));
  }
}

double Decode(const std::string& t, uint64_t raw) {
  if (t == "f32") {
    const uint32_t u = static_cast<uint32_t>(raw);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  if (t == "f64") {
    double d;
    std::memcpy(&d, &raw, 8);
    return d;
  }
  return static_cast<double>(raw);
}

uint64_t Encode(const std::string& t, double v) {
  if (t == "f32") {
    const float f = static_cast<float>(v);
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
  }
  if (t == "f64") {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    return u;
  }
  const unsigned w = Width(t);
  const uint64_t mask = w == 8 ? ~0ull : ((1ull << (8 * w)) - 1);
  return static_cast<uint64_t>(static_cast<int64_t>(v)) & mask;
}

std::string Format(const std::string& t, uint64_t raw) {
  char buf[48];
  if (t == "f32" || t == "f64") {
    std::snprintf(buf, sizeof(buf), "%.9g", Decode(t, raw));
  } else {
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(raw));
  }
  return buf;
}

bool Keep(const std::string& t, const std::string& cmp, const std::optional<uint64_t>& want,
          uint64_t old, uint64_t cur) {
  if (want) {
    return cur == *want;
  }
  if (cmp == "changed") return cur != old;
  if (cmp == "unchanged") return cur == old;
  const double a = Decode(t, old), b = Decode(t, cur);
  if (std::isnan(a) || std::isnan(b)) return false;
  return cmp == "increased" ? b > a : b < a;
}


// The search state between requests.
struct Page {
  uint32_t addr = 0;
  std::vector<uint8_t> bytes;     // values at the last find / refine
  std::vector<uint8_t> live;      // 1 = still a candidate (per aligned slot)
  size_t count = 0;
};
struct State {
  std::string type;
  bool snapshot = false;          // pages mode (unknown-value search)
  std::vector<Page> pages;
  std::vector<uint32_t> addrs;    // list mode
  std::vector<uint64_t> prev;
} g;

size_t Count() {
  if (!g.snapshot) return g.addrs.size();
  size_t n = 0;
  for (const Page& p : g.pages) n += p.count;
  return n;
}

// Pages mode → list mode once the candidates fit.
void MaybeToList() {
  if (!g.snapshot || Count() > kListCap) return;
  const unsigned w = Width(g.type);
  g.addrs.clear();
  g.prev.clear();
  for (const Page& p : g.pages) {
    for (size_t s = 0; s < p.live.size(); ++s) {
      if (p.live[s]) {
        g.addrs.push_back(p.addr + static_cast<uint32_t>(s * w));
        g.prev.push_back(LoadBe(p.bytes.data() + s * w, w));
      }
    }
  }
  g.pages.clear();
  g.snapshot = false;
}

std::string Result(uint8_t* base) {
  std::string out = "\"count\":" + std::to_string(Count());
  if (g.snapshot) {
    return out + ",\"addrs\":[]";
  }
  out += ",\"addrs\":[";
  const unsigned w = Width(g.type);
  for (size_t i = 0; i < g.addrs.size() && i < 64; ++i) {
    uint8_t b[8] = {};
    const bool ok = native_scene::GuestTryCopy(b, base + g.addrs[i], w);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s\"%08X=%s\"", i ? "," : "", g.addrs[i],
                  ok ? Format(g.type, LoadBe(b, w)).c_str() : "?");
    out += buf;
  }
  return out + "]";
}

}  // namespace

bool ValidType(const std::string& t) {
  return t == "u8" || t == "u16" || t == "u32" || t == "f32" || t == "f64";
}

bool ValidCmp(const std::string& c) {
  return c.empty() || c == "changed" || c == "unchanged" || c == "increased" ||
         c == "decreased";
}

bool Run(uint8_t* base, const Request& req, std::string* json, std::string* error) {
  const unsigned w = Width(req.type);
  const std::optional<uint64_t> want =
      req.value ? std::optional<uint64_t>(Encode(req.type, *req.value)) : std::nullopt;

  if (req.op == "read" || req.op == "write") {
    uint8_t b[8] = {};
    if (!native_scene::GuestTryCopy(b, base + req.addr, w)) {
      *error = "address not mapped";
      return false;
    }
    std::string out;
    if (req.op == "write") {
      out = "\"old\":" + Format(req.type, LoadBe(b, w)) + ",";
      StoreBe(b, w, *want);
      std::memcpy(base + req.addr, b, w);
    }
    *json = out + "\"value\":" + Format(req.type, LoadBe(b, w));
    return true;
  }

  if (req.op == "find") {
    g = State{};
    g.type = req.type;
    if (!want && req.hi - req.lo > kSnapshotSpan) {
      *error = "an unknown-value find (no value) needs range = [lo, hi] of at most 1 GB "
               "(e.g. the heap, [\"0x40000000\", \"0x50000000\"])";
      return false;
    }
    std::vector<uint8_t> buf(kPage);
    ForEachPage(base, req.lo, req.hi, [&](uint64_t a) {
      if (!native_scene::GuestTryCopy(buf.data(), base + a, kPage)) {
        return;
      }
      if (want) {
        for (size_t o = 0; o + w <= kPage; o += w) {
          const uint64_t v = LoadBe(buf.data() + o, w);
          if (v == *want && g.addrs.size() < kListCap) {
            g.addrs.push_back(static_cast<uint32_t>(a + o));
            g.prev.push_back(v);
          }
        }
      } else {
        Page p;
        p.addr = static_cast<uint32_t>(a);
        p.bytes = buf;
        p.live.assign(kPage / w, 1);
        p.count = p.live.size();
        g.pages.push_back(std::move(p));
      }
    });
    g.snapshot = !want;
    *json = Result(base) + (g.addrs.size() >= kListCap ? ",\"capped\":true" : "");
    return true;
  }

  // refine
  if (g.type.empty() || g.type != req.type) {
    *error = "no " + req.type + " find to refine";
    return false;
  }
  if (!want && req.cmp.empty()) {
    *error = "refine needs value = N or cmp = changed|unchanged|increased|decreased";
    return false;
  }
  if (g.snapshot) {
    std::vector<uint8_t> cur(kPage);
    for (Page& p : g.pages) {
      if (!p.count) continue;
      if (!native_scene::GuestTryCopy(cur.data(), base + p.addr, kPage)) {
        p.count = 0;  // unmapped since: drop the page
        std::fill(p.live.begin(), p.live.end(), 0);
        continue;
      }
      size_t n = 0;
      for (size_t s = 0; s < p.live.size(); ++s) {
        if (!p.live[s]) continue;
        const uint64_t o = LoadBe(p.bytes.data() + s * w, w);
        const uint64_t c = LoadBe(cur.data() + s * w, w);
        p.live[s] = Keep(req.type, req.cmp, want, o, c) ? 1 : 0;
        n += p.live[s];
      }
      p.count = n;
      p.bytes = cur;
    }
    MaybeToList();
  } else {
    std::vector<uint32_t> addrs;
    std::vector<uint64_t> prev;
    for (size_t i = 0; i < g.addrs.size(); ++i) {
      uint8_t b[8] = {};
      if (!native_scene::GuestTryCopy(b, base + g.addrs[i], w)) continue;
      const uint64_t c = LoadBe(b, w);
      if (Keep(req.type, req.cmp, want, g.prev[i], c)) {
        addrs.push_back(g.addrs[i]);
        prev.push_back(c);
      }
    }
    g.addrs = std::move(addrs);
    g.prev = std::move(prev);
  }
  *json = Result(base);
  return true;
}

bool WriteSnapshot(uint8_t* base, const std::string& path, uint64_t* bytes, uint32_t* regions,
                   std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot write " + path;
    return false;
  }
  out.write("SK3GSNP1", 8);
  *bytes = 0;
  *regions = 0;
  std::vector<uint8_t> run;  // the current run of readable pages
  uint64_t run_start = 0;
  auto flush = [&] {
    if (run.empty()) return;
    const uint64_t size = run.size();
    out.write(reinterpret_cast<const char*>(&run_start), 8);
    out.write(reinterpret_cast<const char*>(&size), 8);
    out.write(reinterpret_cast<const char*>(run.data()), static_cast<std::streamsize>(size));
    *bytes += size;
    ++*regions;
    run.clear();
  };
  std::vector<uint8_t> page(kPage);
  static const std::vector<uint8_t> kZero(kPage, 0);
  ForEachPage(base, 0, 0x100000000ull, [&](uint64_t a) {
    // All-zero pages are left out: on Linux the whole 4 GB guest space is
    // mapped readable, untouched pages read as zeros (4 GB snapshots).
    const bool ok = native_scene::GuestTryCopy(page.data(), base + a, kPage) &&
                    std::memcmp(page.data(), kZero.data(), kPage) != 0;
    // A gap, a failed page or a full buffer ends the region.
    if (!ok || run.size() >= (64u << 20) || (!run.empty() && run_start + run.size() != a)) {
      flush();
    }
    if (ok) {
      if (run.empty()) run_start = a;
      run.insert(run.end(), page.begin(), page.end());
    }
  });
  flush();
  const uint64_t end_off = ~0ull, end_size = 0;
  out.write(reinterpret_cast<const char*>(&end_off), 8);
  out.write(reinterpret_cast<const char*>(&end_size), 8);
  out.close();
  if (!out) {
    *error = "write failed: " + path;
    return false;
  }
  return true;
}

}  // namespace skate3::harness::mem
