// src/retention.cpp
//
// Retention vocabulary.
//
// Selection answers "which candidate may advance". Retention answers "which
// alternatives stay worth keeping". Every retention entry records which of the
// two questions produced it, so a candidate that was kept because it won is
// never confused with one that was kept because the policy wanted diversity.

#include "autonomous_foundry/retention.hpp"

#include <cstdint>
#include <string_view>

namespace autonomous_foundry {

std::string_view retention_outcome_name(RetentionOutcome outcome) noexcept {
  switch (outcome) {
    case RetentionOutcome::Retained:
      return "Retained";
    case RetentionOutcome::RetainedBecauseSelected:
      return "RetainedBecauseSelected";
    case RetentionOutcome::RetainedForLineageDiversity:
      return "RetainedForLineageDiversity";
    case RetentionOutcome::RetiredRankedBelowCut:
      return "RetiredRankedBelowCut";
    case RetentionOutcome::RetiredLineageCap:
      return "RetiredLineageCap";
    case RetentionOutcome::RetiredCapacityLimit:
      return "RetiredCapacityLimit";
    case RetentionOutcome::RetiredIneligible:
      return "RetiredIneligible";
  }
  return "Unknown";
}

}  // namespace autonomous_foundry
