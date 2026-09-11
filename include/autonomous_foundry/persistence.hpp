#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/foundry.hpp"

// Versioned, integrity checked persistence.
//
// File layout:
//
//   offset 0   magic            "AFSN"              4 bytes
//   offset 4   format version   little endian u32   4 bytes
//   offset 8   payload length   little endian u64   8 bytes
//   offset 16  payload CRC-32C  little endian u32   4 bytes
//   offset 20  reserved         0                   4 bytes
//   offset 24  payload          payload length bytes
//
// The payload is a record stream. Loading validates the file size against the
// declared payload length (so trailing garbage is rejected), the CRC-32C, the
// format version, and then every record semantically: enum ranges, generation
// non-regression, reference integrity, lineage acyclicity, duplicate identity
// rejection and bounded counts. Nothing is allocated directly from a size
// field before it has been checked against a limit.

namespace autonomous_foundry {

inline constexpr std::string_view kSnapshotMagic = "AFSN";

/// Total bytes of the fixed snapshot file header.
inline constexpr std::size_t kSnapshotHeaderBytes = 24;

/// Serialize a snapshot into the file image.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> serialize_snapshot(
    const FoundrySnapshot& snapshot);

/// Parse and semantically validate a file image.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<FoundrySnapshot> deserialize_snapshot(
    std::string_view image);

/// Parse the header only, without allocating the payload.
struct SnapshotHeader {
  std::uint32_t format_version{0};
  std::uint64_t payload_length{0};
  std::uint32_t payload_crc32c{0};
};
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<SnapshotHeader> parse_snapshot_header(
    std::string_view image);

/// Transactional snapshot store.
class AUTONOMOUS_FOUNDRY_API SnapshotStore {
 public:
  explicit SnapshotStore(std::filesystem::path path);

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  /// True when a snapshot file exists.
  [[nodiscard]] bool exists() const;

  [[nodiscard]] Status save(const FoundrySnapshot& snapshot);

  /// Load and validate. Returns PersistenceCorrupt, PersistenceTruncated,
  /// PersistenceVersionUnsupported, PersistenceIntegrityMismatch,
  /// PersistenceTrailingGarbage or PersistenceRejectedContent as appropriate.
  [[nodiscard]] Result<FoundrySnapshot> load() const;

  /// Number of successful saves performed by this instance.
  [[nodiscard]] std::uint64_t save_count() const noexcept { return save_count_; }

  /// Write a snapshot and then verify it reloads to an equal state. Used at
  /// shutdown so that a final durable image is proven readable, not assumed.
  [[nodiscard]] Status save_and_verify(const FoundrySnapshot& snapshot);

 private:
  std::filesystem::path path_;
  std::uint64_t save_count_{0};
};

/// Deep equality of two snapshots on every durable field.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool snapshots_are_equal(const FoundrySnapshot& a,
                                                              const FoundrySnapshot& b);

/// Human readable description of the first difference, for diagnostics.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string first_snapshot_difference(
    const FoundrySnapshot& a, const FoundrySnapshot& b);

}  // namespace autonomous_foundry
