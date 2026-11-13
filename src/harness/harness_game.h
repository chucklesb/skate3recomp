#pragma once
// Scenario harness: calls into the game's own functions (TU3 addresses).
//
// Guest functions can only run on a guest thread, with a PPCContext. The
// harness queues a request here; the main thread's per-frame game-state tick
// (sub_826D34B8 hook) runs it through OnGuestFrame and wakes the waiting
// harness thread. Never the render thread: allocators are per thread.
//
// Teleport: the same call the pause map's "teleport to location" event makes
// (docs/scenario-harness.md, "Notes for later work: game teleport").
//   world      = *(0x83083BCC)
//   registry   = world + 168: EASTL map {name hash -> {type byte, object}}
//   teleporter = *(*(world + 156) + 116)
//   locator    = registry.find(NameHash(name)) -> object (a 4x4 world matrix)
//   mode       = *(*(world + 156) + 40)   (vtable 0x8230DF88; +44 = allowed)
//   sub_82706F50(teleporter, locator, hash, flag, 0, 0, 0) -> needs streaming
//   if it returned 1: sub_82720B50(mode, 0x8270C7D0 teleporting state, teleporter)
// An already-loaded destination moves the player at once; otherwise the
// teleporting state streams the area in and moves the player a few seconds
// later.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;

namespace skate3::harness::game {

// sub_8296EA10: the ELF-style hash the registry is keyed by.
uint32_t NameHash(std::string_view name);

struct TeleportResult {
  bool ok = false;
  bool busy = false;  // transient: the game is mid-transition (retry shortly)
  std::string error;
  uint32_t hash = 0;
  float target[3] = {};   // the locator's translation
  bool streamed = false;  // the area had to be streamed in (teleporting state)
  bool via_service = false;  // the pause map's path (map key known)
  bool map_load = false;     // ... and it loads another map first
  std::string district;      // from the generated locator table ("" if unknown)
};

// map_key value that forces the lower-level sequencer path (no map load; the
// skater falls through a district that is not loaded, which the body-parts
// repro relies on).
constexpr uint64_t kForceSequencer = ~0ull;

// Harness thread: queue a teleport to a named locator (e.g. "Z_UN_ClockTower")
// and wait (up to timeout_s) for the guest thread to issue it. map_key 0 picks
// the key of the locator's district (when known).
TeleportResult Teleport(const std::string& location, double timeout_s, uint64_t map_key = 0);

// Harness thread: teleport the player to world coordinates `pos` facing
// `yaw_deg` (the heading convention of SkaterYawDeg). It builds a yaw-only
// 4x4 transform and hands it to the sequencer like a locator's, so it never
// loads another map: teleport to a locator in the destination's district
// first when it is not the current one.
TeleportResult TeleportToPos(const float pos[3], float yaw_deg, double timeout_s);

// The vetted teleport location (docs/teleport-locations.md) nearest to
// `pos`, and its distance in metres: the stepping stone a far coordinate
// teleport goes through first, so the area is loaded and streamed.
std::string NearestSpot(const float pos[3], float* distance = nullptr);

// The heading of an entity's local-to-world matrix (row-major, rows 0..2 =
// the basis), in degrees: atan2 of row 2's x over its z. TeleportToPos takes
// the same convention; a standing skater reads back within ~10 degrees (its
// idle pose turns the body a little).
float SkaterYawDeg(const float m[16]);

// skate.Reel playback (play_reel): load saved replay `slot` (1-based, as
// listed in My Replays) the game's own way, play it start to end at 1x with
// our own clock and no UI, then return to free skate. Harness thread: start,
// then poll the status.
enum class ReelPhase { kIdle, kLoading, kPlaying, kExiting, kDone, kFailed };
struct ReelOptions {
  bool from_marker = false;     // start at the reel's marker (the start if none)
  std::vector<float> stills_s;  // hold at these reel times (s) for screenshots
  float speed = 1.0f;           // replay clock rate (0.25 = quarter speed)
};
struct ReelStatus {
  ReelPhase phase = ReelPhase::kIdle;
  int slot = 0;
  float duration_s = 0;  // the reel's length
  float t_s = 0;         // playback position
  float start_s = 0;     // where playback started
  float marker_s = -1;   // the reel's marker, -1 if none
  uint64_t frames = 0;   // editor frames played
  bool holding = false;  // stopped at still hold_index, waiting for ReleaseReelHold
  int hold_index = -1;
  std::string error;
};
bool StartReel(int slot, const ReelOptions& options, std::string* error);
ReelStatus GetReelStatus();
// Harness thread: the still's screenshot is taken; continue playback.
void ReleaseReelHold();

// True while the harness wants the game's whole 2D pass (FE movies + HUD)
// skipped, e.g. during reel playback (FrontEndManager::Render2D hook).
bool SuppressUi();

// Main-thread game tick hook, after the original: runs the queued request, if any.
void OnGuestFrame(PPCContext& ctx, uint8_t* base);

}  // namespace skate3::harness::game
