#pragma once
// The harness `mem` verb: find a game value in guest memory and change it
// (docs/scenario-harness.md, docs/re/README.md "Toolkit").
//
//   find   value=N             every aligned address holding N
//   find   (no value)          snapshot every mapped page in `range`, for an
//                              unknown value: change it in game, then refine
//   refine value=N | cmp=...   keep candidates now equal to N, or that
//                              changed / unchanged / increased / decreased
//                              since the last find or refine
//   read / write addr=0x...    one value (write also needs value)
//
// Types (big-endian, as the guest stores them): u8 u16 u32 f32 f64.
// The search state lives here between requests (one runner thread).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skate3::harness::mem {

struct Request {
  std::string op;            // find | refine | read | write
  std::string type = "u32";  // u8 | u16 | u32 | f32 | f64
  std::string cmp;           // refine: changed | unchanged | increased | decreased
  std::optional<double> value;
  uint32_t addr = 0;
  uint64_t lo = 0x10000;       // scanned range [lo, hi)
  uint64_t hi = 0x100000000ull;
};

bool ValidType(const std::string& type);
bool ValidCmp(const std::string& cmp);

// Runs one request. On success *json holds the result fields (the inside of
// a JSON object); on failure *error says why.
bool Run(uint8_t* base, const Request& req, std::string* json, std::string* error);

// A whole guest-memory snapshot (.gsnap, the format tools/re/gsnap.py reads:
// "SK3GSNP1", then {u64 LE guest offset, u64 LE size, bytes} regions, then
// {~0, 0}). Every 64 KB page that reads is kept; adjacent ones are merged.
// Portable (Windows, Linux, macOS). The game should be paused around it.
// Objects in [lo, hi) whose first word (big-endian) is `vtable` and whose
// u64 at +8 is `key`: the game's named records (e.g. challenges, vtable
// 0x82326AF4, keyed by their 64-bit name hash). Scans readable pages only.
std::vector<uint32_t> FindKeyedObjects(uint8_t* base, uint32_t vtable, uint64_t key, uint64_t lo,
                                       uint64_t hi);
bool WriteSnapshot(uint8_t* base, const std::string& path, uint64_t* bytes, uint32_t* regions,
                   std::string* error);

}  // namespace skate3::harness::mem
