#pragma once

// Shared support for the distributed multiprocess proof suites.
//
// These helpers exist so that the three distributed suites can drive a REAL
// coordinator process, REAL worker processes and a REAL framed TCP controller
// session without duplicating the process/transport plumbing three times.
//
// What is here, and why:
//
//   * ChildProcess - a ManagedProcess wrapper that also removes the transient
//     log directory the process layer creates, so a finished case leaves no
//     temporary artifact behind.
//
//   * ProtocolClient - a framed TCP client built from the PUBLIC transport and
//     protocol headers. It is the controller the distributed suites use, and it
//     is also the raw worker socket used by the stale-authority proofs.
//
//   * Message helpers - typed request/response wrappers around
//     encode_payload()/decode_*(), so a case reads as a sequence of protocol
//     operations rather than as a pile of byte shuffling.
//
//   * Wait helpers - unbounded loops that re-issue a real request each round.
//     There is no deadline, no attempt cap and no watchdog anywhere in this
//     file. A case that never satisfies its predicate is localised by the PHASE
//     marker the caller emitted immediately before the loop.
//
// This header is header-only on purpose: CMakeLists.txt is not modified, and
// all three distributed suites are compiled into af_distributed_tests together
// with the shared harness translation unit.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/process.hpp"
#include "autonomous_foundry/protocol.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/retention.hpp"
#include "autonomous_foundry/selection.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/transport.hpp"
#include "autonomous_foundry/worker.hpp"
#include "test_support.hpp"

namespace af_dist {

using namespace autonomous_foundry;  // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Failure reporting inside helpers
// ---------------------------------------------------------------------------
//
// The expectation macros expand the caller's file and line only where they are
// written, so a helper that wants to report the CALLER's location needs its own
// macro. Both macros below are used exclusively inside case bodies.

#define AF_DIST_REQUIRE(af_context, af_condition, af_message)                             \
  do {                                                                                   \
    if (!(af_condition)) {                                                                \
      (af_context).fail_at(__FILE__, __LINE__, std::string(af_message));                  \
    }                                                                                     \
  } while (false)

// The expression is materialised into a named local first, so a Result<T>
// prvalue is never destroyed while a reference into it is still being read.
#define AF_DIST_REQUIRE_OK(af_context, af_expr)                                            \
  do {                                                                                     \
    auto af_dist_value__ = (af_expr);                                                       \
    const ::autonomous_foundry::Status& af_dist_status__ =                                  \
        ::autonomous_foundry::to_status(af_dist_value__);                                   \
    if (!af_dist_status__.ok()) {                                                           \
      (af_context).fail_at(                                                                 \
          __FILE__, __LINE__,                                                               \
          std::string("expected success from " #af_expr " but got code=") +                 \
              std::string(::autonomous_foundry::error_code_name(af_dist_status__.code())) + \
              " message='" + af_dist_status__.message() + "'");                             \
    }                                                                                       \
  } while (false)

// ---------------------------------------------------------------------------
// Executables
// ---------------------------------------------------------------------------

/// Resolve a sibling executable: the environment variable when it is set,
/// otherwise the executable next to the running test binary. The environment
/// spelling is what CMake sets for the ctest registration; the sibling fallback
/// is what a developer gets from a plain "cmake --build" run.
[[nodiscard]] inline std::filesystem::path locate_executable(std::string_view environment_name,
                                                             std::string_view leaf) {
  const std::string name(environment_name);
  if (const char* const configured = std::getenv(name.c_str());
      configured != nullptr && configured[0] != '\0') {
    return std::filesystem::path(configured);
  }
  const Result<std::filesystem::path> self = current_executable_path();
  if (!self.ok()) {
    return std::filesystem::path();
  }
  return self.value().parent_path() / std::string(leaf);
}

/// Fail the case unless the executable exists. A distributed proof that cannot
/// launch its process must say so rather than quietly proving nothing.
[[nodiscard]] inline std::filesystem::path require_executable(
    const af_test::TestContext& context, const std::filesystem::path& candidate,
    std::string_view label) {
  std::error_code error;
  if (candidate.empty() || !std::filesystem::is_regular_file(candidate, error)) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("the ") + std::string(label) + " executable is not available at '" +
                        candidate.string() + "'");
  }
  return candidate;
}

/// Deterministic identity value in one domain, used to name workers the case
/// owns. The kind byte must be the domain's own tag value, because the wire
/// decoder validates the domain of every identity it reads.
[[nodiscard]] inline std::uint64_t identity_raw(IdKind kind, std::uint32_t salt,
                                                std::uint32_t counter) {
  return (static_cast<std::uint64_t>(kind) << detail::kIdKindShift) |
         ((static_cast<std::uint64_t>(salt) & detail::kIdSaltMask) << detail::kIdSaltShift) |
         (static_cast<std::uint64_t>(counter) & detail::kIdCounterMask);
}

// ---------------------------------------------------------------------------
// Child processes
// ---------------------------------------------------------------------------

/// A child process launched through ManagedProcess.
///
/// ManagedProcess allocates a transient log directory per child and never
/// removes it; this wrapper removes that directory once the child has been
/// reaped, so a finished case leaves nothing behind. The destructor kills a
/// child that is still running, which is what makes "no child is left alive" a
/// property of the type rather than of every call site.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { close(); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&&) = delete;
  ChildProcess& operator=(ChildProcess&&) = delete;

  Status start(ChildProcessOptions options) {
    options.capture_output = false;
    const Status started = process_.start(options);
    if (started.ok()) {
      log_directory_ = process_.log_path().parent_path().parent_path();
    }
    return started;
  }

  [[nodiscard]] bool started() const noexcept { return pid() != 0; }
  [[nodiscard]] bool running() const { return process_.running(); }
  [[nodiscard]] std::uint32_t pid() const noexcept { return process_.process_id(); }

  /// Terminate the child and wait for it to disappear.
  Status kill() {
    if (pid() == 0) {
      return Status(ErrorCode::NotConnected, "no child process has been started");
    }
    if (!process_.running()) {
      return Status();
    }
    return process_.kill();
  }

  /// Wait for natural exit.
  Status wait(int* exit_code) { return process_.wait(exit_code); }

  /// Bounded read of the child's captured stdout/stderr.
  [[nodiscard]] std::string log() const {
    const Result<std::string> text = process_.read_log(256u * 1024u);
    return text.ok() ? text.value() : std::string();
  }

  /// Kill, reap and remove the transient log directory. Idempotent.
  void close() {
    if (pid() != 0) {
      if (process_.running()) {
        (void)process_.kill();
      }
      int code = 0;
      (void)process_.wait(&code);
    }
    remove_log_directory();
  }

 private:
  void remove_log_directory() {
    if (log_directory_.empty()) {
      return;
    }
    std::error_code error;
    std::filesystem::remove_all(log_directory_, error);
    log_directory_.clear();
  }

  ManagedProcess process_;
  std::filesystem::path log_directory_;
};

/// Fail the case when a child the proof depends on has died.
inline void require_running(const af_test::TestContext& context, const ChildProcess& child,
                            std::string_view label) {
  if (!child.running()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string(label) + " exited while the case was still waiting for it; log:\n" +
                        child.log());
  }
}

// ---------------------------------------------------------------------------
// Framed TCP client
// ---------------------------------------------------------------------------

/// One framed TCP connection driven by the test.
///
/// The Connection delivers whole frames on its own reader thread; this class
/// hands them to the case on the calling thread. request() sends a frame and
/// blocks until a frame of an expected type, or an ErrorResponse, arrives.
///
/// There is no timeout anywhere: a request waits until the coordinator answers
/// or the connection closes, and the caller emits its PHASE marker before
/// calling.
class ProtocolClient {
 public:
  ProtocolClient() = default;
  ~ProtocolClient() { close(); }

  ProtocolClient(const ProtocolClient&) = delete;
  ProtocolClient& operator=(const ProtocolClient&) = delete;

  Status connect(const Endpoint& endpoint) {
    AF_TRY(SocketSubsystem::ensure_initialized());
    SocketHandle socket;
    AF_TRY_ASSIGN(socket, connect_to(endpoint));

    // The callbacks capture this raw pointer. That is sound because the only
    // shared_ptr to the Connection is owned here, and close() releases it,
    // which joins both connection threads before any member of this object is
    // destroyed.
    ProtocolClient* const self = this;
    ConnectionCallbacks callbacks;
    callbacks.on_frame = [self](const Frame& frame) { self->deliver(frame); };
    callbacks.on_closed = [self](const Status& reason) { self->note_closed(reason); };

    std::shared_ptr<Connection> created = std::make_shared<Connection>(
        std::move(socket), endpoint.to_string(), std::move(callbacks));
    AF_TRY(created->start());
    connection_ = std::move(created);
    return Status();
  }

  /// Send a Hello for this session role and wait for the HelloAck. The
  /// acknowledgement is retained, because it carries the coordinates of the
  /// live session and the epoch this client is being served by.
  Result<HelloAckMessage> hello(const af_test::TestContext& context, SessionRole role,
                                WorkerId worker = WorkerId(), WorkerBootId boot = WorkerBootId(),
                                std::string label = std::string(),
                                std::string capability = std::string()) {
    HelloMessage message;
    message.role = role;
    message.run = FoundryRunId();
    message.worker = worker;
    message.boot = boot;
    message.controller = ControllerId();
    message.label = std::move(label);
    message.capability = std::move(capability);
    message.process_id = 0;
    message.observed_epoch = CoordinatorEpoch();

    std::string payload;
    AF_TRY_ASSIGN(payload, encode_payload(message));
    Frame frame;
    AF_TRY_ASSIGN(frame, exchange(context, MessageType::Hello, std::move(payload),
                                  {MessageType::HelloAck}));
    HelloAckMessage ack;
    AF_TRY_ASSIGN(ack, decode_hello_ack(frame.payload));
    have_ack_ = true;
    ack_ = ack;
    return ack;
  }

  /// Send one frame and return the first frame of an expected type. A frame
  /// whose type is not expected is reported and skipped, and an ErrorResponse
  /// becomes a failed Result carrying the coordinator's own error code, so a
  /// refusal is observed by code rather than by string matching.
  Result<Frame> exchange(const af_test::TestContext& context, MessageType type,
                         std::string payload, std::initializer_list<MessageType> expected) {
    if (!connection_) {
      return Status(ErrorCode::NotConnected, "the client is not connected");
    }
    AF_TRY(connection_->send(type, std::move(payload)));
    for (;;) {
      Result<Frame> received = next_frame();
      if (!received.ok()) {
        return received.status();
      }
      Frame frame = std::move(received).value();
      if (frame.header.type == MessageType::ErrorResponse) {
        const Result<ErrorResponseMessage> decoded = decode_error_response(frame.payload);
        if (!decoded.ok()) {
          return Status(ErrorCode::ProtocolViolation,
                        "the coordinator answered with an undecodable error response");
        }
        return Status(decoded.value().code, decoded.value().detail);
      }
      for (const MessageType wanted : expected) {
        if (wanted == frame.header.type) {
          return frame;
        }
      }
      context.note(std::string("ignored an unexpected frame of type '") +
                   std::string(message_type_name(frame.header.type)) + "'");
    }
  }

  /// Close the connection and join its threads.
  void close() {
    if (connection_) {
      connection_->request_close(
          Status(ErrorCode::ConnectionClosed, "the test client is closing the connection"));
      connection_.reset();
    }
  }

  [[nodiscard]] bool connected() const noexcept { return static_cast<bool>(connection_); }
  [[nodiscard]] bool have_ack() const noexcept { return have_ack_; }
  [[nodiscard]] const HelloAckMessage& ack() const noexcept { return ack_; }

  /// Frames this client actually received. A proof that frames crossed a socket
  /// can assert on this rather than on a coincidence.
  [[nodiscard]] std::uint64_t frames_received() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return frames_received_;
  }

 private:
  void deliver(const Frame& frame) {
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      inbox_.push_back(frame);
    }
    wake_.notify_all();
  }

  void note_closed(const Status& reason) {
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      closed_ = true;
      if (!reason.ok() && close_detail_.empty()) {
        close_detail_ = reason.message();
      }
    }
    wake_.notify_all();
  }

  Result<Frame> next_frame() {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [this] { return !inbox_.empty() || closed_; });
    if (inbox_.empty()) {
      return Status(ErrorCode::ConnectionClosed,
                    close_detail_.empty() ? std::string("the peer closed the connection")
                                          : close_detail_);
    }
    Frame frame = std::move(inbox_.front());
    inbox_.pop_front();
    frames_received_ += 1;
    return frame;
  }

  std::shared_ptr<Connection> connection_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Frame> inbox_;
  bool closed_{false};
  std::string close_detail_;
  std::uint64_t frames_received_{0};
  bool have_ack_{false};
  HelloAckMessage ack_;
};

// ---------------------------------------------------------------------------
// Authority construction
// ---------------------------------------------------------------------------

[[nodiscard]] inline ControllerSessionAuthority controller_session(const HelloAckMessage& ack) {
  ControllerSessionAuthority session;
  session.coordinator_epoch = ack.coordinator_epoch;
  session.run = ack.run;
  // A controller that has not been told which controller identity it speaks for
  // sends none, and the coordinator resolves it to the operator controller it
  // issued for its own surface.
  session.controller = ControllerId();
  session.session = ack.session;
  session.session_generation = ack.session_generation;
  return session;
}

/// The coordinates a controller operation authority carries. The protocol
/// decoder requires every identity field in the block to be present, so a
/// binding names the population, its task and its policy explicitly rather than
/// leaving the coordinator to guess which one an operation meant.
struct PopulationBinding {
  PopulationId population;
  PopulationGeneration population_generation;
  TaskId task;
  TaskGeneration task_generation;
  PolicyId policy;
  PolicyGeneration policy_generation;
};

[[nodiscard]] inline PopulationBinding population_binding(PopulationId population,
                                                         PopulationGeneration generation,
                                                         TaskId task,
                                                         TaskGeneration task_generation,
                                                         PolicyId policy,
                                                         PolicyGeneration policy_generation) {
  PopulationBinding binding;
  binding.population = population;
  binding.population_generation = generation;
  binding.task = task;
  binding.task_generation = task_generation;
  binding.policy = policy;
  binding.policy_generation = policy_generation;
  return binding;
}

[[nodiscard]] inline ControllerOperationAuthority operation_authority(
    const HelloAckMessage& ack, const PopulationBinding& binding) {
  ControllerOperationAuthority authority;
  authority.session = controller_session(ack);
  authority.population = binding.population;
  authority.population_generation = binding.population_generation;
  authority.task = binding.task;
  authority.task_generation = binding.task_generation;
  authority.policy = binding.policy;
  authority.policy_generation = binding.policy_generation;
  return authority;
}

[[nodiscard]] inline WorkerSessionAuthority worker_session(const HelloAckMessage& ack,
                                                          WorkerId worker, WorkerBootId boot) {
  WorkerSessionAuthority session;
  session.coordinator_epoch = ack.coordinator_epoch;
  session.run = ack.run;
  session.worker = worker;
  session.boot = boot;
  session.session = ack.session;
  session.session_generation = ack.session_generation;
  return session;
}

// ---------------------------------------------------------------------------
// Control-plane requests
// ---------------------------------------------------------------------------

[[nodiscard]] inline Result<PolicyDefinedMessage> define_policy(
    const af_test::TestContext& context, ProtocolClient& client, const FoundryPolicy& policy) {
  DefinePolicyMessage message;
  message.session = controller_session(client.ack());
  message.policy = policy;
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::DefinePolicy, std::move(payload),
                                       {MessageType::PolicyDefined}));
  PolicyDefinedMessage reply;
  AF_TRY_ASSIGN(reply, decode_policy_defined(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<TaskCreatedMessage> create_task(const af_test::TestContext& context,
                                                            ProtocolClient& client,
                                                            const TaskSpec& task) {
  CreateTaskMessage message;
  message.session = controller_session(client.ack());
  message.task = task;
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::CreateTask, std::move(payload),
                                       {MessageType::TaskCreated}));
  TaskCreatedMessage reply;
  AF_TRY_ASSIGN(reply, decode_task_created(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<PopulationCreatedMessage> create_population(
    const af_test::TestContext& context, ProtocolClient& client, const PopulationSpec& spec) {
  CreatePopulationMessage message;
  message.session = controller_session(client.ack());
  message.spec = spec;
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::CreatePopulation, std::move(payload),
                                       {MessageType::PopulationCreated}));
  PopulationCreatedMessage reply;
  AF_TRY_ASSIGN(reply, decode_population_created(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<PopulationStateMessage> start_population(
    const af_test::TestContext& context, ProtocolClient& client,
    const PopulationBinding& binding) {
  StartPopulationMessage message;
  message.authority = operation_authority(client.ack(), binding);
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::StartPopulation, std::move(payload),
                                       {MessageType::PopulationStarted}));
  PopulationStateMessage reply;
  AF_TRY_ASSIGN(reply, decode_population_state(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<SelectionDecisionMessage> request_selection(
    const af_test::TestContext& context, ProtocolClient& client,
    const PopulationBinding& binding) {
  RequestSelectionMessage message;
  message.authority = operation_authority(client.ack(), binding);
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::RequestSelection, std::move(payload),
                                       {MessageType::SelectionDecisionMessage}));
  SelectionDecisionMessage reply;
  AF_TRY_ASSIGN(reply, decode_selection_decision(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<RetentionDecisionMessage> request_retention(
    const af_test::TestContext& context, ProtocolClient& client,
    const PopulationBinding& binding) {
  RequestRetentionMessage message;
  message.authority = operation_authority(client.ack(), binding);
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::RequestRetention, std::move(payload),
                                       {MessageType::RetentionDecisionMessage}));
  RetentionDecisionMessage reply;
  AF_TRY_ASSIGN(reply, decode_retention_decision(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<RevalidationAppliedMessage> request_revalidation(
    const af_test::TestContext& context, ProtocolClient& client,
    const PopulationBinding& binding, std::string reason) {
  RequestRevalidationMessage message;
  message.authority = operation_authority(client.ack(), binding);
  message.reason = std::move(reason);
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::RequestRevalidation,
                                       std::move(payload),
                                       {MessageType::RevalidationApplied}));
  RevalidationAppliedMessage reply;
  AF_TRY_ASSIGN(reply, decode_revalidation_applied(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<PopulationDetailMessage> query_population(
    const af_test::TestContext& context, ProtocolClient& client, PopulationId population) {
  QueryPopulationMessage message;
  message.session = controller_session(client.ack());
  message.population = population;
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::QueryPopulation, std::move(payload),
                                       {MessageType::PopulationDetail}));
  PopulationDetailMessage reply;
  AF_TRY_ASSIGN(reply, decode_population_detail(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<CandidateDetailMessage> query_candidate(
    const af_test::TestContext& context, ProtocolClient& client, CandidateId candidate) {
  QueryCandidateMessage message;
  message.session = controller_session(client.ack());
  message.candidate = candidate;
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::QueryCandidate, std::move(payload),
                                       {MessageType::CandidateDetail}));
  CandidateDetailMessage reply;
  AF_TRY_ASSIGN(reply, decode_candidate_detail(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<StatisticsDetailMessage> query_statistics(
    const af_test::TestContext& context, ProtocolClient& client) {
  QueryStatisticsMessage message;
  message.session = controller_session(client.ack());
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::QueryStatistics, std::move(payload),
                                       {MessageType::StatisticsDetail}));
  StatisticsDetailMessage reply;
  AF_TRY_ASSIGN(reply, decode_statistics_detail(frame.payload));
  return reply;
}

/// Ask the coordinator to shut down and wait for the acknowledgement. The
/// acknowledgement is queued before the coordinator begins stopping, so the
/// reply arriving is the proof that the control-plane shutdown was accepted.
[[nodiscard]] inline Result<ControlAckMessage> shutdown_coordinator(
    const af_test::TestContext& context, ProtocolClient& client) {
  ShutdownCoordinatorMessage message;
  message.session = controller_session(client.ack());
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::ShutdownCoordinator,
                                       std::move(payload), {MessageType::ShutdownAck}));
  ControlAckMessage reply;
  AF_TRY_ASSIGN(reply, decode_control_ack(frame.payload));
  return reply;
}

// ---------------------------------------------------------------------------
// Worker-plane messages
// ---------------------------------------------------------------------------

[[nodiscard]] inline Result<ControlAckMessage> worker_ready(const af_test::TestContext& context,
                                                            ProtocolClient& client,
                                                            const WorkerSessionAuthority& session,
                                                            std::string capability) {
  WorkerReadyMessage message;
  message.session = session;
  message.capability = std::move(capability);
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::WorkerReady, std::move(payload),
                                       {MessageType::ControlAck}));
  ControlAckMessage reply;
  AF_TRY_ASSIGN(reply, decode_control_ack(frame.payload));
  return reply;
}

[[nodiscard]] inline Result<RevalidateAckMessage> revalidate_worker(
    const af_test::TestContext& context, ProtocolClient& client,
    const WorkerSessionAuthority& session, std::string detail) {
  RevalidateMessage message;
  message.session = session;
  message.detail = std::move(detail);
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::Revalidate, std::move(payload),
                                       {MessageType::RevalidateAck}));
  RevalidateAckMessage reply;
  AF_TRY_ASSIGN(reply, decode_revalidate_ack(frame.payload));
  return reply;
}

/// Send an AttemptAccepted carrying an explicit operation authority, so a case
/// can present authority the coordinator must refuse.
[[nodiscard]] inline Result<ControlAckMessage> acknowledge_attempt(
    const af_test::TestContext& context, ProtocolClient& client,
    const WorkerOperationAuthority& authority) {
  AttemptAcceptedMessage message;
  message.authority = authority;
  std::string payload;
  AF_TRY_ASSIGN(payload, encode_payload(message));
  Frame frame;
  AF_TRY_ASSIGN(frame, client.exchange(context, MessageType::AttemptAccepted, std::move(payload),
                                       {MessageType::ControlAck}));
  ControlAckMessage reply;
  AF_TRY_ASSIGN(reply, decode_control_ack(frame.payload));
  return reply;
}

// ---------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------

/// Loop until the predicate holds. Every predicate in these suites performs a
/// real request/response round trip, so the loop is paced by the coordinator's
/// own progress and by the network. There is no deadline and no iteration cap:
/// a case that never satisfies its predicate is localised by the PHASE marker
/// the caller emitted immediately before entering, which is exactly what the
/// harness contract requires.
template <typename Predicate>
inline void await_true(const af_test::TestContext& context, std::string_view phase,
                       const Predicate& predicate) {
  context.phase(phase);
  while (!predicate()) {
  }
}

// ---------------------------------------------------------------------------
// Process launching
// ---------------------------------------------------------------------------

[[nodiscard]] inline Result<std::uint16_t> reserve_free_port() {
  AF_TRY(SocketSubsystem::ensure_initialized());
  Endpoint endpoint;
  endpoint.host = "127.0.0.1";
  endpoint.port = 0;
  TcpListener listener;
  AF_TRY_ASSIGN(listener, TcpListener::bind(endpoint));
  const std::uint16_t port = listener.port();
  listener.close();
  return port;
}

[[nodiscard]] inline Status spawn_coordinator(ChildProcess& child,
                                              const std::filesystem::path& executable,
                                              std::uint16_t port,
                                              const std::filesystem::path& state_path,
                                              const std::filesystem::path& workspace_root,
                                              bool fresh_start,
                                              std::uint32_t evaluation_concurrency) {
  ChildProcessOptions options;
  options.executable = executable;
  options.arguments = {"--port",
                       std::to_string(static_cast<unsigned>(port)),
                       "--state",
                       state_path.string(),
                       "--workspace",
                       workspace_root.string(),
                       "--evaluation-concurrency",
                       std::to_string(evaluation_concurrency)};
  if (fresh_start) {
    options.arguments.push_back("--fresh");
  }
  options.inherit_environment = true;
  options.capture_output = false;
  return child.start(std::move(options));
}

[[nodiscard]] inline Status spawn_worker(ChildProcess& child,
                                         const std::filesystem::path& executable,
                                         std::uint16_t port, WorkerId worker,
                                         const std::filesystem::path& workspace_root,
                                         std::string_view strategy, std::string_view label) {
  ChildProcessOptions options;
  options.executable = executable;
  options.arguments = {"--coordinator",
                       "127.0.0.1:" + std::to_string(static_cast<unsigned>(port)),
                       "--worker-id",
                       std::to_string(worker.raw()),
                       "--strategy",
                       std::string(strategy),
                       "--workspace",
                       workspace_root.string(),
                       "--label",
                       std::string(label)};
  options.inherit_environment = true;
  options.capture_output = false;
  return child.start(std::move(options));
}

/// Connect to a coordinator that may still be binding. The loop ends when the
/// connection is accepted; it fails the case immediately when the child has
/// already exited, so a coordinator that could not start is reported with its
/// own log instead of parking the case in a wait it can never leave.
inline void connect_client(const af_test::TestContext& context, ProtocolClient& client,
                           const ChildProcess& child, std::uint16_t port, std::string_view label) {
  context.phase("CONNECT");
  for (;;) {
    Endpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = port;
    const Status connected = client.connect(endpoint);
    if (connected.ok()) {
      return;
    }
    if (!child.running()) {
      context.fail_at(__FILE__, __LINE__,
                      std::string(label) + " exited before accepting a connection; log:\n" +
                          child.log());
    }
  }
}

// ---------------------------------------------------------------------------
// Task and policy construction
// ---------------------------------------------------------------------------

/// Identity that is valid for the wire but is never used as a durable name: the
/// coordinator allocates the real identity and the case reads it back from the
/// reply. make_reference_task() insists on a valid policy identity, so one is
/// minted here purely to satisfy the builder.
[[nodiscard]] inline PolicyId placeholder_policy_id(std::uint32_t salt) {
  return PolicyId::from_raw(identity_raw(IdKind::Policy, salt, 1));
}

[[nodiscard]] inline TaskId placeholder_task_id(std::uint32_t salt) {
  return TaskId::from_raw(identity_raw(IdKind::Task, salt, 1));
}

[[nodiscard]] inline WorkerId worker_identity(std::uint32_t salt, std::uint32_t counter) {
  return WorkerId::from_raw(identity_raw(IdKind::Worker, salt, counter));
}

[[nodiscard]] inline WorkerBootId boot_identity(std::uint32_t salt, std::uint32_t counter) {
  return WorkerBootId::from_raw(
      identity_raw(IdKind::WorkerBoot, salt, counter * 0x0102u + 7u));
}

/// The reference task contract (two mandatory gates, two optional factors),
/// carrying valid wire identities. The protocol codec requires a non-null task
/// and policy identity on every frame that names one, and the coordinator adopts
/// the identities a controller supplies rather than allocating its own.
[[nodiscard]] inline Result<TaskSpec> reference_task(std::uint32_t salt, PolicyId policy,
                                                     PolicyGeneration policy_generation,
                                                     std::uint32_t candidate_budget) {
  TaskSpec task;
  AF_TRY_ASSIGN(task, make_reference_task(placeholder_task_id(salt), policy, policy_generation,
                                          candidate_budget, BudgetLimits{}));
  return task;
}

/// Append deterministic input files to a task.
///
/// A worker writes every declared input file into its attempt workspace before
/// it publishes. Giving the task a measurable amount of input bytes makes that
/// window long enough for a case to observe an in-flight attempt over the wire
/// and then kill the process holding it, without relying on a coincidence of
/// scheduling. The bytes are ordinary task inputs; nothing about the runtime's
/// semantics changes.
inline void add_bulk_inputs(TaskSpec& task, std::size_t file_count, std::size_t bytes_per_file) {
  for (std::size_t index = 0; index < file_count; ++index) {
    InputFile file;
    file.name = "bulk-input-" + std::to_string(index) + ".txt";
    std::string content;
    content.reserve(bytes_per_file);
    const char seed = static_cast<char>('a' + static_cast<int>(index % 26));
    for (std::size_t offset = 0; offset < bytes_per_file; ++offset) {
      content.push_back((offset % 64) == 63 ? '\n' : seed);
    }
    file.content_digest = sha256_hex(content);
    file.content = std::move(content);
    task.inputs.push_back(std::move(file));
  }
}

// ---------------------------------------------------------------------------
// Small predicates used by more than one suite
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool candidate_state_at_least_published(CandidateState state) {
  switch (state) {
    case CandidateState::Published:
    case CandidateState::Evaluating:
    case CandidateState::Evaluated:
    case CandidateState::RevalidationRequired:
    case CandidateState::Selected:
    case CandidateState::Retained:
    case CandidateState::Disqualified:
      return true;
    default:
      return false;
  }
}

/// Wait-predicate accessors. A request that fails is a defect in the case, not
/// a reason to keep waiting, so these fail the case with the coordinator's own
/// error code instead of silently spinning forever.
[[nodiscard]] inline PopulationRecord population_of(const af_test::TestContext& context,
                                                    ProtocolClient& client,
                                                    PopulationId population) {
  const Result<PopulationDetailMessage> reply = query_population(context, client, population);
  return af_test::require_value(context, reply, "query_population", __FILE__, __LINE__).population;
}

[[nodiscard]] inline CandidateRecord candidate_of(const af_test::TestContext& context,
                                                  ProtocolClient& client,
                                                  CandidateId candidate) {
  const Result<CandidateDetailMessage> reply = query_candidate(context, client, candidate);
  return af_test::require_value(context, reply, "query_candidate", __FILE__, __LINE__).candidate;
}

[[nodiscard]] inline FoundryStatistics statistics_of(const af_test::TestContext& context,
                                                     ProtocolClient& client) {
  const Result<StatisticsDetailMessage> reply = query_statistics(context, client);
  return af_test::require_value(context, reply, "query_statistics", __FILE__, __LINE__).statistics;
}

/// The candidate ids a population currently holds, in the order the population
/// lists them.
[[nodiscard]] inline Result<std::vector<CandidateId>> population_candidates(
    const af_test::TestContext& context, ProtocolClient& client, PopulationId population) {
  PopulationDetailMessage snapshot;
  AF_TRY_ASSIGN(snapshot, query_population(context, client, population));
  return snapshot.population.candidates;
}

/// Candidate ids, failing the case when the request itself fails.
[[nodiscard]] inline std::vector<CandidateId> candidates_of(const af_test::TestContext& context,
                                                            ProtocolClient& client,
                                                            PopulationId population) {
  const Result<std::vector<CandidateId>> reply =
      population_candidates(context, client, population);
  return af_test::require_value(context, reply, "population_candidates", __FILE__, __LINE__);
}

}  // namespace af_dist
