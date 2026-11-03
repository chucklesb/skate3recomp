// Unit test for src/native/skate3_file_stamp.h (KI-75): diagnostic file
// names (screenshots, scene ring dumps, snapshots) must never collide, even
// when two are written in the same second or millisecond.

#include "native/skate3_file_stamp.h"

#include <atomic>
#include <ctime>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "test_check.h"

using namespace skate3::file_stamp;

int main() {
  // Same millisecond: each call still moves forward by one.
  {
    std::atomic<int64_t> last{0};
    CHECK(NextUniqueMs(last, 1000) == 1000);
    CHECK(NextUniqueMs(last, 1000) == 1001);
    CHECK(NextUniqueMs(last, 1000) == 1002);
    // The clock catches up: back to the clock value.
    CHECK(NextUniqueMs(last, 5000) == 5000);
    // Clock steps back: still strictly increasing.
    CHECK(NextUniqueMs(last, 10) == 5001);
  }

  // Fixed-width format, and the millisecond field separates same-second names.
  {
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 10;
    tm.tm_mday = 1;
    tm.tm_hour = 9;
    tm.tm_min = 5;
    tm.tm_sec = 7;
    CHECK(FormatStamp(tm, 3) == "20261101_090507_003");
    CHECK(FormatStamp(tm, 999) == "20261101_090507_999");
    CHECK(FormatStamp(tm, 3) != FormatStamp(tm, 4));
    CHECK(FormatStamp(tm, 3) < FormatStamp(tm, 40));
  }

  // The real clock: a tight burst of stamps (well within one second) is all
  // distinct and already sorted, including across threads.
  {
    std::vector<std::string> stamps;
    for (int i = 0; i < 200; ++i) {
      stamps.push_back(NextFileStamp());
    }
    for (size_t i = 1; i < stamps.size(); ++i) {
      CHECK(stamps[i - 1] < stamps[i]);
    }
    CHECK(stamps.front().size() == 19);

    std::vector<std::vector<int64_t>> per_thread(4);
    std::vector<std::thread> threads;
    for (auto& out : per_thread) {
      threads.emplace_back([&out] {
        for (int i = 0; i < 500; ++i) {
          out.push_back(NextStampMs());
        }
      });
    }
    for (auto& t : threads) {
      t.join();
    }
    std::set<int64_t> all;
    size_t total = 0;
    for (const auto& out : per_thread) {
      all.insert(out.begin(), out.end());
      total += out.size();
    }
    CHECK(all.size() == total);
  }

  return skate3_test::TestExit("PASS: file_stamp_test");
}
