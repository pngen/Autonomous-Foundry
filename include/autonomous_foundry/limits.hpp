#pragma once

#include <cstddef>
#include <cstdint>

#include "autonomous_foundry/export.hpp"

// Hard resource bounds.
//
// Every bound in this header exists because the corresponding value arrives
// from an untrusted source: a worker process, a socket, a snapshot file or an
// operator command line. Nothing in Autonomous Foundry allocates directly from
// an untrusted size field; the limits below are the checked ceilings.

namespace autonomous_foundry {

// -- protocol ---------------------------------------------------------------

/// Maximum accepted size of a single protocol frame payload.
inline constexpr std::uint32_t kMaxFramePayloadBytes = 4u * 1024u * 1024u;

/// Absolute ceiling the transport will ever allocate for one frame.
inline constexpr std::uint32_t kAbsoluteMaxFramePayloadBytes = 16u * 1024u * 1024u;

/// Maximum number of frames queued for one connection before the connection is
/// considered too slow and is closed.
inline constexpr std::size_t kMaxOutboundQueueDepth = 4096;

// -- persistence ------------------------------------------------------------

/// Maximum accepted size of a snapshot payload.
inline constexpr std::uint64_t kMaxSnapshotPayloadBytes = 512ull * 1024ull * 1024ull;

/// Maximum accepted number of records of any single kind in a snapshot.
inline constexpr std::uint32_t kMaxSnapshotRecordsPerKind = 4u * 1024u * 1024u;

/// Maximum accepted length of a single string field inside a snapshot.
inline constexpr std::uint32_t kMaxSnapshotStringBytes = 4u * 1024u * 1024u;

// -- domain objects ---------------------------------------------------------

inline constexpr std::size_t kMaxNameLength = 128;
inline constexpr std::size_t kMaxObjectiveLength = 4096;
inline constexpr std::size_t kMaxDiagnosticsLength = 8192;
inline constexpr std::size_t kMaxInputFileBytes = 1024u * 1024u;
inline constexpr std::size_t kMaxInputFiles = 32;
inline constexpr std::size_t kMaxEvaluationRequirements = 32;
inline constexpr std::size_t kMaxRequiredOutputs = 16;
inline constexpr std::size_t kMaxConstraints = 32;
inline constexpr std::size_t kMaxRankingFactors = 16;
inline constexpr std::size_t kMaxLineageDepth = 64;

// -- execution --------------------------------------------------------------

/// Maximum bytes of stdout or stderr captured from a single evaluated process.
inline constexpr std::size_t kMaxCapturedOutputBytes = 256u * 1024u;

/// Maximum number of candidates any single population may register.
inline constexpr std::uint32_t kMaxCandidatesPerPopulation = 100000u;

/// Maximum number of concurrent worker connections.
inline constexpr std::size_t kMaxConnections = 256;

}  // namespace autonomous_foundry
