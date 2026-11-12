#pragma once
// Scenario harness: scripted, recordable, observable game runs.
//
// Off unless --skate3_harness_scenario=<file.toml> (an unattended run that
// exits with a result code) or --skate3_harness=true (live channel and
// input recording only) is given. With both off every entry point below is
// a single relaxed atomic load.
//
// Each run writes <skate3_harness_out>/<stamp>_<name>/:
//   result.json   status, exit code, failures, counts
//   events.jsonl  every step and command with frame number and time
//   state.jsonl   periodic game state (presence, FE screen, skater, camera)
//   log.txt       this run's log lines
//   shots/        PNG screenshots
//   *.s3rec       input recordings
// <skate3_harness_out>/inbox/*.toml takes live commands (same keys as a
// scenario [[step]]); replies land in <skate3_harness_out>/outbox/.
// docs/scenario-harness.md documents the scenario format.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skate3::harness {

// Latest presented guest frame as tightly packed RGBA8 (alpha 255).
using CaptureFn = std::function<bool(std::vector<uint8_t>& rgba, uint32_t& width,
                                     uint32_t& height)>;

// OnPostSetup, before the selected profile is applied: loads the scenario
// and applies its cvars (including the demo-path boot), so the profile
// override and the demo-path hooks see them. Exits the process with code 3
// when the scenario file does not parse.
void Prepare();

// Asks the app to shut down normally (any thread; returns at once).
using CloseFn = std::function<void()>;

// OnPostSetup, after the guest hooks are installed: starts the run, the
// live channel and the log tap. With RenderDoc attached, a finished run
// shuts the game down through `close` instead of exiting the process at
// once (a hard exit right after a capture can hang in the GPU driver);
// result.json is written first either way.
void Start(CaptureFn capture, CloseFn close = {});

// OnShutdown.
void Shutdown();

bool Enabled();

// Guest D3D Swap (frame boundary), guest thread.
void OnFrame(uint8_t* base);

// Sk8::SkaterPresEntity::BindConstants, r3 = entity. Guest thread.
void OnSkaterEntity(uint32_t entity);

// Toggle input recording (bind), any thread.
void ToggleRecording();

// Heartbeats from existing guest hooks (any guest thread):
// Sk8::Challenge::PhotoReplayController::Update (replay playback/editor) and
// rw::movie::MovieDecoder::Decode (any movie). State reports each as active
// while it ticked within the last few frames.
void OnReplayTick();
void OnMovieTick();

// Free-form event for events.jsonl from other subsystems (any thread);
// `fields_json` is the inside of a JSON object ("\"a\":1,\"b\":\"x\"").
void Note(const char* kind, const std::string& fields_json);

// For the on-screen indicator (UI thread).
struct Status {
  bool enabled = false;
  bool recording = false;
  bool playing = false;
  bool scripted = false;
  bool paused = false;
  double record_seconds = 0.0;
};
Status CurrentStatus();

}  // namespace skate3::harness
