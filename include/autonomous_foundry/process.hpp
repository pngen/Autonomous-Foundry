#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/limits.hpp"

// Child process management.
//
// Every process Autonomous Foundry launches is launched through an explicit
// executable plus an argument vector. No shell is ever involved, so no
// argument can be reinterpreted as shell syntax. Child processes are created
// without a visible console window and without inheriting arbitrary handles,
// so automated validation never spawns a flashing Command Prompt and never
// produces an interactive error dialog.
//
// No execution duration limit is imposed anywhere in this layer: evaluation
// runs to natural completion and a hang is a defect to diagnose, not something
// to paper over with a deadline.

namespace autonomous_foundry {

struct ChildProcessOptions {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::filesystem::path working_directory;
  /// Explicit environment. When inherit_environment is false the child sees
  /// exactly these variables and nothing else.
  std::map<std::string, std::string> environment;
  bool inherit_environment{true};
  bool capture_output{true};
  std::size_t max_capture_bytes{kMaxCapturedOutputBytes};
};

struct ProcessResult {
  bool started{false};
  bool exited{false};
  int exit_code{0};
  /// True when the operating system reported a termination cause rather than a
  /// normal exit status (for example a crash or an external kill).
  bool terminated_abnormally{false};
  std::uint32_t termination_status{0};
  std::string standard_output;
  std::string standard_error;
  bool output_truncated{false};
};

/// Launch, wait for completion, capture bounded output. Never imposes a
/// duration limit.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<ProcessResult> run_process(
    const ChildProcessOptions& options);

/// A child process the caller controls explicitly. Used by tests and by the
/// reference deployment to start and kill coordinator and worker processes.
class AUTONOMOUS_FOUNDRY_API ManagedProcess {
 public:
  ManagedProcess();
  ~ManagedProcess();
  ManagedProcess(const ManagedProcess&) = delete;
  ManagedProcess& operator=(const ManagedProcess&) = delete;
  ManagedProcess(ManagedProcess&&) noexcept;
  ManagedProcess& operator=(ManagedProcess&&) noexcept;

  Status start(const ChildProcessOptions& options);
  [[nodiscard]] bool running() const;
  [[nodiscard]] std::uint32_t process_id() const noexcept { return process_id_; }

  /// Terminate the process and wait for it to disappear. This is used by
  /// failure proofs that deliberately kill a worker incarnation.
  Status kill();

  /// Wait for natural exit.
  Status wait(int* exit_code);

  /// Path of the file capturing this process's stdout/stderr, if any.
  [[nodiscard]] const std::filesystem::path& log_path() const noexcept { return log_path_; }

  /// Read the captured log, bounded.
  [[nodiscard]] Result<std::string> read_log(std::uint64_t max_bytes) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::uint32_t process_id_{0};
  std::filesystem::path log_path_;
};

/// Absolute path of the currently running executable.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::filesystem::path> current_executable_path();

/// True when a process with this identifier is alive.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool process_is_alive(std::uint32_t process_id);

/// Directory used for transient runtime state: workspaces, snapshots and logs
/// created by tests and by the reference deployment.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::filesystem::path default_transient_root();

/// Create a unique directory below the transient root.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::filesystem::path> make_transient_directory(
    std::string_view prefix);

}  // namespace autonomous_foundry
