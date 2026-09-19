#pragma once

// Backpressure Fabric - filesystem primitives for durable state.
//
// Durable mutation follows validate -> bind -> plan -> reserve -> journal ->
// work -> verify -> commit -> retire. These helpers provide the crash-safe
// primitives the journal is built from: bounded reads, atomic replacement via
// temporary file plus rename, and explicit durability barriers.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "backpressure/core/status.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace backpressure::fsutil {

/// Explicit durability barrier for a file handle.
[[nodiscard]] inline Status sync_handle(std::FILE* f) {
  if (std::fflush(f) != 0) {
    return Status::error(ErrorCode::IoError, "fflush");
  }
#if defined(_WIN32)
  if (::_commit(::_fileno(f)) != 0) {
    return Status::error(ErrorCode::IoError, "commit");
  }
#else
  if (::fsync(::fileno(f)) != 0) {
    return Status::error(ErrorCode::IoError, "fsync");
  }
#endif
  return Status::success();
}

[[nodiscard]] inline bool exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

[[nodiscard]] inline Status ensure_directory(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (std::filesystem::exists(p, ec)) {
    if (!std::filesystem::is_directory(p, ec)) {
      return Status::error(ErrorCode::IoError, "state path is not a directory");
    }
    return Status::success();
  }
  std::filesystem::create_directories(p, ec);
  if (ec) {
    return Status::error(ErrorCode::IoError, "create directories");
  }
  return Status::success();
}

/// Read at most \p max_bytes. Returns OversizedInput when the file is larger.
[[nodiscard]] inline Result<std::vector<std::byte>> read_file(const std::string& path,
                                                              std::size_t max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
  if (ec) {
    return fail<std::vector<std::byte>>(ErrorCode::NotFound, "read file size");
  }
  if (size > static_cast<std::uintmax_t>(max_bytes)) {
    return fail<std::vector<std::byte>>(ErrorCode::OversizedInput, "file size",
                                        static_cast<std::uint64_t>(size));
  }
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return fail<std::vector<std::byte>>(ErrorCode::IoError, "open for read");
  }
  std::vector<std::byte> out(static_cast<std::size_t>(size));
  std::size_t read = 0;
  if (!out.empty()) {
    read = std::fread(out.data(), 1, out.size(), f);
  }
  std::fclose(f);
  if (read != out.size()) {
    return fail<std::vector<std::byte>>(ErrorCode::TruncatedInput, "short read", read);
  }
  return Result<std::vector<std::byte>>(std::move(out));
}

/// Write atomically: temporary file, durability barrier, rename over target.
[[nodiscard]] inline Status write_file_atomic(const std::string& path,
                                              std::span<const std::byte> bytes) {
  const std::string temp = path + ".tmp";
  std::FILE* f = std::fopen(temp.c_str(), "wb");
  if (f == nullptr) {
    return Status::error(ErrorCode::IoError, "open temp for write");
  }
  if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
    std::fclose(f);
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(temp), ec);
    return Status::error(ErrorCode::IoError, "short write");
  }
  const Status synced = sync_handle(f);
  std::fclose(f);
  if (!synced.ok()) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(temp), ec);
    return synced;
  }
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(temp), std::filesystem::path(path), ec);
  if (ec) {
    std::filesystem::remove(std::filesystem::path(temp), ec);
    return Status::error(ErrorCode::IoError, "atomic rename");
  }
  return Status::success();
}

[[nodiscard]] inline Status remove_file(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(path), ec);
  if (ec) {
    return Status::error(ErrorCode::IoError, "remove file");
  }
  return Status::success();
}

/// Remove stray temporary artifacts left behind by an interrupted write.
[[nodiscard]] inline std::size_t remove_stale_temp_files(const std::string& directory) {
  std::error_code ec;
  std::size_t removed = 0;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(directory), ec)) {
    if (ec) {
      break;
    }
    const std::string name = entry.path().filename().string();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
      std::error_code inner;
      if (std::filesystem::remove(entry.path(), inner)) {
        ++removed;
      }
    }
  }
  return removed;
}

/// Exclusive, process-scoped lock over one file.
///
/// A durable store has exactly one writer. The lock is advisory at the
/// filesystem level but enforced by the operating system: a second process (or
/// a second store instance in the same process) that tries to open the same
/// directory is refused rather than allowed to interleave journal appends.
class ExclusiveLock {
 public:
  ExclusiveLock() = default;
  ExclusiveLock(const ExclusiveLock&) = delete;
  ExclusiveLock& operator=(const ExclusiveLock&) = delete;
  ExclusiveLock(ExclusiveLock&& other) noexcept;
  ExclusiveLock& operator=(ExclusiveLock&& other) noexcept;
  ~ExclusiveLock();

  [[nodiscard]] static Result<ExclusiveLock> acquire(const std::string& path);

  [[nodiscard]] Status release() noexcept;
  [[nodiscard]] bool held() const noexcept { return descriptor_ >= 0; }

 private:
  int descriptor_ = -1;
};

inline ExclusiveLock::ExclusiveLock(ExclusiveLock&& other) noexcept
    : descriptor_(other.descriptor_) {
  other.descriptor_ = -1;
}

inline ExclusiveLock& ExclusiveLock::operator=(ExclusiveLock&& other) noexcept {
  if (this != &other) {
    (void)release();
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
  }
  return *this;
}

inline ExclusiveLock::~ExclusiveLock() { (void)release(); }

inline Result<ExclusiveLock> ExclusiveLock::acquire(const std::string& path) {
  ExclusiveLock lock;
#if defined(_WIN32)
  int descriptor = -1;
  const errno_t rc = ::_sopen_s(&descriptor, path.c_str(), _O_CREAT | _O_RDWR | _O_BINARY,
                                _SH_DENYRW, _S_IREAD | _S_IWRITE);
  if (rc != 0 || descriptor < 0) {
    return fail<ExclusiveLock>(ErrorCode::Conflict, "durable store already open",
                               static_cast<std::uint64_t>(rc));
  }
  lock.descriptor_ = descriptor;
#else
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
  if (descriptor < 0) {
    return fail<ExclusiveLock>(ErrorCode::IoError, "open lock file");
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    ::close(descriptor);
    return fail<ExclusiveLock>(ErrorCode::Conflict, "durable store already open");
  }
  lock.descriptor_ = descriptor;
#endif
  return Result<ExclusiveLock>(std::move(lock));
}

inline Status ExclusiveLock::release() noexcept {
  if (descriptor_ < 0) {
    return Status::success();
  }
#if defined(_WIN32)
  ::_close(descriptor_);
#else
  (void)::flock(descriptor_, LOCK_UN);
  ::close(descriptor_);
#endif
  descriptor_ = -1;
  return Status::success();
}

}  // namespace backpressure::fsutil