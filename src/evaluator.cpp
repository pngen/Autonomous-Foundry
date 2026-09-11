#include "autonomous_foundry/evaluator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/limits.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/workspace.hpp"

namespace autonomous_foundry {
namespace {

std::string read_env(std::string_view name) {
  const std::string key(name);
#if defined(_MSC_VER)
  char* buffer = nullptr;
  std::size_t length = 0;
  if (::_dupenv_s(&buffer, &length, key.c_str()) != 0 || buffer == nullptr) {
    return std::string();
  }
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char* value = std::getenv(key.c_str());
  return value == nullptr ? std::string() : std::string(value);
#endif
}

Status write_text_file(const std::filesystem::path& path, std::string_view content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return Status(ErrorCode::IoFailure, "cannot create '" + path_to_utf8(path) + "'");
  }
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  stream.flush();
  if (!stream) {
    return Status(ErrorCode::IoFailure, "cannot write '" + path_to_utf8(path) + "'");
  }
  stream.close();
  return Status();
}

std::string bound_text(std::string_view text, std::size_t limit) {
  if (text.size() <= limit) {
    return std::string(text);
  }
  std::string bounded(text.substr(0, limit));
  bounded.append("...(truncated)");
  return bounded;
}

/// Truncate captured output and strip the parts a candidate could use to
/// smuggle terminal control sequences into a diagnostic stream.
std::string sanitize_output(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char raw : text) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    if (byte == 0x09u || byte == 0x0Au || byte == 0x0Du || (byte >= 0x20u && byte <= 0x7Eu)) {
      out.push_back(raw);
    } else {
      out.push_back('.');
    }
    if (out.size() >= kMaxDiagnosticsLength) {
      out.append("...(truncated)");
      break;
    }
  }
  return out;
}

std::string toolchain_flavour_from_env() {
  const std::string flavour = read_env("AF_REFERENCE_FLAVOUR");
  if (!flavour.empty()) {
    return flavour;
  }
#if defined(_MSC_VER)
  return "msvc";
#else
  return "gnu";
#endif
}

struct Invocation {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::filesystem::path working_directory;
};

std::string measured_digest(std::string_view evaluator_key, std::string_view detail) {
  Sha256 hasher;
  hasher.update(evaluator_key);
  hasher.update("\x1f");
  hasher.update(detail);
  const auto digest = hasher.finish();
  return to_hex(digest.data(), digest.size());
}

std::uint64_t elapsed_micros(std::chrono::steady_clock::time_point start,
                             std::chrono::steady_clock::time_point finish) {
  const auto delta = std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
  return delta <= 0 ? 0u : static_cast<std::uint64_t>(delta);
}

Result<std::string> artifact_text(const EvaluationRequest& request, std::string_view name) {
  const auto found = request.artifacts.find(std::string(name));
  if (found == request.artifacts.end()) {
    return Status(ErrorCode::MandatoryEvidenceMissing,
                  "candidate did not declare artifact '" + std::string(name) + "'");
  }
  return found->second;
}

/// Build a compile-and-link invocation for the fixed harness plus the
/// candidate translation unit. The harness is always the evaluator's own copy.
Result<Invocation> make_compile_invocation(const EvaluationRequest& request,
                                           std::string_view harness_source,
                                           std::string_view output_name) {
  const std::filesystem::path& scratch = request.scratch_directory;
  if (scratch.empty()) {
    return Status(ErrorCode::InvalidArgument, "evaluation request has no scratch directory");
  }

  std::string candidate_source;
  AF_TRY_ASSIGN(candidate_source, artifact_text(request, kReferenceTaskSourceArtifact));

  const std::filesystem::path candidate_path = scratch / "candidate_unit.cpp";
  const std::filesystem::path harness_path = scratch / "foundry_harness.cpp";
  const std::filesystem::path header_path = scratch / "af_reference.hpp";

  AF_TRY(write_text_file(header_path, reference_api_header_source()));
  AF_TRY(write_text_file(harness_path, harness_source));
  AF_TRY(write_text_file(candidate_path, candidate_source));

  Invocation invocation;
  invocation.executable = request.toolchain.compiler;
  invocation.working_directory = scratch;

  const std::string include_flag =
      request.toolchain.flavour == "msvc" ? "/I" + path_to_utf8(scratch) : "-I" + path_to_utf8(scratch);

  if (request.toolchain.flavour == "msvc") {
    invocation.arguments.push_back("/nologo");
    invocation.arguments.push_back("/std:c++20");
    invocation.arguments.push_back("/EHsc");
    invocation.arguments.push_back("/O2");
    invocation.arguments.push_back("/W3");
    invocation.arguments.push_back("/permissive-");
    invocation.arguments.push_back(include_flag);
    invocation.arguments.push_back("/Fe:" + path_to_utf8(scratch / std::string(output_name)));
    invocation.arguments.push_back(path_to_utf8(harness_path));
    invocation.arguments.push_back(path_to_utf8(candidate_path));
  } else {
    invocation.arguments.push_back("-std=c++20");
    invocation.arguments.push_back("-O2");
    invocation.arguments.push_back("-Wall");
    invocation.arguments.push_back(include_flag);
    invocation.arguments.push_back(path_to_utf8(harness_path));
    invocation.arguments.push_back(path_to_utf8(candidate_path));
    invocation.arguments.push_back("-o");
    invocation.arguments.push_back(path_to_utf8(scratch / std::string(output_name)));
  }
  return invocation;
}

ChildProcessOptions make_process_options(const EvaluationRequest& request,
                                         const Invocation& invocation) {
  ChildProcessOptions options;
  options.executable = invocation.executable;
  options.arguments = invocation.arguments;
  options.working_directory = invocation.working_directory;
  options.capture_output = true;
  options.max_capture_bytes = kMaxCapturedOutputBytes;
  options.inherit_environment = true;
  if (!request.toolchain.include_directories.empty()) {
    options.environment.emplace("INCLUDE",
                                path_to_utf8(request.toolchain.include_directories));
  }
  if (!request.toolchain.library_directories.empty()) {
    options.environment.emplace("LIB", path_to_utf8(request.toolchain.library_directories));
  }
  return options;
}

/// Compile the candidate and return the produced executable path.
Result<std::filesystem::path> compile_candidate(const EvaluationRequest& request,
                                                std::string_view harness_source,
                                                std::string_view output_name,
                                                ProcessResult* compile_result) {
  Invocation invocation;
  AF_TRY_ASSIGN(invocation, make_compile_invocation(request, harness_source, output_name));
  const ChildProcessOptions options = make_process_options(request, invocation);
  ProcessResult result;
  AF_TRY_ASSIGN(result, run_process(options));
  *compile_result = result;
  if (!result.started) {
    return Status(ErrorCode::ProcessSpawnFailure, "compiler could not be started");
  }
  if (!result.exited || result.exit_code != 0) {
    return Status(ErrorCode::ProcessWaitFailure, "candidate did not compile");
  }
  return request.scratch_directory / std::string(output_name);
}

}  // namespace

std::string ReferenceToolchain::describe() const {
  if (!usable()) {
    return "no reference C++ toolchain resolved";
  }
  std::string text = flavour;
  text.append(" compiler at ");
  text.append(path_to_utf8(compiler));
  return text;
}

Result<ReferenceToolchain> resolve_reference_toolchain() {
  ReferenceToolchain toolchain;
  toolchain.flavour = toolchain_flavour_from_env();

  const std::string configured = read_env("AF_REFERENCE_CXX");
  if (!configured.empty()) {
    toolchain.compiler = std::filesystem::path(configured);
    const std::string include_dirs = read_env("AF_REFERENCE_INCLUDE");
    if (!include_dirs.empty()) {
      toolchain.include_directories = std::filesystem::path(include_dirs);
    }
    const std::string library_dirs = read_env("AF_REFERENCE_LIB");
    if (!library_dirs.empty()) {
      toolchain.library_directories = std::filesystem::path(library_dirs);
    }
    if (!std::filesystem::exists(toolchain.compiler)) {
      return Status(ErrorCode::Unsupported,
                    "AF_REFERENCE_CXX points at a compiler that does not exist: " + configured);
    }
    return toolchain;
  }

  const std::vector<std::string> candidates =
      toolchain.flavour == "msvc" ? std::vector<std::string>{"cl.exe"}
                                  : std::vector<std::string>{"c++", "g++", "clang++"};

  const std::string path_value = read_env("PATH");
  std::size_t begin = 0;
  while (begin <= path_value.size()) {
    const std::size_t end = path_value.find(';', begin);
    const std::size_t stop = end == std::string::npos ? path_value.size() : end;
    const std::string directory = path_value.substr(begin, stop - begin);
    if (!directory.empty()) {
      for (const std::string& name : candidates) {
        const std::filesystem::path probe = std::filesystem::path(directory) / name;
        if (std::filesystem::exists(probe)) {
          toolchain.compiler = probe;
          const std::string include_dirs = read_env("INCLUDE");
          if (!include_dirs.empty()) {
            toolchain.include_directories = std::filesystem::path(include_dirs);
          }
          const std::string library_dirs = read_env("LIB");
          if (!library_dirs.empty()) {
            toolchain.library_directories = std::filesystem::path(library_dirs);
          }
          return toolchain;
        }
      }
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }

  return Status(ErrorCode::Unsupported,
                "no reference C++ toolchain is available; set AF_REFERENCE_CXX to a compiler path");
}

Evaluator::~Evaluator() = default;

// ---------------------------------------------------------------------------
// compile-and-run
// ---------------------------------------------------------------------------

std::string CompileAndRunEvaluator::key() const { return std::string(kEvaluatorCompileAndRun); }

EvaluationResult CompileAndRunEvaluator::evaluate(const EvaluationRequest& request) {
  EvaluationResult result;
  const auto started = std::chrono::steady_clock::now();

  if (!request.toolchain.usable()) {
    result.outcome = EvaluationOutcome::Unsupported;
    result.diagnostics =
        "no C++ toolchain resolved; compiler-backed evaluation cannot determine an outcome";
    result.evidence_digest = measured_digest(key(), result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  const auto source = request.artifacts.find(std::string(kReferenceTaskSourceArtifact));
  if (source == request.artifacts.end()) {
    result.outcome = EvaluationOutcome::Fail;
    result.diagnostics = "candidate declared no solution.cpp artifact";
    result.evidence_digest = measured_digest(key(), result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  ProcessResult compile_result;
  const std::string binary_name = "foundry_conformance.exe";
  auto compiled = compile_candidate(request, reference_test_harness_source(), binary_name,
                                    &compile_result);
  if (!compiled.ok()) {
    result.outcome = EvaluationOutcome::Fail;
    result.diagnostics = "compile stage failed: " + compiled.status().message() +
                         " | compiler stdout: " + sanitize_output(compile_result.standard_output) +
                         " | compiler stderr: " + sanitize_output(compile_result.standard_error);
    result.evidence_digest = measured_digest(
        key(), source->second + "\x1f" + result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  Invocation run;
  run.executable = compiled.value();
  run.working_directory = request.scratch_directory;
  const ChildProcessOptions run_options = make_process_options(request, run);
  auto executed = run_process(run_options);
  if (!executed.ok()) {
    result.outcome = EvaluationOutcome::Error;
    result.diagnostics = "conformance process could not be run: " + executed.status().message();
    result.evidence_digest =
        measured_digest(key(), source->second + "\x1f" + result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  const ProcessResult& process = executed.value();
  const bool reported_ok = process.standard_output.find("AF_REFERENCE_OK") != std::string::npos;
  const bool clean_exit = process.exited && !process.terminated_abnormally && process.exit_code == 0;

  if (clean_exit && reported_ok) {
    result.outcome = EvaluationOutcome::Pass;
    result.diagnostics = "conformance process exited with status 0 and reported AF_REFERENCE_OK";
  } else {
    result.outcome = EvaluationOutcome::Fail;
    result.diagnostics = "conformance process exit=" + std::to_string(process.exit_code) +
                         " abnormal=" + (process.terminated_abnormally ? "yes" : "no") +
                         " reported_ok=" + (reported_ok ? "yes" : "no") +
                         " | stdout: " + sanitize_output(process.standard_output) +
                         " | stderr: " + sanitize_output(process.standard_error);
  }

  result.evidence_digest = measured_digest(
      key(), source->second + "\x1f" + std::string(reference_test_harness_source()) + "\x1f" +
                 std::string(evaluation_outcome_name(result.outcome)) + "\x1f" +
                 bound_text(process.standard_output, 4096));
  result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
  return result;
}

// ---------------------------------------------------------------------------
// source policy
// ---------------------------------------------------------------------------

std::string SourcePolicyEvaluator::key() const { return std::string(kEvaluatorSourcePolicy); }

EvaluationResult SourcePolicyEvaluator::evaluate(const EvaluationRequest& request) {
  EvaluationResult result;
  const auto started = std::chrono::steady_clock::now();

  const auto source = request.artifacts.find(std::string(kReferenceTaskSourceArtifact));
  if (source == request.artifacts.end()) {
    result.outcome = EvaluationOutcome::Fail;
    result.diagnostics = "candidate declared no solution.cpp artifact";
    result.evidence_digest = measured_digest(key(), result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  const std::string& text = source->second;

  const std::string required = std::string(kReferenceTaskApiName);
  if (text.find(required) == std::string::npos) {
    result.outcome = EvaluationOutcome::Fail;
    result.diagnostics = "required symbol '" + required + "' is not present in the candidate source";
    result.evidence_digest = measured_digest(key(), text + "\x1f" + result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  const std::vector<std::string> forbidden = {
      "system(", "popen", "CreateProcess", "fork(", "execv", "WinExec", "ShellExecute",
      "#include <windows.h>", "#include <winsock2.h>", "__asm", "#pragma comment"};
  std::vector<std::string> violations;
  for (const std::string& token : forbidden) {
    if (text.find(token) != std::string::npos) {
      violations.push_back(token);
    }
  }
  if (text.size() > kMaxInputFileBytes / 4u) {
    violations.push_back("source exceeds the reference task size bound");
  }

  if (violations.empty()) {
    result.outcome = EvaluationOutcome::Pass;
    result.diagnostics = "static policy satisfied";
  } else {
    result.outcome = EvaluationOutcome::Fail;
    result.diagnostics = "forbidden construct(s) present:";
    for (const std::string& violation : violations) {
      result.diagnostics.append(" [").append(violation).append("]");
    }
  }

  result.evidence_digest = measured_digest(key(), text + "\x1f" + result.diagnostics);
  result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
  return result;
}

// ---------------------------------------------------------------------------
// performance
// ---------------------------------------------------------------------------

std::string PerformanceEvaluator::key() const { return std::string(kEvaluatorPerformance); }

EvaluationResult PerformanceEvaluator::evaluate(const EvaluationRequest& request) {
  EvaluationResult result;
  const auto started = std::chrono::steady_clock::now();

  if (!request.toolchain.usable()) {
    result.outcome = EvaluationOutcome::Unsupported;
    result.diagnostics = "no C++ toolchain resolved; the workload cannot be measured";
    result.evidence_digest = measured_digest(key(), result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  ProcessResult compile_result;
  auto compiled = compile_candidate(request, reference_benchmark_harness_source(),
                                    "foundry_benchmark.exe", &compile_result);
  if (!compiled.ok()) {
    result.outcome = EvaluationOutcome::Error;
    result.diagnostics = "benchmark harness did not build: " + compiled.status().message();
    result.evidence_digest = measured_digest(key(), result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  Invocation run;
  run.executable = compiled.value();
  run.working_directory = request.scratch_directory;
  auto executed = run_process(make_process_options(request, run));
  if (!executed.ok()) {
    result.outcome = EvaluationOutcome::Error;
    result.diagnostics = "benchmark process could not be run: " + executed.status().message();
    result.evidence_digest = measured_digest(key(), result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  const ProcessResult& process = executed.value();
  const std::string marker = "AF_BENCH_OPS_PER_SEC ";
  const std::size_t position = process.standard_output.find(marker);
  if (!process.exited || process.exit_code != 0 || position == std::string::npos) {
    result.outcome = EvaluationOutcome::Error;
    result.diagnostics = "benchmark produced no measurement: " +
                         sanitize_output(process.standard_output) +
                         sanitize_output(process.standard_error);
    result.evidence_digest = measured_digest(key(), result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  const std::size_t value_begin = position + marker.size();
  const std::size_t value_end = process.standard_output.find('\n', value_begin);
  const std::string value_text = process.standard_output.substr(
      value_begin, value_end == std::string::npos ? std::string::npos : value_end - value_begin);

  double measured = 0.0;
  {
    std::istringstream stream(value_text);
    stream >> measured;
    if (!stream) {
      measured = 0.0;
    }
  }
  if (!std::isfinite(measured) || measured <= 0.0) {
    result.outcome = EvaluationOutcome::Error;
    result.diagnostics = "benchmark reported a non-positive measurement '" + value_text + "'";
    result.evidence_digest = measured_digest(key(), result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  result.outcome = EvaluationOutcome::Pass;
  result.has_score = true;
  result.score = measured;
  result.diagnostics = "measured reference workload throughput";
  result.diagnostics.append(" ops_per_second=").append(value_text);
  result.evidence_digest = measured_digest(key(), value_text);
  result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
  return result;
}

// ---------------------------------------------------------------------------
// parsimony
// ---------------------------------------------------------------------------

std::string ParsimonyEvaluator::key() const { return std::string(kEvaluatorParsimony); }

EvaluationResult ParsimonyEvaluator::evaluate(const EvaluationRequest& request) {
  EvaluationResult result;
  const auto started = std::chrono::steady_clock::now();

  const auto source = request.artifacts.find(std::string(kReferenceTaskSourceArtifact));
  if (source == request.artifacts.end()) {
    result.outcome = EvaluationOutcome::Error;
    result.diagnostics = "candidate declared no solution.cpp artifact";
    result.evidence_digest = measured_digest(key(), result.diagnostics);
    result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
    return result;
  }

  std::size_t tokens = 0;
  bool in_token = false;
  for (const char raw : source->second) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    const bool is_token_byte = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                               (byte >= '0' && byte <= '9') || byte == '_';
    if (is_token_byte && !in_token) {
      ++tokens;
    }
    in_token = is_token_byte;
  }

  result.outcome = EvaluationOutcome::Pass;
  result.has_score = true;
  result.score = static_cast<double>(tokens);
  result.diagnostics = "reference structural metric: identifier-like token count";
  result.evidence_digest = measured_digest(key(), source->second);
  result.duration_micros = elapsed_micros(started, std::chrono::steady_clock::now());
  return result;
}

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

EvaluatorRegistry::EvaluatorRegistry() = default;

void EvaluatorRegistry::add(std::shared_ptr<Evaluator> evaluator) {
  if (evaluator == nullptr) {
    return;
  }
  for (const std::shared_ptr<Evaluator>& existing : evaluators_) {
    if (existing->key() == evaluator->key()) {
      return;
    }
  }
  evaluators_.push_back(std::move(evaluator));
}

std::shared_ptr<Evaluator> EvaluatorRegistry::find(std::string_view key) const {
  for (const std::shared_ptr<Evaluator>& evaluator : evaluators_) {
    if (evaluator->key() == key) {
      return evaluator;
    }
  }
  return nullptr;
}

std::vector<std::string> EvaluatorRegistry::keys() const {
  std::vector<std::string> result;
  result.reserve(evaluators_.size());
  for (const std::shared_ptr<Evaluator>& evaluator : evaluators_) {
    result.push_back(evaluator->key());
  }
  std::sort(result.begin(), result.end());
  return result;
}

EvaluatorRegistry EvaluatorRegistry::make_reference() {
  EvaluatorRegistry registry;
  registry.add(std::make_shared<CompileAndRunEvaluator>());
  registry.add(std::make_shared<SourcePolicyEvaluator>());
  registry.add(std::make_shared<PerformanceEvaluator>());
  registry.add(std::make_shared<ParsimonyEvaluator>());
  return registry;
}

}  // namespace autonomous_foundry
