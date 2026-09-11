#pragma once

// src/coordinator_control_plane.hpp
//
// Private definition of the coordinator's controller plane.
//
// This header is not part of the public include tree and is included exactly
// once, from src/coordinator.cpp, after FoundryCoordinator::Impl is complete.
// It contains out-of-class member definitions of that impl, so including it
// twice would be an ODR violation; the file is split out only to keep each
// translation unit a readable size, exactly as foundry_core_impl.hpp keeps the
// state machine shareable without leaking internals into include/.
//
// The controller plane is read-mostly and request-shaped: every case decodes a
// controller message, calls FoundryCore with the authority the message carries,
// persists the resulting transition, and only then replies. Nothing here takes
// the registry lock, and nothing here performs I/O while holding any lock.

namespace autonomous_foundry {

Status FoundryCoordinator::Impl::handle_controller_message(const std::uint64_t connection_id,
                                                           const Frame& frame) {
  switch (frame.header.type) {
    case MessageType::CreateTask: {
      CreateTaskMessage message;
      AF_TRY_ASSIGN(message, decode_create_task(frame.payload));
      TaskId created;
      AF_TRY_ASSIGN(created, core->define_task(message.task));
      AF_TRY(persist(*core));
      TaskCreatedMessage response;
      response.task = created;
      response.generation = TaskGeneration::first();
      response.content_digest = message.task.content_digest;
      response.detail = "task defined and durably recorded";
      return reply(connection_id, MessageType::TaskCreated, response);
    }
    case MessageType::DefinePolicy: {
      DefinePolicyMessage message;
      AF_TRY_ASSIGN(message, decode_define_policy(frame.payload));
      PolicyId created;
      AF_TRY_ASSIGN(created, core->define_policy(message.policy));
      AF_TRY(persist(*core));
      PolicyDefinedMessage response;
      response.policy = created;
      response.generation = PolicyGeneration::first();
      response.content_digest = message.policy.content_digest;
      return reply(connection_id, MessageType::PolicyDefined, response);
    }
    case MessageType::CreatePopulation: {
      CreatePopulationMessage message;
      AF_TRY_ASSIGN(message, decode_create_population(frame.payload));
      // A population that names a task or policy the foundry does not hold is
      // refused here rather than becoming a record nothing can ever satisfy.
      AF_TRY(core->task(message.spec.task));
      AF_TRY(core->policy(message.spec.policy));
      PopulationId created;
      AF_TRY_ASSIGN(created, core->create_population(message.spec));
      AF_TRY(persist(*core));
      PopulationCreatedMessage response;
      response.population = created;
      response.generation = PopulationGeneration::first();
      return reply(connection_id, MessageType::PopulationCreated, response);
    }
    case MessageType::StartPopulation: {
      StartPopulationMessage message;
      AF_TRY_ASSIGN(message, decode_start_population(frame.payload));
      PopulationRecord population;
      AF_TRY_ASSIGN(population, named_population(message.authority));
      AF_TRY(core->start_population(population.id, population.generation));
      AF_TRY(persist(*core));
      const PopulationRecord current = core->population(population.id).value();
      PopulationStateMessage response;
      response.population = current.id;
      response.generation = current.generation;
      response.state = current.state;
      response.detail = "population started";
      wake_pump();
      return reply(connection_id, MessageType::PopulationStarted, response);
    }
    case MessageType::ClosePopulation: {
      ClosePopulationMessage message;
      AF_TRY_ASSIGN(message, decode_close_population(frame.payload));
      PopulationRecord population;
      AF_TRY_ASSIGN(population, named_population(message.authority));
      AF_TRY(core->close_population(population.id));
      AF_TRY(persist(*core));
      const PopulationRecord current = core->population(population.id).value();
      PopulationStateMessage response;
      response.population = current.id;
      response.generation = current.generation;
      response.state = current.state;
      response.detail = "population closed";
      return reply(connection_id, MessageType::PopulationClosed, response);
    }
    case MessageType::AdvancePopulation: {
      AdvancePopulationMessage message;
      AF_TRY_ASSIGN(message, decode_advance_population(frame.payload));
      PopulationRecord population;
      AF_TRY_ASSIGN(population, named_population(message.authority));
      PopulationId successor;
      AF_TRY_ASSIGN(successor, core->advance_population(population.id, message.next_name));
      AF_TRY(persist(*core));
      const PopulationRecord created = core->population(successor).value();
      PopulationAdvancedMessage response;
      response.predecessor = population.id;
      response.successor = successor;
      response.successor_generation = created.generation;
      response.seed_candidate = created.seed_candidate;
      response.seed_candidate_generation = created.seed_candidate_generation;
      response.detail = "successor population created; a carried elite never inherits evidence";
      wake_pump();
      return reply(connection_id, MessageType::PopulationAdvanced, response);
    }
    case MessageType::RequestSelection: {
      RequestSelectionMessage message;
      AF_TRY_ASSIGN(message, decode_request_selection(frame.payload));
      PopulationRecord population;
      AF_TRY_ASSIGN(population, named_population(message.authority));
      SelectionDecision prepared;
      AF_TRY_ASSIGN(prepared, core->prepare_selection(population.id));
      // Commit revalidates the prepared decision against current state, so a
      // candidate that changed between preparation and commit cannot be
      // selected on the strength of a stale ranking.
      SelectionDecision committed;
      AF_TRY_ASSIGN(committed, core->commit_selection(prepared));
      AF_TRY(persist(*core));
      SelectionDecisionMessage response;
      response.decision = committed;
      response.committed = true;
      return reply(connection_id, MessageType::SelectionDecisionMessage, response);
    }
    case MessageType::RequestRetention: {
      RequestRetentionMessage message;
      AF_TRY_ASSIGN(message, decode_request_retention(frame.payload));
      PopulationRecord population;
      AF_TRY_ASSIGN(population, named_population(message.authority));
      RetentionDecision prepared;
      AF_TRY_ASSIGN(prepared, core->prepare_retention(population.id));
      RetentionDecision committed;
      AF_TRY_ASSIGN(committed, core->commit_retention(prepared));
      AF_TRY(persist(*core));
      RetentionDecisionMessage response;
      response.decision = committed;
      return reply(connection_id, MessageType::RetentionDecisionMessage, response);
    }
    case MessageType::RequestPromotion: {
      RequestPromotionMessage message;
      AF_TRY_ASSIGN(message, decode_request_promotion(frame.payload));
      PopulationRecord population;
      AF_TRY_ASSIGN(population, named_population(message.authority));
      PromotionRequest request;
      AF_TRY_ASSIGN(request, core->prepare_promotion_request(population.id));
      // The request is durable before it leaves this process, so a crash after
      // the handoff still leaves evidence of exactly what was handed over.
      AF_TRY(persist(*core));

      PromotionReceipt receipt;
      if (promotion_sink) {
        const Result<PromotionReceipt> submitted = promotion_sink->submit(request);
        if (submitted.ok()) {
          receipt = submitted.value();
        } else {
          receipt.handoff = PromotionHandoffState::SinkUnavailable;
          receipt.sink_name = promotion_sink->name();
          receipt.detail =
              "the promotion sink refused the handoff: " + submitted.status().message();
        }
      } else {
        receipt.handoff = PromotionHandoffState::SinkUnavailable;
        receipt.sink_name = "none";
        receipt.detail = "no promotion sink is installed; the request is durable but no adjacent "
                         "system has been told about it";
      }
      AF_TRY(core->record_promotion_receipt(request.id, receipt));
      AF_TRY(persist(*core));
      PromotionRequestMessage response;
      response.request = request;
      response.receipt = receipt;
      return reply(connection_id, MessageType::PromotionRequestMessage, response);
    }
    case MessageType::RequestRevalidation: {
      RequestRevalidationMessage message;
      AF_TRY_ASSIGN(message, decode_request_revalidation(frame.payload));
      PopulationRecord population;
      AF_TRY_ASSIGN(population, named_population(message.authority));
      AF_TRY(core->request_population_revalidation(population.id, message.reason));
      AF_TRY(persist(*core));
      const PopulationRecord current = core->population(population.id).value();
      RevalidationAppliedMessage response;
      response.population = current.id;
      response.generation = current.generation;
      response.state = current.state;
      response.detail = current.status_detail;
      return reply(connection_id, MessageType::RevalidationApplied, response);
    }
    case MessageType::CancelAttemptRequest: {
      CancelAttemptRequestMessage message;
      AF_TRY_ASSIGN(message, decode_cancel_attempt_request(frame.payload));
      PopulationRecord population;
      AF_TRY_ASSIGN(population, named_population(message.authority));
      (void)population;
      AF_TRY(core->cancel_attempt(message.attempt, message.reason));
      AF_TRY(persist(*core));
      const AttemptRecord cancelled = core->attempt(message.attempt).value();
      AttemptCancelledMessage response;
      response.attempt = cancelled.id;
      response.state = cancelled.state;
      response.detail = cancelled.failure_detail;
      wake_pump();
      return reply(connection_id, MessageType::AttemptCancelled, response);
    }
    case MessageType::QueryPopulation: {
      QueryPopulationMessage message;
      AF_TRY_ASSIGN(message, decode_query_population(frame.payload));
      PopulationRecord population;
      AF_TRY_ASSIGN(population, core->population(message.population));
      PopulationDetailMessage response;
      response.population = population;
      response.committed_selection_generation = population.committed_selection.value();
      response.retention_committed = population.retention_committed;
      response.candidate_count = population.candidates.size();
      response.closure_blockers = core->closure_blockers(population.id);
      return reply(connection_id, MessageType::PopulationDetail, response);
    }
    case MessageType::QueryCandidate: {
      QueryCandidateMessage message;
      AF_TRY_ASSIGN(message, decode_query_candidate(frame.payload));
      CandidateRecord candidate;
      AF_TRY_ASSIGN(candidate, core->candidate(message.candidate));
      CandidateDetailMessage response;
      response.candidate = candidate;
      response.evaluations = core->candidate_evaluations(candidate.id);
      return reply(connection_id, MessageType::CandidateDetail, response);
    }
    case MessageType::QueryLineage: {
      QueryLineageMessage message;
      AF_TRY_ASSIGN(message, decode_query_lineage(frame.payload));
      LineageDetailMessage response;
      const std::vector<LineageNode> nodes = core->lineage_nodes();
      if (!message.root.valid()) {
        response.nodes = nodes;
        return reply(connection_id, MessageType::LineageDetail, response);
      }
      // A named root returns its ancestry root first and does not repeat the
      // candidate itself, which is the same shape a promotion request uses, so
      // an operator reads one provenance shape everywhere.
      CandidateRecord root;
      AF_TRY_ASSIGN(root, core->candidate(message.root));
      std::vector<CandidateId> path;
      CandidateId cursor = root.id;
      for (std::size_t guard = 0; guard <= kMaxLineageDepth; ++guard) {
        path.push_back(cursor);
        const auto node = std::find_if(
            nodes.begin(), nodes.end(),
            [cursor](const LineageNode& candidate) { return candidate.candidate == cursor; });
        if (node == nodes.end() || node->parents.empty()) {
          break;
        }
        cursor = node->parents.front();
      }
      std::reverse(path.begin(), path.end());
      // The path was built from the candidate upwards, so the first element is
      // the candidate itself.
      if (!path.empty() && path.front() == root.id) {
        path.erase(path.begin());
      }
      for (const CandidateId id : path) {
        const auto node = std::find_if(
            nodes.begin(), nodes.end(),
            [id](const LineageNode& candidate) { return candidate.candidate == id; });
        if (node == nodes.end()) {
          return Status(ErrorCode::LineageOrphan,
                        "the ancestry of " + root.id.to_string() + " names " + id.to_string() +
                            " which has no lineage node");
        }
        response.nodes.push_back(*node);
      }
      return reply(connection_id, MessageType::LineageDetail, response);
    }
    case MessageType::QueryStatistics: {
      QueryStatisticsMessage message;
      AF_TRY_ASSIGN(message, decode_query_statistics(frame.payload));
      StatisticsDetailMessage response;
      response.statistics = core->statistics();
      response.revision = core->revision();
      response.epoch = core->epoch();
      response.run = core->run();
      return reply(connection_id, MessageType::StatisticsDetail, response);
    }
    case MessageType::QueryAudit: {
      QueryAuditMessage message;
      AF_TRY_ASSIGN(message, decode_query_audit(frame.payload));
      AuditDetailMessage response;
      response.violations = core->audit();
      return reply(connection_id, MessageType::AuditDetail, response);
    }
    case MessageType::ShutdownCoordinator: {
      ShutdownCoordinatorMessage message;
      AF_TRY_ASSIGN(message, decode_shutdown_coordinator(frame.payload));
      // The protocol has no distinct shutdown acknowledgement body: the
      // acknowledgement is the ControlAck body sent under ShutdownAck.
      ControlAckMessage ack;
      ack.ok = true;
      ack.code = ErrorCode::Ok;
      ack.detail = "shutdown accepted";
      AF_TRY(reply(connection_id, MessageType::ShutdownAck, ack));
      // The acknowledgement is queued first and only then is the shutdown
      // requested. request_shutdown never joins a thread, so this is safe to run
      // on a connection reader thread.
      {
        const std::lock_guard<std::mutex> guard(lifecycle_mutex);
        shutdown_requested = true;
        if (shutdown_reason.empty()) {
          shutdown_reason = "shutdown requested by a controller";
        }
      }
      lifecycle_wake.notify_all();
      return Status();
    }
    default:
      return Status(ErrorCode::ProtocolViolation,
                    "message type '" + std::string(message_type_name(frame.header.type)) +
                        "' is not a controller message this coordinator accepts");
  }
}

}  // namespace autonomous_foundry
