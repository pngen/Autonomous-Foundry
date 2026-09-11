#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/process.hpp"
#include "autonomous_foundry/transport.hpp"
#include "autonomous_foundry/worker.hpp"

// Reference worker runtime.
//
// The worker is an ordinary external process. It speaks the framed protocol,
// produces a real candidate output inside the workspace it was given, and
// publishes it. It never evaluates its own work and it never decides anything:
// it produces, and it reports.

namespace autonomous_foundry {

struct WorkerRuntimeConfig {
  Endpoint coordinator;
  WorkerId worker;
  std::string label;
  ReferenceStrategy strategy{ReferenceStrategy::ClosedForm};
  std::filesystem::path workspace_root;
  /// When true the worker reports success even when its output is wrong. Used
  /// by the adversarial proof that a self report is evidence, not authority.
  bool self_report_success_unconditionally{false};
};

class AUTONOMOUS_FOUNDRY_API ReferenceWorkerRuntime {
 public:
  explicit ReferenceWorkerRuntime(WorkerRuntimeConfig config);
  ~ReferenceWorkerRuntime();

  ReferenceWorkerRuntime(const ReferenceWorkerRuntime&) = delete;
  ReferenceWorkerRuntime& operator=(const ReferenceWorkerRuntime&) = delete;

  /// Connect, register, serve assignments until shutdown. Returns Ok on a
  /// clean shutdown and a Status describing the failure otherwise.
  Status run();

  [[nodiscard]] std::uint64_t assignments_served() const noexcept { return assignments_served_; }
  [[nodiscard]] const WorkerBootId& boot() const noexcept { return boot_; }

 private:
  struct Impl;
  // The implementation hands the connection callbacks a std::weak_ptr to itself
  // so a frame that arrives while the runtime is being torn down is dropped
  // instead of touching freed state. enable_shared_from_this only produces a
  // usable weak pointer when the object actually has a shared owner, so the
  // pimpl is owned by a shared_ptr rather than a unique_ptr: with a unique
  // owner weak_from_this() stays empty, every callback silently locks to
  // nothing, and the worker would never observe its own handshake.
  std::shared_ptr<Impl> impl_;
  WorkerRuntimeConfig config_;
  WorkerBootId boot_;
  std::uint64_t assignments_served_{0};
};

/// Entry point used by the af_worker executable and by tests.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API int run_worker_process(const WorkerRuntimeConfig& config);

}  // namespace autonomous_foundry
