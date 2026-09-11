// Persistence suite.
//
// Proves the snapshot codec end to end: a populated snapshot survives
// serialize -> deserialize -> serialize byte for byte, every documented header
// rejection carries its own code, and the semantic payload checks refuse a
// duplicate identity and an identity counter lower than a counter the loaded
// state already observes. Every corrupt image is produced by mutating the bytes
// of a real serialized snapshot, never by inventing one.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/coordinator.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/persistence.hpp"
#include "autonomous_foundry/transport.hpp"
#include "autonomous_foundry/version.hpp"
#include "autonomous_foundry/workspace.hpp"
#include "test_fixture.hpp"

namespace {

using namespace autonomous_foundry;

// ---------------------------------------------------------------------------
// Image surgery
//
// A corrupt image is always a real image with known bytes changed, so a
// rejection can only have come from the check under test.
// ---------------------------------------------------------------------------

unsigned byte_shift(std::size_t index) {
  return static_cast<unsigned>(8u * static_cast<unsigned>(index));
}

void write_u32_le(std::string& image, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    const auto byte = static_cast<unsigned char>((value >> byte_shift(index)) & 0xFFu);
    image[offset + index] = static_cast<char>(byte);
  }
}

void write_u64_le(std::string& image, std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    const auto byte = static_cast<unsigned char>((value >> byte_shift(index)) & 0xFFu);
    image[offset + index] = static_cast<char>(byte);
  }
}

[[nodiscard]] std::uint32_t read_u32_le(std::string_view bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    const auto byte =
        static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index]));
    value |= byte << byte_shift(index);
  }
  return value;
}

/// The payload bytes of a serialized image.
[[nodiscard]] std::string_view payload_view(const std::string& image) {
  return std::string_view(image).substr(kSnapshotHeaderBytes);
}

/// Rewrite the declared payload length and payload CRC-32C so that a mutated
/// payload is internally consistent and only the semantic check under test can
/// reject it.
void refresh_payload_integrity(std::string& image) {
  const std::size_t payload_length = image.size() - kSnapshotHeaderBytes;
  write_u64_le(image, 8, static_cast<std::uint64_t>(payload_length));
  write_u32_le(image, 16, crc32c(payload_view(image)));
}

/// One framed record inside a serialized payload: kind, body length, body.
struct RecordSpan {
  std::uint8_t kind{0};
  std::size_t begin{0};
  std::size_t end{0};
};

/// Walk the framed record stream of a serialized payload.
[[nodiscard]] std::vector<RecordSpan> payload_records(std::string_view payload) {
  std::vector<RecordSpan> records;
  const std::uint32_t count = read_u32_le(payload, 0);
  std::size_t offset = 4;
  for (std::uint32_t index = 0; index < count; ++index) {
    RecordSpan span;
    span.kind = static_cast<std::uint8_t>(static_cast<unsigned char>(payload[offset]));
    const std::uint32_t body_length = read_u32_le(payload, offset + 1u);
    span.begin = offset;
    span.end = offset + 5u + static_cast<std::size_t>(body_length);
    records.push_back(span);
    offset = span.end;
  }
  return records;
}

/// A foundry holding two tasks, two policies, one running population and its
/// budget ledger, serialized into a real file image.
///
/// No worker is registered, and that is a property of the snapshot validator
/// rather than of this suite: the identity counter cross-check compares every
/// identity in the state against the coordinator's own id_counters, while
/// WorkerId and WorkerBootId are minted by the peer and never allocated by the
/// coordinator, so id_counters[Worker] stays zero and any image holding a
/// registered worker is refused. That is reported as a library defect instead of
/// being asserted here, and the round trip cases below cover the durable state
/// the codec can read.
[[nodiscard]] std::string populated_image(const af_test::TestContext& context,
                                          FoundrySnapshot& snapshot_out) {
  af_test::FoundryFixture fixture(af_test::make_config(0x51u));
  af_test::build_running_fixture(context, fixture, 8u);

  FoundryPolicy second_policy = af_test::make_policy(PolicyId());
  REQUIRE_VALUE(context, PolicyId, second_policy_id, fixture.core.define_policy(second_policy));
  EXPECT_TRUE(context, second_policy_id.valid());

  TaskSpec second_task = af_test::make_task(TaskId(), second_policy_id, 4u);
  REQUIRE_VALUE(context, TaskId, second_task_id, fixture.core.define_task(second_task));
  EXPECT_TRUE(context, second_task_id.valid());

  snapshot_out = fixture.core.snapshot();
  return af_test::require_value(context, serialize_snapshot(snapshot_out), "serialize_snapshot",
                                __FILE__, __LINE__);
}

}  // namespace

AF_TEST_CASE(persistence, round_trip_preserves_a_populated_snapshot) {
  af_ctx.phase("SETUP");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);
  EXPECT_TRUE(af_ctx, image.size() > kSnapshotHeaderBytes);

  // The state is genuinely populated: an empty snapshot would round trip just
  // as well and prove nothing.
  EXPECT_EQ(af_ctx, snapshot.tasks.size(), std::size_t{2});
  EXPECT_EQ(af_ctx, snapshot.policies.size(), std::size_t{2});
  EXPECT_EQ(af_ctx, snapshot.populations.size(), std::size_t{1});
  EXPECT_EQ(af_ctx, snapshot.ledgers.size(), std::size_t{1});
  EXPECT_TRUE(af_ctx, snapshot.workers.empty());
  EXPECT_TRUE(af_ctx, snapshot.candidates.empty());
  af_ctx.note(
      "LIMITATION: an image holding a registered worker currently cannot be deserialized - the "
      "identity counter cross-check in validate_semantics compares the peer minted WorkerId and "
      "WorkerBootId counters against the coordinator's own id_counters, which never counts them, "
      "so the reload is refused with PersistenceRejectedContent. Reported as a library defect; this "
      "case round trips the durable control plane state the codec can read, and "
      "populated_round_trip_with_workers_and_unpublished_candidates covers the worker image.");

  af_ctx.phase("VERIFY_HEADER");
  const std::string_view payload = payload_view(image);
  const Result<SnapshotHeader> header = parse_snapshot_header(image);
  EXPECT_OK(af_ctx, header);
  EXPECT_EQ(af_ctx, header.value().format_version, kSnapshotFormatVersion);
  EXPECT_EQ(af_ctx, header.value().payload_length, static_cast<std::uint64_t>(payload.size()));
  EXPECT_EQ(af_ctx, header.value().payload_crc32c, crc32c(payload));
  EXPECT_EQ(af_ctx, read_u32_le(image, 20), std::uint32_t{0});

  af_ctx.phase("ROUND_TRIP");
  const Result<FoundrySnapshot> restored = deserialize_snapshot(image);
  EXPECT_OK(af_ctx, restored);
  EXPECT_TRUE(af_ctx, snapshots_are_equal(snapshot, restored.value()));
  const std::string difference = first_snapshot_difference(snapshot, restored.value());
  EXPECT_EQ(af_ctx, difference, std::string("no difference found"));

  // The comparator is not vacuously true: a single mutated field is reported.
  FoundrySnapshot altered = restored.value();
  altered.revision += 1u;
  EXPECT_FALSE(af_ctx, snapshots_are_equal(snapshot, altered));
  EXPECT_NE(af_ctx, first_snapshot_difference(snapshot, altered),
            std::string("no difference found"));

  // A snapshot restored from disk re-encodes to exactly the bytes that were
  // read, so the image is a function of the state alone.
  const Result<std::string> reencoded = serialize_snapshot(restored.value());
  EXPECT_OK(af_ctx, reencoded);
  EXPECT_EQ(af_ctx, reencoded.value(), image);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(persistence, populated_round_trip_with_workers_and_unpublished_candidates) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(0x71u));
  af_test::build_running_fixture(af_ctx, fixture, 8u);

  const WorkerSessionAuthority publisher = af_test::connect_worker(af_ctx, fixture.core, 0u);
  const WorkerSessionAuthority holder = af_test::connect_worker(af_ctx, fixture.core, 1u);

  // Both workers take a candidate slot before either attempt completes, so the
  // population still holds an unpublished candidate once the first one is
  // published.
  const PendingDispatch published_pending = af_test::require_value(
      af_ctx, fixture.core.authorize_attempt(publisher), "authorize_attempt", __FILE__, __LINE__);
  const PendingDispatch held_pending = af_test::require_value(
      af_ctx, fixture.core.authorize_attempt(holder), "authorize_attempt", __FILE__, __LINE__);
  EXPECT_NE(af_ctx, published_pending.attempt.candidate, held_pending.attempt.candidate);

  // Carry the first attempt through publication: its candidate gains a producer,
  // its worker goes idle (null active attempt and null active assignment) and
  // the second worker stays busy on a candidate slot that was never published.
  const AttemptPackage package = af_test::require_value(
      af_ctx,
      fixture.core.confirm_dispatch(publisher, published_pending.attempt.id,
                                    published_pending.attempt.generation),
      "confirm_dispatch", __FILE__, __LINE__);
  WorkerOperationAuthority authority;
  authority.session = publisher;
  authority.population = package.population;
  authority.population_generation = package.population_generation;
  authority.task = package.task;
  authority.task_generation = package.task_generation;
  authority.candidate = package.candidate;
  authority.candidate_generation = package.candidate_generation;
  authority.attempt = package.attempt;
  authority.attempt_generation = package.attempt_generation;
  authority.assignment = package.assignment;
  EXPECT_OK(af_ctx, fixture.core.acknowledge_attempt(authority));

  const std::string source = af_test::reference_solution_source(0);
  std::vector<ArtifactRef> artifacts;
  ArtifactRef solution;
  solution.name = std::string(kReferenceTaskSourceArtifact);
  solution.size_bytes = static_cast<std::uint64_t>(source.size());
  solution.content_digest = sha256_hex(source);
  artifacts.push_back(solution);
  REQUIRE_VALUE(af_ctx, PublicationOutcome, outcome,
                fixture.core.publish_candidate(authority, artifacts, "reference"));
  EXPECT_TRUE(af_ctx, outcome.candidate.valid());

  const FoundrySnapshot snapshot = fixture.core.snapshot();
  const WorkerRecord idle =
      af_test::require_value(af_ctx, fixture.core.worker(publisher.worker), "worker", __FILE__,
                             __LINE__);
  EXPECT_FALSE(af_ctx, idle.active_attempt.valid());
  EXPECT_FALSE(af_ctx, idle.active_assignment.valid());
  const WorkerRecord busy =
      af_test::require_value(af_ctx, fixture.core.worker(holder.worker), "worker", __FILE__,
                             __LINE__);
  EXPECT_TRUE(af_ctx, busy.active_attempt.valid());
  const CandidateRecord unpublished =
      af_test::load_candidate(af_ctx, fixture.core, held_pending.attempt.candidate);
  EXPECT_FALSE(af_ctx, unpublished.producer_worker.valid());
  EXPECT_FALSE(af_ctx, unpublished.producer_boot.valid());
  const CandidateRecord published_record =
      af_test::load_candidate(af_ctx, fixture.core, outcome.candidate);
  EXPECT_TRUE(af_ctx, published_record.producer_worker.valid());
  EXPECT_EQ(af_ctx, snapshot.workers.size(), std::size_t{2});
  EXPECT_EQ(af_ctx, snapshot.candidates.size(), std::size_t{2});

  af_ctx.phase("COMMIT");
  REQUIRE_VALUE(af_ctx, std::string, image, serialize_snapshot(snapshot));

  af_ctx.phase("VERIFY");
  // An image that holds registered workers round-trips: worker and worker-boot
  // identities are minted by the peer, so the peer domains are not cross-checked
  // against the coordinator allocator, and restore instead seeds those domains
  // with the highest counter actually present so a later allocation cannot
  // collide. The round trip below is the real proof.
  const Result<FoundrySnapshot> loaded = deserialize_snapshot(image);
  REQUIRE_VALUE(af_ctx, FoundrySnapshot, restored, loaded);
  EXPECT_TRUE(af_ctx, snapshots_are_equal(snapshot, restored));
  EXPECT_EQ(af_ctx, first_snapshot_difference(snapshot, restored),
            std::string("no difference found"));

  // The image is a function of the state alone, so it re-encodes byte for byte.
  const Result<std::string> reencoded = serialize_snapshot(restored);
  EXPECT_OK(af_ctx, reencoded);
  EXPECT_EQ(af_ctx, reencoded.value(), image);

  // The restored state really does hold both nullable shapes this case exists
  // for, so the equality above is not proved on a state that lost them.
  EXPECT_TRUE(af_ctx, restored.workers.at(publisher.worker).active_attempt.is_null());
  EXPECT_TRUE(af_ctx, restored.workers.at(holder.worker).active_attempt.valid());
  EXPECT_TRUE(af_ctx,
              restored.candidates.at(held_pending.attempt.candidate).producer_worker.is_null());
  EXPECT_TRUE(af_ctx, restored.candidates.at(outcome.candidate).producer_worker.valid());

  af_ctx.phase("SHUTDOWN");
}
AF_TEST_CASE(persistence, corrupted_payload_byte_is_an_integrity_mismatch) {
  af_ctx.phase("SETUP");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);

  af_ctx.phase("COMMIT");
  std::string corrupted = image;
  const std::size_t probe = kSnapshotHeaderBytes;
  corrupted[probe] = static_cast<char>(static_cast<unsigned char>(corrupted[probe]) ^ 0xFFu);
  // The header CRC is deliberately left untouched: only the payload moved.
  EXPECT_EQ(af_ctx, read_u32_le(corrupted, 16), read_u32_le(image, 16));
  EXPECT_NE(af_ctx, corrupted, image);

  af_ctx.phase("VERIFY");
  const Result<FoundrySnapshot> rejected = deserialize_snapshot(corrupted);
  EXPECT_STATUS_CODE(af_ctx, rejected, ErrorCode::PersistenceIntegrityMismatch);
  EXPECT_TRUE(af_ctx, is_persistence_rejection(rejected.status().code()));

  // The untouched image still loads, so the rejection is caused by the flip.
  const Result<FoundrySnapshot> untouched = deserialize_snapshot(image);
  EXPECT_OK(af_ctx, untouched);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(persistence, truncation_is_rejected) {
  af_ctx.phase("SETUP");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);

  af_ctx.phase("VERIFY_ONE_BYTE_SHORT");
  std::string one_short = image;
  one_short.resize(one_short.size() - 1u);
  const Result<FoundrySnapshot> missing_tail = deserialize_snapshot(one_short);
  EXPECT_STATUS_CODE(af_ctx, missing_tail, ErrorCode::PersistenceTruncated);

  af_ctx.phase("VERIFY_HALF_PAYLOAD");
  std::string half = image;
  half.resize(kSnapshotHeaderBytes + (payload_view(image).size() / 2u));
  const Result<FoundrySnapshot> missing_half = deserialize_snapshot(half);
  EXPECT_STATUS_CODE(af_ctx, missing_half, ErrorCode::PersistenceTruncated);

  af_ctx.phase("VERIFY_HEADER_ONLY");
  const std::string header_only = image.substr(0, kSnapshotHeaderBytes);
  const Result<FoundrySnapshot> no_payload = deserialize_snapshot(header_only);
  EXPECT_STATUS_CODE(af_ctx, no_payload, ErrorCode::PersistenceTruncated);

  af_ctx.phase("VERIFY_BELOW_HEADER");
  const std::string below_header = image.substr(0, 8);
  const Result<FoundrySnapshot> no_header = deserialize_snapshot(below_header);
  EXPECT_STATUS_CODE(af_ctx, no_header, ErrorCode::PersistenceTruncated);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(persistence, trailing_byte_is_rejected) {
  af_ctx.phase("SETUP");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);

  af_ctx.phase("COMMIT");
  std::string trailing = image;
  trailing.push_back('\x2A');

  af_ctx.phase("VERIFY");
  // The declared payload length is unchanged, so the extra byte is refused
  // rather than ignored.
  EXPECT_EQ(af_ctx, read_u32_le(trailing, 16), read_u32_le(image, 16));
  const Result<FoundrySnapshot> rejected = deserialize_snapshot(trailing);
  EXPECT_STATUS_CODE(af_ctx, rejected, ErrorCode::PersistenceTrailingGarbage);

  std::string two_extra = image;
  two_extra.append("zz");
  const Result<FoundrySnapshot> rejected_two = deserialize_snapshot(two_extra);
  EXPECT_STATUS_CODE(af_ctx, rejected_two, ErrorCode::PersistenceTrailingGarbage);

  const Result<FoundrySnapshot> untouched = deserialize_snapshot(image);
  EXPECT_OK(af_ctx, untouched);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(persistence, unsupported_format_version_is_rejected) {
  af_ctx.phase("SETUP");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);

  af_ctx.phase("VERIFY_NEWER_VERSION");
  std::string newer = image;
  write_u32_le(newer, 4, kSnapshotFormatVersion + 1u);
  const Result<FoundrySnapshot> rejected_newer = deserialize_snapshot(newer);
  EXPECT_STATUS_CODE(af_ctx, rejected_newer, ErrorCode::PersistenceVersionUnsupported);
  const Result<SnapshotHeader> header_only = parse_snapshot_header(newer);
  EXPECT_STATUS_CODE(af_ctx, header_only, ErrorCode::PersistenceVersionUnsupported);

  af_ctx.phase("VERIFY_OLDER_VERSION");
  std::string older = image;
  write_u32_le(older, 4, 0u);
  EXPECT_NE(af_ctx, read_u32_le(older, 4), kSnapshotFormatVersion);
  const Result<FoundrySnapshot> rejected_older = deserialize_snapshot(older);
  EXPECT_STATUS_CODE(af_ctx, rejected_older, ErrorCode::PersistenceVersionUnsupported);

  // The version check is not the CRC check: a version bump alone is refused
  // before the payload is ever parsed.
  EXPECT_EQ(af_ctx, read_u32_le(newer, 16), read_u32_le(image, 16));

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(persistence, bad_magic_is_rejected) {
  af_ctx.phase("SETUP");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);
  EXPECT_EQ(af_ctx, std::string_view(image).substr(0, kSnapshotMagic.size()), kSnapshotMagic);

  af_ctx.phase("COMMIT");
  std::string corrupted = image;
  corrupted[0] = 'Z';
  corrupted[1] = 'Z';
  corrupted[2] = 'Z';
  corrupted[3] = 'Z';

  af_ctx.phase("VERIFY");
  const Result<FoundrySnapshot> rejected = deserialize_snapshot(corrupted);
  EXPECT_STATUS_CODE(af_ctx, rejected, ErrorCode::PersistenceCorrupt);
  // The magic is checked before the version, so an image that is not this
  // format at all never reports a version problem.
  EXPECT_NE(af_ctx, read_u32_le(corrupted, 4), std::uint32_t{0xFFFFFFFFu});

  const Result<SnapshotHeader> header_only = parse_snapshot_header(corrupted);
  EXPECT_STATUS_CODE(af_ctx, header_only, ErrorCode::PersistenceCorrupt);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(persistence, non_zero_reserved_header_field_is_rejected) {
  af_ctx.phase("SETUP");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);
  EXPECT_EQ(af_ctx, read_u32_le(image, 20), std::uint32_t{0});

  af_ctx.phase("COMMIT");
  std::string corrupted = image;
  write_u32_le(corrupted, 20, 1u);

  af_ctx.phase("VERIFY");
  // Every other header field, including the CRC, is untouched.
  EXPECT_EQ(af_ctx, read_u32_le(corrupted, 16), read_u32_le(image, 16));
  EXPECT_EQ(af_ctx, read_u32_le(corrupted, 4), read_u32_le(image, 4));
  const Result<FoundrySnapshot> rejected = deserialize_snapshot(corrupted);
  EXPECT_STATUS_CODE(af_ctx, rejected, ErrorCode::PersistenceCorrupt);
  const Result<SnapshotHeader> header_only = parse_snapshot_header(corrupted);
  EXPECT_STATUS_CODE(af_ctx, header_only, ErrorCode::PersistenceCorrupt);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(persistence, duplicate_identity_inside_the_payload_is_rejected) {
  af_ctx.phase("SETUP");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);
  const std::string_view payload = payload_view(image);
  const std::vector<RecordSpan> records = payload_records(payload);
  EXPECT_TRUE(af_ctx, records.size() >= 3u);
  // The first record is the identity header, the second the statistics block;
  // the first map backed record is the one that can be duplicated.
  EXPECT_EQ(af_ctx, records.front().kind, std::uint8_t{1});
  EXPECT_EQ(af_ctx, records[1].kind, std::uint8_t{2});

  std::size_t duplicate_begin = 0;
  std::size_t duplicate_end = 0;
  bool found = false;
  for (const RecordSpan& span : records) {
    if (span.kind >= 3) {
      duplicate_begin = span.begin;
      duplicate_end = span.end;
      found = true;
      break;
    }
  }
  EXPECT_TRUE(af_ctx, found);

  af_ctx.phase("COMMIT");
  std::string duplicated = image;
  duplicated.append(payload.substr(duplicate_begin, duplicate_end - duplicate_begin));
  write_u32_le(duplicated, kSnapshotHeaderBytes, read_u32_le(payload, 0) + 1u);
  refresh_payload_integrity(duplicated);

  // The image is structurally perfect: same magic, same version, zero reserved
  // field, correct length and a CRC that matches the new payload.
  EXPECT_EQ(af_ctx, read_u32_le(duplicated, 16), crc32c(payload_view(duplicated)));
  EXPECT_EQ(af_ctx, std::string_view(duplicated).substr(0, kSnapshotMagic.size()), kSnapshotMagic);
  EXPECT_EQ(af_ctx, payload_records(payload_view(duplicated)).size(), records.size() + std::size_t{1});

  af_ctx.phase("VERIFY");
  const Result<FoundrySnapshot> rejected = deserialize_snapshot(duplicated);
  EXPECT_STATUS_CODE(af_ctx, rejected, ErrorCode::PersistenceRejectedContent);
  EXPECT_TRUE(af_ctx, is_persistence_rejection(rejected.status().code()));

  const Result<FoundrySnapshot> untouched = deserialize_snapshot(image);
  EXPECT_OK(af_ctx, untouched);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(persistence, identity_counter_below_an_observed_counter_is_rejected) {
  af_ctx.phase("SETUP");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);
  EXPECT_FALSE(af_ctx, snapshot.tasks.empty());

  // The fixture is worker free by construction: a snapshot that holds a
  // registered worker is refused by the very identity counter cross-check this
  // case exercises, because WorkerId and WorkerBootId are minted by the peer and
  // never counted by the coordinator's allocator. The worker image lives in
  // populated_round_trip_with_workers_and_unpublished_candidates; this case
  // proves the counter regression on an image the codec accepts.
  EXPECT_TRUE(af_ctx, snapshot.workers.empty());
  EXPECT_TRUE(af_ctx, snapshot.candidates.empty());

  // The identity header record is the first record and its body is
  //   FoundryId u64, FoundryRunId u64, epoch u64, revision u64, id_salt u32,
  //   then kIdKindCount u32 counters in IdKind order.
  const std::vector<RecordSpan> records = payload_records(payload_view(image));
  EXPECT_TRUE(af_ctx, records.size() >= 3u);
  EXPECT_EQ(af_ctx, records.front().kind, std::uint8_t{1});
  const std::size_t body = records.front().begin + 5u;
  const std::size_t counters = body + 8u + 8u + 8u + 8u + 4u;
  const std::size_t task_index = static_cast<std::size_t>(IdKind::Task);
  const std::size_t task_counter = counters + 4u * task_index;
  const std::uint32_t declared =
      read_u32_le(image, kSnapshotHeaderBytes + task_counter);
  EXPECT_TRUE(af_ctx, declared > 0u);
  EXPECT_EQ(af_ctx, declared, snapshot.id_counters[task_index]);

  af_ctx.phase("COMMIT");
  std::string regressed = image;
  write_u32_le(regressed, kSnapshotHeaderBytes + task_counter, 0u);
  refresh_payload_integrity(regressed);
  EXPECT_EQ(af_ctx, read_u32_le(regressed, 16), crc32c(payload_view(regressed)));

  af_ctx.phase("VERIFY");
  // Restoring this snapshot would re-issue a task identity that already exists
  // in the loaded state, so the payload is refused as content, not as bytes.
  const Result<FoundrySnapshot> rejected = deserialize_snapshot(regressed);
  EXPECT_STATUS_CODE(af_ctx, rejected, ErrorCode::PersistenceRejectedContent);
  EXPECT_TRUE(af_ctx, rejected.status().message().find("Task") != std::string::npos);

  const Result<FoundrySnapshot> untouched = deserialize_snapshot(image);
  EXPECT_OK(af_ctx, untouched);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(persistence, snapshot_store_round_trip_and_garbage_refusal) {
  af_ctx.phase("SETUP");
  af_test::TempDirectory directory("persistence-store");
  FoundrySnapshot snapshot;
  const std::string image = populated_image(af_ctx, snapshot);

  SnapshotStore store(directory.child("snapshot.afsn"));
  EXPECT_FALSE(af_ctx, store.exists());
  EXPECT_EQ(af_ctx, store.save_count(), std::uint64_t{0});

  af_ctx.phase("COMMIT");
  const Status saved = store.save(snapshot);
  EXPECT_OK(af_ctx, saved);
  EXPECT_TRUE(af_ctx, store.exists());
  EXPECT_EQ(af_ctx, store.save_count(), std::uint64_t{1});

  af_ctx.phase("VERIFY_RELOAD");
  const Result<FoundrySnapshot> reloaded = store.load();
  EXPECT_OK(af_ctx, reloaded);
  EXPECT_TRUE(af_ctx, snapshots_are_equal(snapshot, reloaded.value()));
  EXPECT_EQ(af_ctx, first_snapshot_difference(snapshot, reloaded.value()),
            std::string("no difference found"));

  // The bytes on disk are exactly the bytes the codec produces.
  const Result<std::string> on_disk = read_file_bounded(store.path(), 1u << 20);
  EXPECT_OK(af_ctx, on_disk);
  EXPECT_EQ(af_ctx, on_disk.value(), image);

  af_ctx.phase("VERIFY_SAVE_AND_VERIFY");
  const Status verified = store.save_and_verify(snapshot);
  EXPECT_OK(af_ctx, verified);
  EXPECT_EQ(af_ctx, store.save_count(), std::uint64_t{2});
  EXPECT_TRUE(af_ctx, store.exists());

  af_ctx.phase("VERIFY_GARBAGE_REFUSAL");
  SnapshotStore garbage_store(directory.child("garbage.afsn"));
  EXPECT_FALSE(af_ctx, garbage_store.exists());
  const std::string garbage(static_cast<std::size_t>(64), 'X');
  const Status written = atomic_write_file(garbage_store.path(), garbage);
  EXPECT_OK(af_ctx, written);
  EXPECT_TRUE(af_ctx, garbage_store.exists());

  const Result<FoundrySnapshot> refused = garbage_store.load();
  EXPECT_NOT_OK(af_ctx, refused);
  EXPECT_TRUE(af_ctx, is_persistence_rejection(refused.status().code()));
  EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceCorrupt);
  // A failed load is not a save.
  EXPECT_EQ(af_ctx, garbage_store.save_count(), std::uint64_t{0});

  af_ctx.phase("SHUTDOWN");
  directory.remove_now();
  EXPECT_FALSE(af_ctx, std::filesystem::exists(directory.path()));
}

// Repeated replacement of one durable file inside one coordinator lifetime.
//
// The second save of a file that already exists is the path where the runtime
// must replace, not create, the target. It is the path that historically died
// with a stack-cookie failure, so the case performs many replacements in the
// same process and the same coordinator, and verifies after every single one
// that the bytes on disk are exactly the bytes the codec produced and that no
// staging sibling survived.
AF_TEST_CASE(persistence, repeated_replacement_inside_one_coordinator_lifetime) {
  af_ctx.phase("SETUP");
  const Status sockets = SocketSubsystem::ensure_initialized();
  EXPECT_OK(af_ctx, sockets);
  af_test::TempDirectory directory("persistence-repeat");

  af_test::FoundryFixture fixture(af_test::make_config(0x5A5Au));
  af_test::build_running_fixture(af_ctx, fixture, 2u);

  const std::filesystem::path state = directory.child("coordinator.afsn");
  const std::filesystem::path workspace = directory.child("workspace");
  std::error_code created;
  std::filesystem::create_directories(workspace, created);
  EXPECT_FALSE(af_ctx, static_cast<bool>(created));

  af_ctx.phase("REPLACE_SNAPSHOT_STORE");
  // Twelve replacements of one target inside one process: the first creates the
  // file, every later one must replace it.
  SnapshotStore store(state);
  for (int round = 0; round < 12; ++round) {
    const FoundrySnapshot current = fixture.core.snapshot();
    const Status saved = store.save(current);
    if (!saved.ok()) {
      af_ctx.fail_at(__FILE__, __LINE__,
                     "replacement round " + std::to_string(round) + " failed: " +
                         std::string(error_code_name(saved.code())) + " '" + saved.message() + "'");
      break;
    }
    const Result<std::string> on_disk = read_file_bounded(store.path(), 1u << 24);
    if (!on_disk.ok()) {
      af_ctx.fail_at(__FILE__, __LINE__,
                     "replacement round " + std::to_string(round) +
                         " left an unreadable target: " + on_disk.status().message());
      break;
    }
    std::string expected;
    const Result<std::string> encoded = serialize_snapshot(current);
    if (encoded.ok()) {
      expected = encoded.value();
    }
    EXPECT_EQ(af_ctx, on_disk.value(), expected);
    // No staging sibling survives a replacement, on any branch.
    std::size_t siblings = 0;
    for (std::filesystem::directory_iterator entry(directory.path(), created), end;
         !created && entry != end; entry.increment(created)) {
      const std::string name = entry->path().filename().string();
      if (name.find(".afltmp-") != std::string::npos) {
        ++siblings;
      }
    }
    EXPECT_EQ(af_ctx, siblings, static_cast<std::size_t>(0));
  }
  EXPECT_EQ(af_ctx, store.save_count(), static_cast<std::uint64_t>(12));

  af_ctx.phase("REPLACE_IN_COORDINATOR_LIFETIME");
  CoordinatorConfig config;
  config.endpoint.host = "127.0.0.1";
  config.endpoint.port = 0;
  config.state_path = state;
  config.workspace_root = workspace;
  config.evaluation_concurrency = 1;

  {
    FoundryCoordinator coordinator(config);
    const Status started = coordinator.start();
    EXPECT_OK(af_ctx, started);
    if (started.ok()) {
      EXPECT_TRUE(af_ctx, coordinator.port() != 0);
      for (int round = 0; round < 8; ++round) {
        const Status persisted = coordinator.persist();
        if (!persisted.ok()) {
          af_ctx.fail_at(__FILE__, __LINE__,
                         "coordinator save " + std::to_string(round) + " failed: " +
                             std::string(error_code_name(persisted.code())) + " '" +
                             persisted.message() + "'");
          break;
        }
        SnapshotStore probe(config.state_path);
        EXPECT_TRUE(af_ctx, probe.exists());
        const Result<FoundrySnapshot> reloaded = probe.load();
        if (!reloaded.ok()) {
          af_ctx.fail_at(__FILE__, __LINE__,
                         "coordinator save " + std::to_string(round) +
                             " left an unloadable snapshot: " + reloaded.status().message());
          break;
        }
        EXPECT_TRUE(af_ctx, reloaded.value().revision == coordinator.core().revision());
      }
      const Status stopped = coordinator.shutdown("repeated save proof complete");
      EXPECT_OK(af_ctx, stopped);
    }
  }

  af_ctx.phase("VERIFY_NO_STAGING_RESIDUE");
  std::size_t residue = 0;
  for (std::filesystem::directory_iterator entry(directory.path(), created), end;
       !created && entry != end; entry.increment(created)) {
    if (entry->path().filename().string().find(".afltmp-") != std::string::npos) {
      ++residue;
    }
  }
  EXPECT_EQ(af_ctx, residue, static_cast<std::size_t>(0));

  af_ctx.phase("SHUTDOWN");
  directory.remove_now();
  EXPECT_FALSE(af_ctx, std::filesystem::exists(directory.path()));
}
