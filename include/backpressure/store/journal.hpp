#pragma once

// Backpressure Fabric - crash-safe append-only journal.
//
// The journal is the durable source of truth for configuration, fences,
// epoch/boot binding, propagation lineage and attempt outcomes. Every record is
// length delimited, CRC protected and chained to its predecessor, so a torn or
// tampered tail is detected rather than interpreted. A damaged tail is
// truncated back to the last intact record; the reader never invents state.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/generation.hpp"
#include "backpressure/store/fsutil.hpp"

namespace backpressure {

enum class RecordType : std::uint16_t {
  Epoch = 1,
  Boot = 2,
  Policy = 3,
  FencePut = 4,
  FenceRemove = 5,
  TopologyBinding = 6,
  Lineage = 7,
  Revalidation = 8,
  AttemptBegin = 9,
  AttemptCommit = 10,
  AttemptAbort = 11,
  Compaction = 12,
  Count,
};

[[nodiscard]] constexpr const char* to_string(RecordType t) noexcept {
  switch (t) {
    case RecordType::Epoch: return "Epoch";
    case RecordType::Boot: return "Boot";
    case RecordType::Policy: return "Policy";
    case RecordType::FencePut: return "FencePut";
    case RecordType::FenceRemove: return "FenceRemove";
    case RecordType::TopologyBinding: return "TopologyBinding";
    case RecordType::Lineage: return "Lineage";
    case RecordType::Revalidation: return "Revalidation";
    case RecordType::AttemptBegin: return "AttemptBegin";
    case RecordType::AttemptCommit: return "AttemptCommit";
    case RecordType::AttemptAbort: return "AttemptAbort";
    case RecordType::Compaction: return "Compaction";
    case RecordType::Count: break;
  }
  return "Invalid";
}

struct JournalLimits {
  std::size_t max_record_bytes = 1u << 12;
  std::size_t max_records = 1u << 18;
  std::size_t max_file_bytes = 1ull << 30;
  /// Bound on the bytes the journal may retain in memory for replay/compaction.
  std::size_t max_retained_bytes = 1u << 28;
};

struct JournalRecord {
  std::uint64_t sequence = 0;
  RecordType type = RecordType::Epoch;
  Tick tick = kNoTick;
  Epoch epoch{};
  Digest128 chain{};
  std::vector<std::byte> payload{};
};

struct JournalStats {
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
  std::uint64_t truncated_tail_records = 0;
  std::uint64_t sequence = 0;
  Digest128 chain{};
  bool repaired = false;
};

/// Append-only journal over a single file.
class Journal {
 public:
  static constexpr std::size_t kHeaderBytes = 32;
  static constexpr std::size_t kRecordHeaderBytes = 52;
  static constexpr char kMagic[8] = {'B', 'P', 'F', 'A', 'B', 'J', '0', '1'};

  Journal() = default;
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  Journal(Journal&& other) noexcept;
  Journal& operator=(Journal&& other) noexcept;
  ~Journal();

  /// Open (creating when asked) and replay. A damaged or torn tail is repaired
  /// by truncation; the number of discarded records is reported in stats().
  [[nodiscard]] static Result<Journal> open(const std::string& path,
                                            const JournalLimits& limits,
                                            bool create_if_missing);

  [[nodiscard]] Status append(RecordType type, Tick tick, Epoch epoch,
                              std::span<const std::byte> payload);
  [[nodiscard]] Status flush();
  [[nodiscard]] Status sync();

  [[nodiscard]] const std::vector<JournalRecord>& records() const noexcept { return records_; }
  [[nodiscard]] const JournalStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
  [[nodiscard]] const JournalLimits& limits() const noexcept { return limits_; }
  [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_bytes_; }

  /// Replace the journal contents with \p records, crash safely.
  [[nodiscard]] Status rewrite(const std::vector<JournalRecord>& records);

  void close() noexcept;

 private:
  [[nodiscard]] Status open_handle(bool for_append);
  [[nodiscard]] Status write_header();
  [[nodiscard]] Result<std::size_t> replay_file();

  std::string path_;
  JournalLimits limits_{};
  std::FILE* handle_ = nullptr;
  std::vector<JournalRecord> records_{};
  JournalStats stats_{};
  std::uint64_t file_bytes_ = 0;
  std::size_t retained_bytes_ = 0;
};

}  // namespace backpressure
