#include "skate3_user_settings.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

#include <rex/cvar.h>

#include <toml++/toml.hpp>

namespace skate3 {
namespace {

std::string MakeProfileId(std::string_view gamertag) {
  std::string id;
  id.reserve(gamertag.size());
  bool previous_dash = false;
  for (char c : gamertag) {
    char out = 0;
    if (c >= 'A' && c <= 'Z') {
      out = static_cast<char>(c - 'A' + 'a');
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out = c;
    } else if (!previous_dash) {
      out = '-';
    }
    if (out) {
      id.push_back(out);
      previous_dash = out == '-';
    }
  }
  while (!id.empty() && id.back() == '-') {
    id.pop_back();
  }
  if (id.empty()) {
    id = "default";
  }
  return id;
}

uint64_t StableXuidForId(std::string_view id) {
  uint64_t hash = 1469598103934665603ull;
  for (char c : id) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  return 0xB13E000000000000ull | (hash & 0x00003FFFFFFFFFFFull);
}

bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool IsDecimalDigit(char c) {
  return c >= '0' && c <= '9';
}

// Parses the `xuid` string of a profiles.toml entry. The only writer,
// FormatXuid, emits exactly 16 uppercase hex digits (zero padded), and that
// form must keep reading back as hex. The rules, in order:
//   - a `0x` / `0X` prefix: hex, 1 to 16 digits after the prefix.
//   - exactly 16 hex digits: hex.
//   - only decimal digits, any other length: decimal.
//   - any other run of 1 to 15 hex digits with a letter A-F in it: hex,
//     since it can't be a decimal number (hand-edited files).
// Ambiguous case: 16 decimal digits ("1234567890123456") read as hex,
// because that is what FormatXuid writes for a XUID whose hex form has no
// letters. A decimal XUID of exactly 16 digits has to be written in hex or
// as a TOML integer (`xuid = 1234567890123456`, which LoadProfiles also
// accepts). Real decimal XUIDs (E000... / B13E... in hex) have 19 or 20
// digits and are unaffected.
// Anything else (empty, signs, other characters, out of range) returns 0,
// which LoadProfiles treats as "no XUID".
uint64_t ParseXuidString(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }

  int base = 0;
  if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
    value.remove_prefix(2);
    base = 16;
  }
  if (value.empty()) {
    return 0;
  }
  const bool all_hex = std::all_of(value.begin(), value.end(), IsHexDigit);
  const bool all_decimal = std::all_of(value.begin(), value.end(), IsDecimalDigit);
  if (base == 0) {
    if (all_hex && value.size() == 16) {
      base = 16;
    } else if (all_decimal) {
      base = 10;
    } else if (all_hex) {
      base = 16;
    } else {
      return 0;
    }
  }
  if (base == 16 && (!all_hex || value.size() > 16)) {
    return 0;
  }

  uint64_t parsed = 0;
  auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
  if (ec != std::errc() || ptr != value.data() + value.size()) {
    return 0;
  }
  return parsed;
}

// Quotes `value` as a TOML basic string. Per the TOML spec, `"` and `\` are
// escaped, the control characters with a short form use it (\b \t \n \f \r),
// and every other control character (U+0000-U+001F, U+007F) is written as
// \uXXXX. Bytes >= 0x80 pass through, so UTF-8 text stays as it is.
// LoadProfiles reads the file back with toml++, which decodes all of these.
std::string TomlString(std::string_view value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\f': out += "\\f"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20 || c == 0x7F) {
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace

std::filesystem::path ProfilesFilePath(const std::filesystem::path& user_data_root) {
  return user_data_root / "profiles" / "profiles.toml";
}

LocalProfileStore LoadProfiles(const std::filesystem::path& profiles_path) {
  LocalProfileStore store;
  if (!std::filesystem::exists(profiles_path)) {
    return store;
  }
  try {
    auto table = toml::parse_file(profiles_path.string());
    store.selected_profile = table["selected_profile"].value_or(std::string{});
    if (auto profiles = table["profiles"].as_array()) {
      profiles->for_each([&](toml::table& profile_table) {
        LocalProfile profile;
        profile.id = profile_table["id"].value_or(std::string{});
        profile.gamertag = profile_table["gamertag"].value_or(std::string{});
        profile.signed_in = profile_table["signed_in"].value_or(true);
        profile.live_signed_in = profile_table["live_signed_in"].value_or(false);
        if (auto xuid = profile_table["xuid"].value<std::string>()) {
          profile.xuid = ParseXuidString(*xuid);
        } else if (auto xuid_int = profile_table["xuid"].value<int64_t>()) {
          profile.xuid = static_cast<uint64_t>(*xuid_int);
        }
        if (!profile.id.empty() && !profile.gamertag.empty() && profile.xuid != 0) {
          store.profiles.push_back(std::move(profile));
        }
      });
    }
  } catch (const toml::parse_error&) {
    store = {};
  }
  return store;
}

bool SaveProfiles(const std::filesystem::path& profiles_path, const LocalProfileStore& store) {
  std::error_code ec;
  std::filesystem::create_directories(profiles_path.parent_path(), ec);
  if (ec) {
    return false;
  }

  std::ofstream file(profiles_path, std::ios::trunc);
  if (!file) {
    return false;
  }

  file << "selected_profile = " << TomlString(store.selected_profile) << "\n\n";
  for (const auto& profile : store.profiles) {
    file << "[[profiles]]\n";
    file << "id = " << TomlString(profile.id) << "\n";
    file << "gamertag = " << TomlString(profile.gamertag) << "\n";
    file << "xuid = " << TomlString(FormatXuid(profile.xuid)) << "\n";
    file << "signed_in = " << (profile.signed_in ? "true" : "false") << "\n";
    file << "live_signed_in = " << (profile.live_signed_in ? "true" : "false") << "\n\n";
  }
  return true;
}

LocalProfile MakeDefaultProfile(std::string gamertag) {
  if (gamertag.empty()) {
    gamertag = "Player";
  }
  LocalProfile profile;
  profile.id = MakeProfileId(gamertag);
  profile.gamertag = std::move(gamertag);
  profile.xuid = StableXuidForId(profile.id);
  return profile;
}

LocalProfile* FindSelectedProfile(LocalProfileStore& store) {
  auto it = std::find_if(store.profiles.begin(), store.profiles.end(),
                         [&](const LocalProfile& profile) {
                           return profile.id == store.selected_profile;
                         });
  if (it != store.profiles.end()) {
    return &*it;
  }
  return store.profiles.empty() ? nullptr : &store.profiles.front();
}

const LocalProfile* FindSelectedProfile(const LocalProfileStore& store) {
  auto it = std::find_if(store.profiles.begin(), store.profiles.end(),
                         [&](const LocalProfile& profile) {
                           return profile.id == store.selected_profile;
                         });
  if (it != store.profiles.end()) {
    return &*it;
  }
  return store.profiles.empty() ? nullptr : &store.profiles.front();
}

void EnsureUsableProfileStore(LocalProfileStore& store, std::string default_gamertag) {
  if (store.profiles.empty()) {
    auto profile = MakeDefaultProfile(std::move(default_gamertag));
    store.selected_profile = profile.id;
    store.profiles.push_back(std::move(profile));
    return;
  }
  if (!FindSelectedProfile(store)) {
    store.selected_profile = store.profiles.front().id;
  }
}

void ApplyProfileCvars(const LocalProfile& profile) {
  rex::cvar::SetFlagByName("selected_user_profile", profile.id);
  rex::cvar::SetFlagByName("user_profile_name", profile.gamertag);
  rex::cvar::SetFlagByName("user_profile_xuid", FormatXuid(profile.xuid));
  rex::cvar::SetFlagByName("user_profile_signed_in", profile.signed_in ? "true" : "false");
  rex::cvar::SetFlagByName("user_live_signed_in",
                           profile.signed_in && profile.live_signed_in ? "true" : "false");
}

void ApplyVideoCvars(int resolution_scale, double refresh_rate) {
  resolution_scale = std::clamp(resolution_scale, 1, 8);
  refresh_rate = std::clamp(refresh_rate, 24.0, 240.0);
  const auto scale = std::to_string(resolution_scale);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
  rex::cvar::SetFlagByName("video_mode_refresh_rate", std::to_string(refresh_rate));
}

std::string FormatXuid(uint64_t xuid) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << xuid;
  return stream.str();
}

}  // namespace skate3
