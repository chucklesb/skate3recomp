#pragma once

// Crash-safe file helpers for small user files (profiles.toml).
//
// - ReadWholeFile tells "missing" apart from "exists but can't be read"
//   (locked, permission denied), so callers never treat a read failure as an
//   empty file.
// - WriteFileAtomic writes a temp file next to the target, flushes it to disk
//   and then replaces the target in one rename, so a crash mid-save leaves
//   either the old file or the new one, never a truncated one.
// - QuarantineFile moves a file that failed to parse aside to
//   `<name>.bad-<timestamp>` before anything may overwrite it.
//
// No logging and no rex dependencies: callers log the returned errors.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace skate3::safe_file {

enum class ReadStatus {
  kOk,       // `bytes` holds the whole file.
  kMissing,  // The file does not exist.
  kError,    // It exists (or its state is unknown) but could not be read.
};

struct ReadResult {
  ReadStatus status = ReadStatus::kError;
  std::string bytes;
  std::string error;  // Set for kError.
};

ReadResult ReadWholeFile(const std::filesystem::path& path);

// Returns false (and sets `error`) if the target was not replaced. On failure
// the target is untouched and the temp file is removed.
bool WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes,
                     std::string* error = nullptr);

// Moves `path` to `<path>.bad-YYYYMMDD-HHMMSS[-N]` in the same folder. If the
// move fails (for example another process has the file open without delete
// sharing), copies it there instead and checks the copy matches
// `expected_bytes` (the bytes the caller read). Afterwards prunes the oldest
// backups so at most `keep_backups` remain (the new one is never pruned).
// Returns the backup path, or nullopt with `error` set if no verified backup
// exists; the caller must then not overwrite `path`.
std::optional<std::filesystem::path> QuarantineFile(const std::filesystem::path& path,
                                                    std::string_view expected_bytes,
                                                    std::size_t keep_backups,
                                                    std::string* error = nullptr);

// UTF-8 form of a path for log messages (path::string() can throw on Windows
// for characters outside the ANSI code page).
std::string PathForLog(const std::filesystem::path& path);

}  // namespace skate3::safe_file
