#pragma once

// Autonomous Foundry benchmark harness.
//
// A benchmark body performs one complete run of real work and reports what it
// finished, how many completed units that was, and how long its documented
// timed section took. The harness repeats the body, takes the best and the
// median of the completed runs, checks the result-integrity verdict of every
// run, and prints stable machine-readable lines:
//
//   BENCH <name> unit=<unit> runs=<n> timed=<what is inside the measurement>
//   BENCH <name> candidates=<n> evaluations=<n> factors=<n> best_ms=<x> median_ms=<x> ops_per_second=<x>
//   BENCH <name> integrity=ok
//
// An integrity failure prints "BENCH <name> integrity=FAILED detail=<...>" and
// makes the harness fail loudly: the process exit code becomes non-zero.
//
// Setup work that a benchmark needs before its measured section (building a
// population, publishing candidates, writing a snapshot) happens inside the
// body and is excluded from the reported time. It is repeated per run, and the
// header of the benchmark output states this explicitly.
//
// Precision is never invented: milliseconds are printed with three decimals
// (the resolution of the clocks actually used) and rates with one decimal.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace af_bench {

/// What one completed run of a benchmark did.
struct CompletedRun {
  /// Candidates the run left in a durable, completed state.
  std::uint64_t candidates{0};
  /// Evaluation records the run created or consumed.
  std::uint64_t evaluations{0};
  /// Ranking factors the policy in force declares.
  std::uint64_t factors{0};
  /// Completed units of work the reported rate is computed from.
  std::uint64_t operations{0};
  /// Duration of the documented timed section, in milliseconds.
  double elapsed_ms{0.0};
  /// Result-integrity verdict for this run.
  bool integrity_ok{false};
  std::string integrity_detail;
};

using CompletedRunFunction = std::function<CompletedRun()>;

/// Aggregated result of repeating one benchmark at one size.
struct BenchRecord {
  std::string name;
  std::string unit;
  std::string timed_section;
  std::uint64_t size{0};
  std::uint64_t candidates{0};
  std::uint64_t evaluations{0};
  std::uint64_t factors{0};
  std::uint64_t operations{0};
  double best_ms{0.0};
  double median_ms{0.0};
  double ops_per_second{0.0};
  std::uint64_t runs{0};
  bool integrity_ok{false};
  std::string integrity_detail;
};

struct BenchOptions {
  std::vector<std::uint64_t> sizes;
  std::uint32_t repeats{3};
  std::string only;
};

/// 10, 100, 1000 and 10000 candidates.
std::vector<std::uint64_t> default_sizes();

/// Parse "--sizes 10,100 --repeat 3 --only select_candidates".
bool parse_bench_options(int argc, char** argv, BenchOptions& options, std::string& error);

/// True when the benchmark name passes the --only filter.
bool benchmark_selected(const BenchOptions& options, const std::string& name);

class BenchHarness {
 public:
  explicit BenchHarness(std::uint32_t repeats);
  BenchHarness(const BenchHarness&) = delete;
  BenchHarness& operator=(const BenchHarness&) = delete;

  /// Plain informational line.
  void line(const std::string& text);
  /// Section header.
  void header(const std::string& text);

  /// Repeat one named benchmark at one size and print its lines.
  BenchRecord run(const std::string& name, std::uint64_t size, const std::string& unit,
                  const std::string& timed_section, const CompletedRunFunction& body);

  /// Print the observed growth between two sizes next to an expected ratio.
  void scaling(const std::string& name, std::uint64_t small_size, double small_ms,
               std::uint64_t large_size, double large_ms, double expected_ratio,
               const std::string& expectation);

  /// True when any run failed its integrity check.
  [[nodiscard]] bool failed() const noexcept { return failed_; }

 private:
  std::uint32_t repeats_;
  bool failed_{false};
};

}  // namespace af_bench
