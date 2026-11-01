#include "skate3_user_settings.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <toml++/toml.hpp>

#include "skate3_safe_file.h"

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

// Reads one element of the `profiles` array. Returns nullopt for an entry
// that is not a table or lacks a usable id, gamertag or xuid; LoadProfiles
// skips those and SaveProfiles backs the file up before dropping them.
std::optional<LocalProfile> ParseProfileEntry(const toml::node& node) {
  const auto* profile_table = node.as_table();
  if (!profile_table) {
    return std::nullopt;
  }
  LocalProfile profile;
  profile.id = (*profile_table)["id"].value_or(std::string{});
  profile.gamertag = (*profile_table)["gamertag"].value_or(std::string{});
  profile.signed_in = (*profile_table)["signed_in"].value_or(true);
  profile.live_signed_in = (*profile_table)["live_signed_in"].value_or(false);
  if (auto xuid = (*profile_table)["xuid"].value<std::string>()) {
    profile.xuid = ParseXuidString(*xuid);
  } else if (auto xuid_int = (*profile_table)["xuid"].value<int64_t>()) {
    profile.xuid = static_cast<uint64_t>(*xuid_int);
  }
  if (profile.id.empty() || profile.gamertag.empty() || profile.xuid == 0) {
    return std::nullopt;
  }
  return profile;
}

// True if replacing a file holding `bytes` with a fresh save would lose
// something: the file does not parse (it was edited or broken after
// LoadProfiles), or it has profile entries LoadProfiles skipped.
bool SaveWouldDropData(const std::string& bytes, const std::string& log_path) {
  toml::table table;
  try {
    table = toml::parse(bytes, log_path);
  } catch (const toml::parse_error&) {
    return true;
  }
  if (auto profiles = table["profiles"].as_array()) {
    for (auto& node : *profiles) {
      if (!ParseProfileEntry(node)) {
        return true;
      }
    }
  } else if (table.contains("profiles")) {
    return true;  // `profiles` is not an array: LoadProfiles ignored all of it.
  }
  return false;
}

}  // namespace

std::filesystem::path ProfilesFilePath(const std::filesystem::path& user_data_root) {
  return user_data_root / "profiles" / "profiles.toml";
}

LocalProfileStore LoadProfiles(const std::filesystem::path& profiles_path) {
  // How many `profiles.toml.bad-*` backups of unparseable files to keep.
  constexpr std::size_t kMaxBadBackups = 5;

  LocalProfileStore store;
  const std::string log_path = safe_file::PathForLog(profiles_path);
  auto read = safe_file::ReadWholeFile(profiles_path);
  if (read.status == safe_file::ReadStatus::kMissing) {
    return store;
  }
  if (read.status != safe_file::ReadStatus::kOk) {
    // Locked, permission denied, ...: the file may be perfectly fine, so it
    // must not be treated as empty and overwritten.
    REXLOG_ERROR("Profiles: cannot read {}: {}. Using a temporary default profile; "
                 "profile changes are not saved until the file can be read.",
                 log_path, read.error);
    store.save_blocked = true;
    return store;
  }

  toml::table table;
  try {
    table = toml::parse(read.bytes, log_path);
  } catch (const toml::parse_error& err) {
    const auto& where = err.source().begin;
    REXLOG_ERROR("Profiles: {} is malformed at line {}, column {}: {}", log_path, where.line,
                 where.column, err.description());
    std::string backup_error;
    if (auto backup = safe_file::QuarantineFile(profiles_path, read.bytes, kMaxBadBackups,
                                                &backup_error)) {
      REXLOG_ERROR("Profiles: PROFILES RESET. The unparseable file is backed up as {} "
                   "(original bytes kept); a default profile replaces it. Fix that file "
                   "and rename it back to profiles.toml to restore the old profiles.",
                   safe_file::PathForLog(*backup));
    } else {
      REXLOG_ERROR("Profiles: could not back up {} ({}). Using a temporary default profile; "
                   "the file is left untouched and profile changes are not saved.",
                   log_path, backup_error);
      store.save_blocked = true;
    }
    return store;
  }

  store.selected_profile = table["selected_profile"].value_or(std::string{});
  size_t skipped = 0;
  if (auto profiles = table["profiles"].as_array()) {
    for (auto& node : *profiles) {
      if (auto profile = ParseProfileEntry(node)) {
        store.profiles.push_back(std::move(*profile));
      } else {
        ++skipped;
      }
    }
  } else if (table.contains("profiles")) {
    ++skipped;
  }
  if (skipped != 0) {
    REXLOG_WARN("Profiles: {} skipped {} entr{} without a usable id, gamertag and xuid; "
                "the next profile save first backs the file up as {}.dropped-<time>, then "
                "drops them",
                log_path, skipped, skipped == 1 ? "y" : "ies",
                safe_file::PathForLog(profiles_path.filename()));
  }
  return store;
}

bool SaveProfiles(const std::filesystem::path& profiles_path, const LocalProfileStore& store) {
  // How many `profiles.toml.dropped-*` backups to keep.
  constexpr std::size_t kMaxDroppedBackups = 5;

  const std::string log_path = safe_file::PathForLog(profiles_path);
  if (store.save_blocked) {
    REXLOG_WARN("Profiles: not saving {}: it could not be read or backed up this session",
                log_path);
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(profiles_path.parent_path(), ec);
  if (ec) {
    REXLOG_ERROR("Profiles: cannot create {}: {}",
                 safe_file::PathForLog(profiles_path.parent_path()), ec.message());
    return false;
  }

  std::ostringstream file;
  file << "selected_profile = " << TomlString(store.selected_profile) << "\n\n";
  for (const auto& profile : store.profiles) {
    file << "[[profiles]]\n";
    file << "id = " << TomlString(profile.id) << "\n";
    file << "gamertag = " << TomlString(profile.gamertag) << "\n";
    file << "xuid = " << TomlString(FormatXuid(profile.xuid)) << "\n";
    file << "signed_in = " << (profile.signed_in ? "true" : "false") << "\n";
    file << "live_signed_in = " << (profile.live_signed_in ? "true" : "false") << "\n\n";
  }

  std::string bytes = file.str();
#if defined(_WIN32)
  // Same CRLF line endings as the text-mode stream this used to write with.
  std::string crlf;
  crlf.reserve(bytes.size() + bytes.size() / 16);
  for (char c : bytes) {
    if (c == '\n') {
      crlf.push_back('\r');
    }
    crlf.push_back(c);
  }
  bytes = std::move(crlf);
#endif

  // The save rewrites the file from `store`, which lacks any entries
  // LoadProfiles skipped. Keep the old file's bytes first, so a hand-edited
  // entry with a bad xuid (or similar) is never lost silently. Once saved,
  // the file has no such entries, so this happens once, not on every save.
  auto current = safe_file::ReadWholeFile(profiles_path);
  if (current.status == safe_file::ReadStatus::kError) {
    REXLOG_ERROR("Profiles: not saving {}: cannot read the current file to check it: {}",
                 log_path, current.error);
    return false;
  }
  if (current.status == safe_file::ReadStatus::kOk &&
      SaveWouldDropData(current.bytes, log_path)) {
    std::string backup_error;
    auto backup = safe_file::WriteBackupCopy(profiles_path, ".dropped-", current.bytes,
                                             kMaxDroppedBackups, &backup_error);
    if (!backup) {
      REXLOG_ERROR("Profiles: not saving {}: it has entries this save would drop, and backing "
                   "it up failed: {}",
                   log_path, backup_error);
      return false;
    }
    REXLOG_WARN("Profiles: {} has entries this save drops; the old file is backed up as {}",
                log_path, safe_file::PathForLog(*backup));
  }

  std::string write_error;
  if (!safe_file::WriteFileAtomic(profiles_path, bytes, &write_error)) {
    REXLOG_ERROR("Profiles: save failed, {} left unchanged: {}", log_path, write_error);
    return false;
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
