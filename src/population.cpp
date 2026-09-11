// src/population.cpp
//
// Population lifecycle.
//
// A population is a durable runtime object with its own closure contract, not a
// loop counter. This file owns the pure part of that contract: the stable
// enumerator spellings, which states are terminal, and which lifecycle edges
// exist. The authority that actually performs a transition lives in the foundry
// core; the legality table here is the single source of truth it consults.

#include "autonomous_foundry/population.hpp"

#include <cstdint>
#include <string_view>

namespace autonomous_foundry {

std::string_view population_state_name(PopulationState state) noexcept {
  switch (state) {
    case PopulationState::Created:
      return "Created";
    case PopulationState::Ready:
      return "Ready";
    case PopulationState::Running:
      return "Running";
    case PopulationState::Evaluating:
      return "Evaluating";
    case PopulationState::Selecting:
      return "Selecting";
    case PopulationState::Advancing:
      return "Advancing";
    case PopulationState::RevalidationRequired:
      return "RevalidationRequired";
    case PopulationState::Closing:
      return "Closing";
    case PopulationState::Closed:
      return "Closed";
    case PopulationState::Failed:
      return "Failed";
    case PopulationState::Cancelled:
      return "Cancelled";
  }
  return "Unknown";
}

bool population_state_is_terminal(PopulationState state) noexcept {
  switch (state) {
    case PopulationState::Closed:
    case PopulationState::Failed:
    case PopulationState::Cancelled:
      return true;
    case PopulationState::Created:
    case PopulationState::Ready:
    case PopulationState::Running:
    case PopulationState::Evaluating:
    case PopulationState::Selecting:
    case PopulationState::Advancing:
    case PopulationState::RevalidationRequired:
    case PopulationState::Closing:
      return false;
  }
  return false;
}

bool population_transition_is_legal(PopulationState from, PopulationState to) noexcept {
  // A self transition is always legal: it is how a caller records "unchanged"
  // without inventing a state.
  if (from == to) {
    return true;
  }
  switch (from) {
    case PopulationState::Created:
      switch (to) {
        case PopulationState::Ready:
        case PopulationState::Cancelled:
        case PopulationState::Failed:
          return true;
        default:
          return false;
      }
    case PopulationState::Ready:
      switch (to) {
        case PopulationState::Running:
        case PopulationState::Cancelled:
        case PopulationState::Failed:
        case PopulationState::RevalidationRequired:
          return true;
        default:
          return false;
      }
    case PopulationState::Running:
      switch (to) {
        case PopulationState::Evaluating:
        case PopulationState::Selecting:
        case PopulationState::RevalidationRequired:
        case PopulationState::Closing:
        case PopulationState::Cancelled:
        case PopulationState::Failed:
          return true;
        default:
          return false;
      }
    case PopulationState::Evaluating:
      switch (to) {
        case PopulationState::Selecting:
        case PopulationState::Running:
        case PopulationState::RevalidationRequired:
        case PopulationState::Closing:
        case PopulationState::Cancelled:
        case PopulationState::Failed:
          return true;
        default:
          return false;
      }
    case PopulationState::Selecting:
      switch (to) {
        case PopulationState::Advancing:
        case PopulationState::Closing:
        case PopulationState::Running:
        case PopulationState::RevalidationRequired:
        case PopulationState::Cancelled:
        case PopulationState::Failed:
          return true;
        default:
          return false;
      }
    case PopulationState::Advancing:
      switch (to) {
        // Advancing means the selection for this generation is committed. The
        // population may legitimately run another selection round, either
        // because new evidence arrived or because the caller re-derives the
        // decision; the edge must exist for that to be expressible.
        case PopulationState::Selecting:
        case PopulationState::Running:
        case PopulationState::Closing:
        case PopulationState::RevalidationRequired:
        case PopulationState::Failed:
          return true;
        default:
          return false;
      }
    case PopulationState::RevalidationRequired:
      switch (to) {
        case PopulationState::Running:
        case PopulationState::Evaluating:
        case PopulationState::Selecting:
        case PopulationState::Closing:
        case PopulationState::Failed:
        case PopulationState::Cancelled:
          return true;
        default:
          return false;
      }
    case PopulationState::Closing:
      switch (to) {
        case PopulationState::Closed:
        case PopulationState::Failed:
          return true;
        default:
          return false;
      }
    case PopulationState::Closed:
    case PopulationState::Failed:
    case PopulationState::Cancelled:
      // Terminal populations are immutable. A closed population is never
      // reopened, because the decision that closed it is already durable.
      return false;
  }
  return false;
}

}  // namespace autonomous_foundry
