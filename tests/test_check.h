#pragma once

// Minimal always-on check for the standalone C++ tests. Unlike assert(), it
// is not compiled out by NDEBUG, so the tests check the same things in every
// build type. A failed CHECK prints its location and expression and keeps
// going, so one run reports every failure; TestExit() then makes the process
// exit nonzero for CTest.

#include <cstdio>
#include <cstdlib>

namespace skate3_test {

inline int& FailureCount() {
  static int failures = 0;
  return failures;
}

inline void ReportFailure(const char* file, int line, const char* expr) {
  ++FailureCount();
  std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
  std::fflush(stderr);
}

// Call as `return skate3_test::TestExit("PASS: ...");` at the end of main.
inline int TestExit(const char* pass_message) {
  if (FailureCount() != 0) {
    std::fprintf(stderr, "FAIL: %d check(s) failed\n", FailureCount());
    return EXIT_FAILURE;
  }
  std::printf("%s\n", pass_message);
  return EXIT_SUCCESS;
}

}  // namespace skate3_test

#define CHECK(expr)                                               \
  do {                                                            \
    if (!(expr)) {                                                \
      ::skate3_test::ReportFailure(__FILE__, __LINE__, #expr);    \
    }                                                             \
  } while (0)
