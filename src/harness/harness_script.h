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
// Verbs and keys are listed with Verb below; docs/scenario-harness.md has
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
  kCapture,     // tag="x" frames=1: RenderDoc capture of the next N frames (RenderDoc attached)
  kCrash,       // where="runner"|"guest": fault on purpose (needs allow_crash = true)
  kTeleport,    // location="Z_UN_ClockTower" (a registry locator) tolerance=5 timeout_s=60
                // settle=true (false: return on arrival, e.g. a floorless locator)
  kRenderer,    // renderer="native"|"emulated" frames=30: the F5 switch, then settle
  kPlayReel,    // clip=N (a saved skate.Reel replay, 1-based as listed in My Replays):
                // plays it start to end at 1x with no UI, then returns to free skate;
                // from="start"|"marker" stills=[s...] settle_frames=20 every_frames=N tag=x
                // lens=false (true: keep the camera's fisheye and vignette)
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

  // crash
  std::string where = "runner";

  // teleport
  std::string location;
  double tolerance = 5.0;  // metres from the locator the skater must end up
  bool settle = true;      // false: done once the skater reaches the locator (may keep moving)
  int retries = 3;         // re-issues after falling through an unloaded floor
  uint64_t map_key = 0;    // override of the destination's map key (hex string in TOML)
  bool via_sequencer = false;  // path = "sequencer": the lower-level teleport (no map load)
  // pos = [x, y, z]: teleport to world coordinates (the `state` skater pos)
  // instead of / after a locator; yaw = heading in degrees (the `state` yaw),
  // default the skater's current heading.
  bool has_pos = false;
  float pos[3] = {};
  std::optional<double> yaw;
  // pos without location: hop to the nearest vetted locator first (loads and
  // streams the area). Unset = only when the target is far from the skater.
  std::optional<bool> hop;

  // play_reel
  int64_t clip = 0;            // 1-based saved replay slot
  std::string reel;            // or: the reel's id (hex prefix; see harness_reels.h)
  std::string from = "start";  // "start" | "marker"
  bool lens = false;           // keep the reel camera's fisheye and vignette
  bool stills_capture = false; // a RenderDoc capture at each still (RenderDoc attached)
  std::vector<double> stills;  // reel times (s) to hold at for screenshots
  int64_t settle_frames = 20;  // frames to wait at a still before its screenshot
  double speed = 1.0;          // replay clock rate (0 < speed <= 4; 0.25 = quarter)
  std::vector<std::string> stills_renderers;  // shoot each still on these renderers
  // Shoot each still once per variant: set its cvars, settle, screenshot
  // tagged _<tag>. Values persist (a last variant can reset them).
  struct StillVariant {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> set;
  };
  std::vector<StillVariant> stills_variants;

  // renderer
  std::string renderer;  // "native" | "emulated"
};

struct Scenario {
  std::string name = "scenario";
  std::string path;
  double timeout_s = 600.0;
  bool boot_demo = true;
  int64_t state_every_frames = 10;
  // Vulkan validation layers wanted. The instance exists before the harness
  // starts, so run_scenario.py passes --vulkan_validation_enabled and
  // --vulkan_log_debug_messages; the harness only checks and reports.
  bool vulkan_validation = false;
  // The `crash` verb only runs in a scenario that opts in.
  bool allow_crash = false;
  // Locator to teleport to after the first `wait gameplay = true` step and a
  // 90-frame settle (a map-loading teleport issued on the gameplay frame
  // itself never completes), so every target starts at one place
  // (docs/teleport-locations.md).
  std::string start_spot;
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
