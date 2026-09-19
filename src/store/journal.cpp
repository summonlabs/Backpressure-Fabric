// Backpressure Fabric - crash-safe journal implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/store/journal.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace backpressure {
namespace {

constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint32_t kHeaderMagicCrcSeed = 0x42414650u;

void put_u16(std::byte* p, std::uint16_t v) noexcept {
  p[0] = std::byte{static_cast<unsigned char>(v & 0xFFu)};
  p[1] = std::byte{static_cast<unsigned char>((v >> 8u) & 0xFFu)};
}

void put_u32(std::byte* p, std::uint32_t v) noexcept {
  for (unsigned i = 0; i < 4u; ++i) {
    p[i] = std::byte{static_cast<unsigned char>((v >> (8u * i)) & 0xFFu)};
  }
}

void put_u64(std::byte* p, std::uint64_t v) noexcept {
  for (unsigned i = 0; i < 8u; ++i) {
    p[i] = std::byte{static_cast<unsigned char>((v >> (8u * i)) & 0xFFu)};
  }
}

[[nodiscard]] std::uint16_t get_u16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(p[1]))
                                    << 8u);
}

[[nodiscard]] std::uint32_t get_u32(const std::byte* p) noexcept {
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4u; ++i) {
    v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (8u * i);
  }
  return v;
}

[[nodiscard]] std::uint64_t get_u64(const std::byte* p) noexcept {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8u; ++i) {
    v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8u * i);
  }
  return v;
}

[[nodiscard]] Status truncate_handle(std::FILE* f, std::uint64_t size) {
#if defined(_WIN32)
  if (::_chsize_s(::_fileno(f), static_cast<__int64>(size)) != 0) {
    return Status::error(ErrorCode::IoError, "truncate");
  }
#else
  if (::ftruncate(::fileno(f), static_cast<off_t>(size)) != 0) {
    return Status::error(ErrorCode::IoError, "truncate");
  }
#endif
  return Status::success();
}

[[nodiscard]] Status seek_end(std::FILE* f) {
  if (std::fseek(f, 0, SEEK_END) != 0) {
    return Status::error(ErrorCode::IoError, "seek end");
  }
  return Status::success();
}

[[nodiscard]] Digest128 chain_of(Digest128 previous, std::uint64_t sequence, RecordType type,
                                 Tick tick, Epoch epoch,
                                 std::span<const std::byte> payload) noexcept {
  DigestBuilder b;
  b.domain(0x90u);
  b.update_digest(previous);
  b.update_u64(sequence);
  b.update_u16(static_cast<std::uint16_t>(type));
  b.update_u64(tick);
  digest_epoch(b, epoch);
  b.update(payload);
  return b.finish();
}

}  // namespace

Journal::Journal(Journal&& other) noexcept
    : path_(std::move(other.path_)),
      limits_(other.limits_),
      handle_(other.handle_),
      records_(std::move(other.records_)),
      stats_(other.stats_),
      file_bytes_(other.file_bytes_),
      retained_bytes_(other.retained_bytes_) {
  other.handle_ = nullptr;
}

Journal& Journal::operator=(Journal&& other) noexcept {
  if (this != &other) {
    close();
    path_ = std::move(other.path_);
    limits_ = other.limits_;
    handle_ = other.handle_;
    records_ = std::move(other.records_);
    stats_ = other.stats_;
    file_bytes_ = other.file_bytes_;
    retained_bytes_ = other.retained_bytes_;
    other.handle_ = nullptr;
  }
  return *this;
}

Journal::~Journal() { close(); }

void Journal::close() noexcept {
  if (handle_ != nullptr) {
    std::fclose(handle_);
    handle_ = nullptr;
  }
}

Status Journal::open_handle(bool for_append) {
  if (handle_ != nullptr) {
    std::fclose(handle_);
    handle_ = nullptr;
  }
  handle_ = std::fopen(path_.c_str(), for_append ? "r+b" : "rb");
  if (handle_ == nullptr) {
    return Status::error(ErrorCode::IoError, "open journal");
  }
  return Status::success();
}

Status Journal::write_header() {
  std::byte header[kHeaderBytes] = {};
  std::memcpy(header, kMagic, 8);
  put_u16(header + 8, kFormatVersion);
  put_u16(header + 10, 0);
  put_u32(header + 12, static_cast<std::uint32_t>(kHeaderBytes));
  put_u64(header + 16, 0);
  put_u32(header + 24, 0);
  const std::uint32_t crc =
      crc32_update(kHeaderMagicCrcSeed, std::span<const std::byte>(header, 28));
  put_u32(header + 28, crc);
  if (std::fwrite(header, 1, kHeaderBytes, handle_) != kHeaderBytes) {
    return Status::error(ErrorCode::IoError, "write journal header");
  }
  if (std::fflush(handle_) != 0) {
    return Status::error(ErrorCode::IoError, "flush journal header");
  }
  file_bytes_ = kHeaderBytes;
  return Status::success();
}

Result<std::size_t> Journal::replay_file() {
  if (std::fseek(handle_, 0, SEEK_SET) != 0) {
    return fail<std::size_t>(ErrorCode::IoError, "seek start");
  }
  std::byte header[kHeaderBytes] = {};
  if (std::fread(header, 1, kHeaderBytes, handle_) != kHeaderBytes) {
    return fail<std::size_t>(ErrorCode::TruncatedInput, "journal header");
  }
  if (std::memcmp(header, kMagic, 8) != 0) {
    return fail<std::size_t>(ErrorCode::MalformedInput, "journal magic");
  }
  if (get_u16(header + 8) != kFormatVersion) {
    return fail<std::size_t>(ErrorCode::VersionMismatch, "journal version", get_u16(header + 8));
  }
  const std::uint32_t crc =
      crc32_update(kHeaderMagicCrcSeed, std::span<const std::byte>(header, 28));
  if (crc != get_u32(header + 28)) {
    return fail<std::size_t>(ErrorCode::IntegrityMismatch, "journal header crc");
  }

  std::uint64_t offset = kHeaderBytes;
  Digest128 previous{};
  std::vector<std::byte> record_header(kRecordHeaderBytes);
  for (;;) {
    const std::size_t got = std::fread(record_header.data(), 1, kRecordHeaderBytes, handle_);
    if (got == 0) {
      break;
    }
    if (got != kRecordHeaderBytes) {
      ++stats_.truncated_tail_records;
      break;
    }
    const std::uint32_t length = get_u32(record_header.data());
    const std::uint32_t stored_crc = get_u32(record_header.data() + 4);
    if (length > limits_.max_record_bytes) {
      ++stats_.truncated_tail_records;
      break;
    }
    if (records_.size() >= limits_.max_records) {
      ++stats_.truncated_tail_records;
      break;
    }
    std::vector<std::byte> payload(length);
    if (length > 0 && std::fread(payload.data(), 1, length, handle_) != length) {
      ++stats_.truncated_tail_records;
      break;
    }
    const std::uint32_t crc_value =
        crc32_update(crc32(std::span<const std::byte>(record_header.data() + 8, kRecordHeaderBytes - 8u)),
                     payload);
    if (crc_value != stored_crc) {
      ++stats_.truncated_tail_records;
      break;
    }
    JournalRecord record;
    record.sequence = get_u64(record_header.data() + 8);
    const std::uint16_t raw_type = get_u16(record_header.data() + 16);
    if (raw_type == 0 || raw_type >= static_cast<std::uint16_t>(RecordType::Count)) {
      ++stats_.truncated_tail_records;
      break;
    }
    record.type = static_cast<RecordType>(raw_type);
    record.tick = get_u64(record_header.data() + 20);
    record.epoch = Epoch::from_raw(get_u64(record_header.data() + 28));
    record.chain = Digest128{get_u64(record_header.data() + 36), get_u64(record_header.data() + 44)};
    const Digest128 expected =
        chain_of(previous, record.sequence, record.type, record.tick, record.epoch, payload);
    if (expected != record.chain) {
      ++stats_.truncated_tail_records;
      break;
    }
    if (!records_.empty() && record.sequence != records_.back().sequence + 1u) {
      ++stats_.truncated_tail_records;
      break;
    }
    if (records_.empty() && record.sequence != 1u) {
      ++stats_.truncated_tail_records;
      break;
    }
    previous = record.chain;
    stats_.sequence = record.sequence;
    record.payload = std::move(payload);
    retained_bytes_ += record.payload.size();
    if (retained_bytes_ > limits_.max_retained_bytes) {
      return fail<std::size_t>(ErrorCode::LimitExceeded, "journal retained bytes",
                               retained_bytes_);
    }
    records_.push_back(std::move(record));
    offset += kRecordHeaderBytes + length;
  }

  stats_.records = records_.size();
  stats_.chain = previous;
  stats_.bytes = offset;

  std::error_code ec;
  const auto file_size = std::filesystem::file_size(std::filesystem::path(path_), ec);
  if (!ec && static_cast<std::uint64_t>(file_size) > offset) {
    BPFAB_TRY(truncate_handle(handle_, offset));
    stats_.repaired = true;
  }
  BPFAB_TRY(seek_end(handle_));
  file_bytes_ = offset;
  return Result<std::size_t>(static_cast<std::size_t>(offset));
}

Result<Journal> Journal::open(const std::string& path, const JournalLimits& limits,
                              bool create_if_missing) {
  if (limits.max_record_bytes < 16 || limits.max_records == 0 || limits.max_file_bytes < kHeaderBytes) {
    return fail<Journal>(ErrorCode::InvalidArgument, "journal limits");
  }
  const bool present = fsutil::exists(path);
  if (!present && !create_if_missing) {
    return fail<Journal>(ErrorCode::NotFound, "journal missing");
  }

  Journal journal;
  journal.path_ = path;
  journal.limits_ = limits;

  if (!present) {
    journal.handle_ = std::fopen(path.c_str(), "w+b");
    if (journal.handle_ == nullptr) {
      return fail<Journal>(ErrorCode::IoError, "create journal");
    }
    BPFAB_TRY(journal.write_header());
  } else {
    BPFAB_TRY(journal.open_handle(true));
  }
  const Result<std::size_t> replayed = journal.replay_file();
  if (!replayed.ok()) {
    return fail<Journal>(replayed.status().code(), replayed.status().context());
  }
  return Result<Journal>(std::move(journal));
}

Status Journal::append(RecordType type, Tick tick, Epoch epoch,
                       std::span<const std::byte> payload) {
  if (handle_ == nullptr) {
    return Status::error(ErrorCode::NotReady, "journal closed");
  }
  if (type == RecordType::Count) {
    return Status::error(ErrorCode::InvalidArgument, "record type");
  }
  if (payload.size() > limits_.max_record_bytes) {
    return Status::error(ErrorCode::OversizedInput, "record payload", payload.size());
  }
  if (records_.size() >= limits_.max_records) {
    return Status::error(ErrorCode::LimitExceeded, "journal records", records_.size());
  }
  const std::size_t record_bytes = kRecordHeaderBytes + payload.size();
  if (file_bytes_ + record_bytes > limits_.max_file_bytes) {
    return Status::error(ErrorCode::LimitExceeded, "journal file bytes", file_bytes_);
  }
  if (retained_bytes_ + payload.size() > limits_.max_retained_bytes) {
    return Status::error(ErrorCode::LimitExceeded, "journal retained bytes", retained_bytes_);
  }

  const std::uint64_t sequence = stats_.sequence + 1u;
  const Digest128 chain = chain_of(stats_.chain, sequence, type, tick, epoch, payload);

  std::vector<std::byte> buffer(record_bytes);
  put_u32(buffer.data(), static_cast<std::uint32_t>(payload.size()));
  put_u64(buffer.data() + 8, sequence);
  put_u16(buffer.data() + 16, static_cast<std::uint16_t>(type));
  put_u16(buffer.data() + 18, 0);
  put_u64(buffer.data() + 20, tick);
  put_u64(buffer.data() + 28, epoch.value());
  put_u64(buffer.data() + 36, chain.hi);
  put_u64(buffer.data() + 44, chain.lo);
  if (!payload.empty()) {
    std::memcpy(buffer.data() + kRecordHeaderBytes, payload.data(), payload.size());
  }
  const std::uint32_t crc_value = crc32_update(
      crc32(std::span<const std::byte>(buffer.data() + 8, kRecordHeaderBytes - 8u)), payload);
  put_u32(buffer.data() + 4, crc_value);

  if (std::fwrite(buffer.data(), 1, buffer.size(), handle_) != buffer.size()) {
    return Status::error(ErrorCode::IoError, "journal append");
  }

  JournalRecord record;
  record.sequence = sequence;
  record.type = type;
  record.tick = tick;
  record.epoch = epoch;
  record.chain = chain;
  record.payload.assign(payload.begin(), payload.end());

  stats_.sequence = sequence;
  stats_.chain = chain;
  stats_.records = records_.size() + 1u;
  file_bytes_ += record_bytes;
  stats_.bytes = file_bytes_;
  retained_bytes_ += record.payload.size();
  records_.push_back(std::move(record));
  return Status::success();
}

Status Journal::flush() {
  if (handle_ == nullptr) {
    return Status::error(ErrorCode::NotReady, "journal closed");
  }
  if (std::fflush(handle_) != 0) {
    return Status::error(ErrorCode::IoError, "journal flush");
  }
  return Status::success();
}

Status Journal::sync() {
  if (handle_ == nullptr) {
    return Status::error(ErrorCode::NotReady, "journal closed");
  }
  return fsutil::sync_handle(handle_);
}

Status Journal::rewrite(const std::vector<JournalRecord>& records) {
  if (records.size() > limits_.max_records) {
    return Status::error(ErrorCode::LimitExceeded, "rewrite records", records.size());
  }
  std::vector<std::byte> buffer;
  buffer.resize(kHeaderBytes);
  std::memcpy(buffer.data(), kMagic, 8);
  put_u16(buffer.data() + 8, kFormatVersion);
  put_u16(buffer.data() + 10, 0);
  put_u32(buffer.data() + 12, static_cast<std::uint32_t>(kHeaderBytes));
  put_u64(buffer.data() + 16, 0);
  put_u32(buffer.data() + 24, 0);
  put_u32(buffer.data() + 28,
          crc32_update(kHeaderMagicCrcSeed, std::span<const std::byte>(buffer.data(), 28)));

  Digest128 previous{};
  std::uint64_t sequence = 0;
  for (const JournalRecord& record : records) {
    if (record.payload.size() > limits_.max_record_bytes) {
      return Status::error(ErrorCode::OversizedInput, "rewrite payload", record.payload.size());
    }
    ++sequence;
    const Digest128 chain =
        chain_of(previous, sequence, record.type, record.tick, record.epoch, record.payload);
    const std::size_t base = buffer.size();
    buffer.resize(base + kRecordHeaderBytes + record.payload.size());
    std::byte* p = buffer.data() + base;
    put_u32(p, static_cast<std::uint32_t>(record.payload.size()));
    put_u64(p + 8, sequence);
    put_u16(p + 16, static_cast<std::uint16_t>(record.type));
    put_u16(p + 18, 0);
    put_u64(p + 20, record.tick);
    put_u64(p + 28, record.epoch.value());
    put_u64(p + 36, chain.hi);
    put_u64(p + 44, chain.lo);
    if (!record.payload.empty()) {
      std::memcpy(p + kRecordHeaderBytes, record.payload.data(), record.payload.size());
    }
    put_u32(p + 4, crc32_update(crc32(std::span<const std::byte>(p + 8, kRecordHeaderBytes - 8u)),
                                record.payload));
    previous = chain;
    if (buffer.size() > limits_.max_file_bytes) {
      return Status::error(ErrorCode::LimitExceeded, "rewrite file bytes", buffer.size());
    }
  }

  close();
  // The previous contents are superseded: the in-memory replay state must be
  // forgotten before the rewritten file is replayed, otherwise the sequence and
  // chain checks compare new records against records that no longer exist.
  records_.clear();
  stats_ = JournalStats{};
  retained_bytes_ = 0;
  file_bytes_ = 0;
  BPFAB_TRY(fsutil::write_file_atomic(path_, std::span<const std::byte>(buffer.data(), buffer.size())));
  BPFAB_TRY(open_handle(true));
  const Result<std::size_t> replayed = replay_file();
  if (!replayed.ok()) {
    return replayed.status();
  }
  return Status::success();
}

}  // namespace backpressure