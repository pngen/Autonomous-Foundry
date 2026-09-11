#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "autonomous_foundry/accounting.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/evaluator.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/persistence.hpp"
#include "autonomous_foundry/promotion.hpp"
#include "autonomous_foundry/transport.hpp"

// The coordinator runtime.
//
// One coordinator process owns durable foundry state. It accepts framed TCP
// connections from workers and controllers, drives the foundry state machine,
// runs evaluations on a bounded pool, and persists at every durable boundary
// before it tells anyone about it.
//
// Ordering rule that the whole recovery story rests on: mutate -> persist ->
// notify. An authorization that was never persisted is never sent, and work
// that was never durably dispatched is never mistaken for work that happened.

namespace autonomous_foundry {

struct CoordinatorConfig {
  Endpoint endpoint;

  /// Durable snapshot file.
  std::filesystem::path state_path;

  /// Root below which per-attempt workspaces are created.
  std::filesystem::path workspace_root;

  /// Identity of the foundry. Stable across coordinator restarts.
  FoundryId foundry;

  /// Start a brand new run, discarding any existing snapshot.
  bool fresh_start{false};

  BudgetLimits budgets{};

  /// Bounded evaluation worker threads. Evaluations never run on a connection
  /// thread and never hold the state lock.
  std::uint32_t evaluation_concurrency{4};

  /// Reference toolchain for compile-backed evaluation.
  bool enable_toolchain_probe{true};

  /// Directory handed to the reference local promotion sink. Empty means no
  /// promotion sink is installed.
  std::filesystem::path promotion_sink_directory;

  /// Reference policy parameters applied to populations the controller creates
  /// without an explicit policy.
  FoundryPolicy default_policy{};
};

struct CoordinatorStatistics {
  std::uint64_t connections_accepted{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_sent{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t evaluations_started{0};
  std::uint64_t evaluations_completed{0};
  std::uint64_t evaluations_failed{0};
  std::uint64_t snapshots_written{0};
  std::uint64_t stale_rejections{0};
  std::uint64_t protocol_violations{0};
  std::uint64_t worker_disconnects{0};
};

class AUTONOMOUS_FOUNDRY_API FoundryCoordinator {
 public:
  explicit FoundryCoordinator(CoordinatorConfig config);
  ~FoundryCoordinator();

  FoundryCoordinator(const FoundryCoordinator&) = delete;
  FoundryCoordinator& operator=(const FoundryCoordinator&) = delete;

  /// Bind, recover or create state, start the accept loop and the pump thread.
  Status start();

  /// Block until shutdown is requested through the control plane and every
  /// thread has stopped. Returns once shutdown is complete.
  Status run();

  /// Ask the coordinator to stop. Safe to call from a connection thread.
  void request_shutdown(std::string reason);

  [[nodiscard]] Status shutdown(std::string reason);

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] FoundryRunId run() const;
  [[nodiscard]] FoundryCore& core() noexcept { return *core_; }
  [[nodiscard]] const CoordinatorStatistics& statistics() const noexcept { return statistics_; }
  [[nodiscard]] bool shutting_down() const noexcept { return shutting_down_.load(); }

  /// Force a durable snapshot now. Used by tests and by the CLI.
  Status persist();

 private:
  struct Impl;
  /// Shared ownership on purpose: every thread this runtime starts captures a
  /// weak reference to the impl, and the accept loop asks for one through
  /// weak_from_this(). A unique_ptr owner would make that request throw
  /// bad_weak_ptr, so the impl is created as a shared_ptr and the threads still
  /// cannot keep the coordinator alive after its owner is gone.
  std::shared_ptr<Impl> impl_;
  std::unique_ptr<FoundryCore> core_;
  std::uint16_t port_{0};
  CoordinatorStatistics statistics_{};
  std::atomic<bool> shutting_down_{false};
};

/// Convenience entry point used by the af_coordinator executable and by tests.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API int run_coordinator_process(
    const CoordinatorConfig& config);

}  // namespace autonomous_foundry
