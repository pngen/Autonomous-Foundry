#include "test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "autonomous_foundry/error.hpp"

namespace af_test {

// ---------------------------------------------------------------------------
// TestFailure
// ---------------------------------------------------------------------------

TestFailure::TestFailure(std::string file, int line, std::string message)
    : std::runtime_error(std::move(message)), file_(std::move(file)), line_(line) {}

// ---------------------------------------------------------------------------
// describe()
// ---------------------------------------------------------------------------

std::string describe(bool value) { return value ? std::string("true") : std::string("false"); }

std::string describe(float value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

std::string describe(double value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

std::string describe(autonomous_foundry::ErrorCode value) {
  return std::string(autonomous_foundry::error_code_name(value));
}

std::string describe(const autonomous_foundry::Status& value) {
  return std::string(autonomous_foundry::error_code_name(value.code())) + " ('" +
         value.message() + "')";
}

bool status_code_is_one_of(autonomous_foundry::ErrorCode code,
                           std::initializer_list<autonomous_foundry::ErrorCode> allowed) {
  for (const autonomous_foundry::ErrorCode candidate : allowed) {
    if (candidate == code) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// MarkerSink
// ---------------------------------------------------------------------------

MarkerSink& MarkerSink::instance() {
  static MarkerSink sink;
  return sink;
}

void MarkerSink::line(std::string_view text) {
  // One write followed by one flush. The stream is unbuffered as well, but the
  // explicit flush keeps the guarantee independent of how stdout was set up.
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  lines_.fetch_add(1, std::memory_order_relaxed);
}

void MarkerSink::phase(std::string_view case_name, std::string_view phase_name) {
  std::string text("PHASE ");
  text.append(case_name);
  text.push_back(' ');
  text.append(phase_name);
  line(text);
}

std::uint64_t MarkerSink::lines_written() const noexcept {
  return lines_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// TestContext
// ---------------------------------------------------------------------------

TestContext::TestContext(std::string qualified_name, std::string source_file)
    : qualified_name_(std::move(qualified_name)), source_file_(std::move(source_file)) {}

void TestContext::phase(std::string_view phase_name) const {
  ++phase_count_;
  MarkerSink::instance().phase(qualified_name_, phase_name);
}

void TestContext::note(std::string_view text) const {
  MarkerSink::instance().line(text);
}

void TestContext::fail_at(std::string file, int line, std::string message) const {
  throw TestFailure(std::move(file), line, std::move(message));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

CaseRegistrar::CaseRegistrar(const char* suite, const char* name, const char* file,
                             void (*body)(TestContext&)) {
  CaseEntry entry;
  entry.suite = suite == nullptr ? std::string() : std::string(suite);
  entry.name = name == nullptr ? std::string() : std::string(name);
  entry.file = file == nullptr ? std::string() : std::string(file);
  entry.body = body;
  registry().push_back(std::move(entry));
}

std::vector<CaseEntry>& registry() {
  static std::vector<CaseEntry> entries;
  return entries;
}

// ---------------------------------------------------------------------------
// TempDirectory
// ---------------------------------------------------------------------------

TempDirectory::TempDirectory(std::string_view prefix) {
  const autonomous_foundry::Result<std::filesystem::path> created =
      autonomous_foundry::make_transient_directory(prefix);
  if (!created.ok()) {
    throw TestFailure(__FILE__, __LINE__,
                      std::string("make_transient_directory failed: ") +
                          std::string(autonomous_foundry::error_code_name(created.status().code())) +
                          " '" + created.status().message() + "'");
  }
  path_ = created.value();
}

TempDirectory::~TempDirectory() {
  if (removed_) {
    return;
  }
  std::error_code failure;
  std::filesystem::remove_all(path_, failure);
  removed_ = true;
}

std::filesystem::path TempDirectory::child(std::string_view leaf) const {
  return path_ / std::filesystem::path(std::string(leaf));
}

void TempDirectory::remove_now() {
  if (removed_) {
    return;
  }
  const autonomous_foundry::Status removed =
      autonomous_foundry::remove_tree_bounded(path_);
  if (!removed.ok()) {
    throw TestFailure(__FILE__, __LINE__,
                      std::string("remove_tree_bounded failed: ") +
                          std::string(autonomous_foundry::error_code_name(removed.code())) + " '" +
                          removed.message() + "'");
  }
  removed_ = true;
}

}  // namespace af_test
