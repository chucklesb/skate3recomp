// Unit test for src/harness/harness_reels.cpp: finding the profile's
// REPLY_SKATER save, reading INDEX order and reel ids from the MC02 headers,
// and resolving an id (prefix) to its My Replays position.

#include "harness/harness_reels.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_check.h"

namespace fs = std::filesystem;
using namespace skate3::harness;

namespace {

void PutBE32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  b[off] = uint8_t(v >> 24);
  b[off + 1] = uint8_t(v >> 16);
  b[off + 2] = uint8_t(v >> 8);
  b[off + 3] = uint8_t(v);
}

// An MC02 file: magic, total size at +4, an 8-byte "hash" at +20, payload
// from +32.
std::vector<uint8_t> MC02(size_t size, uint8_t hash_seed) {
  std::vector<uint8_t> b(size, 0);
  b[0] = 'M';
  b[1] = 'C';
  b[2] = '0';
  b[3] = '2';
  PutBE32(b, 4, uint32_t(size));
  for (int i = 0; i < 8; ++i) {
    b[20 + i] = uint8_t(hash_seed + i * 17);
  }
  return b;
}

void Write(const fs::path& p, const std::vector<uint8_t>& b) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()),
                                           std::streamsize(b.size()));
}

// INDEX listing `slots` in order, then a 0 terminator.
void MakeSave(const fs::path& dir, const std::vector<uint32_t>& slots) {
  std::vector<uint8_t> index = MC02(96, 0x11);
  for (size_t i = 0; i < slots.size(); ++i) {
    PutBE32(index, 32 + 4 * i, slots[i]);
  }
  Write(dir / "INDEX", index);
  for (uint32_t s : slots) {
    Write(dir / (std::to_string(s) + ".B"), MC02(64, uint8_t(0xA0 + s)));
  }
}

}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / "skate3_harness_reels_test";
  fs::remove_all(root);

  // Nothing there yet.
  {
    reels::SetSaveRoots({root}, 0);
    std::vector<reels::Reel> list;
    fs::path dir;
    std::string err;
    CHECK(!reels::List(&list, &dir, &err));
    CHECK(!err.empty());
  }

  // Two profiles; the signed-in XUID picks the second. INDEX order (3, 1, 2)
  // is the list order, not the slot numbers.
  const fs::path other = root / "E000000000000001" / "454108E6" / "00000001" / "REPLY_SKATER";
  const fs::path mine = root / "B13E07DFF9AB6772" / "454108E6" / "00000001" / "REPLY_SKATER";
  MakeSave(other, {1});
  MakeSave(mine, {3, 1, 2});
  reels::SetSaveRoots({root / "missing", root}, 0xB13E07DFF9AB6772ull);
  {
    std::vector<reels::Reel> list;
    fs::path dir;
    std::string err;
    CHECK(reels::List(&list, &dir, &err));
    CHECK(dir == mine);
    CHECK(list.size() == 3);
    if (list.size() == 3) {
      CHECK(list[0].clip == 1 && list[0].slot == 3);
      CHECK(list[1].clip == 2 && list[1].slot == 1);
      CHECK(list[2].clip == 3 && list[2].slot == 2);
      // Slot 3's hash bytes: 0xA3, 0xA3+17, ... -> "a3b4c5d6e7f8091a".
      CHECK(list[0].id == "a3b4c5d6e7f8091a");
      CHECK(list[0].bytes == 64);
    }
    const std::string json = reels::ListJson(list);
    CHECK(json.find("\"clip\":1,\"slot\":3,\"id\":\"a3b4c5d6e7f8091a\"") != std::string::npos);
  }

  // Resolve: full id, prefix, upper case; too short, unknown, non-hex.
  {
    std::string err;
    reels::Reel found;
    CHECK(reels::Resolve("a3b4c5d6e7f8091a", &err, &found) == 1);
    CHECK(found.slot == 3);
    CHECK(reels::Resolve("A1B2C3", &err) == 2);  // slot 1: a1 b2 c3 ...
    CHECK(reels::Resolve("a2b3c4d5", &err) == 3);
    CHECK(reels::Resolve("a3b4c", &err) == 0);  // < 6 digits
    CHECK(err.find("at least 6") != std::string::npos);
    CHECK(reels::Resolve("ffffff", &err) == 0);
    CHECK(err.find("no saved reel") != std::string::npos);
    CHECK(reels::Resolve("a3b4c5-x", &err) == 0);
  }

  // Ambiguous prefix: two reels sharing the first 6 digits.
  {
    const fs::path amb = root / "amb" / "REPLY_SKATER";
    MakeSave(amb, {1, 2});
    std::vector<uint8_t> b = MC02(64, 0xA1);
    b[23] = 0x00;  // differs from slot 1 only after the 6th digit
    Write(amb / "2.B", b);
    reels::SetSaveRoots({root / "amb"}, 0);
    std::string err;
    CHECK(reels::Resolve("a1b2c3", &err) == 0);
    CHECK(err.find("ambiguous") != std::string::npos);
    CHECK(reels::Resolve("a1b2c3d4", &err) == 1);
  }

  // Not an MC02 INDEX.
  {
    const fs::path bad = root / "bad" / "REPLY_SKATER";
    Write(bad / "INDEX", std::vector<uint8_t>(64, 0x55));
    reels::SetSaveRoots({root / "bad"}, 0);
    std::vector<reels::Reel> list;
    fs::path dir;
    std::string err;
    CHECK(!reels::List(&list, &dir, &err));
    CHECK(err.find("MC02") != std::string::npos);
  }

  fs::remove_all(root);
  return skate3_test::TestExit("PASS: harness_reels_test");
}
