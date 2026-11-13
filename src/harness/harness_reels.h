#pragma once
// Saved skate.Reel replays, identified by their content rather than by their
// position in My Replays (play_reel clip = N shifts whenever a reel is saved
// or deleted).
//
// The profile's REPLY_SKATER save holds INDEX (an "MC02" container whose
// payload lists the reel slots, big-endian u32, in My Replays order, 0 =
// end), and per slot N.H (header with the thumbnail) and N.B (the replay),
// N in uppercase hex (slot 10 = A.B).
// Every MC02 file carries a content hash at +20; a reel's id is the first
// 8 bytes of N.B's (16 hex digits), stable for as long as the reel isn't
// saved again. The list position (clip) is the slot's place in INDEX,
// 1-based, matching the content manager list play_reel selects from.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skate3::harness::reels {

struct Reel {
  int clip = 0;          // 1-based position in My Replays (INDEX order)
  uint32_t slot = 0;     // file slot (N.B / N.H)
  std::string id;        // 16 hex digits
  uint64_t bytes = 0;    // N.B size
};

// App side, after the profile is applied: where saved-game content lives
// (the portable saves folder when active, the user data root) and the
// signed-in XUID (0 = unknown), used to pick the profile's REPLY_SKATER when
// several exist.
void SetSaveRoots(std::vector<std::filesystem::path> roots, uint64_t xuid);

// Lists the reels; false with *error when no REPLY_SKATER/INDEX was found
// or it doesn't parse. *dir receives the folder used.
bool List(std::vector<Reel>* out, std::filesystem::path* dir, std::string* error);

// Resolves an id or a unique id prefix (>= 6 hex digits, any case) to its
// list position; 0 with *error when not found or ambiguous.
int Resolve(const std::string& id_prefix, std::string* error, Reel* found = nullptr);

// JSON array of {clip, slot, id, bytes} for replies / events.
std::string ListJson(const std::vector<Reel>& reels);

}  // namespace skate3::harness::reels
