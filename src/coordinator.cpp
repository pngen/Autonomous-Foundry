// src/coordinator.cpp
//
// The coordinator runtime.
//
// One coordinator process owns durable foundry state. It accepts framed TCP
// connections from workers and controllers, drives the foundry state machine,
// runs evaluations on a bounded pool, and persists at every durable boundary
// before it tells anyone about it.
//
// ===========================================================================
// THE ORDERING RULE
// ===========================================================================
//
//     mutate -> persist -> notify
//
// Every durable transition is applied to FoundryCore, then written to the
// snapshot store, and only then announced on a socket. Before AssignAttempt is
// sent, the Dispatched attempt is on disk. Before PublishAck is sent, the
// publication is on disk. A coordinator that dies between the mutation and the
// notification therefore leaves a state that reads as "this happened and nobody
// was told", which is exactly the state a recovery has to be able to explain.
// The opposite order - tell first, write later - would leave workers acting on
// authority no successor coordinator can see.
//
// ===========================================================================
// CONCURRENCY CONTRACT
// ===========================================================================
//
// There are exactly five locks in this file, and no lock is ever held across a
// socket operation, a process wait, an evaluation or a join.
//
//   1. FoundryCore's own shared_mutex. Held only inside core method calls. It
//      is the single state lock and this file never takes it directly.
//
//   2. registry_mutex. Guards the connection registry: which connections exist,
//      what session each worker connection registered, and the per connection
//      "current attempt authority" side table. A frame handler takes it to read
//      or update one connection's bookkeeping and releases it before calling
//      into the core.
//
//   3. artifact_mutex. Guards the bounded in-memory artifact cache that lets an
//      evaluator be handed artifact bytes.
//
//   4. pool_mutex. Guards the evaluation job queue and its wake condition. A
//      pool thread holds it only to take one job or to observe the drain flag.
//
//   5. pump_mutex. Guards the pump thread's wake condition. It is not a data
//      lock: the pump reads its inputs from FoundryCore and the registry under
//      their own locks.
//
// Lock order, when more than one is needed: registry_mutex -> (nothing else).
// Core calls are always made with no other lock held. artifact_mutex, pool_mutex
// and pump_mutex are leaves. That is what makes the arrangement auditable.
//
// ===========================================================================
// THE ARTIFACT CACHE IS A REFERENCE-DEPLOYMENT CONVENIENCE
// ===========================================================================
//
// A worker publishes artifact references - name, size, digest - not artifact
// bytes, while the evaluator contract takes bytes. This coordinator therefore
// keeps a bounded in-memory map from candidate identity to artifact name to
// content, filled when a CandidatePublish arrives, bounded per candidate by
// kMaxArtifactsPerCandidate and kMaxArtifactBytes, and dropped as soon as the
// candidate's last evaluation record is written.
//
// That cache is NOT durable artifact storage, is not shared between
// coordinators, and does not survive a restart. Durable artifact storage belongs
// to an adjacent system. What this file does guarantee is narrower and honest:
// an evaluator either receives the exact bytes the worker published, or it
// reports UNSUPPORTED. It is never handed a substitute, and a missing evaluator
// is never reported as a pass.
//
// ===========================================================================
// WHAT RUNS WHERE
// ===========================================================================
//
//   acceptor thread   : blocks in TcpListener::accept, hands each socket to a
//                       Connection, never touches foundry state.
//   connection reader : runs handle_frame for one connection. It decodes, calls
//     thread            the core, and enqueues replies. It never blocks.
//   connection writer : owns the socket for sends. Exactly one writer thread
//     thread            per connection, so two coordinator threads can never
//                       interleave partial frames.
//   pump thread       : starts evaluations and authorizes dispatches.
//   pool threads      : run evaluations off the pump thread and off the state
//                       lock, and each writes its records under the state lock.
//
// There is no wall-clock deadline anywhere in this file. The pump's condition
// variable wait uses a 20 ms scheduling tick so the pump re-examines state a
// connection thread changed; it bounds nothing but the pump's own wakeup, and no
// evaluation, connection or test is ever cut short by it.

#include "autonomous_foundry/coordinator.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/evaluator.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/workspace.hpp"
#include "local_promotion_sink.hpp"

namespace autonomous_foundry {
namespace {

/// Pump scheduling tick. This is how often the pump re-examines state a
/// connection thread may have changed. It is not a deadline for anything.
constexpr std::chrono::milliseconds kPumpTick{20};

/// A connection reader thread must never be able to take the whole coordinator
/// down: an exception that escapes per-connection work is reported here and
/// contained, so only that connection is dropped and the durable state stays as
/// the last accepted message left it. The report goes to stdout because that is
/// the operator-visible log, and a fault must survive the failing thread.
void af_report_connection_fault(const std::uint64_t connection_id, const char* phase,
                                const char* detail) {
  std::printf("af_coordinator: connection %llu failed while handling %s: %s\n",
              static_cast<unsigned long long>(connection_id), phase,
              detail == nullptr ? "unknown exception" : detail);
  std::fflush(stdout);
}

/// Identity of the controller authority the coordinator issues for its own
/// operator surface. Derived from the durable foundry identity, so it is stable
/// across a restart and never collides with an operator's own identity.
[[nodiscard]] ControllerId reference_controller_id(FoundryId foundry) {
  std::uint64_t counter =
      (fnv1a64("autonomous-foundry.controller") ^ foundry.raw()) & detail::kIdCounterMask;
  if (counter == 0) {
    counter = 1;
  }
  const std::uint64_t salt = (foundry.raw() >> detail::kIdSaltShift) & detail::kIdSaltMask;
  return StrongId<ControllerIdTag>::from_raw(
      (static_cast<std::uint64_t>(IdKind::Controller) << detail::kIdKindShift) |
      (salt << detail::kIdSaltShift) | counter);
}

/// Identity of a named evaluator. Deterministic: one evaluator key always
/// produces one identity, so a candidate's record set is stable for a given
/// foundry identity.
[[nodiscard]] EvaluatorId evaluator_identity(std::string_view key, std::uint32_t salt) {
  const std::uint64_t mixed =
      fnv1a64(key) ^ (static_cast<std::uint64_t>(salt) * 0x9E3779B97F4A7C15ull);
  std::uint64_t counter = (mixed >> 40) & detail::kIdCounterMask;
  if (counter == 0) {
    counter = 1;
  }
  return StrongId<EvaluatorIdTag>::from_raw(
      (static_cast<std::uint64_t>(IdKind::Evaluator) << detail::kIdKindShift) |
      ((static_cast<std::uint64_t>(salt) & detail::kIdSaltMask) << detail::kIdSaltShift) | counter);
}

/// Rebuild the full operation authority of a worker message: the session is the
/// connection's live session, and the coordinates are exactly the ones the
/// message itself carries.
[[nodiscard]] WorkerOperationAuthority operation_authority(const WorkerSessionAuthority& session,
                                                           WorkerOperationAuthority named) {
  named.session = session;
  return named;
}

/// The evaluator key under which a worker's own claim about its output is
/// recorded. It is deliberately not one of the reference task's requirement
/// keys, and the record's kind is WorkerSelfReport, which
/// evaluator_kind_is_authoritative() rejects. A claim can therefore be durable
/// evidence and still never satisfy a mandatory gate.
constexpr std::string_view kWorkerSelfReportKey = "worker-self-report";

/// True for a message a worker connection may send to the coordinator.
[[nodiscard]] bool message_type_is_worker(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello:
    case MessageType::WorkerReady:
    case MessageType::Revalidate:
    case MessageType::Heartbeat:
    case MessageType::AttemptAccepted:
    case MessageType::AttemptResult:
    case MessageType::AttemptFailed:
    case MessageType::CandidatePublish:
    case MessageType::SelfReport:
      return true;
    default:
      return false;
  }
}

/// True for a message a controller connection may send to the coordinator.
[[nodiscard]] bool message_type_is_controller(MessageType type) noexcept {
  switch (type) {
    case MessageType::CreateTask:
    case MessageType::DefinePolicy:
    case MessageType::CreatePopulation:
    case MessageType::StartPopulation:
    case MessageType::ClosePopulation:
    case MessageType::AdvancePopulation:
    case MessageType::RequestSelection:
    case MessageType::RequestRetention:
    case MessageType::RequestPromotion:
    case MessageType::RequestRevalidation:
    case MessageType::CancelAttemptRequest:
    case MessageType::QueryPopulation:
    case MessageType::QueryCandidate:
    case MessageType::QueryLineage:
    case MessageType::QueryStatistics:
    case MessageType::QueryAudit:
    case MessageType::ShutdownCoordinator:
      return true;
    default:
      return false;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct FoundryCoordinator::Impl
    : public std::enable_shared_from_this<FoundryCoordinator::Impl> {
  explicit Impl(const CoordinatorConfig& coordinator_config)
      : config(coordinator_config),
        store(std::make_unique<SnapshotStore>(coordinator_config.state_path)) {}

  CoordinatorConfig config;
  std::unique_ptr<SnapshotStore> store;
  std::unique_ptr<TcpListener> listener;
  std::unique_ptr<EvaluatorRegistry> evaluator_registry;
  std::shared_ptr<PromotionSink> promotion_sink;
  ReferenceToolchain toolchain;
  bool toolchain_resolved{false};
  ControllerId controller;
  std::atomic<std::uint64_t> scratch_counter{0};

  /// The core this coordinator drives. Assigned once, before any thread starts,
  /// and never reassigned while threads are running.
  FoundryCore* core{nullptr};

  // -- statistics ----------------------------------------------------------
  //
  // Counters are written by connection threads, the pump, the pool and the
  // public start/shutdown path, so every increment goes through mutate_stats
  // under one mutex. The public statistics() accessor returns a reference into
  // this store, so a concurrent read is a race the frozen public interface does
  // not let us remove; what this runtime guarantees is that no two counters can
  // be corrupted and that every mutation is serialized.
  mutable std::mutex stats_mutex;
  CoordinatorStatistics statistics;

  template <typename Mutate>
  void mutate_stats(Mutate&& mutate) {
    const std::lock_guard<std::mutex> guard(stats_mutex);
    mutate(statistics);
  }

  // -- connection registry -------------------------------------------------

  struct RegistryEntry {
    std::shared_ptr<Connection> connection;
    bool is_worker{false};
    bool has_session{false};
    WorkerSessionAuthority session;
    bool has_authority{false};
    WorkerOperationAuthority authority;
    bool close_noted{false};
  };

  mutable std::mutex registry_mutex;
  std::unordered_map<std::uint64_t, RegistryEntry> registry;
  std::uint64_t next_connection_id{1};

  // -- artifact cache ------------------------------------------------------

  struct CandidateArtifacts {
    /// Declared references exactly as published: metadata only.
    std::vector<ArtifactRef> declared;
    /// Content for the artifacts the coordinator could actually read back.
    std::map<std::string, std::string> content;
  };

  mutable std::mutex artifact_mutex;
  std::map<CandidateId, CandidateArtifacts> artifacts;

  // -- evaluation pool -----------------------------------------------------

  struct EvaluationJob {
    CandidateId candidate;
    CandidateGeneration candidate_generation;
  };

  std::mutex pool_mutex;
  std::condition_variable pool_wake;
  std::deque<EvaluationJob> jobs;
  bool pool_stop{false};
  std::vector<std::thread> pool_threads;

  /// Guards the set of candidates that already have a job queued, so the pump
  /// can never schedule the same candidate twice.
  std::mutex scheduling_mutex;
  std::set<CandidateId> scheduled;

  // -- pump ----------------------------------------------------------------

  std::mutex pump_mutex;
  std::condition_variable pump_wake;
  bool pump_stop{false};
  std::thread pump_thread;

  // -- accept --------------------------------------------------------------

  std::thread acceptor_thread;

  // -- lifecycle -----------------------------------------------------------

  std::mutex lifecycle_mutex;
  std::condition_variable lifecycle_wake;
  bool start_guard{false};
  std::atomic<bool> started{false};
  bool stopping{false};
  std::atomic<bool> shutdown_complete{false};
  bool shutdown_requested{false};
  std::string shutdown_reason;

  // -----------------------------------------------------------------------
  // Small helpers
  // -----------------------------------------------------------------------

  [[nodiscard]] Status persist(FoundryCore& core_state) {
    FoundrySnapshot snapshot = core_state.snapshot();
    const Status saved = store->save(snapshot);
    if (!saved.ok()) {
      return saved.with_context("writing the durable snapshot");
    }
    mutate_stats([](CoordinatorStatistics& stats) { stats.snapshots_written += 1; });
    return Status();
  }

  [[nodiscard]] Status persist_final(FoundryCore& core_state) {
    FoundrySnapshot snapshot = core_state.snapshot();
    const Status saved = store->save_and_verify(snapshot);
    return saved;
  }

  void wake_pump() { pump_wake.notify_all(); }

  [[nodiscard]] bool shutdown_is_stopping() {
    const std::lock_guard<std::mutex> guard(lifecycle_mutex);
    return stopping;
  }

  // -----------------------------------------------------------------------
  // Declarations implemented below
  // -----------------------------------------------------------------------

  void acceptor_loop();

  void note_close(std::uint64_t connection_id, const Status& reason);

  [[nodiscard]] Status send_frame(std::uint64_t connection_id, MessageType type,
                                  std::string payload);

  [[nodiscard]] Status send_error(std::uint64_t connection_id, ErrorCode code, std::string detail,
                                  MessageType in_reply_to);

  [[nodiscard]] Status protocol_violation(std::uint64_t connection_id, const Status& status,
                                          MessageType in_reply_to);

  void handle_frame(std::uint64_t connection_id, const Frame& frame);

  [[nodiscard]] Status handle_worker_message(std::uint64_t connection_id, const Frame& frame);

  [[nodiscard]] Status handle_controller_message(std::uint64_t connection_id, const Frame& frame);

  template <typename Encodable>
  [[nodiscard]] Status reply(std::uint64_t connection_id, MessageType type,
                             const Encodable& message) {
    std::string payload;
    AF_TRY_ASSIGN(payload, encode_payload(message));
    return send_frame(connection_id, type, std::move(payload));
  }

  [[nodiscard]] WorkerSessionAuthority worker_authority_of(const WorkerRecord& worker) const;

  [[nodiscard]] ControllerOperationAuthority controller_authority(
      const ControllerSessionAuthority& session) const;

  /// Resolve the population an operation authority names, and refuse an
  /// authority that does not name one at all.
  [[nodiscard]] Result<PopulationRecord> named_population(
      const ControllerOperationAuthority& authority);

  void pump_loop();
  void start_evaluations();
  void dispatch_ready_workers();
  void begin_dispatch(std::uint64_t connection_id, const WorkerRecord& worker,
                      FoundryCore& core_state);

  void enqueue_evaluations(std::deque<EvaluationJob> new_jobs);
  void pool_loop();
  void run_evaluation(const EvaluationJob& job);

  [[nodiscard]] EvaluationRequest make_request(
      const CandidateRecord& candidate, const std::string& evaluator_key,
      RequirementClass requirement_class, const std::filesystem::path& scratch) const;

  [[nodiscard]] std::map<std::string, std::string> artifact_content(CandidateId candidate) const;
  void cache_declared(CandidateId candidate, const std::vector<ArtifactRef>& declared);
  void drop_artifacts(CandidateId candidate);

  /// Run one evaluator and turn whatever happened into a record. A missing
  /// evaluator, a refused pre-flight check and an evaluator that throws all
  /// become honest non-pass outcomes; none of them can produce a pass.
  [[nodiscard]] EvaluationRecord evaluate_requirement(
      FoundryCore& core_state, const CandidateRecord& candidate, const TaskSpec& task,
      const EvaluationRequirement& requirement, const std::filesystem::path& scratch);
};

// ---------------------------------------------------------------------------
// Impl: connection bookkeeping
// ---------------------------------------------------------------------------

Status FoundryCoordinator::Impl::send_frame(const std::uint64_t connection_id, MessageType type,
                                            std::string payload) {
  std::shared_ptr<Connection> connection;
  {
    const std::lock_guard<std::mutex> guard(registry_mutex);
    const auto found_entry = registry.find(connection_id);
    if (found_entry == registry.end()) {
      return Status(ErrorCode::NotConnected,
                    "connection " + std::to_string(connection_id) + " is no longer registered");
    }
    connection = found_entry->second.connection;
  }
  // The registry lock is released before any socket work. send() only encodes
  // and enqueues; the connection's own writer thread performs the I/O.
  const Status sent = connection->send(type, std::move(payload));
  if (sent.ok()) {
    mutate_stats([](CoordinatorStatistics& stats) { stats.frames_sent += 1; });
  }
  return sent;
}

Status FoundryCoordinator::Impl::send_error(const std::uint64_t connection_id, ErrorCode code,
                                            std::string detail, MessageType in_reply_to) {
  ErrorResponseMessage response;
  response.code = code;
  response.detail = std::move(detail);
  response.in_reply_to = in_reply_to;
  return reply(connection_id, MessageType::ErrorResponse, response);
}

Status FoundryCoordinator::Impl::protocol_violation(const std::uint64_t connection_id,
                                                    const Status& status,
                                                    MessageType in_reply_to) {
  mutate_stats([](CoordinatorStatistics& stats) {
    stats.frames_rejected += 1;
    stats.protocol_violations += 1;
  });
  const Status replied = send_error(connection_id, status.code(), status.message(), in_reply_to);
  std::shared_ptr<Connection> connection;
  {
    const std::lock_guard<std::mutex> guard(registry_mutex);
    const auto found_entry = registry.find(connection_id);
    if (found_entry != registry.end()) {
      connection = found_entry->second.connection;
    }
  }
  if (connection) {
    // The byte stream is no longer trustworthy: a peer that sent an undecodable
    // frame may be confusing framing with payload, so continuing would only
    // reinterpret its garbage as protocol. The error response is queued first,
    // and the connection is then asked to close.
    connection->request_close(status);
  }
  return replied;
}

void FoundryCoordinator::Impl::note_close(const std::uint64_t connection_id,
                                          const Status& reason) {
  bool was_worker = false;
  bool had_session = false;
  WorkerSessionAuthority session;
  {
    const std::lock_guard<std::mutex> guard(registry_mutex);
    const auto found_entry = registry.find(connection_id);
    if (found_entry == registry.end()) {
      return;
    }
    if (found_entry->second.close_noted) {
      return;
    }
    found_entry->second.close_noted = true;
    was_worker = found_entry->second.is_worker;
    had_session = found_entry->second.has_session;
    session = found_entry->second.session;
    registry.erase(found_entry);
  }

  // A worker that disconnects with an open attempt leaves work whose outcome
  // nobody observed. The core records OutcomeUnknown rather than inventing a
  // success or a failure, and no other worker is touched.
  if (was_worker && had_session && core != nullptr) {
    mutate_stats([](CoordinatorStatistics& stats) { stats.worker_disconnects += 1; });
    const Status disconnected =
        core->worker_disconnected(session, "connection closed: " + reason.message());
    if (disconnected.ok()) {
      const Status saved = persist(*core);
    }
  }
  wake_pump();
}

void FoundryCoordinator::Impl::acceptor_loop() {
  for (;;) {
    Result<SocketHandle> accepted = listener->accept();
    if (!accepted.ok()) {
      // A closed listener is how an accept loop is asked to stop; anything else
      // is reported and ends the loop, because continuing to spin on a broken
      // listener would be busy work rather than progress.
      return;
    }

    std::uint64_t connection_id = 0;
    {
      const std::lock_guard<std::mutex> guard(registry_mutex);
      if (registry.size() >= kMaxConnections) {
        connection_id = 0;
      } else {
        connection_id = next_connection_id;
        next_connection_id += 1;
      }
    }
    if (connection_id == 0) {
      // Over the accepted-connection bound: close the socket without ever
      // handing it a reader thread.
      SocketHandle rejected = std::move(accepted).value();
      rejected.close();
      continue;
    }

    SocketHandle socket = std::move(accepted).value();
    std::string peer = config.endpoint.to_string();
    const std::weak_ptr<Impl> weak = weak_from_this();
    ConnectionCallbacks callbacks;
    callbacks.on_frame = [weak, connection_id](const Frame& frame) {
      if (const std::shared_ptr<Impl> impl = weak.lock()) {
        try {
          impl->handle_frame(connection_id, frame);
        } catch (const std::exception& error) {
          af_report_connection_fault(connection_id, "a frame", error.what());
        } catch (...) {
          af_report_connection_fault(connection_id, "a frame", nullptr);
        }
      }
    };
    callbacks.on_closed = [weak, connection_id](const Status& reason) {
      if (const std::shared_ptr<Impl> impl = weak.lock()) {
        try {
          impl->note_close(connection_id, reason);
        } catch (const std::exception& error) {
          af_report_connection_fault(connection_id, "connection close", error.what());
        } catch (...) {
          af_report_connection_fault(connection_id, "connection close", nullptr);
        }
      }
    };

    std::shared_ptr<Connection> connection =
        std::make_shared<Connection>(std::move(socket), std::move(peer), std::move(callbacks));
    {
      RegistryEntry entry;
      entry.connection = connection;
      const std::lock_guard<std::mutex> guard(registry_mutex);
      registry.emplace(connection_id, std::move(entry));
    }
    const Status connection_started = connection->start();
    if (!connection_started.ok()) {
      {
        const std::lock_guard<std::mutex> guard(registry_mutex);
        registry.erase(connection_id);
      }
      continue;
    }
    mutate_stats([](CoordinatorStatistics& stats) { stats.connections_accepted += 1; });
  }
}

// ---------------------------------------------------------------------------
// Impl: frame dispatch
// ---------------------------------------------------------------------------

void FoundryCoordinator::Impl::handle_frame(const std::uint64_t connection_id, const Frame& frame) {
  // Every frame runs on the reader thread of exactly one connection. It may
  // call into the core and it may enqueue a reply, but it must never block and
  // it must never hold the registry lock across a socket operation.
  mutate_stats([](CoordinatorStatistics& stats) { stats.frames_received += 1; });

  bool is_worker = false;
  bool has_session = false;
  bool known = false;
  {
    const std::lock_guard<std::mutex> guard(registry_mutex);
    const auto found_entry = registry.find(connection_id);
    if (found_entry != registry.end()) {
      known = true;
      is_worker = found_entry->second.is_worker;
      has_session = found_entry->second.has_session;
    }
  }
  if (!known || core == nullptr) {
    (void)send_error(connection_id, ErrorCode::NotConnected,
                     "the connection is not registered with a live coordinator",
                     frame.header.type);
    return;
  }

  // A worker connection may only send worker messages, and a controller
  // connection may only send controller messages. A peer that mixes the two
  // planes is guessing at the protocol, so it is rejected rather than
  // interpreted.
  const bool worker_message = message_type_is_worker(frame.header.type);
  const bool controller_message = message_type_is_controller(frame.header.type);
  if (!worker_message && !controller_message) {
    (void)protocol_violation(
        connection_id,
        Status(ErrorCode::ProtocolViolation,
               "message type '" + std::string(message_type_name(frame.header.type)) +
                   "' is not a message a peer may send to the coordinator"),
        frame.header.type);
    return;
  }
  // Hello is the message that establishes a session, so a connection that has
  // not registered one yet is exactly the connection that is allowed to send
  // it. It is the first message on the connection, and it is what tells the
  // coordinator whether this peer is a worker or a controller; refusing it
  // against a role that does not exist yet would refuse the handshake itself.
  // Every other message is still held to the plane of the session the
  // connection established.
  const bool establishes_session =
      frame.header.type == MessageType::Hello && !has_session;
  if (!establishes_session && is_worker != worker_message) {
    (void)protocol_violation(
        connection_id,
        Status(ErrorCode::ProtocolViolation,
               "a " + std::string(is_worker ? "worker" : "controller") +
                   " connection sent a message from the other plane ('" +
                   std::string(message_type_name(frame.header.type)) + "')"),
        frame.header.type);
    return;
  }

  const Status handled =
      worker_message ? handle_worker_message(connection_id, frame)
                     : handle_controller_message(connection_id, frame);
  if (!handled.ok()) {
    // A handler failure is a domain rejection, not a framing failure: the peer
    // is told exactly what was rejected and why, and the connection stays open.
    (void)send_error(connection_id, handled.code(), handled.message(), frame.header.type);
  }
  wake_pump();
}

Status FoundryCoordinator::Impl::handle_worker_message(const std::uint64_t connection_id,
                                                       const Frame& frame) {
  switch (frame.header.type) {
    case MessageType::Hello: {
      HelloMessage hello;
      AF_TRY_ASSIGN(hello, decode_hello(frame.payload));
      if (hello.role == SessionRole::Controller) {
        // A controller connection is a session without a worker incarnation. It
        // is answered with the epoch and run it is about to be served by, so the
        // operator can detect that it is talking to a different incumbent than
        // the one it last spoke to. Nothing about a controller is registered as
        // durable state, because a controller holds no durable authority.
        SessionId session;
        {
          const std::lock_guard<std::mutex> guard(registry_mutex);
          const std::uint64_t counter = next_connection_id == 0 ? 1 : next_connection_id;
          session = StrongId<SessionIdTag>::from_raw(
              (static_cast<std::uint64_t>(IdKind::Session) << detail::kIdKindShift) |
              ((static_cast<std::uint64_t>(core->config().id_salt) & detail::kIdSaltMask)
               << detail::kIdSaltShift) |
              (counter & detail::kIdCounterMask));
        }
        HelloAckMessage ack;
        ack.coordinator_epoch = core->epoch();
        ack.run = core->run();
        ack.session = session;
        ack.session_generation = WorkerSessionGeneration::first();
        ack.revalidation_required = false;
        ack.detail = "controller session established; nothing durable was created";
        return reply(connection_id, MessageType::HelloAck, ack);
      }
      if (hello.role != SessionRole::Worker) {
        return Status(ErrorCode::ProtocolViolation,
                      "a session that is neither a worker nor a controller may not register");
      }

      WorkerRegistrationRequest request;
      request.worker = hello.worker;
      request.boot = hello.boot;
      request.label = hello.label;
      request.capability = hello.capability;
      request.process_id = hello.process_id;

      WorkerSessionAuthority authority;
      AF_TRY_ASSIGN(authority, core->register_worker(request));
      // The registration is durable before the acknowledgement is queued, so a
      // coordinator that dies here is recovered with the worker already known.
      AF_TRY(persist(*core));

      const WorkerRecord worker = core->worker(request.worker).value();
      {
        const std::lock_guard<std::mutex> guard(registry_mutex);
        const auto found_entry = registry.find(connection_id);
        if (found_entry == registry.end()) {
          return Status(ErrorCode::NotConnected, "the connection disappeared during registration");
        }
        found_entry->second.is_worker = true;
        found_entry->second.has_session = true;
        found_entry->second.session = authority;
      }

      HelloAckMessage ack;
      ack.coordinator_epoch = authority.coordinator_epoch;
      ack.run = authority.run;
      ack.session = authority.session;
      ack.session_generation = authority.session_generation;
      ack.revalidation_required = worker.state == WorkerState::RevalidationRequired;
      ack.detail = ack.revalidation_required
                       ? "reconnected after a coordinator restart; revalidation is required "
                         "before this incarnation may accept work"
                       : "registered";
      return reply(connection_id, MessageType::HelloAck, ack);
    }
    case MessageType::WorkerReady: {
      WorkerReadyMessage ready;
      AF_TRY_ASSIGN(ready, decode_worker_ready(frame.payload));
      AF_TRY(core->worker_ready(ready.session, ready.capability));
      AF_TRY(persist(*core));
      ControlAckMessage ack;
      ack.ok = true;
      ack.detail = "worker is ready";
      return reply(connection_id, MessageType::ControlAck, ack);
    }
    case MessageType::Revalidate: {
      RevalidateMessage message;
      AF_TRY_ASSIGN(message, decode_revalidate(frame.payload));
      WorkerSessionAuthority fresh;
      AF_TRY_ASSIGN(fresh, core->revalidate_worker(message.session, message.detail));
      AF_TRY(persist(*core));
      {
        const std::lock_guard<std::mutex> guard(registry_mutex);
        const auto found_entry = registry.find(connection_id);
        if (found_entry != registry.end()) {
          found_entry->second.has_session = true;
          found_entry->second.session = fresh;
        }
      }
      RevalidateAckMessage ack;
      ack.session = fresh;
      ack.accepted = true;
      ack.detail = "a fresh session and session generation were issued; authority issued before "
                   "the restart is unusable";
      return reply(connection_id, MessageType::RevalidateAck, ack);
    }
    case MessageType::Heartbeat: {
      HeartbeatMessage message;
      AF_TRY_ASSIGN(message, decode_heartbeat(frame.payload));
      // The protocol has no HeartbeatAck payload struct: the acknowledgement
      // of a heartbeat is the ControlAck with the heartbeat's own sequence
      // echoed in its detail, so an operator can correlate the two.
      ControlAckMessage ack;
      ack.ok = true;
      ack.code = ErrorCode::Ok;
      ack.detail = "heartbeat " + std::to_string(message.sequence) + " acknowledged";
      return reply(connection_id, MessageType::ControlAck, ack);
    }
    case MessageType::AttemptAccepted: {
      AttemptAcceptedMessage message;
      AF_TRY_ASSIGN(message, decode_attempt_accepted(frame.payload));
      AF_TRY(core->acknowledge_attempt(message.authority));
      AF_TRY(persist(*core));
      ControlAckMessage ack;
      ack.detail = "assignment acknowledged";
      return reply(connection_id, MessageType::ControlAck, ack);
    }
    case MessageType::CandidatePublish: {
      CandidatePublishMessage message;
      AF_TRY_ASSIGN(message, decode_candidate_publish(frame.payload));
      WorkerSessionAuthority session;
      {
        const std::lock_guard<std::mutex> guard(registry_mutex);
        const auto entry = registry.find(connection_id);
        if (entry == registry.end() || !entry->second.has_session) {
          return Status(ErrorCode::NotConnected,
                        "a publication arrived on a connection with no worker session");
        }
        session = entry->second.session;
      }

      PublicationOutcome outcome;
      AF_TRY_ASSIGN(outcome,
                    core->publish_candidate(operation_authority(session, message.authority),
                                            message.artifacts, message.declared_strategy));
      // mutate -> persist -> notify. The publication is durable before any
      // acknowledgement exists, so a restart can never lose a candidate whose
      // PublishAck was already on the wire.
      AF_TRY(persist(*core));

      cache_declared(outcome.candidate, message.artifacts);

      // The worker's own claim is recorded as durable evidence, never as
      // authority: kind WorkerSelfReport is not an authoritative-capable kind,
      // so this record can never satisfy a mandatory gate.
      EvaluationRecord claim;
      claim.candidate = outcome.candidate;
      claim.candidate_generation = outcome.candidate_generation;
      claim.population = message.authority.population;
      claim.population_generation = message.authority.population_generation;
      claim.task = message.authority.task;
      claim.task_generation = message.authority.task_generation;
      claim.evaluator = evaluator_identity(
          kWorkerSelfReportKey, static_cast<std::uint32_t>(fnv1a64(kWorkerSelfReportKey)));
      claim.evaluator_key = std::string(kWorkerSelfReportKey);
      claim.kind = EvaluatorKind::WorkerSelfReport;
      claim.requirement_class = RequirementClass::Optional;
      claim.outcome = message.self_reported_success ? EvaluationOutcome::Pass
                                                    : EvaluationOutcome::Unknown;
      claim.complete = true;
      claim.decided_epoch = core->epoch();
      claim.diagnostics = message.self_report_detail.empty()
                              ? std::string("worker self report")
                              : message.self_report_detail;
      (void)core->record_evaluation(std::move(claim));
      (void)persist(*core);

      PublishAckMessage ack;
      ack.candidate = outcome.candidate;
      ack.candidate_generation = outcome.candidate_generation;
      ack.accepted = true;
      ack.detail = "output committed transactionally and durably; the worker's own claim was "
                   "recorded as evidence, not as authority";
      wake_pump();
      return reply(connection_id, MessageType::PublishAck, ack);
    }
    case MessageType::AttemptResult:
    case MessageType::AttemptFailed: {
      WorkerOperationAuthority authority;
      std::string reason;
      if (frame.header.type == MessageType::AttemptResult) {
        AttemptResultMessage message;
        AF_TRY_ASSIGN(message, decode_attempt_result(frame.payload));
        authority = message.authority;
        reason = message.detail.empty() ? std::string("the worker reported an attempt result")
                                        : message.detail;
      } else {
        AttemptFailedMessage message;
        AF_TRY_ASSIGN(message, decode_attempt_failed(frame.payload));
        authority = message.authority;
        reason = message.reason.empty() ? std::string("the worker reported an attempt failure")
                                        : message.reason;
      }
      WorkerSessionAuthority session;
      {
        const std::lock_guard<std::mutex> guard(registry_mutex);
        const auto found_entry = registry.find(connection_id);
        session = found_entry == registry.end() ? WorkerSessionAuthority() : found_entry->second.session;
      }
      AF_TRY(core->report_attempt_failure(operation_authority(session, authority), reason));
      AF_TRY(persist(*core));
      ControlAckMessage ack;
      ack.detail = "attempt failure recorded";
      wake_pump();
      return reply(connection_id, MessageType::ControlAck, ack);
    }
    case MessageType::SelfReport: {
      SelfReportMessage message;
      AF_TRY_ASSIGN(message, decode_self_report(frame.payload));
      bool has_authority = false;
      WorkerOperationAuthority authority;
      {
        const std::lock_guard<std::mutex> guard(registry_mutex);
        const auto entry = registry.find(connection_id);
        if (entry == registry.end() || !entry->second.has_session) {
          return Status(ErrorCode::NotConnected,
                        "a self report arrived on a connection with no worker session");
        }
        has_authority = entry->second.has_authority;
        authority = entry->second.authority;
      }
      if (!has_authority) {
        return Status(ErrorCode::NotReady,
                      "no attempt authority is known for this connection, so the claim cannot be "
                      "attached to a candidate");
      }
      EvaluationRecord claim;
      claim.candidate = authority.candidate;
      claim.candidate_generation = authority.candidate_generation;
      claim.population = authority.population;
      claim.population_generation = authority.population_generation;
      claim.task = authority.task;
      claim.task_generation = authority.task_generation;
      claim.evaluator = evaluator_identity(
          kWorkerSelfReportKey, static_cast<std::uint32_t>(fnv1a64(kWorkerSelfReportKey)));
      claim.evaluator_key = std::string(kWorkerSelfReportKey);
      claim.kind = EvaluatorKind::WorkerSelfReport;
      claim.requirement_class = RequirementClass::Optional;
      claim.outcome =
          message.claimed_success ? EvaluationOutcome::Pass : EvaluationOutcome::Unknown;
      claim.complete = true;
      claim.decided_epoch = core->epoch();
      claim.diagnostics = message.claim;
      // A duplicate self report for the same candidate generation is expected
      // and harmless: the first complete record is the evidence, and the rest
      // are refused by the core as duplicates.
      (void)core->record_evaluation(std::move(claim));
      ControlAckMessage ack;
      ack.detail = "claim recorded as worker self report evidence";
      return reply(connection_id, MessageType::ControlAck, ack);
    }
    default:
      return Status(ErrorCode::ProtocolViolation,
                    "message type '" + std::string(message_type_name(frame.header.type)) +
                        "' is not a worker message this coordinator accepts");
  }
}

// ---------------------------------------------------------------------------
// Impl: authority helpers
// ---------------------------------------------------------------------------

WorkerSessionAuthority FoundryCoordinator::Impl::worker_authority_of(
    const WorkerRecord& worker) const {
  WorkerSessionAuthority authority;
  authority.coordinator_epoch = worker.registered_epoch.valid() ? worker.registered_epoch
                                                                : core->epoch();
  authority.run = core->run();
  authority.worker = worker.id;
  authority.boot = worker.boot;
  authority.session = worker.session;
  authority.session_generation = worker.session_generation;
  return authority;
}

ControllerOperationAuthority FoundryCoordinator::Impl::controller_authority(
    const ControllerSessionAuthority& session) const {
  // The session is a controller of the foundry until the durable state names
  // one of its own. The parameters of the operation are supplied by the request
  // itself, and the core re-validates every generation it is given.
  ControllerOperationAuthority authority;
  authority.session.coordinator_epoch = core->epoch();
  authority.session.run = core->run();
  authority.session.controller =
      session.controller.valid() ? session.controller : controller;
  authority.session.session = session.session;
  authority.session.session_generation = session.session_generation;
  return authority;
}

Result<PopulationRecord> FoundryCoordinator::Impl::named_population(
    const ControllerOperationAuthority& authority) {
  PopulationId population = authority.population;
  if (population.valid()) {
    return core->population(population);
  }
  // The request did not bind a population. Exactly one live population is an
  // unambiguous target; anything else has to be named explicitly, because
  // guessing which population an operator meant is not a decision this runtime
  // is entitled to make for them.
  const std::vector<PopulationId> ids = core->population_ids();
  if (ids.size() == 1) {
    return core->population(ids.front());
  }
  if (ids.empty()) {
    return Status(ErrorCode::UnknownIdentity,
                  "no population exists, so the request names nothing that can be acted on");
  }
  return Status(ErrorCode::InvalidArgument,
                "the request names no population and " + std::to_string(ids.size()) +
                    " exist; name the population explicitly (the last population created is "
                    "the most recent)");
}

// ---------------------------------------------------------------------------
// Impl: artifact cache
// ---------------------------------------------------------------------------

void FoundryCoordinator::Impl::cache_declared(const CandidateId candidate,
                                              const std::vector<ArtifactRef>& declared) {
  CandidateArtifacts cached;
  cached.declared = declared;
  const std::lock_guard<std::mutex> guard(artifact_mutex);
  artifacts[candidate] = std::move(cached);
}

void FoundryCoordinator::Impl::drop_artifacts(const CandidateId candidate) {
  const std::lock_guard<std::mutex> guard(artifact_mutex);
  artifacts.erase(candidate);
}

std::map<std::string, std::string> FoundryCoordinator::Impl::artifact_content(
    const CandidateId candidate) const {
  const std::lock_guard<std::mutex> guard(artifact_mutex);
  const auto found_entry = artifacts.find(candidate);
  if (found_entry == artifacts.end()) {
    return {};
  }
  return found_entry->second.content;
}

EvaluationRequest FoundryCoordinator::Impl::make_request(
    const CandidateRecord& candidate, const std::string& evaluator_key,
    const RequirementClass requirement_class, const std::filesystem::path& scratch) const {
  EvaluationRequest request;
  request.candidate = candidate.id;
  request.candidate_generation = candidate.generation;
  request.task = candidate.task;
  request.task_generation = candidate.task_generation;
  request.population = candidate.population;
  request.population_generation = candidate.population_generation;
  request.artifacts = artifact_content(candidate.id);
  request.evaluator_key = evaluator_key;
  request.requirement_class = requirement_class;
  request.scratch_directory = scratch;
  if (const Result<TaskSpec> task = core->task(candidate.task); task.ok()) {
    for (const InputFile& input : task.value().inputs) {
      request.inputs.emplace(input.name, input.content);
    }
  }
  request.toolchain = toolchain;
  return request;
}

// ---------------------------------------------------------------------------
// Impl: pump
// ---------------------------------------------------------------------------
//
// The pump is the only thread that starts work on its own. It holds pump_mutex
// for nothing but its own wait, reads its inputs from FoundryCore under the
// core's lock and from the registry under the registry lock, and holds no lock
// at all across a core call, a snapshot write or a socket send. Every tick
// re-examines state that a connection thread has just changed; the 20 ms
// scheduling tick only bounds how long the pump sleeps, never how long any
// evaluation, connection or process may run.

void FoundryCoordinator::Impl::pump_loop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(pump_mutex);
      // The tick wakes the pump so it re-reads state a connection thread
      // changed; the shutdown flag ends the loop.
      pump_wake.wait_for(lock, kPumpTick, [this] { return pump_stop; });
      if (pump_stop) {
        return;
      }
    }
    // Nothing is published or authorized once shutdown has been requested: the
    // pump stops starting work on the tick that observes it.
    if (shutdown_is_stopping()) {
      return;
    }
    if (core == nullptr) {
      continue;
    }
    start_evaluations();
    dispatch_ready_workers();
  }
}

void FoundryCoordinator::Impl::start_evaluations() {
  FoundryCore* const core_state = core;
  if (core_state == nullptr) {
    return;
  }
  // The core decides which candidates still need evidence; the pump only asks.
  const std::vector<CandidateId> candidates = core_state->candidates_awaiting_evaluation();
  if (candidates.empty()) {
    return;
  }
  // BEGIN EVALUATION is what takes a published candidate out of the set a
  // second pump tick would consider, so the job is enqueued only when the core
  // accepted the transition. A candidate that is already Evaluating accepts it
  // again, which is how a job lost with the previous coordinator is resumed.
  std::deque<EvaluationJob> new_jobs;
  for (const CandidateId candidate : candidates) {
    {
      // The in-flight set is the second guard: a candidate whose job is still
      // queued or still running is never scheduled twice.
      const std::lock_guard<std::mutex> guard(scheduling_mutex);
      if (scheduled.find(candidate) != scheduled.end()) {
        continue;
      }
    }
    const Status begun = core_state->begin_evaluation(candidate);
    if (!begun.ok()) {
      // The candidate is no longer in a state that awaits evaluation: the
      // transition is the authority here, and a refused one schedules nothing.
      continue;
    }
    {
      const std::lock_guard<std::mutex> guard(scheduling_mutex);
      if (!scheduled.insert(candidate).second) {
        continue;
      }
    }
    EvaluationJob job;
    job.candidate = candidate;
    // The generation the pump observed travels with the job; the core
    // re-validates it when the record is committed, so evidence recorded
    // against a superseded generation is refused rather than attached.
    const Result<CandidateRecord> record = core_state->candidate(candidate);
    job.candidate_generation = record.ok() ? record.value().generation : CandidateGeneration();
    new_jobs.push_back(job);
  }
  enqueue_evaluations(std::move(new_jobs));
}

void FoundryCoordinator::Impl::dispatch_ready_workers() {
  if (core == nullptr) {
    return;
  }
  if (shutdown_is_stopping()) {
    return;
  }
  // Snapshot the live worker sessions, then release the registry lock before
  // any core call or socket work.
  struct LiveWorker {
    std::uint64_t connection;
    WorkerId worker;
  };
  std::vector<LiveWorker> sessions;
  {
    const std::lock_guard<std::mutex> guard(registry_mutex);
    for (const auto& entry : registry) {
      if (!entry.second.is_worker || !entry.second.has_session) {
        continue;
      }
      if (entry.second.connection && entry.second.connection->closed()) {
        continue;
      }
      LiveWorker found;
      found.connection = entry.first;
      found.worker = entry.second.session.worker;
      sessions.push_back(found);
    }
  }
  for (const LiveWorker& entry : sessions) {
    if (core == nullptr) {
      return;
    }
    const Result<WorkerRecord> worker = core->worker(entry.worker);
    if (!worker.ok()) {
      continue;
    }
    begin_dispatch(entry.connection, worker.value(), *core);
  }
}

void FoundryCoordinator::Impl::begin_dispatch(const std::uint64_t connection_id,
                                              const WorkerRecord& worker,
                                              FoundryCore& core_state) {
  if (worker.state != WorkerState::Ready) {
    // A worker that is Busy, Draining, Offline or awaiting revalidation is not
    // a dispatch target, and the core would refuse it anyway.
    return;
  }
  // The live session is rebuilt from the durable worker record, exactly as
  // every other authority this file hands to the core is, so a connection whose
  // session has since been revalidated cannot dispatch on a stale one.
  const WorkerSessionAuthority session = worker_authority_of(worker);
  // authorize_attempt creates the attempt durably in Authorized state and
  // reserves its budgets. Unavailable and NotReady are ordinary answers - no
  // population is running, no slot is free - and the next tick tries again.
  const Result<PendingDispatch> pending = core_state.authorize_attempt(session);
  if (!pending.ok()) {
    return;
  }
  const AttemptRecord& attempt = pending.value().attempt;
  bool sent = false;
  {
    // mutate -> persist -> notify. The Authorized attempt is on disk BEFORE the
    // assignment is confirmed as dispatched and BEFORE any frame leaves this
    // process, so a coordinator that dies here leaves an attempt a recovery can
    // explain instead of authority a worker is already acting on.
    const Status saved = persist(core_state);
    if (!saved.ok()) {
      // The attempt stays Authorized and unsent: the next tick re-authorizes
      // from the same durable evidence rather than inventing a dispatch.
      return;
    }
    const Result<AttemptPackage> package =
        core_state.confirm_dispatch(session, attempt.id, attempt.generation);
    if (!package.ok()) {
      return;
    }
    AssignAttemptMessage assignment;
    assignment.session = session;
    assignment.package = package.value();
    std::string payload;
    // The frame is encoded here and handed to the connection's own writer
    // thread, so this thread holds no lock and performs no socket I/O while the
    // package is built.
    const Result<std::string> encoded = encode_payload(assignment);
    if (encoded.ok()) {
      payload = encoded.value();
    }
    if (!payload.empty()) {
      sent = send_frame(connection_id, MessageType::AssignAttempt, std::move(payload)).ok();
    }
  }
  // When the frame could not be queued the assignment stays durably Dispatched
  // and nothing else is done here. That is the ambiguous case the ordering rule
  // exists for: a worker that reconnects and reports an outcome is believed, and
  // one that never does leaves an attempt a recovery reads as OutcomeUnknown
  // rather than as work that never happened.
}

// ---------------------------------------------------------------------------
// Impl: evaluation
// ---------------------------------------------------------------------------

EvaluationRecord FoundryCoordinator::Impl::evaluate_requirement(
    FoundryCore& core_state, const CandidateRecord& candidate, const TaskSpec& task,
    const EvaluationRequirement& requirement, const std::filesystem::path& scratch) {
  EvaluationRecord record;
  record.candidate = candidate.id;
  // The job's generation is the one the pump observed; the candidate record is
  // re-read here, and the core re-validates the generation on the record.
  record.task = task.id;
  record.task_generation = task.generation;
  record.population = candidate.population;
  record.population_generation = candidate.population_generation;
  record.evaluator = evaluator_identity(requirement.evaluator_key, core_state.config().id_salt);
  record.evaluator_key = requirement.evaluator_key;
  record.requirement_class = requirement.requirement_class;
  record.kind = EvaluatorKind::PortableReference;
  record.outcome = EvaluationOutcome::Unknown;
  record.complete = true;
  record.decided_epoch = core_state.epoch();

  const std::shared_ptr<Evaluator> evaluator =
      evaluator_registry ? evaluator_registry->find(requirement.evaluator_key) : nullptr;
  if (!evaluator) {
    // A missing evaluator is UNSUPPORTED. It is never a pass, and it never
    // silently disappears: the record says exactly which key could not be
    // resolved, so the gap is visible in the durable evidence.
    record.kind = EvaluatorKind::PortableReference;
    record.outcome = EvaluationOutcome::Unsupported;
    record.diagnostics = "no evaluator is registered under key '" + requirement.evaluator_key +
                         "'; this requirement is UNSUPPORTED in this deployment and cannot "
                         "satisfy a mandatory gate";
    return record;
  }

  record.kind = evaluator->kind();
  const EvaluationRequest request =
      make_request(candidate, requirement.evaluator_key, requirement.requirement_class, scratch);
  if (request.artifacts.empty()) {
    // The evaluator contract takes bytes, and this coordinator has none for the
    // candidate. Reporting UNSUPPORTED is the honest answer; inventing a pass
    // from metadata would not be.
    record.outcome = EvaluationOutcome::Unsupported;
    record.diagnostics = "artifact content for candidate " + candidate.id.to_string() +
                         " is not available to this coordinator, so the evaluation cannot be "
                         "performed and is reported as UNSUPPORTED";
    return record;
  }

  try {
    const EvaluationResult result = evaluator->evaluate(request);
    record.outcome = result.outcome;
    record.has_score = result.has_score;
    record.score = result.score;
    record.diagnostics = result.diagnostics;
    record.evidence_digest = result.evidence_digest;
    record.duration_micros = result.duration_micros;
  } catch (const std::exception& error) {
    // An evaluator that throws has told us nothing about the candidate, so the
    // record is Error rather than a guess in either direction.
    record.outcome = EvaluationOutcome::Error;
    record.diagnostics = std::string("evaluator '") + requirement.evaluator_key +
                         "' raised: " + error.what();
  } catch (...) {
    record.outcome = EvaluationOutcome::Error;
    record.diagnostics =
        std::string("evaluator '") + requirement.evaluator_key + "' raised an unknown exception";
  }
  return record;
}

void FoundryCoordinator::Impl::enqueue_evaluations(std::deque<EvaluationJob> new_jobs) {
  if (new_jobs.empty()) {
    return;
  }
  {
    const std::lock_guard<std::mutex> guard(pool_mutex);
    for (EvaluationJob& job : new_jobs) {
      jobs.push_back(std::move(job));
    }
  }
  pool_wake.notify_all();
}

void FoundryCoordinator::Impl::pool_loop() {
  for (;;) {
    EvaluationJob job;
    {
      std::unique_lock<std::mutex> lock(pool_mutex);
      pool_wake.wait(lock, [this] { return pool_stop || !jobs.empty(); });
      if (jobs.empty()) {
        if (pool_stop) {
          return;
        }
        continue;
      }
      job = std::move(jobs.front());
      jobs.pop_front();
    }
    run_evaluation(job);
  }
}

void FoundryCoordinator::Impl::run_evaluation(const EvaluationJob& job) {
  // Runs OFF the pump thread and OFF the state lock. The only lock this holds
  // for any length of time is the core's own, and only for the moment a record
  // is committed.
  if (core == nullptr) {
    return;
  }
  FoundryCore& core_state = *core;

  const Result<CandidateRecord> candidate = core_state.candidate(job.candidate);
  if (!candidate.ok()) {
    const std::lock_guard<std::mutex> guard(scheduling_mutex);
    scheduled.erase(job.candidate);
    return;
  }
  const Result<TaskSpec> task = core_state.task(candidate.value().task);
  if (!task.ok()) {
    const std::lock_guard<std::mutex> guard(scheduling_mutex);
    scheduled.erase(job.candidate);
    return;
  }
  const Result<std::vector<EvaluationRequirement>> pending =
      core_state.pending_evaluations(job.candidate);
  if (!pending.ok()) {
    const std::lock_guard<std::mutex> guard(scheduling_mutex);
    scheduled.erase(job.candidate);
    return;
  }

  // One scratch directory per job, below the configured workspace root. A
  // failure to create it is not fatal to the job: the evaluator is told which
  // directory it may use and reports UNSUPPORTED when it cannot use it.
  std::filesystem::path scratch;
  const std::uint64_t index = scratch_counter.fetch_add(1);
  const Result<std::filesystem::path> created = make_unique_directory(
      core_state.config().workspace_root / "scratch", "candidate-" + std::to_string(index));
  if (created.ok()) {
    scratch = created.value();
  }

  for (const EvaluationRequirement& requirement : pending.value()) {
    EvaluationRecord record = evaluate_requirement(core_state, candidate.value(), task.value(),
                                                   requirement, scratch);
    mutate_stats([&record](CoordinatorStatistics& stats) {
      stats.evaluations_completed += 1;
      if (record.outcome == EvaluationOutcome::Error) {
        stats.evaluations_failed += 1;
      }
    });
    // The record is committed and the commit is then persisted. A coordinator
    // that dies between the two loses the record, which reads as "this
    // evaluation never completed" - the truth - rather than as a pass.
    const Status recorded = core_state.record_evaluation(std::move(record));
    if (recorded.ok()) {
      (void)persist(core_state);
    }
  }

  if (!scratch.empty()) {
    (void)remove_tree_bounded(scratch);
  }

  // Once no requirement is outstanding, the artifact bytes this coordinator
  // was holding for the candidate are no longer needed.
  const Result<std::vector<EvaluationRequirement>> remaining =
      core_state.pending_evaluations(job.candidate);
  if (remaining.ok() && remaining.value().empty()) {
    drop_artifacts(job.candidate);
  }

  {
    const std::lock_guard<std::mutex> guard(scheduling_mutex);
    scheduled.erase(job.candidate);
  }
  wake_pump();
}


// ---------------------------------------------------------------------------
// Impl: controller plane (defined in the private companion header)
// ---------------------------------------------------------------------------

}  // namespace autonomous_foundry

#include "coordinator_control_plane.hpp"

namespace autonomous_foundry {

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

FoundryCoordinator::FoundryCoordinator(CoordinatorConfig config)
    : impl_(std::make_shared<Impl>(config)) {}

FoundryCoordinator::~FoundryCoordinator() {
  // A coordinator destroyed without an explicit shutdown still stops its
  // threads: shutdown is idempotent, and a destructor that left threads running
  // would be a lifetime defect rather than a convenience.
  if (impl_ && impl_->started.load() && !impl_->shutdown_complete.load()) {
    (void)shutdown("coordinator destroyed without an explicit shutdown");
  }
}

Status FoundryCoordinator::start() {
  if (!impl_) {
    return Status(ErrorCode::Internal, "the coordinator has no implementation state");
  }
  {
    std::lock_guard<std::mutex> guard(impl_->lifecycle_mutex);
    if (impl_->started.load() && !impl_->shutdown_complete.load()) {
      return Status(ErrorCode::AlreadyExists, "the coordinator is already running");
    }
    if (impl_->start_guard) {
      return Status(ErrorCode::AlreadyExists, "the coordinator is already starting");
    }
    impl_->start_guard = true;
    // Repeated start after shutdown is real: the stop flags are reset so the
    // same object can run again.
    impl_->stopping = false;
    impl_->shutdown_complete = false;
    impl_->shutdown_requested = false;
    impl_->pump_stop = false;
    impl_->pool_stop = false;
    impl_->shutdown_reason.clear();
    std::lock_guard<std::mutex> pool_guard(impl_->pool_mutex);
    impl_->jobs.clear();
  }
  const auto abort_start = [this](const Status& status) -> Status {
    {
      const std::lock_guard<std::mutex> guard(impl_->lifecycle_mutex);
      impl_->start_guard = false;
    }
    return status;
  };

  // 1. The socket subsystem has to exist before any socket does.
  AF_TRY(abort_start(SocketSubsystem::ensure_initialized()));

  // 2. Recover or create durable state. Identity, run and epoch are decided
  //    here and nowhere else.
  FoundryId foundry = impl_->config.foundry;
  FoundryRunId run;
  CoordinatorEpoch epoch = CoordinatorEpoch::from_value(1);
  std::uint32_t salt = 0;
  const bool snapshot_exists = impl_->store->exists();
  if (impl_->config.fresh_start || !snapshot_exists) {
    salt = make_id_salt();
    run = make_foundry_run_id();
    epoch = CoordinatorEpoch::from_value(1);
    if (!foundry.valid()) {
      IdAllocator allocator(salt);
      FoundryId minted;
      AF_TRY_ASSIGN(minted, allocator.next<FoundryIdTag>());
      foundry = minted;
    }
  } else {
    Result<FoundrySnapshot> loaded = impl_->store->load();
    if (!loaded.ok()) {
      // A load failure is never silently ignored: a coordinator that cannot read
      // its own durable state must not pretend to start fresh, because that
      // would silently discard a run that exists on disk.
      return abort_start(loaded.status().with_context(
          "loading the snapshot at '" + path_to_utf8(impl_->config.state_path) + "'"));
    }
    FoundrySnapshot snapshot = std::move(loaded).value();
    if (!foundry.valid()) {
      foundry = snapshot.foundry;
    }
    run = snapshot.run;
    salt = snapshot.id_salt;
    const std::optional<CoordinatorEpoch> advanced = snapshot.epoch.next();
    if (!advanced.has_value()) {
      return abort_start(Status(ErrorCode::IdentityExhausted,
                                "the coordinator epoch cannot advance past its limit"));
    }
    epoch = advanced.value();

    FoundryConfig recovered_config;
    recovered_config.foundry = foundry;
    recovered_config.run = run;
    recovered_config.epoch = epoch;
    recovered_config.id_salt = salt;
    recovered_config.workspace_root = impl_->config.workspace_root;
    recovered_config.default_budgets = impl_->config.budgets;
    core_ = std::make_unique<FoundryCore>(std::move(recovered_config));
    impl_->core = core_.get();
    // recover() adopts the durable state under the advanced epoch, so every
    // authority issued by the previous incumbent is stale by construction.
    AF_TRY(abort_start(core_->recover(std::move(snapshot))));
  }

  if (!core_) {
    FoundryConfig fresh_config;
    fresh_config.foundry = foundry;
    fresh_config.run = run;
    fresh_config.epoch = epoch;
    fresh_config.id_salt = salt;
    fresh_config.workspace_root = impl_->config.workspace_root;
    fresh_config.default_budgets = impl_->config.budgets;
    core_ = std::make_unique<FoundryCore>(std::move(fresh_config));
  }
  impl_->core = core_.get();
  impl_->controller = reference_controller_id(foundry);

  impl_->evaluator_registry =
      std::make_unique<EvaluatorRegistry>(EvaluatorRegistry::make_reference());
  if (!impl_->config.promotion_sink_directory.empty()) {
    impl_->promotion_sink =
        std::make_shared<LocalPromotionSink>(impl_->config.promotion_sink_directory);
  }
  if (impl_->config.enable_toolchain_probe) {
    const Result<ReferenceToolchain> resolved = resolve_reference_toolchain();
    if (resolved.ok()) {
      impl_->toolchain = resolved.value();
      impl_->toolchain_resolved = true;
    }
    // A missing toolchain is not a startup failure: the evaluator reports
    // UNSUPPORTED, which is the honest answer, and never a pass.
  }

  // 3. Bind. Port 0 selects an ephemeral port, read back from the listener so
  //    tests and operators can address this exact instance.
  Result<TcpListener> bound = TcpListener::bind(impl_->config.endpoint);
  if (!bound.ok()) {
    return abort_start(bound.status());
  }
  impl_->listener = std::make_unique<TcpListener>(std::move(bound).value());
  port_ = impl_->listener->port();

  // 4. The initial (or recovered) state is durable before anything can observe
  //    it, so the first worker to connect is answered from disk-backed state.
  AF_TRY(abort_start(impl_->persist(*core_)));

  // 5. Threads. Each captures a weak reference, so a thread can never keep the
  //    coordinator alive after its owner is gone.
  const std::weak_ptr<Impl> weak = impl_->shared_from_this();
  try {
    impl_->acceptor_thread = std::thread([weak] {
      if (const std::shared_ptr<Impl> state = weak.lock()) {
        state->acceptor_loop();
      }
    });
  } catch (const std::exception&) {
    return abort_start(
        Status(ErrorCode::ResourceExhausted, "the acceptor thread could not be created"));
  }
  try {
    impl_->pump_thread = std::thread([weak] {
      if (const std::shared_ptr<Impl> state = weak.lock()) {
        state->pump_loop();
      }
    });
  } catch (const std::exception&) {
    return abort_start(Status(ErrorCode::ResourceExhausted, "the pump thread could not be created"));
  }

  const std::uint32_t concurrency =
      impl_->config.evaluation_concurrency == 0 ? 1u : impl_->config.evaluation_concurrency;
  try {
    for (std::uint32_t index = 0; index < concurrency; ++index) {
      impl_->pool_threads.emplace_back([weak] {
        if (const std::shared_ptr<Impl> state = weak.lock()) {
          state->pool_loop();
        }
      });
    }
  } catch (const std::exception&) {
    return abort_start(Status(ErrorCode::ResourceExhausted,
                              "the evaluation pool threads could not all be created"));
  }

  {
    const std::lock_guard<std::mutex> guard(impl_->lifecycle_mutex);
    impl_->start_guard = false;
    impl_->started = true;
  }
  impl_->wake_pump();
  return Status();
}

Status FoundryCoordinator::run() {
  if (!impl_) {
    return Status(ErrorCode::Internal, "the coordinator has no implementation state");
  }
  std::string reason;
  {
    std::unique_lock<std::mutex> lock(impl_->lifecycle_mutex);
    impl_->lifecycle_wake.wait(
        lock, [this] { return impl_->shutdown_requested || impl_->shutdown_complete.load(); });
    if (impl_->stopping) {
      return Status();
    }
    reason = impl_->shutdown_reason;
  }
  const Status outcome = shutdown(reason.empty()
                                      ? std::string("shutdown requested through the control plane")
                                      : reason);
  return outcome;
}

void FoundryCoordinator::request_shutdown(std::string reason) {
  if (!impl_) {
    return;
  }
  {
    const std::lock_guard<std::mutex> guard(impl_->lifecycle_mutex);
    impl_->shutdown_requested = true;
    if (!reason.empty()) {
      impl_->shutdown_reason = std::move(reason);
    } else if (impl_->shutdown_reason.empty()) {
      impl_->shutdown_reason = "shutdown requested";
    }
  }
  // No thread is joined here. This is safe to call from a connection reader
  // thread, which is exactly where a control-plane shutdown request arrives.
  impl_->lifecycle_wake.notify_all();
  impl_->wake_pump();
}

Status FoundryCoordinator::shutdown(std::string reason) {
  if (!impl_) {
    return Status(ErrorCode::Internal, "the coordinator has no implementation state");
  }
  {
    const std::lock_guard<std::mutex> guard(impl_->lifecycle_mutex);
    if (impl_->shutdown_complete.load()) {
      return Status();
    }
    if (impl_->stopping) {
      return Status(ErrorCode::ShutdownInProgress, "shutdown is already in progress");
    }
    impl_->stopping = true;
    if (impl_->shutdown_reason.empty()) {
      impl_->shutdown_reason = reason.empty() ? std::string("shutdown requested") : reason;
    }
    shutting_down_.store(true, std::memory_order_release);
  }
  impl_->lifecycle_wake.notify_all();
  impl_->wake_pump();

  Status outcome;

  // 1. Stop accepting. Closing the listener is what unblocks a thread parked in
  //    accept().
  if (impl_->listener) {
    impl_->listener->close();
  }
  if (impl_->acceptor_thread.joinable()) {
    impl_->acceptor_thread.join();
  }

  // 2. Stop the pump.
  {
    const std::lock_guard<std::mutex> guard(impl_->pump_mutex);
    impl_->pump_stop = true;
  }
  impl_->wake_pump();
  if (impl_->pump_thread.joinable()) {
    impl_->pump_thread.join();
  }

  // 3. Drain the evaluation pool. Queued jobs are finished, not abandoned and
  //    not cancelled: a job already promised to a candidate is work the durable
  //    evidence depends on.
  {
    const std::lock_guard<std::mutex> guard(impl_->pool_mutex);
    impl_->pool_stop = true;
  }
  impl_->pool_wake.notify_all();
  for (std::thread& thread : impl_->pool_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  impl_->pool_threads.clear();

  // 4. A final verified snapshot. save_and_verify proves the image reloads to an
  //    equal state instead of assuming it does.
  if (core_) {
    const Status saved = impl_->persist_final(*core_);
    if (!saved.ok()) {
      outcome = saved.with_context("the final snapshot could not be written and verified");
    }
  }

  // 5. The workspace root goes away with the runtime.
  if (!impl_->config.workspace_root.empty()) {
    const Status removed = remove_tree_bounded(impl_->config.workspace_root);
    if (!removed.ok() && outcome.ok()) {
      outcome = removed.with_context("removing the workspace root");
    }
  }

  // 6. Ask every connection to stop, outside the registry lock. Workers are told
  //    with a Shutdown frame; the close request follows it and each connection is
  //    then destroyed on this thread, which joins its own reader and writer.
  std::vector<std::pair<std::shared_ptr<Connection>, bool>> connections;
  {
    const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
    for (auto& entry : impl_->registry) {
      connections.emplace_back(entry.second.connection, entry.second.is_worker);
    }
  }
  for (auto& entry : connections) {
    if (entry.second) {
      (void)entry.first->send(MessageType::ShutdownCoordinator, std::string());
    }
    entry.first->request_close(
        Status(ErrorCode::ConnectionClosed, "the coordinator is shutting down"));
  }
  {
    const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
    impl_->registry.clear();
  }

  {
    const std::lock_guard<std::mutex> guard(impl_->lifecycle_mutex);
    impl_->started = false;
    impl_->stopping = false;
    impl_->shutdown_complete = true;
  }
  impl_->lifecycle_wake.notify_all();
  return outcome;
}

Status FoundryCoordinator::persist() {
  if (!impl_ || !core_) {
    return Status(ErrorCode::Internal, "the coordinator has no state to persist");
  }
  return impl_->persist(*core_);
}

CoordinatorEpoch FoundryCoordinator::epoch() const {
  return core_ ? core_->epoch() : CoordinatorEpoch();
}

FoundryRunId FoundryCoordinator::run() const { return core_ ? core_->run() : FoundryRunId(); }

int run_coordinator_process(const CoordinatorConfig& config) {
  try {
    FoundryCoordinator coordinator(config);
    const Status started = coordinator.start();
    if (!started.ok()) {
      return 1;
    }
    const Status finished = coordinator.run();
    if (!finished.ok()) {
      return 1;
    }
    return 0;
  } catch (const std::exception&) {
    return 1;
  } catch (...) {
    return 1;
  }
}

}  // namespace autonomous_foundry
