// Backpressure Fabric - journal and durable state tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"
#include "support.hpp"

using namespace backpressure;

namespace {

std::string journal_path(const std::string& directory) {
  return (std::filesystem::path(directory) / "fabric.bpfj").string();
}

void truncate_file(const std::string& path, std::uintmax_t bytes) {
  std::error_code ec;
  std::filesystem::resize_file(std::filesystem::path(path), bytes, ec);
}

std::uintmax_t file_size(const std::string& path) {
  std::error_code ec;
  return std::filesystem::file_size(std::filesystem::path(path), ec);
}

void flip_byte(const std::string& path, std::uintmax_t offset) {
  std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
  stream.seekg(static_cast<std::streamoff>(offset));
  char value = 0;
  stream.read(&value, 1);
  value = static_cast<char>(value ^ 0x5A);
  stream.seekp(static_cast<std::streamoff>(offset));
  stream.write(&value, 1);
}

}  // namespace

BPFAB_TEST(store, journal_appends_and_replays_in_order) {
  const std::string directory = bpfab_test::make_scratch_directory("journal");
  JournalLimits limits;
  BPFAB_REQUIRE_RESULT(journal, Journal::open(journal_path(directory), limits, true));
  BPFAB_CHECK(journal.records().empty());
  BPFAB_CHECK(journal.stats().sequence == 0u);

  ByteWriter writer(64);
  BPFAB_REQUIRE(writer.u32(7u).ok());
  const std::vector<std::byte> payload = writer.take();
  BPFAB_REQUIRE_OK(journal.append(RecordType::Lineage, 10, Epoch(3), payload));
  BPFAB_REQUIRE_OK(journal.append(RecordType::Lineage, 11, Epoch(3),
                                  std::span<const std::byte>()));
  BPFAB_REQUIRE_OK(journal.sync());
  BPFAB_CHECK(journal.records().size() == 2u);
  BPFAB_CHECK(journal.stats().sequence == 2u);
  const Digest128 chain = journal.stats().chain;
  BPFAB_CHECK(!chain.is_zero());
  journal.close();

  BPFAB_REQUIRE_RESULT(reopened, Journal::open(journal_path(directory), limits, false));
  BPFAB_CHECK(reopened.records().size() == 2u);
  BPFAB_CHECK(reopened.records()[0].type == RecordType::Lineage);
  BPFAB_CHECK(reopened.records()[0].tick == 10u);
  BPFAB_CHECK(reopened.records()[0].epoch == Epoch(3));
  BPFAB_CHECK(reopened.records()[0].payload.size() == 4u);
  BPFAB_CHECK(reopened.records()[1].payload.empty());
  BPFAB_CHECK(reopened.stats().chain == chain);
  BPFAB_CHECK(!reopened.stats().repaired);
}

BPFAB_TEST(store, torn_tail_is_repaired_rather_than_interpreted) {
  const std::string directory = bpfab_test::make_scratch_directory("torn");
  const std::string path = journal_path(directory);
  JournalLimits limits;
  {
    BPFAB_REQUIRE_RESULT(journal, Journal::open(path, limits, true));
    for (std::uint32_t i = 0; i < 6u; ++i) {
      ByteWriter writer(16);
      BPFAB_REQUIRE(writer.u32(i).ok());
      const std::vector<std::byte> payload = writer.take();
      BPFAB_REQUIRE_OK(journal.append(RecordType::Lineage, i, Epoch(1), payload));
    }
    BPFAB_REQUIRE_OK(journal.sync());
  }
  const std::uintmax_t full = file_size(path);
  truncate_file(path, full - 7u);

  BPFAB_REQUIRE_RESULT(repaired, Journal::open(path, limits, false));
  BPFAB_CHECK(repaired.records().size() < 6u);
  BPFAB_CHECK(repaired.stats().truncated_tail_records >= 1u);
  BPFAB_CHECK(repaired.stats().repaired);
  BPFAB_CHECK(file_size(path) == repaired.stats().bytes);

  // The repaired journal accepts new records normally.
  BPFAB_REQUIRE_OK(repaired.append(RecordType::Epoch, 99, Epoch(2),
                                   std::span<const std::byte>()));
  BPFAB_REQUIRE_OK(repaired.sync());
  BPFAB_CHECK(repaired.records().back().tick == 99u);
}

BPFAB_TEST(store, corrupt_record_is_detected_and_discarded) {
  const std::string directory = bpfab_test::make_scratch_directory("corrupt");
  const std::string path = journal_path(directory);
  JournalLimits limits;
  {
    BPFAB_REQUIRE_RESULT(journal, Journal::open(path, limits, true));
    ByteWriter writer(16);
    BPFAB_REQUIRE(writer.u64(0x1122334455667788ull).ok());
    const std::vector<std::byte> payload = writer.take();
    BPFAB_REQUIRE_OK(journal.append(RecordType::Lineage, 1, Epoch(1), payload));
    BPFAB_REQUIRE_OK(journal.append(RecordType::Lineage, 2, Epoch(1), payload));
    BPFAB_REQUIRE_OK(journal.append(RecordType::Lineage, 3, Epoch(1), payload));
    BPFAB_REQUIRE_OK(journal.sync());
  }
  const std::uintmax_t record_bytes = Journal::kRecordHeaderBytes + 8u;
  flip_byte(path, Journal::kHeaderBytes + record_bytes + Journal::kRecordHeaderBytes + 2u);

  BPFAB_REQUIRE_RESULT(journal, Journal::open(path, limits, false));
  BPFAB_CHECK(journal.records().size() == 1u);
  BPFAB_CHECK(journal.stats().truncated_tail_records == 1u);
  BPFAB_CHECK(journal.stats().repaired);
}

BPFAB_TEST(store, journal_refuses_bad_headers_and_bounds) {
  const std::string directory = bpfab_test::make_scratch_directory("headers");
  const std::string path = journal_path(directory);
  JournalLimits limits;
  {
    BPFAB_REQUIRE_RESULT(journal, Journal::open(path, limits, true));
    BPFAB_REQUIRE_OK(journal.append(RecordType::Epoch, 1, Epoch(1), std::span<const std::byte>()));
    BPFAB_REQUIRE_OK(journal.sync());
  }
  flip_byte(path, 0u);
  BPFAB_REQUIRE_CODE(Journal::open(path, limits, false).status(), ErrorCode::MalformedInput);
  flip_byte(path, 0u);

  flip_byte(path, 8u);
  BPFAB_REQUIRE_CODE(Journal::open(path, limits, false).status(), ErrorCode::VersionMismatch);
  flip_byte(path, 8u);

  flip_byte(path, 30u);
  BPFAB_REQUIRE_CODE(Journal::open(path, limits, false).status(), ErrorCode::IntegrityMismatch);
  flip_byte(path, 30u);

  const Result<Journal> intact = Journal::open(path, limits, false);
  BPFAB_REQUIRE(intact.ok());

  JournalLimits tight;
  tight.max_records = 1;
  const std::string second_directory = bpfab_test::make_scratch_directory("bounds");
  const std::string second_path = journal_path(second_directory);
  BPFAB_REQUIRE_RESULT(limited, Journal::open(second_path, tight, true));
  BPFAB_REQUIRE_OK(limited.append(RecordType::Epoch, 1, Epoch(1), std::span<const std::byte>()));
  BPFAB_REQUIRE_CODE(limited.append(RecordType::Epoch, 2, Epoch(1), std::span<const std::byte>()),
                     ErrorCode::LimitExceeded);

  JournalLimits tiny;
  tiny.max_record_bytes = 16;
  const std::string third_directory = bpfab_test::make_scratch_directory("payload");
  BPFAB_REQUIRE_RESULT(payload_bounded,
                       Journal::open(journal_path(third_directory), tiny, true));
  const std::vector<std::byte> oversized(32, std::byte{1});
  BPFAB_REQUIRE_CODE(payload_bounded.append(RecordType::Lineage, 1, Epoch(1), oversized),
                     ErrorCode::OversizedInput);

  BPFAB_REQUIRE_CODE(Journal::open(journal_path(directory), limits, false).status(),
                     ErrorCode::Ok);
}

BPFAB_TEST(store, durable_store_advances_the_epoch_and_records_the_boot) {
  const std::string directory = bpfab_test::make_scratch_directory("epoch");
  DurableConfig config;
  config.directory = directory;

  Epoch first_epoch;
  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_CHECK(!store.state().epoch.known());
    BPFAB_REQUIRE_OK(store.establish_epoch(0));
    BPFAB_CHECK(store.state().epoch.value() == 1u);
    BPFAB_CHECK(!store.state().previous_epoch.known());
    const Incarnation incarnation = bpfab_test::test_incarnation(4u, 11u);
    BPFAB_REQUIRE_OK(store.record_boot(incarnation, 0));
    BPFAB_CHECK(store.state().boot_recorded);
    BPFAB_CHECK(store.state().incarnation == incarnation);
    first_epoch = store.state().epoch;
    BPFAB_CHECK(!store.state().restored_liveness);
  }
  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_CHECK(store.state().epoch == first_epoch);
    // The journal records a single epoch so far, so nothing preceded it.
    BPFAB_CHECK(!store.state().previous_epoch.known());
    BPFAB_REQUIRE_OK(store.establish_epoch(1));
    BPFAB_CHECK(store.state().epoch.value() == first_epoch.value() + 1u);
    BPFAB_CHECK(store.state().previous_epoch == first_epoch);
    BPFAB_CHECK(store.state().records_replayed >= 2u);
    BPFAB_CHECK(!store.state().restored_liveness);
  }
  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_REQUIRE_OK(store.establish_epoch(2));
    BPFAB_CHECK(store.state().epoch.value() == 3u);
  }

  DurableConfig missing;
  BPFAB_REQUIRE_CODE(DurableStore::open(missing).status(), ErrorCode::InvalidArgument);
}

BPFAB_TEST(store, durable_store_persists_policy_topology_and_fences) {
  const std::string directory = bpfab_test::make_scratch_directory("persist");
  DurableConfig config;
  config.directory = directory;
  PropagationPolicy policy = bpfab_test::test_policy(3u, 4u);
  Digest128 topology_digest = digest_of("topology-one");

  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_REQUIRE_OK(store.establish_epoch(0));
    BPFAB_REQUIRE_OK(store.record_boot(bpfab_test::test_incarnation(1u, 1u), 0));
    BPFAB_REQUIRE_OK(store.persist_policy(policy, 0));
    BPFAB_REQUIRE_OK(store.bind_topology(topology_digest, Generation::from_raw(5u), 0));

    Fence volatile_fence;
    volatile_fence.id = FenceId(1u);
    volatile_fence.resource = ResourceId(1u);
    volatile_fence.kind = FenceKind::Barrier;
    volatile_fence.epoch = store.state().epoch;
    BPFAB_REQUIRE_OK(store.put_fence(volatile_fence, 0));

    Fence durable_fence = volatile_fence;
    durable_fence.id = FenceId(2u);
    durable_fence.resource = ResourceId(2u);
    durable_fence.durable = true;
    BPFAB_REQUIRE_OK(store.put_fence(durable_fence, 0));
    BPFAB_REQUIRE_OK(store.record_lineage(digest_of("lineage-a"), 0));
    BPFAB_REQUIRE_OK(store.record_lineage(digest_of("lineage-b"), 0));
    BPFAB_REQUIRE_OK(store.request_revalidation(ResourceId(9u), 0));
    BPFAB_REQUIRE_OK(store.sync());
  }

  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_CHECK(store.state().has_policy);
    BPFAB_CHECK(store.state().policy.id == policy.id);
    BPFAB_CHECK(store.state().policy.generation == policy.generation);
    BPFAB_CHECK(store.state().policy.digest() == policy.digest());
    BPFAB_CHECK(store.state().has_topology_binding);
    BPFAB_CHECK(store.state().topology_digest == topology_digest);
    BPFAB_CHECK(store.state().topology_generation == Generation::from_raw(5u));
    BPFAB_CHECK(store.fence_count() == 2u);
    BPFAB_CHECK(store.state().lineages.size() == 2u);
    BPFAB_CHECK(store.state().revalidation_required.size() == 1u);
    BPFAB_CHECK(store.state().revalidation_required[0] == ResourceId(9u));

    BPFAB_REQUIRE_OK(store.clear_revalidation(ResourceId(9u), 1));
    BPFAB_CHECK(store.state().revalidation_required.empty());
    BPFAB_REQUIRE_OK(store.remove_fence(FenceId(1u), 1));
    BPFAB_CHECK(store.fence_count() == 1u);
  }

  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_CHECK(store.fence_count() == 1u);
    BPFAB_CHECK(store.state().fences[0].durable);
    BPFAB_CHECK(store.state().revalidation_required.empty());
  }
}

BPFAB_TEST(store, attempts_distinguish_open_committed_and_ambiguous) {
  const std::string directory = bpfab_test::make_scratch_directory("attempts");
  DurableConfig config;
  config.directory = directory;
  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_REQUIRE_OK(store.establish_epoch(0));
    BPFAB_REQUIRE_OK(store.begin_attempt(AttemptId(1u), 0));
    BPFAB_REQUIRE_OK(store.begin_attempt(AttemptId(2u), 0));
    BPFAB_REQUIRE_OK(store.begin_attempt(AttemptId(3u), 0));
    BPFAB_REQUIRE_CODE(store.begin_attempt(AttemptId(1u), 0), ErrorCode::Duplicate);
    BPFAB_REQUIRE_OK(store.commit_attempt(AttemptId(1u), 0));
    BPFAB_REQUIRE_OK(store.abort_attempt(AttemptId(2u), 0));
    BPFAB_REQUIRE_CODE(store.commit_attempt(AttemptId(9u), 0), ErrorCode::NotFound);
    BPFAB_REQUIRE_CODE(store.abort_attempt(AttemptId(9u), 0), ErrorCode::NotFound);
    BPFAB_CHECK(store.state().attempts_committed == 1u);
    BPFAB_CHECK(store.state().attempts_aborted == 1u);
    BPFAB_CHECK(store.state().attempts_open == 1u);
  }
  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    // Attempt 3 was begun and never resolved: it is ambiguous, not committed.
    BPFAB_CHECK(store.state().attempts_ambiguous == 1u);
    BPFAB_CHECK(store.state().attempts_open == 1u);
    BPFAB_CHECK(store.state().attempts_committed == 1u);
    BPFAB_CHECK(store.state().attempts_aborted == 1u);
    BPFAB_REQUIRE_OK(store.abort_attempt(AttemptId(3u), 1));
    BPFAB_CHECK(store.state().attempts_open == 0u);
  }
}

BPFAB_TEST(store, compaction_preserves_recovered_state) {
  const std::string directory = bpfab_test::make_scratch_directory("compact");
  DurableConfig config;
  config.directory = directory;
  config.max_lineages = 8;
  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_REQUIRE_OK(store.establish_epoch(0));
    BPFAB_REQUIRE_OK(store.record_boot(bpfab_test::test_incarnation(2u, 2u), 0));
    PropagationPolicy policy = bpfab_test::test_policy(8u, 1u);
    BPFAB_REQUIRE_OK(store.persist_policy(policy, 0));
    for (std::uint32_t i = 0; i < 40u; ++i) {
      Digest128 lineage = digest_of(std::to_string(i));
      BPFAB_REQUIRE_OK(store.record_lineage(lineage, i));
    }
    Fence fence;
    fence.id = FenceId(1u);
    fence.resource = ResourceId(4u);
    fence.kind = FenceKind::RefuseEgress;
    fence.epoch = store.state().epoch;
    fence.durable = true;
    BPFAB_REQUIRE_OK(store.put_fence(fence, 0));
    const std::size_t before = store.journal().records().size();
    BPFAB_REQUIRE_OK(store.compact());
    BPFAB_CHECK(store.journal().records().size() < before);
    BPFAB_CHECK(store.state().lineages.size() == 8u);
    BPFAB_CHECK(store.state().lineage_evictions == 32u);
  }
  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_CHECK(store.state().lineages.size() == 8u);
    BPFAB_CHECK(store.fence_count() == 1u);
    BPFAB_CHECK(store.state().has_policy);
    BPFAB_CHECK(store.state().boot_recorded);
    BPFAB_CHECK(store.state().previous_epoch.value() == 1u);
    BPFAB_CHECK(store.state().epoch.value() == 1u);
  }
}

BPFAB_TEST(store, policy_serialization_round_trips_and_rejects_damage) {
  PropagationPolicy policy = bpfab_test::test_policy(11u, 12u);
  policy.allow_amplification = true;
  policy.max_cumulative_gain = Gain::from_percent_milli(175000u).value();
  policy.aggregation = AggregationRule::SaturatingSum;
  policy.aggregation_ceiling = bpfab_test::mag(90000u);
  policy.hysteresis_delta = bpfab_test::mag(2500u);
  policy.default_cooldown_ticks = 77;
  BPFAB_REQUIRE_OK(policy.validate());

  std::vector<std::byte> bytes;
  BPFAB_REQUIRE_OK(serialize_policy(policy, bytes, 512));
  BPFAB_REQUIRE_RESULT(restored, deserialize_policy(bytes));
  BPFAB_CHECK(restored.digest() == policy.digest());

  std::vector<std::byte> truncated(bytes.begin(), bytes.end() - 3);
  BPFAB_CHECK(deserialize_policy(truncated).status().code() == ErrorCode::TruncatedInput);

  std::vector<std::byte> extended = bytes;
  extended.push_back(std::byte{0});
  BPFAB_CHECK(deserialize_policy(extended).status().code() == ErrorCode::MalformedInput);

  std::vector<std::byte> wrong_version = bytes;
  wrong_version[1] = std::byte{9};
  BPFAB_CHECK(deserialize_policy(wrong_version).status().code() == ErrorCode::VersionMismatch);
}

BPFAB_TEST(store, stale_temp_files_are_removed_on_open) {
  const std::string directory = bpfab_test::make_scratch_directory("temp");
  const std::string temp = (std::filesystem::path(directory) / "fabric.bpfj.tmp").string();
  {
    std::ofstream stream(temp, std::ios::binary);
    stream << "leftover";
  }
  DurableConfig config;
  config.directory = directory;
  BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
  BPFAB_CHECK(store.state().stale_temp_files_removed == 1u);
  BPFAB_CHECK(!std::filesystem::exists(std::filesystem::path(temp)));
}