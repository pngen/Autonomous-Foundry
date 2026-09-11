#pragma once

// Test harness support for the Autonomous Foundry validation suites.
//
// The harness contract this file implements:
//
//   * every suite is a named group of cases, registered statically;
//   * every case emits exactly four marker shapes, one per line:
//       BEGIN <suite>::<case>
//       PHASE <suite>::<case> <PHASE_NAME>
//       PASS  <suite>::<case>
//       FAIL  <suite>::<case>: <reason>
//   * stdout is unbuffered and every marker line is flushed immediately, so the
//     last marker produced survives a case that never returns;
//   * a failure is reported by throwing TestFailure, which carries file, line
//     and message;
//   * cases are independent: every case builds its own state.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <type_traits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/process.hpp"
#include "autonomous_foundry/workspace.hpp"

namespace af_test {

// ---------------------------------------------------------------------------
// Names used by the expectation macros
// ---------------------------------------------------------------------------

/// The expectation macros expand inside the case body, so the names they
/// mention must be reachable there. Making them reachable at global scope lets
/// a case body read the same in every suite.
using autonomous_foundry::ErrorCode;
using autonomous_foundry::Result;
using autonomous_foundry::Status;

// ---------------------------------------------------------------------------
// Failure reporting
// ---------------------------------------------------------------------------

/// Thrown by every expectation helper. Carries the source location that
/// produced the failure plus a human readable reason.
class TestFailure final : public std::runtime_error {
 public:
  TestFailure(std::string file, int line, std::string message);

  [[nodiscard]] const std::string& file() const noexcept { return file_; }
  [[nodiscard]] int line() const noexcept { return line_; }

 private:
  std::string file_;
  int line_;
};

// ---------------------------------------------------------------------------
// Message building
// ---------------------------------------------------------------------------

/// Lazy string builder. Concatenation with the built-in string types never
/// copies until the failure message is actually needed, so a failing
/// expectation cannot allocate its way out of reporting.
class Msg {
 public:
  Msg() = default;
  Msg(const char* text) { stream_ << text; }                 // NOLINT(google-explicit-constructor)
  Msg(std::string_view text) { stream_ << text; }            // NOLINT(google-explicit-constructor)
  Msg(const std::string& text) { stream_ << text; }          // NOLINT(google-explicit-constructor)

  [[nodiscard]] Msg& append(std::string_view text) {
    stream_ << text;
    return *this;
  }

  [[nodiscard]] std::string str() const { return stream_.str(); }

  friend Msg operator+(Msg left, const Msg& right) {
    left.stream_ << right.stream_.str();
    return left;
  }

  template <typename T>
  friend Msg operator+(Msg left, const T& value) {
    left.stream_ << value;
    return left;
  }

  template <typename T>
  friend Msg operator+(const T& value, Msg right) {
    Msg combined;
    combined.stream_ << value << right.stream_.str();
    return combined;
  }

 private:
  std::ostringstream stream_;
};

namespace detail {

/// Detect at compile time whether a value can actually be written to a stream.
/// Identity and generation types deliberately provide no stream operator, so
/// their description falls back to an explicit non-streaming form.
template <typename T, typename = void>
struct has_stream_output : std::false_type {};

template <typename T>
struct has_stream_output<T, std::void_t<decltype(std::declval<std::ostream&>()
                                                 << std::declval<const T&>())>>
    : std::true_type {};

}  // namespace detail

[[nodiscard]] std::string describe(bool value);
[[nodiscard]] std::string describe(float value);
[[nodiscard]] std::string describe(double value);
[[nodiscard]] std::string describe(autonomous_foundry::ErrorCode value);
[[nodiscard]] std::string describe(const autonomous_foundry::Status& value);

/// std::optional is described through its contained value. The standard
/// stream insertion for optional is not usable here: it hard-errors rather than
/// substituting out for a payload with no stream operator.
template <typename T>
[[nodiscard]] std::string describe(const std::optional<T>& value) {
  if (!value.has_value()) {
    return std::string("none");
  }
  return std::string("some(") + describe(*value) + ")";
}

/// Fallback description for a type without a stream operator. Identity and
/// generation values are described through their own explicit accessors rather
/// than being silently unprintable.
template <typename T>
[[nodiscard]] std::string describe_opaque(const T& value) {
  if constexpr (requires(const T& probe) { probe.value(); }) {
    return std::to_string(value.value());
  } else {
    return std::string("<unprintable value>");
  }
}

/// Enum-like values without a documented textual name.
template <typename T>
[[nodiscard]] std::string describe_ordinal(T value) {
  return std::to_string(static_cast<unsigned long long>(value));
}

template <typename T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (detail::has_stream_output<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return describe_opaque(value);
  }
}

[[nodiscard]] bool status_code_is_one_of(
    autonomous_foundry::ErrorCode code,
    std::initializer_list<autonomous_foundry::ErrorCode> allowed);

// ---------------------------------------------------------------------------
// Marker output
// ---------------------------------------------------------------------------

/// Marker writer. Every line is written to stdout and flushed in the same
/// call, so a subsequent hang cannot swallow the last marker.
class MarkerSink {
 public:
  static MarkerSink& instance();

  void line(std::string_view text);
  void phase(std::string_view case_name, std::string_view phase_name);

  [[nodiscard]] std::uint64_t lines_written() const noexcept;

 private:
  MarkerSink() = default;
  std::atomic<std::uint64_t> lines_{0};
};

// ---------------------------------------------------------------------------
// Case context
// ---------------------------------------------------------------------------

/// Per-case helpers handed to a case body. The context knows the fully
/// qualified case name so phase markers need no further qualification.
class TestContext {
 public:
  TestContext(std::string qualified_name, std::string source_file);

  [[nodiscard]] const std::string& name() const noexcept { return qualified_name_; }
  [[nodiscard]] const std::string& source_file() const noexcept { return source_file_; }

  /// Emit "PHASE <suite>::<case> <phase_name>" immediately.
  void phase(std::string_view phase_name) const;

  /// Emit an explicit note line that is not one of the four markers, for
  /// UNSUPPORTED and similar environment statements. Always flushed.
  void note(std::string_view text) const;

  [[noreturn]] void fail_at(std::string file, int line, std::string message) const;

  [[nodiscard]] std::uint64_t phase_count() const noexcept { return phase_count_; }

 private:
  std::string qualified_name_;
  std::string source_file_;
  mutable std::uint64_t phase_count_{0};
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

struct CaseEntry {
  std::string suite;
  std::string name;
  std::string file;
  void (*body)(TestContext&) = nullptr;
};

/// Registration is static: this constructor runs during dynamic
/// initialization of its translation unit.
class CaseRegistrar {
 public:
  CaseRegistrar(const char* suite, const char* name, const char* file,
                void (*body)(TestContext&));
};

[[nodiscard]] std::vector<CaseEntry>& registry();

}  // namespace af_test

// ---------------------------------------------------------------------------
// Registration macro
// ---------------------------------------------------------------------------

#define AF_TEST_CASE(suite_name, case_name)                                        \
  void af_case_body_##suite_name##_##case_name(::af_test::TestContext& af_ctx);    \
  const ::af_test::CaseRegistrar af_case_registrar_##suite_name##_##case_name(     \
      #suite_name, #case_name, __FILE__, &af_case_body_##suite_name##_##case_name); \
  void af_case_body_##suite_name##_##case_name(::af_test::TestContext& af_ctx)

// ---------------------------------------------------------------------------
// Expectation macros
// ---------------------------------------------------------------------------

// C4127 fires when a case asserts on a compile-time constant expression, which
// is a legitimate thing for a test to do, so it is suspended for the guard and
// restored immediately afterwards.
#define EXPECT_TRUE(af_context, expr)                                                      \
  do {                                                                                     \
    __pragma(warning(push)) __pragma(warning(disable : 4127))                              \
    if (!(expr)) {                                                                         \
      (af_context).fail_at(__FILE__, __LINE__,                                             \
                           std::string("EXPECT_TRUE(" #expr ") evaluated false"));         \
    }                                                                                      \
    __pragma(warning(pop))                                                                 \
  } while (false)

#define EXPECT_FALSE(af_context, expr)                                                     \
  do {                                                                                     \
    __pragma(warning(push)) __pragma(warning(disable : 4127))                              \
    if ((expr)) {                                                                          \
      (af_context).fail_at(__FILE__, __LINE__,                                             \
                           std::string("EXPECT_FALSE(" #expr ") evaluated true"));         \
    }                                                                                      \
    __pragma(warning(pop))                                                                 \
  } while (false)

// Both operands are materialised before they are compared. The left operand is
// bound by reference because it is always an lvalue the case already owns; the
// right operand is copied into a named value, because an expression such as
// "load_candidate(...).artifacts.front().size_bytes" yields a subobject of a
// temporary returned by value. Binding that subobject directly to a reference
// leaves the reference observing storage the compiler is free to reuse once the
// initialising full-expression ends, which turns a correct value into whatever
// fill pattern that storage happened to hold. A named copy makes the operand
// outlive the expression that produced it, and costs nothing on success because
// the value is only read when the expectation fails.
#define EXPECT_EQ(af_context, lhs, rhs)                                                    \
  do {                                                                                     \
    const auto& af_lhs__ = (lhs);                                                          \
    const auto af_rhs_value__ = (rhs);                                                     \
    const auto& af_rhs__ = af_rhs_value__;                                                 \
    if (!(af_lhs__ == af_rhs__)) {                                                         \
      (af_context).fail_at(__FILE__, __LINE__,                                             \
                           std::string("EXPECT_EQ(" #lhs ", " #rhs ") left=") +            \
                               ::af_test::describe(af_lhs__) + " right=" +                 \
                               ::af_test::describe(af_rhs__));                             \
    }                                                                                      \
  } while (false)

#define EXPECT_NE(af_context, lhs, rhs)                                                    \
  do {                                                                                     \
    const auto& af_lhs__ = (lhs);                                                          \
    const auto& af_rhs__ = (rhs);                                                          \
    if ((af_lhs__ == af_rhs__)) {                                                          \
      (af_context).fail_at(__FILE__, __LINE__,                                             \
                           std::string("EXPECT_NE(" #lhs ", " #rhs ") both=") +            \
                               ::af_test::describe(af_lhs__));                             \
    }                                                                                      \
  } while (false)

/// Bind a Status or a Result to a Status reference without copying through a
/// dangling temporary, then assert its code.
#define EXPECT_STATUS_CODE(af_context, expr, expected_code)                               \
  do {                                                                                    \
    const auto af_value__ = (expr);                                     \
        const ::autonomous_foundry::Status& af_status__ =                                     \
        ::autonomous_foundry::to_status(af_value__);                                            \
    if (af_status__.code() != (expected_code)) {                                          \
      (af_context).fail_at(                                                               \
          __FILE__, __LINE__,                                                             \
          std::string("EXPECT_STATUS_CODE(" #expr ", " #expected_code ") actual=") +      \
              std::string(::autonomous_foundry::error_code_name(af_status__.code())) +    \
              " message='" + af_status__.message() + "'");                                \
    }                                                                                     \
  } while (false)

#define EXPECT_STATUS_IN(af_context, expr, ...)                                           \
  do {                                                                                    \
    const auto af_value__ = (expr);                                     \
        const ::autonomous_foundry::Status& af_status__ =                                     \
        ::autonomous_foundry::to_status(af_value__);                                            \
    if (!::af_test::status_code_is_one_of(af_status__.code(), {__VA_ARGS__})) {           \
      (af_context).fail_at(                                                               \
          __FILE__, __LINE__,                                                             \
          std::string("EXPECT_STATUS_IN(" #expr ") actual=") +                            \
              std::string(::autonomous_foundry::error_code_name(af_status__.code())) +    \
              " message='" + af_status__.message() + "'");                                \
    }                                                                                     \
  } while (false)

#define EXPECT_OK(af_context, expr)                                                       \
  do {                                                                                    \
    const auto af_value__ = (expr);                                     \
        const ::autonomous_foundry::Status& af_status__ =                                     \
        ::autonomous_foundry::to_status(af_value__);                                            \
    if (!af_status__.ok()) {                                                              \
      (af_context).fail_at(                                                               \
          __FILE__, __LINE__,                                                             \
          std::string("EXPECT_OK(" #expr ") code=") +                                     \
              std::string(::autonomous_foundry::error_code_name(af_status__.code())) +    \
              " message='" + af_status__.message() + "'");                                \
    }                                                                                     \
  } while (false)

#define EXPECT_NOT_OK(af_context, expr)                                                   \
  do {                                                                                    \
    const auto af_value__ = (expr);                                     \
        const ::autonomous_foundry::Status& af_status__ =                                     \
        ::autonomous_foundry::to_status(af_value__);                                            \
    if (af_status__.ok()) {                                                               \
      (af_context).fail_at(__FILE__, __LINE__,                                            \
                           std::string("EXPECT_NOT_OK(" #expr ") succeeded"));            \
    }                                                                                     \
  } while (false)

// ---------------------------------------------------------------------------
// Requiring helpers
// ---------------------------------------------------------------------------

namespace af_test {

inline void require_ok(const TestContext& context, const autonomous_foundry::Status& status,
                       const char* expression, const char* file, int line) {
  if (!status.ok()) {
    context.fail_at(file, line, std::string("require_ok(") + expression + ") code=" +
                                   std::string(autonomous_foundry::error_code_name(status.code())) +
                                   " message='" + status.message() + "'");
  }
}

/// Unwrap a Result, throwing on failure, so a case can be written as a chain
/// of straight-line statements.
template <typename T>
[[nodiscard]] T require_value(const TestContext& context,
                              const autonomous_foundry::Result<T>& result, const char* expression,
                              const char* file, int line) {
  if (!result.ok()) {
    context.fail_at(file, line,
                    std::string("require_value(") + expression + ") code=" +
                        std::string(autonomous_foundry::error_code_name(result.status().code())) +
                        " message='" + result.status().message() + "'");
  }
  return result.value();
}

}  // namespace af_test

#define REQUIRE_OK(af_context, expr)                                                       \
  ::af_test::require_ok((af_context), ::autonomous_foundry::to_status(expr), #expr,        \
                        __FILE__, __LINE__)

/// Unwrap a Result<T> into a named local. A type alias is used at the call site
/// when the type itself carries a comma, so the macro stays a plain
/// three-parameter macro. The file and line reported on failure are the
/// caller's, not this header's.
#define REQUIRE_VALUE(af_context, type, name, expr)                                       \
  AF_REQUIRE_VALUE_IMPL((af_context), type, name, (expr), __FILE__, __LINE__)

#define AF_REQUIRE_VALUE_IMPL(af_context, type, name, expr, file, line)                   \
  type name = ::af_test::require_value((af_context), (expr), #expr, (file), (line))

// ---------------------------------------------------------------------------
// Temporary directories
// ---------------------------------------------------------------------------

namespace af_test {

/// Transient directory created below the runtime transient root and removed
/// when the case ends, including on the failure path.
class TempDirectory {
 public:
  /// Create a fresh unique directory. Throws TestFailure when the runtime
  /// cannot create one, because a case that needs a filesystem must not
  /// silently continue without one.
  explicit TempDirectory(std::string_view prefix);
  ~TempDirectory();

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path child(std::string_view leaf) const;

  /// Remove the tree now and report the outcome. The destructor is a second,
  /// idempotent safety net.
  void remove_now();

 private:
  std::filesystem::path path_;
  bool removed_{false};
};

}  // namespace af_test