#pragma once

// Unique, sortable timestamps for diagnostic file names (screenshots, scene
// ring dumps, native-render snapshots). Standard library only, so the unit
// test in tests/file_stamp_test.cpp can build it without the SDK.
//
// A stamp looks like YYYYMMDD_HHMMSS_mmm in local time. Every stamp handed
// out by NextFileStamp() is strictly later than the one before it, process
// wide, so two files written in the same millisecond (or after the clock
// steps back) still get distinct names, in write order (KI-32, KI-75).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace skate3::file_stamp {

// Returns max(now_ms, last + 1) and stores it in last. After a burst or a
// clock step back the result leads the clock until the clock catches up.
inline int64_t NextUniqueMs(std::atomic<int64_t>& last, int64_t now_ms) {
  int64_t prev = last.load(std::memory_order_relaxed);
  int64_t next;
  do {
    next = std::max(now_ms, prev + 1);
  } while (!last.compare_exchange_weak(prev, next, std::memory_order_relaxed));
  return next;
}

// YYYYMMDD_HHMMSS_mmm. Fixed width, so name order equals time order.
inline std::string FormatStamp(const std::tm& tm, int millis) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d_%03d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
  return buf;
}

// Formats ms since the epoch as a local-time stamp.
inline std::string FormatLocalStamp(int64_t ms) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return FormatStamp(tm, static_cast<int>(ms % 1000));
}

// Last stamp handed out, in ms since the epoch. Shared by every caller so
// different kinds of dump never reuse a stamp either.
inline std::atomic<int64_t> g_last_stamp_ms{0};

inline int64_t NextStampMs() {
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  return NextUniqueMs(g_last_stamp_ms, now_ms);
}

inline std::string NextFileStamp() { return FormatLocalStamp(NextStampMs()); }

}  // namespace skate3::file_stamp
