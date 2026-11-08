#pragma once
// Scenario harness: calls into the game's own functions (TU3 addresses).
//
// Guest functions can only run on a guest thread, with a PPCContext. The
// harness queues a request here; the guest D3D Swap hook (frame boundary)
// runs it through OnGuestFrame and wakes the waiting harness thread.
//
// Teleport: the same call the pause map's "teleport to location" event makes
// (README "Notes for later work: game teleport").
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

// Guest D3D Swap hook, before the original: runs the queued request, if any.
void OnGuestFrame(PPCContext& ctx, uint8_t* base);

}  // namespace skate3::harness::game
