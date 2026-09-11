// apps/af_coordinator_main.cpp
//
// The coordinator executable.
//
// It parses an operator command line into a CoordinatorConfig, starts the
// runtime, announces the endpoint it actually bound on one machine-readable
// line, flushes that line immediately, and then blocks until a controller asks
// the coordinator to shut down.
//
// The announcement is flushed before the blocking call on purpose: a test
// harness reads exactly one line from stdout to learn the ephemeral port and
// the epoch, and a buffered line would never arrive while the process is parked
// in run(). This is the only stdout this program produces.
//
// The library entry point run_coordinator_process() performs the same lifecycle
// without the announcement, and is what an embedder should call. This program
// drives FoundryCoordinator directly because it has to read the bound port back
// before it can announce it.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/coordinator.hpp"

namespace {

using autonomous_foundry::CoordinatorConfig;
using autonomous_foundry::Endpoint;
using autonomous_foundry::FoundryCoordinator;
using autonomous_foundry::FoundryId;
using autonomous_foundry::FoundryRunId;
using autonomous_foundry::Status;

constexpr int kUsageExitCode = 3;

void print_usage() {
  std::fputs(
      "usage: af_coordinator [options]\n"
      "  --port <n>                     TCP port to bind; 0 selects an ephemeral port\n"
      "  --state <path>                 durable snapshot file\n"
      "  --workspace <dir>              root below which attempt workspaces are created\n"
      "  --findings <dir>               directory the coordinator hands findings to; also the\n"
      "                                 default promotion sink directory\n"
      "  --promotion-dir <dir>          explicit promotion sink directory\n"
      "  --fresh                        start a brand new run, discarding any snapshot\n"
      "  --policy-dir <dir>             directory of policy documents to inspect\n"
      "  --evaluation-concurrency <n>   bounded evaluation worker threads\n",
      stderr);
}

/// Default location for state and workspace: the platform temporary root. No
/// absolute path is ever compiled into this program.
[[nodiscard]] std::filesystem::path default_root() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    return std::filesystem::path("autonomous-foundry");
  }
  return base / "autonomous-foundry";
}

[[nodiscard]] bool parse_unsigned(std::string_view text, std::uint64_t* out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    const std::uint64_t next = value * 10u + static_cast<std::uint64_t>(digit - '0');
    if (next < value) {
      return false;
    }
    value = next;
  }
  *out = value;
  return true;
}

/// Count the regular files directly inside a directory, bounded. Used only to
/// report what the operator pointed at; nothing here is applied automatically.
[[nodiscard]] std::size_t count_files(const std::filesystem::path& directory) {
  std::error_code error;
  std::size_t count = 0;
  for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end;
       entry.increment(error)) {
    if (entry->is_regular_file(error)) {
      ++count;
    }
  }
  return count;
}

}  // namespace

int main(int argc, char** argv) {
  CoordinatorConfig config;
  std::filesystem::path policy_directory;
  std::string findings_note;

  const std::filesystem::path root = default_root();
  config.endpoint.host = "127.0.0.1";
  config.endpoint.port = 0;
  config.state_path = root / "coordinator.state";
  config.workspace_root = root / "workspace";

  for (int index = 1; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    const auto value = [&](std::string_view name) -> std::string_view {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "af_coordinator: %.*s requires a value\n", static_cast<int>(name.size()),
                     name.data());
        std::exit(kUsageExitCode);
      }
      ++index;
      return std::string_view(argv[index]);
    };

    if (flag == "--port") {
      std::uint64_t port = 0;
      if (!parse_unsigned(value(flag), &port) || port > 65535u) {
        std::fputs("af_coordinator: --port expects a decimal port in 0..65535\n", stderr);
        return kUsageExitCode;
      }
      config.endpoint.port = static_cast<std::uint16_t>(port);
    } else if (flag == "--state") {
      config.state_path = std::filesystem::path(std::string(value(flag)));
    } else if (flag == "--workspace") {
      config.workspace_root = std::filesystem::path(std::string(value(flag)));
    } else if (flag == "--findings") {
      const std::filesystem::path findings(std::string(value(flag)));
      findings_note = findings.string();
      if (config.promotion_sink_directory.empty()) {
        config.promotion_sink_directory = findings;
      }
    } else if (flag == "--promotion-dir") {
      config.promotion_sink_directory = std::filesystem::path(std::string(value(flag)));
    } else if (flag == "--fresh") {
      config.fresh_start = true;
    } else if (flag == "--policy-dir") {
      policy_directory = std::filesystem::path(std::string(value(flag)));
    } else if (flag == "--evaluation-concurrency") {
      std::uint64_t concurrency = 0;
      if (!parse_unsigned(value(flag), &concurrency) || concurrency == 0 ||
          concurrency > 1024u) {
        std::fputs("af_coordinator: --evaluation-concurrency expects 1..1024\n", stderr);
        return kUsageExitCode;
      }
      config.evaluation_concurrency = static_cast<std::uint32_t>(concurrency);
    } else if (flag == "--help" || flag == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "af_coordinator: unknown argument %.*s\n",
                   static_cast<int>(flag.size()), flag.data());
      print_usage();
      return kUsageExitCode;
    }
  }

  if (!policy_directory.empty()) {
    std::error_code error;
    if (!std::filesystem::is_directory(policy_directory, error)) {
      std::fprintf(stderr, "af_coordinator: --policy-dir %s is not a readable directory\n",
                   policy_directory.string().c_str());
      return kUsageExitCode;
    }
    // Policy documents are not applied from a directory. A policy becomes
    // authoritative only when it is defined through the control plane, because
    // that is the only path on which the foundry issues it a durable identity
    // and a content digest. The coordinator does not invent policy identity, so
    // it reports what it found and leaves the decision to the operator.
    std::fprintf(stderr,
                 "af_coordinator: --policy-dir %s holds %zu file(s); policies take effect only "
                 "when defined through the control plane, never by reading a directory\n",
                 policy_directory.string().c_str(), count_files(policy_directory));
  }

  try {
    FoundryCoordinator coordinator(config);
    const Status started = coordinator.start();
    if (!started.ok()) {
      std::fprintf(stderr, "af_coordinator: start failed: %s\n", started.to_string().c_str());
      return 1;
    }

    // FoundryCoordinator::run() is the blocking call, so the durable run
    // identity is read from the core the coordinator owns.
    const FoundryId foundry = coordinator.core().foundry();
    const FoundryRunId run = coordinator.core().run();
    std::printf("af_coordinator listening port=%u epoch=%llu run=%llu foundry=%llu\n",
                static_cast<unsigned>(coordinator.port()),
                static_cast<unsigned long long>(coordinator.epoch().value()),
                static_cast<unsigned long long>(run.raw()),
                static_cast<unsigned long long>(foundry.raw()));
    std::fflush(stdout);

    if (!config.promotion_sink_directory.empty()) {
      std::fprintf(stderr, "af_coordinator: promotion sink directory %s\n",
                   config.promotion_sink_directory.string().c_str());
    }
    if (!findings_note.empty()) {
      std::fprintf(stderr, "af_coordinator: findings directory %s\n", findings_note.c_str());
    }

    const Status finished = coordinator.run();
    if (!finished.ok()) {
      std::fprintf(stderr, "af_coordinator: shutdown reported: %s\n",
                   finished.to_string().c_str());
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "af_coordinator: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "af_coordinator: unknown failure\n");
    return 1;
  }
}
