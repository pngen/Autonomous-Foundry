// Test harness entry point for the Autonomous Foundry validation suites.
//
// Contract:
//
//   * stdout is unbuffered and every marker line is additionally flushed, so
//     the last marker produced survives a case that never returns;
//   * no arguments runs every registered case;
//   * one argument that is either a decimal 1-based index or an exact
//     suite::case name runs exactly that case;
//   * --list prints every case with its index and returns without running any;
//   * the final line is "SUITES <n> CASES <n> PASSED <n> FAILED <n>" and the
//     process returns 0 only when every selected case passed.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.hpp"

namespace {

/// Name of the case currently executing. Used only by the terminate handler so
/// that an abnormal termination still names the case that was running.
char g_active_case[512] = {0};

void set_active_case(std::string_view name) {
  const std::size_t limit = sizeof(g_active_case) - 1;
  const std::size_t count = name.size() < limit ? name.size() : limit;
  std::memcpy(g_active_case, name.data(), count);
  g_active_case[count] = '\0';
}

void write_raw_line(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void on_terminate() {
  const std::string_view active(g_active_case);
  if (!active.empty()) {
    std::string text("FAIL ");
    text.append(active);
    text.append(": std::terminate was called while this case was executing");
    write_raw_line(text);
  } else {
    write_raw_line("FAIL <unknown>::<unknown>: std::terminate outside any case");
  }
  std::fflush(nullptr);
  std::abort();
}

struct Selection {
  bool list{false};
  bool valid{true};
  std::size_t index{0};
  std::string name;
  std::string problem;
};

[[nodiscard]] bool is_all_digits(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Selection parse_arguments(int argc, char** argv) {
  Selection selection;
  if (argc <= 1) {
    return selection;
  }
  if (argc > 2) {
    selection.valid = false;
    selection.problem = "at most one case selector may be given";
    return selection;
  }
  const std::string_view argument(argv[1]);
  if (argument == "--list") {
    selection.list = true;
    return selection;
  }
  if (is_all_digits(argument)) {
    std::size_t value = 0;
    for (const char character : argument) {
      value = (value * 10u) + static_cast<std::size_t>(character - '0');
    }
    if (value == 0) {
      selection.valid = false;
      selection.problem = "case indices are 1-based; 0 is not a valid index";
      return selection;
    }
    selection.index = value;
    return selection;
  }
  selection.name = std::string(argument);
  return selection;
}

[[nodiscard]] std::string qualified_name(const af_test::CaseEntry& entry) {
  std::string name(entry.suite);
  name.append("::");
  name.append(entry.name);
  return name;
}

struct RunOutcome {
  std::size_t passed{0};
  std::size_t failed{0};
};

/// Run one case and emit its terminal marker. The runner owns the failure
/// report so a case body that returns without reporting still produces a
/// verdict, and so an escaping exception is attributed to the right case.
[[nodiscard]] bool run_case(const af_test::CaseEntry& entry, std::string_view display_name) {
  af_test::TestContext context(std::string(display_name), entry.file);
  set_active_case(display_name);

  std::string begin("BEGIN ");
  begin.append(display_name);
  af_test::MarkerSink::instance().line(begin);

  bool reported_failure = false;
  std::string failure_reason;
  try {
    entry.body(context);
  } catch (const af_test::TestFailure& failure) {
    reported_failure = true;
    failure_reason = std::string(failure.what()) + " [" + failure.file() + ":" +
                     std::to_string(failure.line()) + "]";
  } catch (const std::exception& error) {
    reported_failure = true;
    failure_reason = std::string("unexpected standard exception: ") + error.what();
  } catch (...) {
    reported_failure = true;
    failure_reason = "unexpected non-standard exception";
  }

  std::string verdict;
  if (reported_failure) {
    verdict.assign("FAIL ");
    verdict.append(display_name);
    verdict.append(": ");
    verdict.append(failure_reason);
  } else {
    verdict.assign("PASS ");
    verdict.append(display_name);
  }
  af_test::MarkerSink::instance().line(verdict);
  set_active_case(std::string_view());
  return !reported_failure;
}

void print_usage() {
  write_raw_line("usage: af_tests [--list | <1-based index> | <suite>::<case>]");
}

}  // namespace

int main(int argc, char** argv) {
  // Unbuffered stdout for the whole run: the last marker produced must survive
  // a case that never returns.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  std::set_terminate(on_terminate);

  const Selection selection = parse_arguments(argc, argv);
  if (!selection.valid) {
    write_raw_line(std::string("harness error: ") + selection.problem);
    print_usage();
    return 2;
  }

  std::vector<af_test::CaseEntry>& cases = af_test::registry();
  if (cases.empty()) {
    write_raw_line("harness error: no cases are registered");
    return 2;
  }

  if (selection.list) {
    for (std::size_t index = 0; index < cases.size(); ++index) {
      std::string line(std::to_string(index + 1));
      line.push_back(' ');
      line.append(qualified_name(cases[index]));
      line.append(" (");
      line.append(cases[index].file);
      line.push_back(')');
      write_raw_line(line);
    }
    write_raw_line(std::string("TOTAL ") + std::to_string(cases.size()));
    return 0;
  }

  std::vector<std::size_t> selected;
  if (selection.index != 0 || !selection.name.empty()) {
    if (selection.index != 0) {
      if (selection.index > cases.size()) {
        write_raw_line(std::string("harness error: index ") + std::to_string(selection.index) +
                       " is beyond the last case " + std::to_string(cases.size()));
        return 2;
      }
      selected.push_back(selection.index - 1);
    } else {
      bool found = false;
      for (std::size_t index = 0; index < cases.size(); ++index) {
        if (qualified_name(cases[index]) == selection.name) {
          selected.push_back(index);
          found = true;
          break;
        }
      }
      if (!found) {
        write_raw_line(std::string("harness error: no case is named '") + selection.name + "'");
        print_usage();
        return 2;
      }
    }
  } else {
    selected.reserve(cases.size());
    for (std::size_t index = 0; index < cases.size(); ++index) {
      selected.push_back(index);
    }
  }

  RunOutcome outcome;
  std::size_t suites_run = 0;
  std::string last_suite;
  for (const std::size_t index : selected) {
    const af_test::CaseEntry& entry = cases[index];
    if (entry.suite != last_suite) {
      last_suite = entry.suite;
      ++suites_run;
    }
    // A selected case is always displayed by its full name so the marker can
    // be pasted back on the command line.
    if (run_case(entry, qualified_name(entry))) {
      ++outcome.passed;
    } else {
      ++outcome.failed;
    }
  }

  std::string summary("SUITES ");
  summary.append(std::to_string(suites_run));
  summary.append(" CASES ");
  summary.append(std::to_string(selected.size()));
  summary.append(" PASSED ");
  summary.append(std::to_string(outcome.passed));
  summary.append(" FAILED ");
  summary.append(std::to_string(outcome.failed));
  write_raw_line(summary);

  return outcome.failed == 0 ? 0 : 1;
}