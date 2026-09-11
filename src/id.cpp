#include "autonomous_foundry/id.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace autonomous_foundry {

namespace {

std::uint64_t process_identifier() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

// splitmix64 finalizer: every entropy source is diffused before it is combined
// so that a weak source cannot leave structure in the low bits.
std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::uint64_t entropy64() {
  std::random_device device;
  std::uint64_t mixed = (static_cast<std::uint64_t>(device()) << 32) |
                        static_cast<std::uint64_t>(device());
  const auto ticks =
      static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  mixed ^= mix64(ticks);
  mixed ^= mix64(process_identifier());
  const auto address_sample = reinterpret_cast<std::uintptr_t>(&mixed);
  mixed ^= mix64(static_cast<std::uint64_t>(address_sample));
  return mix64(mixed);
}

}  // namespace

std::string_view id_kind_name(IdKind kind) noexcept {
  switch (kind) {
    case IdKind::None:
      return "None";
    case IdKind::Foundry:
      return "Foundry";
    case IdKind::FoundryRun:
      return "FoundryRun";
    case IdKind::Population:
      return "Population";
    case IdKind::Task:
      return "Task";
    case IdKind::Candidate:
      return "Candidate";
    case IdKind::Lineage:
      return "Lineage";
    case IdKind::Worker:
      return "Worker";
    case IdKind::WorkerBoot:
      return "WorkerBoot";
    case IdKind::Attempt:
      return "Attempt";
    case IdKind::Assignment:
      return "Assignment";
    case IdKind::Evaluation:
      return "Evaluation";
    case IdKind::Evaluator:
      return "Evaluator";
    case IdKind::Policy:
      return "Policy";
    case IdKind::Artifact:
      return "Artifact";
    case IdKind::PromotionRequest:
      return "PromotionRequest";
    case IdKind::Session:
      return "Session";
    case IdKind::Controller:
      return "Controller";
    default:
      return "IdKindUnknown";
  }
}

std::string CoordinatorEpoch::to_string() const { return std::to_string(value_); }

std::uint32_t make_id_salt() {
  const std::uint32_t salt =
      static_cast<std::uint32_t>(entropy64() & static_cast<std::uint64_t>(detail::kIdSaltMask));
  return salt == 0 ? 1u : salt;
}

WorkerBootId make_worker_boot_id() {
  const std::uint64_t kind_bits =
      static_cast<std::uint64_t>(IdKind::WorkerBoot) << detail::kIdKindShift;
  std::uint64_t entropy_bits = entropy64() & 0x00FFFFFFFFFFFFFFull;
  if (entropy_bits == 0) {
    entropy_bits = 1;
  }
  return WorkerBootId::from_raw(kind_bits | entropy_bits);
}

FoundryRunId make_foundry_run_id() {
  const std::uint64_t salt_bits =
      static_cast<std::uint64_t>(make_id_salt()) << detail::kIdSaltShift;
  std::uint64_t counter_bits = entropy64() & detail::kIdCounterMask;
  if (counter_bits == 0) {
    counter_bits = 1;
  }
  const std::uint64_t kind_bits =
      static_cast<std::uint64_t>(IdKind::FoundryRun) << detail::kIdKindShift;
  return FoundryRunId::from_raw(kind_bits | salt_bits | counter_bits);
}

IdAllocator::IdAllocator(std::uint32_t salt) noexcept
    : salt_(salt & static_cast<std::uint32_t>(detail::kIdSaltMask)) {}

Status IdAllocator::restore_counter(IdKind kind, std::uint32_t value) {
  const auto index = static_cast<std::size_t>(kind);
  if (index == 0 || index >= counters_.size()) {
    return Status(ErrorCode::Internal,
                  std::string_view("identity counter restored for an unknown domain"));
  }
  if (value < counters_[index]) {
    return Status(ErrorCode::GenerationRegression,
                  std::string("identity counter for domain '") + std::string(id_kind_name(kind)) +
                      "' would regress from " + std::to_string(counters_[index]) + " to " +
                      std::to_string(value));
  }
  counters_[index] = value;
  return Status();
}

Status IdAllocator::adopt_salt(std::uint32_t salt) noexcept {
  for (const std::uint32_t counter : counters_) {
    if (counter != 0) {
      return Status(
          ErrorCode::IllegalStateTransition,
          std::string_view("the identity salt cannot change after identities have been issued"));
    }
  }
  salt_ = salt & static_cast<std::uint32_t>(detail::kIdSaltMask);
  return Status();
}

Result<std::uint64_t> parse_raw_identity(std::string_view text) {
  if (text.empty()) {
    return Status(ErrorCode::InvalidArgument, std::string_view("identity text is empty"));
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return Status(ErrorCode::InvalidArgument,
                    std::string("identity text '") + std::string(text) +
                        "' is not a run of decimal digits");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10u) {
      return Status(ErrorCode::InvalidArgument,
                    std::string("identity text '") + std::string(text) +
                        "' does not fit in a 64-bit identity");
    }
    value = (value * 10u) + digit;
  }
  if (value == 0) {
    return Status(ErrorCode::InvalidArgument,
                  std::string("identity text '") + std::string(text) +
                      "' is the null identity");
  }
  return value;
}

}  // namespace autonomous_foundry
