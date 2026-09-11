#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/id.hpp"

// Opaque artifact references.
//
// Autonomous Foundry decides whether a candidate may advance. It does not own
// the bytes of the artifact, the trust decision about those bytes, or the
// promotion of the artifact. It owns a verifiable reference: identity, size,
// content digest and the logical role the artifact played in the attempt.

namespace autonomous_foundry {

/// Maximum number of artifact references accepted on a single candidate.
inline constexpr std::size_t kMaxArtifactsPerCandidate = 64;

/// Maximum length accepted for an artifact logical name.
inline constexpr std::size_t kMaxArtifactNameLength = 256;

/// Maximum accepted size of a single published artifact, in bytes.
inline constexpr std::uint64_t kMaxArtifactBytes = 16ull * 1024ull * 1024ull;

struct ArtifactRef {
  /// Logical role inside the candidate, for example "solution" or "report".
  /// Restricted to a conservative character set so that it can never be
  /// interpreted as a path by a downstream consumer.
  std::string name;

  /// Size in bytes of the artifact content as published.
  std::uint64_t size_bytes{0};

  /// Lowercase hexadecimal SHA-256 of the artifact content.
  std::string content_digest;

  friend bool operator==(const ArtifactRef&, const ArtifactRef&) noexcept = default;
  friend bool operator<(const ArtifactRef& a, const ArtifactRef& b) noexcept {
    if (a.name != b.name) {
      return a.name < b.name;
    }
    if (a.size_bytes != b.size_bytes) {
      return a.size_bytes < b.size_bytes;
    }
    return a.content_digest < b.content_digest;
  }
};

/// Validate a logical artifact name. Rejects empty names, over-long names,
/// path separators, traversal sequences, drive letters, control characters and
/// non-ASCII bytes, because the name travels into logs, JSON and eventually
/// downstream promotion systems.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status validate_artifact_name(std::string_view name);

/// Validate an artifact reference as received from a worker or a snapshot.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status validate_artifact_ref(const ArtifactRef& ref);

/// Canonical byte encoding of a set of artifact references, used to derive the
/// candidate state digest. The encoding is order independent: references are
/// sorted by name first.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string encode_artifact_set(
    const std::vector<ArtifactRef>& artifacts);

/// SHA-256 over the canonical encoding of the artifact set.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string artifact_set_digest(
    const std::vector<ArtifactRef>& artifacts);

}  // namespace autonomous_foundry
