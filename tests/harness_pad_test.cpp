// Unit test for src/harness/harness_pad.h: the scenario harness's button
// names, stick/trigger scaling, overlay merge and the .s3rec recording
// format (round trip, malformed input, playback lookup).

#include "harness/harness_pad.h"

#include <string>

#include "test_check.h"

using namespace skate3::harness;

int main() {
  // Button names and specs.
  {
    CHECK(ParseButtons("") == uint16_t{0});
    CHECK(ParseButtons("a") == uint16_t{kA});
    CHECK(ParseButtons("A+LB+up") == uint16_t(kA | kLB | kDpadUp));
    CHECK(ParseButtons("start, back | rb x") == uint16_t(kStart | kBack | kRB | kX));
    CHECK(ParseButtons("select") == uint16_t{kBack});
    std::string bad;
    CHECK(!ParseButtons("a+jump", &bad).has_value());
    CHECK(bad == "jump");
  }

  // Axis and trigger scaling, clamped, NaN-safe.
  {
    CHECK(AxisFromUnit(0.0) == 0);
    CHECK(AxisFromUnit(1.0) == 32767);
    CHECK(AxisFromUnit(-1.0) == -32768);
    CHECK(AxisFromUnit(5.0) == 32767);
    CHECK(AxisFromUnit(-5.0) == -32768);
    CHECK(AxisFromUnit(0.5) == 16384);
    CHECK(AxisFromUnit(std::nan("")) == 0);
    CHECK(TriggerFromUnit(0.0) == 0);
    CHECK(TriggerFromUnit(1.0) == 255);
    CHECK(TriggerFromUnit(2.0) == 255);
    CHECK(TriggerFromUnit(-1.0) == 0);
  }

  // Overlay merge: buttons OR, triggers max, sticks larger magnitude.
  {
    PadState a{kA, 10, 200, 1000, -20000, 0, 5};
    PadState b{kB, 50, 100, -3000, 10000, 7, -6};
    const PadState o = Overlay(a, b);
    CHECK(o.buttons == uint16_t(kA | kB));
    CHECK(o.lt == 50);
    CHECK(o.rt == 200);
    CHECK(o.lx == -3000);
    CHECK(o.ly == -20000);
    CHECK(o.rx == 7);
    CHECK(o.ry == -6);
    CHECK(PadState{}.Neutral());
    CHECK(!a.Neutral());
  }

  // .s3rec round trip.
  {
    Recording rec;
    rec.header_json = "{\"note\":\"test\"}";
    rec.rows.push_back({0, 0, PadState{}});
    rec.rows.push_back({12, 11, PadState{kA, 0, 255, 32767, -32768, 0, 0}});
    rec.rows.push_back({40, 39, PadState{0, 0, 0, 0, 16384, -1, 1}});
    std::string text = FormatRecHeader(rec.header_json);
    for (const auto& r : rec.rows) {
      text += FormatRecRow(r);
    }
    std::string error;
    auto parsed = ParseRecording(text, &error);
    CHECK(parsed.has_value());
    if (parsed) {
      CHECK(parsed->header_json == rec.header_json);
      CHECK(parsed->rows == rec.rows);
    }
    // CRLF line endings and comment lines are fine.
    std::string crlf = "# s3rec 1 {}\r\n# comment\r\n0 0 0000 0 0 0 0 0 0\r\n\r\n5 4 1000 0 0 0 0 0 0\r\n";
    auto p2 = ParseRecording(crlf, &error);
    CHECK(p2.has_value() && p2->rows.size() == 2);
  }

  // Malformed recordings are rejected with a reason.
  {
    std::string error;
    CHECK(!ParseRecording("", &error).has_value());
    CHECK(!ParseRecording("0 0 0000 0 0 0 0 0 0\n", &error).has_value());
    CHECK(error.find("header") != std::string::npos);
    CHECK(!ParseRecording("# s3rec 1 {}\n1 2 3\n", &error).has_value());
    CHECK(error.find("line 2") != std::string::npos);
    CHECK(!ParseRecording("# s3rec 1 {}\n0 0 10000 0 0 0 0 0 0\n", &error).has_value());  // buttons
    CHECK(!ParseRecording("# s3rec 1 {}\n0 0 0000 256 0 0 0 0 0\n", &error).has_value());  // trigger
    CHECK(!ParseRecording("# s3rec 1 {}\n0 0 0000 0 0 40000 0 0 0\n", &error).has_value());  // axis
  }

  // Playback lookup: the last row at or before the poll.
  {
    Recording rec;
    rec.rows.push_back({0, 0, PadState{}});
    rec.rows.push_back({10, 10, PadState{kA}});
    rec.rows.push_back({20, 20, PadState{kB}});
    size_t cursor = 0;
    CHECK(PadAtPoll(rec, 0, &cursor)->buttons == 0);
    CHECK(PadAtPoll(rec, 9, &cursor)->buttons == 0);
    CHECK(PadAtPoll(rec, 10, &cursor)->buttons == kA);
    CHECK(PadAtPoll(rec, 19, &cursor)->buttons == kA);
    CHECK(PadAtPoll(rec, 25, &cursor)->buttons == kB);
    // Going backwards resets the cursor.
    CHECK(PadAtPoll(rec, 5, &cursor)->buttons == 0);
    Recording late;
    late.rows.push_back({3, 3, PadState{kX}});
    CHECK(PadAtPoll(late, 1, nullptr) == nullptr);
    CHECK(PadAtPoll(Recording{}, 0, nullptr) == nullptr);
  }

  // JSON escaping of strings the harness writes.
  {
    CHECK(JsonEscape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
    CHECK(JsonEscape(std::string("\x01", 1)) == "\\u0001");
  }

  return skate3_test::TestExit("PASS: harness_pad");
}
