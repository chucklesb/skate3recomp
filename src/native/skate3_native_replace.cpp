// RE-7 replacement framework (see the header).

#include "native/skate3_native_replace.h"

#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_guest_read.h"

REXCVAR_DEFINE_STRING(skate3_native_replace_all, "default", "Skate 3",
                      "RE-7 native replacements, all families at once: default (each "
                      "family's own cvar) | off (every recompiled function) | shadow (every "
                      "family computes and compares; replace: lines)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_replace {
namespace {

using skate3::native_scene::GuestTryCopy;

constexpr int kMaxFamilies = 64;
Family* g_families[kMaxFamilies] = {};
std::atomic<int> g_count{0};
std::mutex g_register_mu;
std::atomic<int> g_override{0};  // 0 default, 1 off, 2 shadow
std::atomic<uint64_t> g_logged_diffs{0};
uint64_t g_frames = 0;

int ReadOverride() {
  const std::string v = REXCVAR_GET(skate3_native_replace_all);
  return v == "off" ? 1 : v == "shadow" ? 2 : 0;
}

// One pending shadow computation per family and thread (hooks run on the
// game's job threads too).
thread_local Writes tl_writes[kMaxFamilies];

bool Compare(uint8_t* base, const Family& f, const Writes& w) {
  const uint32_t rw = w.row_words;
  for (size_t r = 0; r < w.addr.size(); ++r) {
    uint32_t got[16];
    if (rw > 16 || !GuestTryCopy(got, base + w.addr[r], rw * 4)) {
      return false;
    }
    for (uint32_t k = 0; k < rw; ++k) {
      if (got[k] != w.words[r * rw + k]) {
        if (g_logged_diffs.fetch_add(1, std::memory_order_relaxed) < 16) {
          REXLOG_WARN("replace: {} DIFF row {} at {:08X} word {}: guest {:08X} native {:08X}",
                      f.name, r, w.addr[r], k, __builtin_bswap32(got[k]),
                      __builtin_bswap32(w.words[r * rw + k]));
        }
        return false;
      }
    }
  }
  return true;
}

void Write(uint8_t* base, const Writes& w) {
  for (size_t r = 0; r < w.addr.size(); ++r) {
    std::memcpy(base + w.addr[r], w.words.data() + r * w.row_words, w.row_words * 4);
  }
}

}  // namespace

void Register(Family* f) {
  std::lock_guard lk(g_register_mu);
  if (f->id >= 0) return;
  const int i = g_count.load();
  if (i >= kMaxFamilies) {
    REXLOG_ERROR("replace: too many families ({}); {} stays recompiled", kMaxFamilies, f->name);
    return;
  }
  f->id = i;
  g_families[i] = f;
  g_count.store(i + 1);
}

int ModeOf(const Family& f) {
  switch (g_override.load(std::memory_order_relaxed)) {
    case 1: return 0;
    case 2: return 1;
    default: return f.mode();
  }
}

bool BodyOn(Family& f) {
  if (f.id < 0) Register(&f);
  return f.id >= 0 && g_override.load(std::memory_order_relaxed) == 0 && f.mode() == 2;
}

void Run(Family& f, PPCContext& ctx, uint8_t* base, PPCFn orig) {
  if (f.body) {
    if (!BodyOn(f)) {
      orig(ctx, base);
      return;
    }
    f.calls.fetch_add(1, std::memory_order_relaxed);
    f.native.fetch_add(1, std::memory_order_relaxed);
    f.body(ctx, base);
    return;
  }
  if (f.id < 0) Register(&f);
  const int mode = f.id < 0 ? 0 : ModeOf(f);
  if (mode == 0) {
    orig(ctx, base);
    return;
  }
  f.calls.fetch_add(1, std::memory_order_relaxed);
  Writes& w = tl_writes[f.id];
  if (!f.compute(ctx, base, &w)) {
    f.bad.fetch_add(1, std::memory_order_relaxed);
    orig(ctx, base);
    return;
  }
  f.rows.fetch_add(w.addr.size(), std::memory_order_relaxed);
  if (mode == 2) {
    Write(base, w);
    f.native.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  orig(ctx, base);
  (Compare(base, f, w) ? f.exact : f.diff).fetch_add(1, std::memory_order_relaxed);
}

void OnFrameBuilt() {
  g_override.store(ReadOverride(), std::memory_order_relaxed);
  if (++g_frames % 120 != 0) return;
  std::string line;
  const auto x = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
  const int n = g_count.load();
  for (int i = 0; i < n; ++i) {
    Family& f = *g_families[i];
    const int mode = f.body ? (BodyOn(f) ? 2 : 0) : ModeOf(f);
    if (mode == 0) continue;
    line += fmt::format(" {}[{} calls={} exact={} diff={} native={} bad={} rows={}]", f.name,
                        mode == 2 ? "on" : "shadow", x(f.calls), x(f.exact), x(f.diff),
                        x(f.native), x(f.bad), x(f.rows));
  }
  if (!line.empty()) {
    REXLOG_INFO("replace: frame {}{}", g_frames, line);
  }
}

}  // namespace skate3::native_replace
