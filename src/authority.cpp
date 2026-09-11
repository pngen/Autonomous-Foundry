#include "autonomous_foundry/authority.hpp"

#include <string>
#include <string_view>

namespace autonomous_foundry {

namespace {

std::string authority_mismatch(std::string_view field, std::string_view expected,
                               std::string_view actual) {
  std::string text("authority field '");
  text.append(field);
  text.append("' expected '");
  text.append(expected);
  text.append("' but current state is '");
  text.append(actual);
  text.push_back('\'');
  return text;
}

}  // namespace

Status check_coordinator_epoch(CoordinatorEpoch expected, CoordinatorEpoch actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(ErrorCode::StaleCoordinatorEpoch,
                authority_mismatch("coordinator_epoch", expected.to_string(), actual.to_string()));
}

Status check_run(FoundryRunId expected, FoundryRunId actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(ErrorCode::StaleAuthority,
                authority_mismatch("run", expected.to_string(), actual.to_string()));
}

Status check_worker_boot(WorkerBootId expected, WorkerBootId actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(ErrorCode::StaleWorkerBoot,
                authority_mismatch("worker_boot", expected.to_string(), actual.to_string()));
}

Status check_session(SessionId expected, SessionId actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(ErrorCode::StaleSession,
                authority_mismatch("session", expected.to_string(), actual.to_string()));
}

Status check_session_generation(WorkerSessionGeneration expected, WorkerSessionGeneration actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(
      ErrorCode::StaleSession,
      authority_mismatch("session_generation", expected.to_string(), actual.to_string()));
}

Status check_population_generation(PopulationGeneration expected, PopulationGeneration actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(
      ErrorCode::StalePopulationGeneration,
      authority_mismatch("population_generation", expected.to_string(), actual.to_string()));
}

Status check_task_generation(TaskGeneration expected, TaskGeneration actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(ErrorCode::StaleTaskGeneration,
                authority_mismatch("task_generation", expected.to_string(), actual.to_string()));
}

Status check_candidate_generation(CandidateGeneration expected, CandidateGeneration actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(
      ErrorCode::StaleCandidateGeneration,
      authority_mismatch("candidate_generation", expected.to_string(), actual.to_string()));
}

Status check_evaluation_generation(EvaluationGeneration expected, EvaluationGeneration actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(
      ErrorCode::StaleEvaluationGeneration,
      authority_mismatch("evaluation_generation", expected.to_string(), actual.to_string()));
}

Status check_policy_generation(PolicyGeneration expected, PolicyGeneration actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(
      ErrorCode::StalePolicyGeneration,
      authority_mismatch("policy_generation", expected.to_string(), actual.to_string()));
}

Status check_attempt_generation(AttemptGeneration expected, AttemptGeneration actual) {
  if (expected == actual) {
    return Status();
  }
  return Status(ErrorCode::StaleAttempt,
                authority_mismatch("attempt_generation", expected.to_string(), actual.to_string()));
}

Status check_canonical_digest(std::string_view expected, std::string_view actual) {
  if (expected == actual) {
    return Status();
  }
  if (expected.empty()) {
    return Status(
        ErrorCode::StaleSelection,
        std::string("authority field 'canonical_state_digest' carried no state digest but current "
                    "state is '") +
            std::string(actual) + "'");
  }
  return Status(ErrorCode::StaleSelection,
                std::string("authority field 'canonical_state_digest' expected '") +
                    std::string(expected) + "' but current state is '" + std::string(actual) + "'");
}

}  // namespace autonomous_foundry
