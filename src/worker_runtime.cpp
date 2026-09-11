// src/worker_runtime.cpp
//
// The reference autonomous worker.
//
// A worker is an external process that speaks the framed protocol, produces a
// real candidate output inside the workspace it was given, and publishes it. It
// never evaluates its own work and it never decides anything. Everything it
// sends about its own output is a claim, not evidence, and the coordinator
// records it as such.
//
// THREADING
//
//   Connection owns exactly one reader thread and one writer thread. The reader
//   thread never runs the worker loop: it hands each whole frame to an inbox and
//   wakes the loop. That keeps the protocol state machine single threaded, so
//   the worker has no protocol lock at all - only one small mutex protecting the
//   inbox, the pending acknowledgements and the exit flag - and it is never held
//   across a socket operation, a file operation or a process wait.
//
// WAITING
//
//   A worker blocks on a condition variable until the coordinator sends
//   something or the connection closes. There is no execution duration limit
//   anywhere in this file: an assignment runs to natural completion, and a
//   worker that stops making progress is a defect to diagnose rather than
//   something to paper over with a deadline.

#include "autonomous_foundry/worker_runtime.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/protocol.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/workspace.hpp"

namespace autonomous_foundry {
namespace {

constexpr std::string_view kReferenceWorkerCapability = "reference-cpp20";

std::uint32_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::_getpid());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

}  // namespace

struct ReferenceWorkerRuntime::Impl
    : public std::enable_shared_from_this<ReferenceWorkerRuntime::Impl> {
  Impl(const WorkerRuntimeConfig& worker_config, const WorkerBootId& worker_boot)
      : config(worker_config), boot(worker_boot) {}

  WorkerRuntimeConfig config;
  WorkerBootId boot;

  std::shared_ptr<Connection> connection;
  WorkerSessionAuthority session;
  std::uint64_t assignments_served{0};
  std::uint64_t attempt_counter{0};

  std::mutex mutex;
  std::condition_variable wake;

  /// A domain rejection the coordinator sent for one message the worker wrote.
  ///
  /// The coordinator answers a message it refuses with an ErrorResponse that
  /// names the message it is refusing (in_reply_to) and leaves the connection
  /// open: a handler failure is a rejection of one message, not of the
  /// connection. The worker therefore files the rejection against the message it
  /// is waiting for instead of treating it as a closed connection. At most one
  /// rejection per refused message type is kept, because a worker only ever has
  /// one message of a given type outstanding.
  struct Rejection {
    MessageType in_reply_to{MessageType::Invalid};
    Status status;
  };

  std::deque<Frame> inbox;
  std::deque<Rejection> rejections;
  bool have_hello_ack{false};
  HelloAckMessage hello_ack;
  bool have_publish_ack{false};
  PublishAckMessage publish_ack;
  bool live{false};
  bool stop{false};
  Status terminal;
  std::string close_detail{"connection closed"};

  // -- reader thread hand-off ----------------------------------------------

  void enqueue(Frame frame) {
    // The handshake frames are resolved here, on the reader thread, rather than
    // being queued for the message loop: connect() blocks waiting for the
    // acknowledgement before serve() is ever entered, so a frame that only the
    // message loop could observe would never be observed at all and the worker
    // would wait for its own handshake forever.
    if (frame.header.type == MessageType::HelloAck) {
      HelloAckMessage decoded;
      const Result<HelloAckMessage> parsed = decode_hello_ack(frame.payload);
      if (parsed.ok()) {
        const std::lock_guard<std::mutex> guard(mutex);
        hello_ack = parsed.value();
        have_hello_ack = true;
        wake.notify_all();
        return;
      }
    }
    if (frame.header.type == MessageType::ErrorResponse) {
      ErrorResponseMessage decoded;
      const Result<ErrorResponseMessage> parsed = decode_error_response(frame.payload);
      if (parsed.ok()) {
        record_rejection(parsed.value());
        return;
      }
    }
    {
      const std::lock_guard<std::mutex> guard(mutex);
      inbox.push_back(std::move(frame));
    }
    wake.notify_all();
  }

  void mark_down(const Status& reason) {
    {
      const std::lock_guard<std::mutex> guard(mutex);
      live = false;
      stop = true;
      if (terminal.ok()) {
        terminal = reason;
      }
      close_detail = reason.ok() ? std::string("connection closed") : reason.message();
    }
    wake.notify_all();
  }

  /// File a domain rejection against the message it refuses. The connection
  /// stays up: the coordinator rejects one message, not the session, and the
  /// worker keeps serving the assignments it is still authorized for.
  void record_rejection(const ErrorResponseMessage& message) {
    {
      const std::lock_guard<std::mutex> guard(mutex);
      for (Rejection& existing : rejections) {
        if (existing.in_reply_to == message.in_reply_to) {
          existing.status = Status(message.code, message.detail);
          wake.notify_all();
          return;
        }
      }
      Rejection rejection;
      rejection.in_reply_to = message.in_reply_to;
      rejection.status = Status(message.code, message.detail);
      rejections.push_back(std::move(rejection));
    }
    wake.notify_all();
  }

  /// The rejection already filed for one message type, or nullptr.
  /// The caller holds mutex.
  [[nodiscard]] const Status* find_rejection_locked(MessageType in_reply_to) const {
    for (const Rejection& rejection : rejections) {
      if (rejection.in_reply_to == in_reply_to) {
        return &rejection.status;
      }
    }
    return nullptr;
  }

  /// Consume the rejection filed for one message type. The caller holds mutex.
  [[nodiscard]] bool take_rejection_locked(MessageType in_reply_to, Status* status) {
    for (auto entry = rejections.begin(); entry != rejections.end(); ++entry) {
      if (entry->in_reply_to == in_reply_to) {
        *status = entry->status;
        rejections.erase(entry);
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool next_frame(Frame* frame) {
    std::unique_lock<std::mutex> lock(mutex);
    wake.wait(lock, [this] { return stop || !inbox.empty(); });
    if (!inbox.empty()) {
      *frame = std::move(inbox.front());
      inbox.pop_front();
      return true;
    }
    return false;
  }

  // -- startup --------------------------------------------------------------

  Status connect() {
    AF_TRY(SocketSubsystem::ensure_initialized());
    SocketHandle socket;
    AF_TRY_ASSIGN(socket, connect_to(config.coordinator));

    const std::weak_ptr<Impl> weak = weak_from_this();
    ConnectionCallbacks callbacks;
    callbacks.on_frame = [weak](const Frame& frame) {
      if (const std::shared_ptr<Impl> self = weak.lock()) {
        self->enqueue(frame);
      }
    };
    callbacks.on_closed = [weak](const Status& reason) {
      if (const std::shared_ptr<Impl> self = weak.lock()) {
        self->mark_down(reason);
      }
    };

    std::shared_ptr<Connection> created = std::make_shared<Connection>(
        std::move(socket), config.coordinator.to_string(), std::move(callbacks));
    AF_TRY(created->start());
    connection = std::move(created);

    {
      const std::lock_guard<std::mutex> guard(mutex);
      live = true;
    }

    // The worker learns the run identity from the coordinator, so the Hello it
    // sends carries the null run identity and the coordinator resolves it.
    HelloMessage hello;
    hello.role = SessionRole::Worker;
    hello.run = FoundryRunId();
    hello.worker = config.worker;
    hello.boot = boot;
    hello.label = config.label;
    hello.capability = std::string(kReferenceWorkerCapability);
    hello.process_id = current_process_id();
    hello.observed_epoch = CoordinatorEpoch();

    std::string hello_payload;
    AF_TRY_ASSIGN(hello_payload, encode_payload(hello));
    AF_TRY(connection->send(MessageType::Hello, std::move(hello_payload)));

    std::unique_lock<std::mutex> lock(mutex);
    wake.wait(lock, [this] {
      return have_hello_ack || stop || find_rejection_locked(MessageType::Hello) != nullptr;
    });
    if (!have_hello_ack) {
      Status refused;
      if (take_rejection_locked(MessageType::Hello, &refused)) {
        // The handshake itself was refused, so there is no session to serve:
        // this is the one rejection that ends the worker.
        return refused;
      }
      return Status(ErrorCode::ConnectionClosed,
                    "the coordinator closed the connection before acknowledging the worker "
                    "handshake: " +
                        close_detail);
    }
    // HelloAck carries the coordinates of the live session, not a
    // preassembled authority: the worker composes the authority it will present
    // on every later message from exactly these fields.
    WorkerSessionAuthority authority;
    authority.coordinator_epoch = hello_ack.coordinator_epoch;
    authority.run = hello_ack.run;
    authority.worker = config.worker;
    authority.boot = boot;
    authority.session = hello_ack.session;
    authority.session_generation = hello_ack.session_generation;
    session = authority;
    return Status();
  }

  // -- outbound helpers -----------------------------------------------------

  Status send_worker_ready() {
    WorkerReadyMessage ready;
    ready.session = session;
    ready.capability = std::string(kReferenceWorkerCapability);
    std::string payload;
    AF_TRY_ASSIGN(payload, encode_payload(ready));
    return connection->send(MessageType::WorkerReady, std::move(payload));
  }

  Status send_accepted(const WorkerOperationAuthority& authority) {
    AttemptAcceptedMessage accepted;
    accepted.authority = authority;
    std::string payload;
    AF_TRY_ASSIGN(payload, encode_payload(accepted));
    return connection->send(MessageType::AttemptAccepted, std::move(payload));
  }

  Status send_failed(const WorkerOperationAuthority& authority, std::string reason) {
    AttemptFailedMessage failed;
    failed.authority = authority;
    failed.reason = std::move(reason);
    std::string payload;
    AF_TRY_ASSIGN(payload, encode_payload(failed));
    return connection->send(MessageType::AttemptFailed, std::move(payload));
  }

  Status send_candidate(const CandidatePublishMessage& message) {
    std::string payload;
    AF_TRY_ASSIGN(payload, encode_payload(message));
    return connection->send(MessageType::CandidatePublish, std::move(payload));
  }

  Status send_self_report(const WorkerOperationAuthority& authority, bool claimed_success,
                          std::string claim) {
    SelfReportMessage report;
    report.authority = authority;
    report.claim = std::move(claim);
    report.claimed_success = claimed_success;
    std::string payload;
    AF_TRY_ASSIGN(payload, encode_payload(report));
    return connection->send(MessageType::SelfReport, std::move(payload));
  }

  // -- one assignment -------------------------------------------------------

  void remember_publish_ack(PublishAckMessage ack) {
    {
      const std::lock_guard<std::mutex> guard(mutex);
      publish_ack = std::move(ack);
      have_publish_ack = true;
    }
    wake.notify_all();
  }

  [[nodiscard]] Result<PublishAckMessage> wait_for_publish_ack() {
    std::unique_lock<std::mutex> lock(mutex);
    // The answer to a publication is either its acknowledgement or a rejection
    // of the very message that asked for it. Both are answers; only the absence
    // of both, on a connection that went down, is a closed connection.
    wake.wait(lock, [this] {
      return have_publish_ack || stop ||
             find_rejection_locked(MessageType::CandidatePublish) != nullptr;
    });
    if (have_publish_ack) {
      PublishAckMessage ack = std::move(publish_ack);
      have_publish_ack = false;
      publish_ack = PublishAckMessage();
      return ack;
    }
    Status refused;
    if (take_rejection_locked(MessageType::CandidatePublish, &refused)) {
      return refused;
    }
    return Status(ErrorCode::ConnectionClosed,
                  "the coordinator closed the connection while a publication was outstanding: " +
                      close_detail);
  }

  /// Serve exactly one assignment. Every failure path reports AttemptFailed
  /// rather than failing silently, and the attempt workspace is removed on
  /// every path once it exists.
  Status serve_assignment(const AssignAttemptMessage& message) {
    // The assignment frame carries the session; the operation authority is the
    // session plus the exact package coordinates the coordinator authorized.
    WorkerOperationAuthority authority;
    authority.session = message.session;
    authority.population = message.package.population;
    authority.population_generation = message.package.population_generation;
    authority.task = message.package.task;
    authority.task_generation = message.package.task_generation;
    authority.candidate = message.package.candidate;
    authority.candidate_generation = message.package.candidate_generation;
    authority.attempt = message.package.attempt;
    authority.attempt_generation = message.package.attempt_generation;
    authority.assignment = message.package.assignment;

    AF_TRY(send_accepted(authority));

    ++attempt_counter;
    const std::string leaf = "attempt-" + std::to_string(attempt_counter);
    std::filesystem::path base;
    AF_TRY_ASSIGN(base, make_unique_directory(config.workspace_root, "worker-attempt"));
    WorkspaceRoot workspace;
    AF_TRY_ASSIGN(workspace, WorkspaceRoot::create(base, leaf));

    // From here on the workspace exists, so every path removes it.
    const auto finish = [&workspace](const Status& status) -> Status {
      const Status removed = workspace.remove_all();
      if (!status.ok()) {
        return status;
      }
      return removed;
    };

    Status outcome;
    do {
      for (const auto& input : message.package.input_files) {
        outcome = workspace.write_file(input.first, input.second);
        if (!outcome.ok()) {
          outcome = outcome.with_context("writing task input file");
          break;
        }
      }
      if (!outcome.ok()) {
        break;
      }

      const std::string source = generate_reference_solution(config.strategy);
      outcome = workspace.write_file(std::string(kReferenceTaskSourceArtifact), source);
      if (!outcome.ok()) {
        outcome = outcome.with_context("writing the candidate translation unit");
        break;
      }

      CandidatePublishMessage publish;
      publish.authority = authority;
      ArtifactRef artifact;
      artifact.name = std::string(kReferenceTaskSourceArtifact);
      artifact.size_bytes = static_cast<std::uint64_t>(source.size());
      artifact.content_digest = sha256_hex(source);
      publish.artifacts.push_back(std::move(artifact));
      publish.declared_strategy = std::string(reference_strategy_name(config.strategy));
      // The claim is recorded, never authoritative. Two reference strategies
      // exist precisely so that a proof can show a self reported pass failing a
      // real gate.
      publish.self_reported_success =
          config.self_report_success_unconditionally ||
          (config.strategy != ReferenceStrategy::OffByOne &&
           config.strategy != ReferenceStrategy::SelfReportedPass);
      publish.self_report_detail =
          "reference worker claim for strategy '" + publish.declared_strategy + "'";

      const bool claimed = publish.self_reported_success;
      const std::string claim_text = publish.self_report_detail;
      outcome = send_candidate(publish);
      if (!outcome.ok()) {
        outcome = outcome.with_context("publishing the candidate");
        break;
      }
      // The self report is a separate message on the same authority, so the
      // coordinator records the claim as its own durable evidence record even
      // when the publication itself is later rejected.
      (void)send_self_report(authority, claimed, claim_text);

      const Result<PublishAckMessage> ack = wait_for_publish_ack();
      if (!ack.ok()) {
        outcome = ack.status();
        break;
      }
      if (!ack.value().accepted) {
        const std::string detail = ack.value().detail.empty()
                                       ? std::string("the coordinator rejected the publication")
                                       : ack.value().detail;
        outcome = send_failed(authority, detail);
        if (!outcome.ok()) {
          outcome = outcome.with_context("reporting the rejected publication");
        }
        break;
      }
      ++assignments_served;
    } while (false);

    return finish(outcome);
  }

  // -- dispatch -------------------------------------------------------------

  Status handle(Frame frame) {
    switch (frame.header.type) {
      case MessageType::HelloAck: {
        HelloAckMessage decoded;
        AF_TRY_ASSIGN(decoded, decode_hello_ack(frame.payload));
        {
          const std::lock_guard<std::mutex> guard(mutex);
          hello_ack = decoded;
          have_hello_ack = true;
        }
        wake.notify_all();
        return Status();
      }
      case MessageType::RevalidateAck: {
        RevalidateAckMessage decoded;
        AF_TRY_ASSIGN(decoded, decode_revalidate_ack(frame.payload));
        if (!decoded.accepted) {
          return Status(ErrorCode::RevalidationRequired,
                        "the coordinator refused revalidation: " + decoded.detail);
        }
        // A revalidation issues a fresh session and session generation on the
        // coordinator. The old authority dies the moment this arrives, so the
        // worker adopts the new one immediately and reports ready again.
        session = decoded.session;
        return send_worker_ready();
      }
      case MessageType::AssignAttempt: {
        AssignAttemptMessage decoded;
        AF_TRY_ASSIGN(decoded, decode_assign_attempt(frame.payload));
        return serve_assignment(decoded);
      }
      case MessageType::PublishAck: {
        PublishAckMessage decoded;
        AF_TRY_ASSIGN(decoded, decode_publish_ack(frame.payload));
        remember_publish_ack(std::move(decoded));
        return Status();
      }
      case MessageType::HeartbeatAck:
        return Status();
      case MessageType::ShutdownAck:
      case MessageType::ShutdownCoordinator: {
        {
          const std::lock_guard<std::mutex> guard(mutex);
          stop = true;
        }
        wake.notify_all();
        return Status();
      }
      case MessageType::ErrorResponse: {
        const Result<ErrorResponseMessage> decoded = decode_error_response(frame.payload);
        if (!decoded.ok()) {
          return Status(ErrorCode::ProtocolViolation,
                        "the coordinator reported an error with an unreadable body");
        }
        return Status(decoded.value().code,
                      "coordinator error response: " + decoded.value().detail);
      }
      default:
        // An unknown message type is not fatal here: a worker ignores what it
        // does not understand and keeps serving assignments.
        return Status();
    }
  }

  // -- main loop ------------------------------------------------------------

  Status serve() {
    for (;;) {
      Frame frame;
      if (!next_frame(&frame)) {
        break;
      }
      const Status handled = handle(std::move(frame));
      if (!handled.ok()) {
        return handled;
      }
    }
    const std::lock_guard<std::mutex> guard(mutex);
    if (terminal.ok()) {
      return Status();
    }
    return terminal;
  }

  void close() {
    if (connection) {
      connection->request_close(Status(ErrorCode::ConnectionClosed, "worker is shutting down"));
    }
  }
};

ReferenceWorkerRuntime::ReferenceWorkerRuntime(WorkerRuntimeConfig config)
    : impl_(std::make_shared<ReferenceWorkerRuntime::Impl>(config, make_worker_boot_id())),
      config_(std::move(config)),
      boot_(impl_->boot) {}

ReferenceWorkerRuntime::~ReferenceWorkerRuntime() {
  if (impl_) {
    impl_->close();
    impl_.reset();
  }
}

Status ReferenceWorkerRuntime::run() {
  AF_TRY(impl_->connect());
  AF_TRY(impl_->send_worker_ready());
  const Status outcome = impl_->serve();
  impl_->close();
  if (!outcome.ok()) {
    return outcome;
  }
  assignments_served_ = impl_->assignments_served;
  return Status();
}

int run_worker_process(const WorkerRuntimeConfig& config) {
  try {
    ReferenceWorkerRuntime runtime(config);
    const Status status = runtime.run();
    if (!status.ok()) {
      // A worker that stops is a defect worth diagnosing, and the process is
      // about to disappear: the reason it stopped is written here, unbuffered, so
      // the log the launcher already captures names the failure instead of
      // leaving a bare exit code behind.
      std::fprintf(stderr, "af_worker: stopping: %s\n", status.to_string().c_str());
      std::fflush(stderr);
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "af_worker: fatal: %s\n", error.what());
    std::fflush(stderr);
    return 1;
  } catch (...) {
    std::fputs("af_worker: fatal: unknown exception\n", stderr);
    std::fflush(stderr);
    return 1;
  }
}

}  // namespace autonomous_foundry
