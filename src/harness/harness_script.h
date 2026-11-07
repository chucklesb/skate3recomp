#pragma once
// Scenario harness: scenario files and commands.
//
// A scenario is a TOML file:
//
//   name = "boot_to_plaza"
//   timeout_s = 300            # wall-clock limit; the run fails past it
//   boot = "demo"              # "demo": signed-out demo-path boot to gameplay
//   state_every_frames = 10    # state.jsonl trace interval (0 = off)
//   [cvars]                    # applied before boot, like --name=value
//   skate3_native_render_scene = true
//   [[step]]
//   do = "wait"
//   gameplay = true
//   ...
//
// Live commands (files dropped in <out>/inbox/) use the same keys as one
// [[step]], at the top level of their own TOML file, plus an optional `id`.
// Verbs and keys are listed with Verb below; README "Scenario harness" has
// the user-facing reference.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "harness/harness_pad.h"

namespace skate3::harness {

enum class Verb {
  kWait,        // gameplay=true | frames=N | ms=N | fe_screen=N | presence=N |
                // log="regex" | speed_gt=x | speed_lt=x ; timeout_s=N (default 120)
  kInput,       // buttons="a+lb" lt rt (0..1) lx ly rx ry (-1..1) frames=N
                // mode="override"|"overlay" async=false
  kPlay,        // file="x.s3rec" mode="override"|"overlay" async=false
  kRecord,      // start=true|false file="name.s3rec" note="..."
  kCvar,        // [step.set] name = value ...
  kScreenshot,  // tag="x" count=1 every_frames=1
  kState,       // tag="x" (one line now) | every_frames=N (trace interval)
  kMark,        // text="..."
  kSnapshot,    // full guest-memory snapshot via the diagnostics trigger file
  kPause,       // hold the game at the next frame boundary
  kStep,        // frames=N while paused
  kResume,
  kExpect,      // log="regex" | no_log="regex" | speed_gt | speed_lt | presence
  kExit,        // code=0
  kQuery,       // live channel only: what="state"|"status"
};

struct Step {
  Verb verb = Verb::kMark;
  std::string id;      // inbox correlation id
  size_t index = 0;    // position in the scenario (1-based) for messages

  // wait / expect
  bool gameplay = false;
  std::optional<int64_t> frames;
  std::optional<int64_t> ms;
  std::optional<int64_t> fe_screen;
  std::optional<int64_t> presence;
  std::optional<double> speed_gt;
  std::optional<double> speed_lt;
  std::string log;     // regex
  std::string no_log;  // regex (expect)
  double timeout_s = 120.0;

  // input / play
  PadState pad;
  bool override_pad = true;  // mode = "override" (default) | "overlay"
  bool async = false;

  // play / record
  std::string file;
  bool start = true;
  std::string note;

  // cvar
  std::vector<std::pair<std::string, std::string>> set;

  // screenshot / state
  std::string tag;
  int64_t count = 1;
  int64_t every_frames = 1;
  bool every_frames_given = false;

  // mark
  std::string text;

  // exit
  int code = 0;

  // query
  std::string what = "state";
};

struct Scenario {
  std::string name = "scenario";
  std::string path;
  double timeout_s = 600.0;
  bool boot_demo = true;
  int64_t state_every_frames = 10;
  std::vector<std::pair<std::string, std::string>> cvars;
  std::vector<Step> steps;
};

const char* VerbName(Verb v);

// Parse a scenario file. nullopt with `error` ("<file>: step 3: ...").
std::optional<Scenario> LoadScenario(const std::string& path, std::string* error);

// Parse one live command (TOML text). nullopt with `error`.
std::optional<Step> ParseCommand(const std::string& text, const std::string& source,
                                 std::string* error);

}  // namespace skate3::harness
