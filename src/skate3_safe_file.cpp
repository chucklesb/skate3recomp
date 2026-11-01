#include "skate3_safe_file.h"

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace skate3::safe_file {
namespace {

void SetError(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
}

#if defined(_WIN32)
std::string LastErrorMessage(DWORD code) {
  return std::system_category().message(static_cast<int>(code)) + " (error " +
         std::to_string(code) + ")";
}
#else
std::string ErrnoMessage(int code) {
  return std::generic_category().message(code) + " (errno " + std::to_string(code) + ")";
}
#endif

std::string LocalTimestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
  return buffer;
}

// Hard-links or renames `from` to `to` without ever replacing an existing
// `to`. Returns false with `error` set on failure; `from` is then untouched.
bool MoveNoReplace(const std::filesystem::path& from, const std::filesystem::path& to,
                   std::string* error) {
#if defined(_WIN32)
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
    SetError(error, LastErrorMessage(GetLastError()));
    return false;
  }
  return true;
#else
  // link() fails with EEXIST instead of replacing, unlike rename().
  if (::link(from.c_str(), to.c_str()) != 0) {
    SetError(error, ErrnoMessage(errno));
    return false;
  }
  if (::unlink(from.c_str()) != 0) {
    // Both names now point at the same bytes; the backup exists either way.
    SetError(error, ErrnoMessage(errno));
  }
  return true;
#endif
}

void PruneBackups(const std::filesystem::path& path, const std::filesystem::path& keep_path,
                  std::size_t keep_backups) {
  const std::filesystem::path dir = path.parent_path();
  const std::string prefix = PathForLog(path.filename()) + ".bad-";
  const std::string keep_name = PathForLog(keep_path.filename());
  std::vector<std::pair<std::string, std::filesystem::path>> backups;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir.empty() ? "." : dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    if (!it->is_regular_file(type_ec)) {
      continue;
    }
    std::string name = PathForLog(it->path().filename());
    if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
        name != keep_name) {
      backups.emplace_back(std::move(name), it->path());
    }
  }
  // Timestamps sort lexically; keep_path is the newest and counts as one.
  std::sort(backups.begin(), backups.end());
  const std::size_t allowed_others = keep_backups > 0 ? keep_backups - 1 : 0;
  if (backups.size() <= allowed_others) {
    return;
  }
  const std::size_t remove_count = backups.size() - allowed_others;
  for (std::size_t i = 0; i < remove_count; ++i) {
    std::error_code remove_ec;
    std::filesystem::remove(backups[i].second, remove_ec);
  }
}

}  // namespace

std::string PathForLog(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

ReadResult ReadWholeFile(const std::filesystem::path& path) {
  ReadResult result;
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  // A missing file is reported as not_found *and* sets `ec`, so check the
  // type first; any other error means the state is unknown.
  if (status.type() == std::filesystem::file_type::not_found) {
    result.status = ReadStatus::kMissing;
    return result;
  }
  if (ec) {
    result.error = "cannot stat: " + ec.message();
    return result;
  }
  if (!std::filesystem::is_regular_file(status)) {
    result.error = "not a regular file";
    return result;
  }

  errno = 0;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    const int code = errno;
    result.error = "cannot open: " +
                   (code ? std::generic_category().message(code) : std::string("unknown error"));
    return result;
  }
  char buffer[16384];
  while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
    result.bytes.append(buffer, static_cast<std::size_t>(file.gcount()));
    if (!file) {
      break;
    }
  }
  if (file.bad()) {
    result.bytes.clear();
    result.error = "read failed";
    return result;
  }
  result.status = ReadStatus::kOk;
  return result;
}

bool WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes,
                     std::string* error) {
  std::filesystem::path temp_path = path;
  temp_path += ".tmp";

#if defined(_WIN32)
  HANDLE handle = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    SetError(error, "cannot create " + PathForLog(temp_path) + ": " +
                        LastErrorMessage(GetLastError()));
    return false;
  }
  bool ok = true;
  std::string failure;
  std::size_t offset = 0;
  while (ok && offset < bytes.size()) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    DWORD written = 0;
    if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
      ok = false;
      failure = "write failed: " + LastErrorMessage(GetLastError());
    }
    offset += written;
  }
  if (ok && !FlushFileBuffers(handle)) {
    ok = false;
    failure = "flush failed: " + LastErrorMessage(GetLastError());
  }
  if (!CloseHandle(handle) && ok) {
    ok = false;
    failure = "close failed: " + LastErrorMessage(GetLastError());
  }
  if (ok && !MoveFileExW(temp_path.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ok = false;
    failure = "cannot replace " + PathForLog(path) + ": " + LastErrorMessage(GetLastError());
  }
  if (!ok) {
    DeleteFileW(temp_path.c_str());
    SetError(error, std::move(failure));
  }
  return ok;
#else
  const int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    SetError(error, "cannot create " + PathForLog(temp_path) + ": " + ErrnoMessage(errno));
    return false;
  }
  bool ok = true;
  std::string failure;
  std::size_t offset = 0;
  while (ok && offset < bytes.size()) {
    const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      failure = "write failed: " + ErrnoMessage(errno);
    } else {
      offset += static_cast<std::size_t>(written);
    }
  }
  if (ok && ::fsync(fd) != 0) {
    ok = false;
    failure = "fsync failed: " + ErrnoMessage(errno);
  }
  if (::close(fd) != 0 && ok) {
    ok = false;
    failure = "close failed: " + ErrnoMessage(errno);
  }
  if (ok && ::rename(temp_path.c_str(), path.c_str()) != 0) {
    ok = false;
    failure = "cannot replace " + PathForLog(path) + ": " + ErrnoMessage(errno);
  }
  if (!ok) {
    ::unlink(temp_path.c_str());
    SetError(error, std::move(failure));
    return false;
  }
  // Best effort: persist the rename itself.
  const auto dir = path.parent_path();
  const int dir_fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (dir_fd >= 0) {
    ::fsync(dir_fd);
    ::close(dir_fd);
  }
  return true;
#endif
}

std::optional<std::filesystem::path> QuarantineFile(const std::filesystem::path& path,
                                                    std::string_view expected_bytes,
                                                    std::size_t keep_backups,
                                                    std::string* error) {
  const std::string stamp = LocalTimestamp();
  std::filesystem::path backup;
  for (int n = 1; n <= 100; ++n) {
    std::filesystem::path candidate = path;
    candidate += ".bad-" + stamp + (n > 1 ? "-" + std::to_string(n) : std::string());
    std::error_code ec;
    const bool taken = std::filesystem::exists(candidate, ec);
    if (!ec && !taken) {
      backup = std::move(candidate);
      break;
    }
  }
  if (backup.empty()) {
    SetError(error, "no free backup name for " + PathForLog(path));
    return std::nullopt;
  }

  std::string move_error;
  if (!MoveNoReplace(path, backup, &move_error)) {
    // The original stays in place; take a verified copy instead.
    std::error_code copy_ec;
    std::filesystem::copy_file(path, backup, std::filesystem::copy_options::none, copy_ec);
    if (copy_ec) {
      if (copy_ec != std::errc::file_exists) {
        std::error_code remove_ec;
        std::filesystem::remove(backup, remove_ec);
      }
      SetError(error, "move failed (" + move_error + "), copy failed (" + copy_ec.message() +
                          ")");
      return std::nullopt;
    }
    const ReadResult check = ReadWholeFile(backup);
    if (check.status != ReadStatus::kOk || check.bytes != expected_bytes) {
      std::error_code remove_ec;
      std::filesystem::remove(backup, remove_ec);
      SetError(error, "move failed (" + move_error + "), copy did not match the original");
      return std::nullopt;
    }
  }

  PruneBackups(path, backup, keep_backups);
  return backup;
}

}  // namespace skate3::safe_file
