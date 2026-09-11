#include "autonomous_foundry/attempt.hpp"

#include "autonomous_foundry/error.hpp"

namespace autonomous_foundry {

std::string_view attempt_state_name(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::Created:
      return "Created";
    case AttemptState::Authorized:
      return "Authorized";
    case AttemptState::Dispatched:
      return "Dispatched";
    case AttemptState::Running:
      return "Running";
    case AttemptState::Published:
      return "Published";
    case AttemptState::Evaluating:
      return "Evaluating";
    case AttemptState::Completed:
      return "Completed";
    case AttemptState::Failed:
      return "Failed";
    case AttemptState::Cancelled:
      return "Cancelled";
    case AttemptState::OutcomeUnknown:
      return "OutcomeUnknown";
  }
  // An ordinal outside the declared range is not an attempt state. It renders
  // as the documented fallback name rather than being silently indexed.
  return "Unknown";
}

bool attempt_state_is_terminal(AttemptState state) noexcept {
  // OutcomeUnknown is terminal by design: it is the foundry's final answer
  // when it genuinely cannot determine whether the work completed. Treating it
  // as non-terminal would invite a later writer to overwrite an ambiguous
  // outcome with an invented one.
  switch (state) {
    case AttemptState::Completed:
    case AttemptState::Failed:
    case AttemptState::Cancelled:
    case AttemptState::OutcomeUnknown:
      return true;
    case AttemptState::Created:
    case AttemptState::Authorized:
    case AttemptState::Dispatched:
    case AttemptState::Running:
    case AttemptState::Published:
    case AttemptState::Evaluating:
      return false;
  }
  return true;
}

}  // namespace autonomous_foundry
