#include "harness/harness_reels.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <system_error>

namespace skate3::harness::reels {
namespace fs = std::filesystem;

namespace {

std::mutex g_mu;
std::vector<fs::path> g_roots;
uint64_t g_xuid = 0;

constexpr uint32_t kMagicMC02 = 0x4D433032;  // "MC02"

bool ReadPrefix(const fs::path& p, size_t n, std::vector<uint8_t>* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    return false;
  }
  out->resize(n);
  f.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(n));
  out->resize(static_cast<size_t>(f.gcount()));
  return true;
}

uint32_t BE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

std::string Hex(const uint8_t* p, size_t n) {
  static const char* kDigits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += kDigits[p[i] >> 4];
    s += kDigits[p[i] & 15];
  }
  return s;
}

// Every REPLY_SKATER folder with an INDEX under the roots (shallow search:
// <root>/<xuid>/<title>/<type>/REPLY_SKATER or <saves>/<xuid>/REPLY_SKATER).
std::vector<fs::path> FindReplyDirs(const std::vector<fs::path>& roots) {
  std::vector<fs::path> found;
  for (const fs::path& root : roots) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
      continue;
    }
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it.depth() > 5) {
        it.disable_recursion_pending();
        continue;
      }
      if (it->is_directory(ec) && it->path().filename() == "REPLY_SKATER" &&
          fs::is_regular_file(it->path() / "INDEX", ec)) {
        found.push_back(it->path());
        it.disable_recursion_pending();
      }
    }
  }
  return found;
}

}  // namespace

void SetSaveRoots(std::vector<fs::path> roots, uint64_t xuid) {
  std::lock_guard lk(g_mu);
  g_roots = std::move(roots);
  g_xuid = xuid;
}

bool List(std::vector<Reel>* out, fs::path* dir, std::string* error) {
  out->clear();
  std::vector<fs::path> roots;
  uint64_t xuid = 0;
  {
    std::lock_guard lk(g_mu);
    roots = g_roots;
    xuid = g_xuid;
  }
  const std::vector<fs::path> dirs = FindReplyDirs(roots);
  if (dirs.empty()) {
    *error = "no saved replays (no REPLY_SKATER/INDEX under the save roots)";
    return false;
  }
  fs::path chosen = dirs.front();
  if (dirs.size() > 1 && xuid != 0) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(xuid));
    for (const fs::path& d : dirs) {
      std::string s = d.generic_string();
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
      if (s.find(hex) != std::string::npos) {
        chosen = d;
        break;
      }
    }
  }
  if (dir) {
    *dir = chosen;
  }
  std::vector<uint8_t> index;
  if (!ReadPrefix(chosen / "INDEX", 4096, &index) || index.size() < 36 ||
      BE32(index.data()) != kMagicMC02) {
    *error = "REPLY_SKATER/INDEX is not an MC02 file: " + chosen.generic_string();
    return false;
  }
  const size_t total = std::min<size_t>(BE32(index.data() + 4), index.size());
  for (size_t off = 32; off + 4 <= total; off += 4) {
    const uint32_t slot = BE32(index.data() + off);
    if (slot == 0) {
      break;
    }
    Reel r;
    r.clip = static_cast<int>(out->size()) + 1;
    r.slot = slot;
    // The game names slot files in uppercase hex (slot 10 = A.B).
    char name[16];
    std::snprintf(name, sizeof(name), "%X.B", slot);
    const fs::path body = chosen / name;
    std::vector<uint8_t> head;
    if (ReadPrefix(body, 36, &head) && head.size() >= 28 && BE32(head.data()) == kMagicMC02) {
      r.id = Hex(head.data() + 20, 8);
    }
    std::error_code ec;
    r.bytes = fs::file_size(body, ec);
    if (ec) {
      r.bytes = 0;
    }
    out->push_back(r);
  }
  return true;
}

int Resolve(const std::string& id_prefix, std::string* error, Reel* found) {
  std::string want;
  for (char c : id_prefix) {
    if (std::isxdigit(static_cast<unsigned char>(c))) {
      want += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  if (want.size() < 6 || want.size() != id_prefix.size()) {
    *error = "reel id must be at least 6 hex digits: '" + id_prefix + "'";
    return 0;
  }
  std::vector<Reel> list;
  fs::path dir;
  if (!List(&list, &dir, error)) {
    return 0;
  }
  const Reel* match = nullptr;
  for (const Reel& r : list) {
    if (r.id.compare(0, want.size(), want) == 0) {
      if (match) {
        *error = "reel id '" + id_prefix + "' is ambiguous (clips " + std::to_string(match->clip) +
                 " and " + std::to_string(r.clip) + ")";
        return 0;
      }
      match = &r;
    }
  }
  if (!match) {
    *error = "no saved reel with id '" + id_prefix + "' in " + dir.generic_string() + " (" +
             std::to_string(list.size()) + " reels: " + ListJson(list) + ")";
    return 0;
  }
  if (found) {
    *found = *match;
  }
  return match->clip;
}

std::string ListJson(const std::vector<Reel>& reels) {
  std::string s = "[";
  for (size_t i = 0; i < reels.size(); ++i) {
    const Reel& r = reels[i];
    if (i) {
      s += ",";
    }
    s += "{\"clip\":" + std::to_string(r.clip) + ",\"slot\":" + std::to_string(r.slot) +
         ",\"id\":\"" + r.id + "\",\"bytes\":" + std::to_string(r.bytes) + "}";
  }
  return s + "]";
}

}  // namespace skate3::harness::reels
