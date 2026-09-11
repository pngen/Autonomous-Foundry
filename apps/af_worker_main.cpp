// apps/af_worker_main.cpp
//
// The worker executable.
//
// It parses an operator command line into a WorkerRuntimeConfig, announces the
// identity it will register under on one flushed stdout line, and then serves
// assignments until the coordinator shuts it down.
//
// A worker identity is a durable, operator-owned name. When the operator does
// not supply one, this program mints it deterministically from the strategy
// name, so the same strategy always registers under the same durable identity
// and a repeated run replaces its own previous incarnation instead of
// accumulating anonymous workers.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/worker_runtime.hpp"

namespace {

using autonomous_foundry::ErrorCode;
using autonomous_foundry::IdKind;
using autonomous_foundry::ReferenceStrategy;
using autonomous_foundry::Status;
using autonomous_foundry::StrongId;
using autonomous_foundry::WorkerId;
using autonomous_foundry::WorkerIdTag;
using autonomous_foundry::WorkerRuntimeConfig;

constexpr int kUsageExitCode = 3;

void print_usage() {
  std::fputs(
      "usage: af_worker --coordinator <host:port> [options]\n"
      "  --coordinator <host:port>   coordinator endpoint (required)\n"
      "  --worker-id <decimal>       durable worker identity as a raw decimal value\n"
      "  --strategy <name>           closed-form | iterative | off-by-one | self-reported-pass\n"
      "  --workspace <dir>           root below which attempt workspaces are created\n"
      "  --label <text>              operator-facing worker label\n"
      "  --self-report-success       claim success regardless of the produced output\n",
      stderr);
}

/// Mint a durable worker identity from the strategy name. Deterministic: one
/// strategy always maps to one worker identity.
[[nodiscard]] WorkerId worker_identity_for(std::string_view strategy) {
  const std::uint64_t mixed = autonomous_foundry::fnv1a64(strategy);
  std::uint64_t counter = (mixed >> 24) & 0xFFFFFFFFull;
  if (counter == 0) {
    counter = 1;
  }
  const std::uint64_t salt = mixed & 0xFFFFFFull;
  return StrongId<WorkerIdTag>::from_raw(
      (static_cast<std::uint64_t>(IdKind::Worker) << 56) | (salt << 32) | counter);
}

[[nodiscard]] std::filesystem::path default_workspace_root() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    return std::filesystem::path("autonomous-foundry") / "worker-workspace";
  }
  return base / "autonomous-foundry" / "worker-workspace";
}

}  // namespace

int main(int argc, char** argv) {
  WorkerRuntimeConfig config;
  bool have_coordinator = false;
  bool have_worker_id = false;
  std::uint64_t worker_raw = 0;
  std::string strategy_name("closed-form");
  std::string label;

  for (int index = 1; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    const auto value = [&](std::string_view name) -> std::string_view {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "af_worker: %.*s requires a value\n", static_cast<int>(name.size()),
                     name.data());
        std::exit(kUsageExitCode);
      }
      ++index;
      return std::string_view(argv[index]);
    };

    if (flag == "--coordinator") {
      const auto parsed = autonomous_foundry::parse_endpoint(std::string(value(flag)));
      if (!parsed.ok()) {
        std::fprintf(stderr, "af_worker: %s\n", parsed.status().to_string().c_str());
        return kUsageExitCode;
      }
      config.coordinator = parsed.value();
      have_coordinator = true;
    } else if (flag == "--worker-id") {
      const auto parsed = autonomous_foundry::parse_raw_identity(std::string(value(flag)));
      if (!parsed.ok()) {
        std::fprintf(stderr, "af_worker: %s\n", parsed.status().to_string().c_str());
        return kUsageExitCode;
      }
      worker_raw = parsed.value();
      have_worker_id = true;
    } else if (flag == "--strategy") {
      strategy_name = std::string(value(flag));
      const auto parsed = autonomous_foundry::parse_reference_strategy(strategy_name);
      if (!parsed.ok()) {
        std::fprintf(stderr, "af_worker: %s\n", parsed.status().to_string().c_str());
        return kUsageExitCode;
      }
      config.strategy = parsed.value();
    } else if (flag == "--workspace") {
      config.workspace_root = std::filesystem::path(std::string(value(flag)));
    } else if (flag == "--label") {
      label = std::string(value(flag));
    } else if (flag == "--self-report-success") {
      config.self_report_success_unconditionally = true;
    } else if (flag == "--help" || flag == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "af_worker: unknown argument %.*s\n", static_cast<int>(flag.size()),
                   flag.data());
      print_usage();
      return kUsageExitCode;
    }
  }

  if (!have_coordinator) {
    std::fputs("af_worker: --coordinator is required\n", stderr);
    print_usage();
    return kUsageExitCode;
  }

  if (have_worker_id) {
    config.worker = StrongId<WorkerIdTag>::from_raw(worker_raw);
    if (!config.worker.valid()) {
      std::fputs("af_worker: --worker-id must not be the null identity\n", stderr);
      return kUsageExitCode;
    }
  } else {
    config.worker = worker_identity_for(strategy_name);
  }
  config.label = label.empty() ? std::string("reference-worker") : label;
  if (config.workspace_root.empty()) {
    config.workspace_root = default_workspace_root();
  }

  std::error_code error;
  std::filesystem::create_directories(config.workspace_root, error);
  if (error) {
    std::fprintf(stderr, "af_worker: workspace root %s could not be created: %s\n",
                 config.workspace_root.string().c_str(), error.message().c_str());
    return 1;
  }

  std::printf("af_worker connecting coordinator=%s worker=%llu strategy=%s\n",
              config.coordinator.to_string().c_str(),
              static_cast<unsigned long long>(config.worker.raw()), strategy_name.c_str());
  std::fflush(stdout);

  return autonomous_foundry::run_worker_process(config);
}
