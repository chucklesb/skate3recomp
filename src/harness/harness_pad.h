#pragma once
// Scenario harness: pad state, button names and the .s3rec recording format.
//
// Pure logic with only the C++ standard library, so tests/ can cover it
// (tests/harness_pad_test.cpp). The game-facing side lives in harness.cpp.
//
// .s3rec is line-oriented text, one header line then one line per guest
// input poll:
//
//   # s3rec 1 {"build":"...","scenario":"...","note":"..."}
//   <poll> <frame> <buttons-hex> <lt> <rt> <lx> <ly> <rx> <ry>
//
// poll and frame count from the recording's start; buttons are the XInput
// mask (hex), triggers 0..255, sticks -32768..32767. Text keeps recordings
// diffable and hand-editable; at 60 polls/s it is about 2 KB per second.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skate3::harness {

// XInput button bits (XINPUT_GAMEPAD_*).
enum PadButton : uint16_t {
  kDpadUp = 0x0001,
  kDpadDown = 0x0002,
  kDpadLeft = 0x0004,
  kDpadRight = 0x0008,
  kStart = 0x0010,
  kBack = 0x0020,
  kL3 = 0x0040,
  kR3 = 0x0080,
  kLB = 0x0100,
  kRB = 0x0200,
  kGuide = 0x0400,
  kA = 0x1000,
  kB = 0x2000,
  kX = 0x4000,
  kY = 0x8000,
};

struct PadState {
  uint16_t buttons = 0;
  uint8_t lt = 0;
  uint8_t rt = 0;
  int16_t lx = 0;
  int16_t ly = 0;
  int16_t rx = 0;
  int16_t ry = 0;

  bool operator==(const PadState&) const = default;
  bool Neutral() const { return *this == PadState{}; }
};

inline std::string ToLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

inline std::optional<uint16_t> ButtonFromName(std::string_view raw) {
  const std::string n = ToLower(raw);
  struct Entry {
    const char* name;
    uint16_t bit;
  };
  static constexpr Entry kNames[] = {
      {"a", kA},          {"b", kB},          {"x", kX},           {"y", kY},
      {"lb", kLB},        {"rb", kRB},        {"l3", kL3},         {"r3", kR3},
      {"start", kStart},  {"back", kBack},    {"select", kBack},   {"guide", kGuide},
      {"up", kDpadUp},    {"down", kDpadDown}, {"left", kDpadLeft}, {"right", kDpadRight},
      {"dpad_up", kDpadUp}, {"dpad_down", kDpadDown}, {"dpad_left", kDpadLeft},
      {"dpad_right", kDpadRight},
  };
  for (const auto& e : kNames) {
    if (n == e.name) {
      return e.bit;
    }
  }
  return std::nullopt;
}

// "a+lb+up" (also ',' ' ' '|' separators). Empty -> 0. nullopt names the
// first unknown token through `bad`.
inline std::optional<uint16_t> ParseButtons(std::string_view spec, std::string* bad = nullptr) {
  uint16_t mask = 0;
  size_t i = 0;
  while (i < spec.size()) {
    while (i < spec.size() && (spec[i] == '+' || spec[i] == ',' || spec[i] == ' ' ||
                               spec[i] == '|')) {
      ++i;
    }
    size_t j = i;
    while (j < spec.size() && spec[j] != '+' && spec[j] != ',' && spec[j] != ' ' &&
           spec[j] != '|') {
      ++j;
    }
    if (j > i) {
      const auto bit = ButtonFromName(spec.substr(i, j - i));
      if (!bit) {
        if (bad) {
          *bad = std::string(spec.substr(i, j - i));
        }
        return std::nullopt;
      }
      mask = static_cast<uint16_t>(mask | *bit);
    }
    i = j;
  }
  return mask;
}

// -1..1 -> int16 stick axis (clamped; +1 -> 32767, -1 -> -32768).
inline int16_t AxisFromUnit(double v) {
  if (!std::isfinite(v)) {
    return 0;
  }
  v = std::clamp(v, -1.0, 1.0);
  const double scaled = v >= 0.0 ? v * 32767.0 : v * 32768.0;
  return static_cast<int16_t>(std::lround(scaled));
}

// 0..1 -> trigger 0..255.
inline uint8_t TriggerFromUnit(double v) {
  if (!std::isfinite(v)) {
    return 0;
  }
  v = std::clamp(v, 0.0, 1.0);
  return static_cast<uint8_t>(std::lround(v * 255.0));
}

// Overlay merge, matching the SDK's rex::input::MergeInto: buttons OR,
// triggers max, sticks the larger magnitude.
inline PadState Overlay(const PadState& a, const PadState& b) {
  auto larger = [](int16_t x, int16_t y) {
    return std::abs(static_cast<int>(x)) >= std::abs(static_cast<int>(y)) ? x : y;
  };
  PadState o;
  o.buttons = static_cast<uint16_t>(a.buttons | b.buttons);
  o.lt = std::max(a.lt, b.lt);
  o.rt = std::max(a.rt, b.rt);
  o.lx = larger(a.lx, b.lx);
  o.ly = larger(a.ly, b.ly);
  o.rx = larger(a.rx, b.rx);
  o.ry = larger(a.ry, b.ry);
  return o;
}

// ---------------------------------------------------------------- .s3rec

struct RecRow {
  uint64_t poll = 0;
  uint64_t frame = 0;
  PadState pad;
  bool operator==(const RecRow&) const = default;
};

struct Recording {
  std::string header_json = "{}";  // free-form metadata object
  std::vector<RecRow> rows;
};

inline constexpr std::string_view kRecMagic = "# s3rec 1 ";

inline std::string FormatRecRow(const RecRow& r) {
  char line[128];
  std::snprintf(line, sizeof(line), "%llu %llu %04x %u %u %d %d %d %d\n",
                static_cast<unsigned long long>(r.poll),
                static_cast<unsigned long long>(r.frame), r.pad.buttons, r.pad.lt, r.pad.rt,
                r.pad.lx, r.pad.ly, r.pad.rx, r.pad.ry);
  return line;
}

inline std::string FormatRecHeader(std::string_view header_json) {
  std::string out(kRecMagic);
  out.append(header_json.empty() ? std::string_view("{}") : header_json);
  out.push_back('\n');
  return out;
}

inline std::optional<RecRow> ParseRecRow(std::string_view line) {
  std::string s(line);
  unsigned long long poll = 0, frame = 0;
  unsigned buttons = 0, lt = 0, rt = 0;
  int lx = 0, ly = 0, rx = 0, ry = 0;
  if (std::sscanf(s.c_str(), "%llu %llu %x %u %u %d %d %d %d", &poll, &frame, &buttons, &lt, &rt,
                  &lx, &ly, &rx, &ry) != 9) {
    return std::nullopt;
  }
  auto in16 = [](int v) { return v >= -32768 && v <= 32767; };
  if (buttons > 0xFFFF || lt > 255 || rt > 255 || !in16(lx) || !in16(ly) || !in16(rx) ||
      !in16(ry)) {
    return std::nullopt;
  }
  RecRow r;
  r.poll = poll;
  r.frame = frame;
  r.pad = {static_cast<uint16_t>(buttons), static_cast<uint8_t>(lt), static_cast<uint8_t>(rt),
           static_cast<int16_t>(lx), static_cast<int16_t>(ly), static_cast<int16_t>(rx),
           static_cast<int16_t>(ry)};
  return r;
}

// Parses a whole .s3rec text. Blank lines and further '#' lines are
// skipped. Returns nullopt (with `error`) on a missing magic or a bad row.
inline std::optional<Recording> ParseRecording(std::string_view text, std::string* error) {
  Recording rec;
  size_t pos = 0;
  size_t line_no = 0;
  bool have_header = false;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) {
      nl = text.size();
    }
    std::string_view line = text.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    ++line_no;
    pos = nl + 1;
    if (!have_header) {
      if (line.substr(0, kRecMagic.size()) != kRecMagic) {
        if (error) {
          *error = "missing '# s3rec 1' header";
        }
        return std::nullopt;
      }
      rec.header_json = std::string(line.substr(kRecMagic.size()));
      have_header = true;
      continue;
    }
    if (line.empty() || line.front() == '#') {
      if (nl == text.size()) {
        break;
      }
      continue;
    }
    auto row = ParseRecRow(line);
    if (!row) {
      if (error) {
        *error = "bad row at line " + std::to_string(line_no);
      }
      return std::nullopt;
    }
    rec.rows.push_back(*row);
    if (nl == text.size()) {
      break;
    }
  }
  if (!have_header) {
    if (error) {
      *error = "empty recording";
    }
    return std::nullopt;
  }
  return rec;
}

// Pad for playback poll `poll` (polls count from the play start): the last
// row at or before it. Rows are in poll order; `cursor` caches the search.
inline const PadState* PadAtPoll(const Recording& rec, uint64_t poll, size_t* cursor) {
  if (rec.rows.empty()) {
    return nullptr;
  }
  size_t i = cursor ? *cursor : 0;
  if (i >= rec.rows.size() || rec.rows[i].poll > poll) {
    i = 0;
  }
  while (i + 1 < rec.rows.size() && rec.rows[i + 1].poll <= poll) {
    ++i;
  }
  if (cursor) {
    *cursor = i;
  }
  return rec.rows[i].poll <= poll ? &rec.rows[i].pad : nullptr;
}

// Minimal JSON string escaping for the files the harness writes.
inline std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

}  // namespace skate3::harness
