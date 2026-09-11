// src/task.cpp
//
// The durable task contract: requirement lookup, canonical encoding, digest and
// full structural validation.
//
// Canonical encoding
// ------------------
//
// Every canonical encoding produced by this file is an explicit, field-tagged,
// length-prefixed byte string of the form
//
//     <field-name> '=' <decimal value length> ':' <raw value bytes> ';'
//
// Field names are fixed ASCII tags that never contain '=', ':' or ';'. The
// length counts raw value bytes, so a value may contain any byte without making
// the encoding ambiguous. Nothing is escaped and nothing is normalised: the
// encoding is a pure function of the logical value, and no two different
// logical values can produce the same bytes.
//
// Doubles are emitted as 16 lowercase hexadecimal digits of their IEEE-754
// binary64 representation, most significant byte first. The raw bit pattern is
// exact (no decimal rounding, no locale) and negative zero is folded onto
// positive zero because the two compare equal.
//
// Enumerator fields are emitted as their decimal ordinal, so even a value that
// arrived out of range from a snapshot encodes distinctly.
//
// A vector is emitted as a decimal count field followed by exactly that many
// element groups; a group is the fixed sequence of fields documented below.
//
// Field sequence emitted by encode_task_canonical. TaskSpec::content_digest is
// deliberately absent: the digest is the output of hashing this encoding, so
// including it would be circular.
//
//   task_id                      decimal raw identity
//   task_generation              decimal generation value
//   name                         raw bytes
//   objective                    raw bytes
//   input_count                  decimal
//   input_name                   raw bytes   } repeated input_count times, in
//   input_content                raw bytes   } ascending (name, content length,
//   input_content_digest         raw bytes   } content, digest) order
//   required_output_count        decimal
//   required_output_name         raw bytes   } repeated, ascending byte order
//   requirement_count            decimal
//   requirement_class            decimal ordinal } repeated in declared order,
//   requirement_key              raw bytes         because the declared order
//   requirement_description      raw bytes         is the order in which the
//   requirement_has_score_floor  decimal 0|1       runtime demands and reports
//   requirement_score_floor      binary64 hex      evidence
//   requirement_ranking_weight   binary64 hex
//   hard_constraint_count        decimal
//   hard_constraint              raw bytes   } repeated in declared order
//   closure_criterion_count      decimal
//   closure_criterion            raw bytes   } repeated in declared order
//   policy_id                    decimal raw identity
//   policy_generation            decimal generation value
//   budget_max_candidate_attempts      decimal } the nine BudgetLimits fields,
//   ...                                       } in declaration order
//   budget_max_evaluation_concurrency  decimal
//
// Inputs and required outputs are sets whose order must not matter, so a copy
// is sorted before emitting. Requirements, hard constraints and closure
// criteria are sequences whose order is part of the contract and are emitted
// exactly as declared.

#include "autonomous_foundry/task.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/accounting.hpp"
#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/limits.hpp"

namespace autonomous_foundry {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr std::size_t kBinary64HexDigits = 16;
constexpr std::uint64_t kNegativeZeroBits = 0x8000000000000000ull;

static_assert(sizeof(double) == sizeof(std::uint64_t),
              "canonical encoding requires IEEE-754 binary64 doubles");

void append_field(std::string& out, std::string_view name, std::string_view value) {
  out.append(name);
  out.push_back('=');
  out.append(std::to_string(value.size()));
  out.push_back(':');
  out.append(value);
  out.push_back(';');
}

void append_count(std::string& out, std::string_view name, std::size_t value) {
  append_field(out, name, std::to_string(value));
}

void append_u32(std::string& out, std::string_view name, std::uint32_t value) {
  append_field(out, name, std::to_string(value));
}

void append_u64(std::string& out, std::string_view name, std::uint64_t value) {
  append_field(out, name, std::to_string(value));
}

void append_bool(std::string& out, std::string_view name, bool value) {
  append_field(out, name, value ? "1" : "0");
}

void append_binary64(std::string& out, std::string_view name, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  if (bits == kNegativeZeroBits) {
    bits = 0;
  }
  std::string digits(kBinary64HexDigits, '0');
  for (std::size_t index = 0; index < digits.size(); ++index) {
    const unsigned shift = static_cast<unsigned>((digits.size() - 1 - index) * 4);
    digits[index] = kHexDigits[(bits >> shift) & 0x0Full];
  }
  append_field(out, name, digits);
}

std::string hex_byte(unsigned char value) {
  std::string text(2, '0');
  text[0] = kHexDigits[(value >> 4) & 0x0Fu];
  text[1] = kHexDigits[value & 0x0Fu];
  return text;
}

Status require_identity(bool valid, std::string_view field) {
  if (!valid) {
    return Status(ErrorCode::NullIdentity, std::string(field) + " is the null identity");
  }
  return Status();
}

Status require_generation(bool valid, std::string_view field) {
  if (!valid) {
    return Status(ErrorCode::InvalidArgument,
                  std::string(field) + " is generation 0, which is not a valid generation");
  }
  return Status();
}

/// The rule an evaluator key must satisfy to be usable as a stable lookup key.
/// Evaluation records apply exactly the same rule, so a key that a task accepts
/// is a key that evidence can be filed under.
Status validate_evaluator_key(std::string_view key, std::string_view field) {
  if (key.empty()) {
    return Status(ErrorCode::InvalidArgument, std::string(field) + " must not be empty");
  }
  if (key.size() > kMaxNameLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  std::string(field) + " is " + std::to_string(key.size()) +
                      " bytes long, which exceeds the maximum of " +
                      std::to_string(kMaxNameLength) + " bytes");
  }
  for (std::size_t index = 0; index < key.size(); ++index) {
    const char ch = key[index];
    const bool letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    const bool digit = ch >= '0' && ch <= '9';
    if (!(letter || digit || ch == '.' || ch == '_' || ch == '-')) {
      return Status(ErrorCode::InvalidArgument,
                    std::string(field) + " contains byte 0x" +
                        hex_byte(static_cast<unsigned char>(ch)) + " at index " +
                        std::to_string(index) +
                        "; only ASCII letters, digits, '.', '_' and '-' are allowed");
    }
  }
  return Status();
}

/// Total order over inputs. Names are unique in a validated task, but the
/// encoder must stay deterministic even for a task that never passed
/// validation, so length, content and digest break ties.
bool input_is_ordered_before(const InputFile* left, const InputFile* right) {
  if (left->name != right->name) {
    return left->name < right->name;
  }
  if (left->content.size() != right->content.size()) {
    return left->content.size() < right->content.size();
  }
  const int content_order = left->content.compare(right->content);
  if (content_order != 0) {
    return content_order < 0;
  }
  return left->content_digest < right->content_digest;
}

}  // namespace

std::string_view requirement_class_name(RequirementClass cls) noexcept {
  switch (cls) {
    case RequirementClass::Mandatory:
      return "Mandatory";
    case RequirementClass::Optional:
      return "Optional";
  }
  return "Unknown";
}

const EvaluationRequirement* TaskSpec::find_requirement(std::string_view key) const noexcept {
  for (const EvaluationRequirement& requirement : requirements) {
    if (std::string_view(requirement.evaluator_key) == key) {
      return &requirement;
    }
  }
  return nullptr;
}

std::size_t TaskSpec::mandatory_requirement_count() const noexcept {
  std::size_t count = 0;
  for (const EvaluationRequirement& requirement : requirements) {
    if (requirement.requirement_class == RequirementClass::Mandatory) {
      ++count;
    }
  }
  return count;
}

std::string encode_task_canonical(const TaskSpec& task) {
  std::string out;
  append_u64(out, "task_id", task.id.raw());
  append_u32(out, "task_generation", task.generation.value());
  append_field(out, "name", task.name);
  append_field(out, "objective", task.objective);

  std::vector<const InputFile*> ordered_inputs;
  ordered_inputs.reserve(task.inputs.size());
  for (const InputFile& input : task.inputs) {
    ordered_inputs.push_back(&input);
  }
  std::sort(ordered_inputs.begin(), ordered_inputs.end(), input_is_ordered_before);
  append_count(out, "input_count", ordered_inputs.size());
  for (const InputFile* input : ordered_inputs) {
    append_field(out, "input_name", input->name);
    append_field(out, "input_content", input->content);
    append_field(out, "input_content_digest", input->content_digest);
  }

  std::vector<std::string> ordered_outputs = task.required_outputs;
  std::sort(ordered_outputs.begin(), ordered_outputs.end());
  append_count(out, "required_output_count", ordered_outputs.size());
  for (const std::string& output : ordered_outputs) {
    append_field(out, "required_output_name", output);
  }

  append_count(out, "requirement_count", task.requirements.size());
  for (const EvaluationRequirement& requirement : task.requirements) {
    append_u32(out, "requirement_class",
               static_cast<std::uint32_t>(requirement.requirement_class));
    append_field(out, "requirement_key", requirement.evaluator_key);
    append_field(out, "requirement_description", requirement.description);
    append_bool(out, "requirement_has_score_floor", requirement.has_score_floor);
    append_binary64(out, "requirement_score_floor", requirement.score_floor);
    append_binary64(out, "requirement_ranking_weight", requirement.ranking_weight);
  }

  append_count(out, "hard_constraint_count", task.hard_constraints.size());
  for (const std::string& constraint : task.hard_constraints) {
    append_field(out, "hard_constraint", constraint);
  }

  append_count(out, "closure_criterion_count", task.closure_criteria.size());
  for (const std::string& criterion : task.closure_criteria) {
    append_field(out, "closure_criterion", criterion);
  }

  append_u64(out, "policy_id", task.policy.raw());
  append_u32(out, "policy_generation", task.policy_generation.value());

  const BudgetLimits& budgets = task.budgets;
  append_u32(out, "budget_max_candidate_attempts", budgets.max_candidate_attempts);
  append_u32(out, "budget_max_worker_assignments", budgets.max_worker_assignments);
  append_u32(out, "budget_max_evaluation_attempts", budgets.max_evaluation_attempts);
  append_u32(out, "budget_max_retained_candidates", budgets.max_retained_candidates);
  append_u32(out, "budget_max_retries", budgets.max_retries);
  append_u32(out, "budget_max_candidates", budgets.max_candidates);
  append_u32(out, "budget_max_workers", budgets.max_workers);
  append_u32(out, "budget_max_active_attempts", budgets.max_active_attempts);
  append_u32(out, "budget_max_evaluation_concurrency", budgets.max_evaluation_concurrency);

  return out;
}

Result<std::string> compute_task_digest(const TaskSpec& task) {
  const std::string encoding = encode_task_canonical(task);
  return sha256_hex(encoding);
}

Status validate_task(const TaskSpec& task) {
  AF_TRY(require_identity(task.id.valid(), "task id"));
  AF_TRY(require_generation(task.generation.valid(), "task generation"));

  if (task.name.empty() || task.name.size() > kMaxNameLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  "task name must be 1 to " + std::to_string(kMaxNameLength) +
                      " bytes long but is " + std::to_string(task.name.size()) + " bytes");
  }
  for (std::size_t index = 0; index < task.name.size(); ++index) {
    const unsigned char byte = static_cast<unsigned char>(task.name[index]);
    if (byte < 0x20u || byte > 0x7Eu) {
      return Status(ErrorCode::MalformedEncoding,
                    "task name contains byte 0x" + hex_byte(byte) + " at index " +
                        std::to_string(index) + "; only printable ASCII is accepted");
    }
  }

  if (task.objective.size() > kMaxObjectiveLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  "task objective is " + std::to_string(task.objective.size()) +
                      " bytes long, which exceeds the maximum of " +
                      std::to_string(kMaxObjectiveLength) + " bytes");
  }

  if (task.inputs.size() > kMaxInputFiles) {
    return Status(ErrorCode::LengthOutOfRange,
                  "task declares " + std::to_string(task.inputs.size()) +
                      " input files, which exceeds the maximum of " +
                      std::to_string(kMaxInputFiles));
  }
  for (std::size_t index = 0; index < task.inputs.size(); ++index) {
    const InputFile& input = task.inputs[index];
    const std::string context = "task input[" + std::to_string(index) + "]";
    const Status name_status = validate_artifact_name(input.name);
    if (!name_status.ok()) {
      return name_status.with_context(context);
    }
    if (input.content.size() > kMaxInputFileBytes) {
      return Status(ErrorCode::LengthOutOfRange,
                    context + " content is " + std::to_string(input.content.size()) +
                        " bytes long, which exceeds the maximum of " +
                        std::to_string(kMaxInputFileBytes) + " bytes");
    }
    const std::string expected_digest = sha256_hex(input.content);
    if (input.content_digest != expected_digest) {
      return Status(ErrorCode::MalformedEncoding,
                    context + " '" + input.name + "' declares content digest '" +
                        input.content_digest + "' but its content hashes to '" + expected_digest +
                        "'");
    }
    for (std::size_t other = 0; other < index; ++other) {
      if (task.inputs[other].name == input.name) {
        return Status(ErrorCode::DuplicateIdentity,
                      context + " repeats the input name '" + input.name +
                          "' already used by task input[" + std::to_string(other) + "]");
      }
    }
  }

  if (task.required_outputs.size() > kMaxRequiredOutputs) {
    return Status(ErrorCode::LengthOutOfRange,
                  "task declares " + std::to_string(task.required_outputs.size()) +
                      " required outputs, which exceeds the maximum of " +
                      std::to_string(kMaxRequiredOutputs));
  }
  for (std::size_t index = 0; index < task.required_outputs.size(); ++index) {
    const std::string& output = task.required_outputs[index];
    const std::string context = "task required output[" + std::to_string(index) + "]";
    const Status name_status = validate_artifact_name(output);
    if (!name_status.ok()) {
      return name_status.with_context(context);
    }
    for (std::size_t other = 0; other < index; ++other) {
      if (task.required_outputs[other] == output) {
        return Status(ErrorCode::DuplicateIdentity,
                      context + " repeats the required output '" + output +
                          "' already declared by task required output[" +
                          std::to_string(other) + "]");
      }
    }
  }

  if (task.requirements.size() > kMaxEvaluationRequirements) {
    return Status(ErrorCode::LengthOutOfRange,
                  "task declares " + std::to_string(task.requirements.size()) +
                      " evaluation requirements, which exceeds the maximum of " +
                      std::to_string(kMaxEvaluationRequirements));
  }
  for (std::size_t index = 0; index < task.requirements.size(); ++index) {
    const EvaluationRequirement& requirement = task.requirements[index];
    const std::string context = "task requirement[" + std::to_string(index) + "]";
    AF_TRY(validate_evaluator_key(requirement.evaluator_key, context + " evaluator key"));
    if (!std::isfinite(requirement.ranking_weight)) {
      return Status(ErrorCode::InvalidArgument,
                    context + " '" + requirement.evaluator_key +
                        "' has a non-finite ranking weight");
    }
    if (requirement.ranking_weight < 0.0) {
      return Status(ErrorCode::InvalidArgument,
                    context + " '" + requirement.evaluator_key +
                        "' has a negative ranking weight");
    }
    if (requirement.has_score_floor && !std::isfinite(requirement.score_floor)) {
      return Status(ErrorCode::InvalidArgument,
                    context + " '" + requirement.evaluator_key +
                        "' declares a score floor that is not finite");
    }
    for (std::size_t other = 0; other < index; ++other) {
      if (task.requirements[other].evaluator_key == requirement.evaluator_key) {
        return Status(ErrorCode::DuplicateIdentity,
                      context + " repeats the evaluator key '" + requirement.evaluator_key +
                          "' already used by task requirement[" + std::to_string(other) + "]");
      }
    }
  }

  if (task.hard_constraints.size() > kMaxConstraints) {
    return Status(ErrorCode::LengthOutOfRange,
                  "task declares " + std::to_string(task.hard_constraints.size()) +
                      " hard constraints, which exceeds the maximum of " +
                      std::to_string(kMaxConstraints));
  }

  if (task.closure_criteria.size() > kMaxConstraints) {
    return Status(ErrorCode::LengthOutOfRange,
                  "task declares " + std::to_string(task.closure_criteria.size()) +
                      " closure criteria, which exceeds the maximum of " +
                      std::to_string(kMaxConstraints));
  }
  for (std::size_t index = 0; index < task.closure_criteria.size(); ++index) {
    if (task.closure_criteria[index].size() > kMaxNameLength) {
      return Status(ErrorCode::LengthOutOfRange,
                    "task closure criterion[" + std::to_string(index) + "] is " +
                        std::to_string(task.closure_criteria[index].size()) +
                        " bytes long, which exceeds the maximum of " +
                        std::to_string(kMaxNameLength) + " bytes");
    }
  }

  AF_TRY(require_identity(task.policy.valid(), "task policy id"));
  AF_TRY(require_generation(task.policy_generation.valid(), "task policy generation"));
  AF_TRY(task.budgets.validate());
  return Status();
}

bool tasks_are_compatible(const TaskSpec& a, const TaskSpec& b) noexcept {
  return a.id == b.id && a.generation == b.generation && a.content_digest == b.content_digest;
}

}  // namespace autonomous_foundry
