#pragma once

// src/cli_impl.hpp
//
// Private declarations shared by the af_cli implementation.
//
// af_cli has two data sources and they answer different questions:
//
//   * an OFFLINE snapshot file, opened with SnapshotStore::load and never
//     written. It holds every durable record, so it can answer enumeration and
//     explanation questions ("which populations exist", "why did this candidate
//     not win", "what was ranked behind it") that the wire protocol does not
//     expose as a query.
//
//   * a RUNNING coordinator, reached over the control plane. It answers
//     point questions about live state, and it is the only source that reflects
//     work in flight.
//
// The CLI never guesses: when a question cannot be answered from the selected
// source it says so and exits with the "rejected" code instead of printing a
// plausible-looking report.

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/selection.hpp"

namespace autonomous_foundry {
namespace cli {

/// Exit codes. Scripts depend on these, so they are part of the contract.
inline constexpr int kExitOk = 0;
/// The operation was understood and refused.
inline constexpr int kExitRejected = 2;
/// The command line itself was wrong.
inline constexpr int kExitUsage = 3;

struct Options {
  /// Offline snapshot file, when the operator selected the offline source.
  std::filesystem::path state_path;
  /// "host:port" of a running coordinator, empty for offline use.
  std::string coordinator;
  /// The command word.
  std::string command;
  /// Everything after the command word.
  std::vector<std::string> operands;
};

/// Parse a command line. Returns InvalidArgument with a diagnostic when the
/// command line is wrong; the caller turns that into kExitUsage.
[[nodiscard]] Status parse_options(int argc, char** argv, Options* out);

/// Print the one-line synopsis of every command.
void print_usage(std::ostream& out);

/// Parse a decimal raw identity operand. Usage errors are reported to the
/// caller with a diagnostic.
[[nodiscard]] Status parse_identity_operand(std::string_view text, std::uint64_t* out);

// -- report rendering -------------------------------------------------------
//
// Every renderer writes a stable, scriptable, human-readable report. Fields are
// always printed in the same order and always labelled, so a human can read the
// report and a script can still parse it.

void print_population_report(std::ostream& out, const FoundryCore& core,
                             const PopulationRecord& population);
void print_candidate_report(std::ostream& out, const FoundryCore& core,
                            const CandidateRecord& candidate);
void print_evaluation_report(std::ostream& out, const FoundryCore& core, CandidateId candidate);
void print_retention_report(std::ostream& out, const FoundryCore& core, PopulationId population);
void print_selection_report(std::ostream& out, const FoundryCore& core, PopulationId population);
void print_lineage_report(std::ostream& out, const FoundryCore& core, CandidateId root);

// -- sources ----------------------------------------------------------------

/// Open the offline snapshot and serve the offline commands.
[[nodiscard]] int run_offline(const Options& options);

/// Reach a running coordinator and serve the live commands.
[[nodiscard]] int run_live(const Options& options);

/// Run one complete in-process population and print what was decided.
[[nodiscard]] int run_reference_demo(const Options& options);

/// True when the command needs a running coordinator.
[[nodiscard]] bool command_is_live(std::string_view command);

/// True when the command is understood at all.
[[nodiscard]] bool command_is_known(std::string_view command);

}  // namespace cli
}  // namespace autonomous_foundry
