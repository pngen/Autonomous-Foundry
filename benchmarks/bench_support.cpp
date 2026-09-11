#include "bench_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace af_bench {
namespace {

constexpr std::uint64_t kMinReportedSize = 1;

bool parse_size(std::string_view text, std::uint64_t& value) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    parsed = (parsed * 10u) + static_cast<std::uint64_t>(character - '0');
    if (parsed > 100000000ull) {
      return false;
    }
  }
  if (parsed < kMinReportedSize) {
    return false;
  }
  value = parsed;
  return true;
}

std::vector<std::uint64_t> parse_size_list(std::string_view text, bool& ok) {
  std::vector<std::uint64_t> sizes;
  std::size_t begin = 0;
  ok = true;
  while (begin <= text.size()) {
    const std::size_t comma = text.find(',', begin);
    const std::size_t stop = comma == std::string_view::npos ? text.size() : comma;
    const std::string_view token = text.substr(begin, stop - begin);
    std::uint64_t value = 0;
    if (!parse_size(token, value)) {
      ok = false;
      return sizes;
    }
    sizes.push_back(value);
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1;
  }
  if (sizes.empty()) {
    ok = false;
  }
  return sizes;
}

double median_of(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  if ((values.size() % 2u) == 1u) {
    return values[middle];
  }
  return (values[middle - 1] + values[middle]) / 2.0;
}

}  // namespace

std::vector<std::uint64_t> default_sizes() { return {10u, 100u, 1000u, 10000u}; }

bool parse_bench_options(int argc, char** argv, BenchOptions& options, std::string& error) {
  options.sizes = default_sizes();
  options.repeats = 3;
  options.only.clear();

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--sizes") {
      if (index + 1 >= argc) {
        error = "--sizes needs a comma separated list, for example --sizes 10,100,1000,10000";
        return false;
      }
      bool ok = false;
      const std::vector<std::uint64_t> sizes = parse_size_list(argv[++index], ok);
      if (!ok) {
        error = "--sizes expects positive decimal sizes separated by commas";
        return false;
      }
      options.sizes = sizes;
    } else if (argument == "--repeat") {
      if (index + 1 >= argc) {
        error = "--repeat needs a count";
        return false;
      }
      std::uint64_t repeats = 0;
      if (!parse_size(argv[++index], repeats) || repeats > 1000u) {
        error = "--repeat expects a count between 1 and 1000";
        return false;
      }
      options.repeats = static_cast<std::uint32_t>(repeats);
    } else if (argument == "--only") {
      if (index + 1 >= argc) {
        error = "--only needs a benchmark name";
        return false;
      }
      options.only = argv[++index];
    } else if (argument == "--help" || argument == "-h") {
      error =
          "usage: af_benchmarks [--sizes 10,100,1000,10000] [--repeat n] [--only name]";
      return false;
    } else {
      error = "unknown argument '" + std::string(argument) + "'";
      return false;
    }
  }
  return true;
}

bool benchmark_selected(const BenchOptions& options, const std::string& name) {
  return options.only.empty() || options.only == name;
}

BenchHarness::BenchHarness(std::uint32_t repeats) : repeats_(repeats == 0 ? 1u : repeats) {}

void BenchHarness::line(const std::string& text) { std::printf("%s\n", text.c_str()); }

void BenchHarness::header(const std::string& text) { std::printf("\n== %s ==\n", text.c_str()); }

BenchRecord BenchHarness::run(const std::string& name, std::uint64_t size, const std::string& unit,
                              const std::string& timed_section,
                              const CompletedRunFunction& body) {
  const std::string qualified = name + "/" + std::to_string(size);
  BenchRecord record;
  record.name = name;
  record.unit = unit;
  record.timed_section = timed_section;
  record.size = size;

  std::vector<double> timings;
  timings.reserve(repeats_);
  double best_ms = 0.0;
  bool have_best = false;
  CompletedRun best_run;

  for (std::uint32_t repetition = 0; repetition < repeats_; ++repetition) {
    const CompletedRun completed = body();
    ++record.runs;
    if (!completed.integrity_ok) {
      record.integrity_ok = false;
      if (record.integrity_detail.empty()) {
        record.integrity_detail = completed.integrity_detail;
      }
      failed_ = true;
    }
    if (!(completed.elapsed_ms > 0.0) || !std::isfinite(completed.elapsed_ms)) {
      record.integrity_ok = false;
      record.integrity_detail = "the run reported a non-positive or non-finite duration";
      failed_ = true;
      continue;
    }
    timings.push_back(completed.elapsed_ms);
    if (!have_best || completed.elapsed_ms < best_ms) {
      best_ms = completed.elapsed_ms;
      best_run = completed;
      have_best = true;
    }
  }

  if (have_best) {
    record.candidates = best_run.candidates;
    record.evaluations = best_run.evaluations;
    record.factors = best_run.factors;
    record.operations = best_run.operations;
    record.best_ms = best_ms;
    record.median_ms = median_of(timings);
    if (best_ms > 0.0 && best_run.operations > 0) {
      record.ops_per_second = static_cast<double>(best_run.operations) * 1000.0 / best_ms;
    }
    if (record.integrity_detail.empty()) {
      record.integrity_ok = true;
    }
  } else {
    record.integrity_ok = false;
    if (record.integrity_detail.empty()) {
      record.integrity_detail = "no completed run reported a usable duration";
    }
    failed_ = true;
  }

  std::printf("BENCH %s unit=%s runs=%llu timed=%s\n", qualified.c_str(), unit.c_str(),
              static_cast<unsigned long long>(record.runs), timed_section.c_str());
  std::printf(
      "BENCH %s candidates=%llu evaluations=%llu factors=%llu best_ms=%.3f median_ms=%.3f "
      "ops_per_second=%.1f\n",
      qualified.c_str(), static_cast<unsigned long long>(record.candidates),
      static_cast<unsigned long long>(record.evaluations),
      static_cast<unsigned long long>(record.factors), record.best_ms, record.median_ms,
      record.ops_per_second);
  if (record.integrity_ok) {
    std::printf("BENCH %s integrity=ok\n", qualified.c_str());
  } else {
    std::printf("BENCH %s integrity=FAILED detail=%s\n", qualified.c_str(),
                record.integrity_detail.c_str());
  }
  std::fflush(stdout);
  return record;
}

void BenchHarness::scaling(const std::string& name, std::uint64_t small_size, double small_ms,
                           std::uint64_t large_size, double large_ms, double expected_ratio,
                           const std::string& expectation) {
  if (!(small_ms > 0.0) || !(large_ms > 0.0) || small_size == 0 || large_size == 0) {
    return;
  }
  const double observed = large_ms / small_ms;
  const double tolerance = expected_ratio * 2.0;
  const char* verdict = observed <= tolerance ? "consistent-with-expected-growth"
                                              : "slower-than-expected-growth";
  std::printf(
      "SCALING %s from=%llu to=%llu observed_ratio=%.2f expected_ratio=%.2f verdict=%s "
      "expectation=%s\n",
      name.c_str(), static_cast<unsigned long long>(small_size),
      static_cast<unsigned long long>(large_size), observed, expected_ratio, verdict,
      expectation.c_str());
  std::fflush(stdout);
}

}  // namespace af_bench
