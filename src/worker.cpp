#include "autonomous_foundry/worker.hpp"

#include <string>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/limits.hpp"

namespace autonomous_foundry {

std::string_view worker_state_name(WorkerState state) noexcept {
  switch (state) {
    case WorkerState::Connecting:
      return "Connecting";
    case WorkerState::Registered:
      return "Registered";
    case WorkerState::Ready:
      return "Ready";
    case WorkerState::Busy:
      return "Busy";
    case WorkerState::RevalidationRequired:
      return "RevalidationRequired";
    case WorkerState::Draining:
      return "Draining";
    case WorkerState::Offline:
      return "Offline";
    case WorkerState::Rejected:
      return "Rejected";
  }
  // An ordinal outside the declared range is not a worker state. It renders as
  // the documented fallback name rather than being silently indexed.
  return "Unknown";
}

std::string_view reference_strategy_name(ReferenceStrategy strategy) noexcept {
  switch (strategy) {
    case ReferenceStrategy::ClosedForm:
      return "closed-form";
    case ReferenceStrategy::Iterative:
      return "iterative";
    case ReferenceStrategy::OffByOne:
      return "off-by-one";
    case ReferenceStrategy::SelfReportedPass:
      return "self-reported-pass";
  }
  return "unknown-strategy";
}

Status validate_worker_text(std::string_view text, std::string_view field) {
  const std::string name(field);
  if (text.size() > kMaxNameLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  name + " exceeds the maximum length of " + std::to_string(kMaxNameLength) +
                      " bytes");
  }
  // Printable ASCII only. A label that carries control characters would be
  // written into diagnostics, logs and the durable snapshot, so it is refused
  // here rather than escaped later in several places.
  for (const char character : text) {
    const unsigned char value = static_cast<unsigned char>(character);
    if (value < 0x20u || value > 0x7Eu) {
      return Status(ErrorCode::InvalidArgument,
                    name + " contains a byte that is not printable ASCII");
    }
  }
  return Status();
}

Result<ReferenceStrategy> parse_reference_strategy(std::string_view text) {
  if (text.empty()) {
    return Status(ErrorCode::InvalidArgument, "reference strategy name is empty");
  }
  if (text.size() > kMaxNameLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  "reference strategy name exceeds the maximum length");
  }

  // Two accepted spellings: the protocol name and the C++ enumerator name.
  // Accepting both keeps the worker command line readable without making the
  // wire contract ambiguous.
  if (text == "closed-form" || text == "ClosedForm") {
    return ReferenceStrategy::ClosedForm;
  }
  if (text == "iterative" || text == "Iterative") {
    return ReferenceStrategy::Iterative;
  }
  if (text == "off-by-one" || text == "OffByOne") {
    return ReferenceStrategy::OffByOne;
  }
  if (text == "self-reported-pass" || text == "SelfReportedPass") {
    return ReferenceStrategy::SelfReportedPass;
  }
  return Status(ErrorCode::InvalidArgument,
                "unknown reference strategy '" + std::string(text) +
                    "'; expected one of closed-form, iterative, off-by-one, self-reported-pass");
}

}  // namespace autonomous_foundry
