# Validation

This document is the maintainer-facing description of the validation surface of Autonomous Foundry: what is built, what is exercised, which commands reproduce it, and what result each step must produce.

It describes the full surface and the exact commands. Where a step cannot run in the current tree, that is stated at the step rather than hidden. The tree currently contains the runtime library, the complete public API, the documentation, the CMake package export, the CMake package config template and the test harness. `CMakeLists.txt` is the authoritative list of the sources a complete build needs: the library sources in `src/`, the core suite cases in `tests/`, the multiprocess suite entry point in `tests/`, the front ends in `apps/`, the examples in `examples/` and the benchmarks in `benchmarks/`. A target whose source is absent from your checkout cannot be produced until the source is present; that is a gap in the checkout, not a validation result.

## The validation surface

| Step | What it proves | Command group |
| --- | --- | --- |
| Release build | the library compiles and links under the warning contract | Configure and build (release) |
| Debug build | no behaviour depends on optimization or `NDEBUG` | Configure and build (debug) |
| Warning contract | zero first-party warnings, warnings are errors | Warning contract |
| Core suite | the state machine, domain types and validation, persistence, protocol, workspace, property, adversarial, concurrency and end-to-end behaviour | Core suite |
| Multiprocess suite | real OS-process workers over loopback TCP, worker death, coordinator restart, the distributed proof | Multiprocess suite |
| ASan run | no memory error in first-party code under the real suites | AddressSanitizer |
| Install and consumer check | the installed package is usable by an independent CMake project | Install and independent consumer |
| Fresh-clone closure | the build, the suites and the install succeed from a clean clone | Fresh-clone closure |
| Cleanup | no disposable artifact is left behind in any touched location | Cleanup |

## Environment

All commands are run from the repository root and from a Visual Studio developer environment, so that `cl`, `link` and the Windows SDK are on `PATH` and `INCLUDE` and `LIB` are populated. Configure once per shell:

```
cmd /c ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && set"
```

Any host toolchain that satisfies C++20 and CMake 3.20 works. The examples below use Ninja; the same commands work with the default Visual Studio generator.

## Configure and build (release)

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Required result: the configuration succeeds, every target builds, and the build log contains no warning. The build directory is disposable.

Build subsets while working on one area:

```
cmake --build build --target autonomous_foundry
cmake --build build --target af_tests
cmake --build build --target af_distributed_tests
cmake --build build --target af_coordinator af_worker af_cli
```

The optional targets are `af_coordinator`, `af_worker` and `af_cli` (from `apps/`), `ex_basic_population`, `ex_selection_and_retention`, `ex_lineage_evolution`, `ex_stale_authority_rejection`, `ex_promotion_handoff`, `ex_persistence_recovery` and `ex_cancellation` (from `examples/`), and `af_benchmarks` (from `benchmarks/`).

Current tree: the library target builds. The targets above that depend on `tests/` case files, `apps/`, `examples/` or `benchmarks/` can be produced exactly when those sources are present in the checkout.

## Configure and build (debug)

Use a separate build directory so that a debug result can never be confused with a release result:

```
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

With a multi-configuration generator, the equivalent is:

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug
```

Required result: the configuration succeeds, every target builds in Debug with no warning, and the suite results in Debug match the suite results in Release. A case that passes in Release and fails in Debug is a defect, not a configuration difference.

## Warning contract

First-party code is compiled with the full warning set and with warnings as errors.

On MSVC:

```
/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor /Zc:inline
/Zc:throwingNew /guard:cf /sdl /MP /WX
```

On GCC and Clang:

```
-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual
-Wdouble-promotion -Wformat=2 -Werror
```

The contract is a zero-warning build. A warning is fixed, never suppressed: no pragma, no per-file warning disable and no `AUTONOMOUS_FOUNDRY_WARNINGS_AS_ERRORS=OFF` is an acceptable way to close a build. The option exists only to read a warning while diagnosing one.

These options apply to first-party targets only. External headers pulled in by the platform SDK are outside the contract.

## Core suite

```
ctest --test-dir build --output-on-failure
```

CTest registers `af_core_tests` for the core suite and, when the front ends are built, `af_distributed_tests` for the multiprocess suite. Run one of them directly:

```
ctest --test-dir build -R af_core_tests --output-on-failure
ctest --test-dir build -R af_distributed_tests --output-on-failure
```

The core suite covers identity and generation encoding, authority and staleness rejection, population and candidate lifecycle, lineage acyclicity and retirement, evaluation evidence and mandatory gates, selection and its exclusions, retention, promotion eligibility, budget accounting, persistence round-trip and rejection, the protocol codec, workspace path safety, property-based checks, adversarial inputs, concurrency, the reference evaluator and the end-to-end foundry flow.

Required result: every case passes, the last line reports `FAILED 0`, and the process exits 0.

## Multiprocess suite

The multiprocess suite launches real processes, so it needs the front ends and the reference toolchain to be visible to the test process. CTest sets all of this; when running the executable directly, set it yourself:

```
AF_COORDINATOR_EXE=<abs path to af_coordinator>
AF_WORKER_EXE=<abs path to af_worker>
AF_CLI_EXE=<abs path to af_cli>
AF_REFERENCE_CXX=<abs path to the C++ compiler>
AF_REFERENCE_INCLUDE=<INCLUDE>
AF_REFERENCE_LIB=<LIB>
AF_REFERENCE_FLAVOUR=msvc
```

The build configures these through the `af_distributed_tests` test environment. `AF_REFERENCE_*` is how the compile-backed evaluator resolves its toolchain; when no toolchain resolves, evaluation reports `UNSUPPORTED` and the case states that rather than passing.

What the suite exercises, all on the loopback interface of one host:

- workers that are genuinely separate operating system processes, connected over framed TCP
- a worker process killed while it holds an assignment, and the resulting `OUTCOME_UNKNOWN` rather than an invented success or failure
- a worker that restarts and reconnects under a fresh boot identity, whose old authority is rejected
- a coordinator process that is stopped and restarted, after which the epoch has advanced, every session is dead, in-flight attempts are `OUTCOME_UNKNOWN` and every live population is in `REVALIDATION_REQUIRED`
- a candidate that passes, followed by a mutation of the state it depended on, followed by a refused stale commit

Required result: every case passes and every case names the phase it was in.

## AddressSanitizer

```
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAUTONOMOUS_FOUNDRY_ENABLE_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

The option instruments first-party code and propagates the required compile and link options to every target that links the library, disables incremental linking and defines `_DISABLE_VECTOR_ANNOTATION` and `_DISABLE_STRING_ANNOTATION`. Use a genuine x64 MSVC ASan build of the suites.

Required result: no ASan report from first-party code in any case. A report is a defect. Note that an ASan run leaves its own crash dumps and logs behind if a case does crash; those are disposable and are removed during cleanup.

## Install and independent consumer

```
cmake --install build --prefix _install
```

Then build a consumer that is not part of this repository and knows nothing about its source tree. Create `_consumer/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(foundry_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(AutonomousFoundry CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE AutonomousFoundry::autonomous_foundry)
```

Create `_consumer/main.cpp` using only the installed public headers:

```cpp
#include <cstdio>

#include "autonomous_foundry/version.hpp"

int main() {
  std::printf("%s\n", std::string(autonomous_foundry::version_string()).c_str());
  return autonomous_foundry::version_number() == 0 ? 1 : 0;
}
```

Configure and build it with no reference to this source tree:

```
cmake -S _consumer -B _consumer/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<abs path to _install>
cmake --build _consumer/build
_consumer/build/consumer
```

Required result: configuration resolves the package, the link succeeds, the program prints `1.0.0` and exits 0. A failure here means the exported package, the exported target namespace or the installed header tree is wrong, regardless of how the in-tree build behaves.

## Fresh-clone closure

Copy the tracked sources to a clean directory - not a `git clone` of a working directory that still holds build output - and run the whole surface there:

```
cmake -S <fresh clone> -B <fresh clone>\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build <fresh clone>\build
ctest --test-dir <fresh clone>\build --output-on-failure
cmake --install <fresh clone>\build --prefix <fresh clone>\_install
```

Required result: configure, build, tests and install all succeed from a tree that contains no build output, no cached toolchain path and no absolute path to the developer machine.

## Harness marker contract

Both suites are the same harness with different registered cases.

```
af_tests                       run every registered case
af_tests --list                print "index suite::case (file)" for every case, then "TOTAL <n>"
af_tests <1-based index>       run exactly that case
af_tests <suite>::<case>       run exactly that case by stable name
```

`af_distributed_tests` accepts the same arguments.

Every case emits four marker shapes, one per line:

```
BEGIN <suite>::<case>
PHASE <suite>::<case> <PHASE_NAME>
PASS <suite>::<case>
FAIL <suite>::<case>: <reason>
```

Rules the harness guarantees, and that a maintainer may rely on:

- stdout and stderr are unbuffered for the whole run, and every marker line is flushed in the same call that writes it, so the last marker produced survives a case that never returns.
- `BEGIN` is written before the case body runs. `PHASE` is written at every blocking, concurrent or process boundary - setup, dispatch, wait, commit, restart, verify, shutdown and per-case phases such as a per-seed marker. `PASS` or `FAIL` is written when the case returns.
- A case that throws, and a case during which `std::terminate` is called, is still attributed to the correct case name.
- Cases are independent: each case builds its own state.
- A selected case is always displayed by its fully qualified `suite::case` name, so a marker can be pasted straight back onto the command line.
- The final line is `SUITES <n> CASES <n> PASSED <n> FAILED <n>`, and the process exits 0 only when every selected case passed.

A regression is diagnosed like this: read the last marker of the failing run, take the case name it contains, and run that case alone with no other case executing. The marker stream then ends at the exact phase that blocked.

## No-timeout policy

This is a design fact of the repository, not a convention.

No operation in this repository imposes a deadline, a watchdog or an execution-duration limit on itself, on a test, or on a child process it launches. Nothing in the build, the suites or the configuration carries a timeout. The evaluation engine runs a child process to natural completion. A hang is a defect to diagnose, never something to paper over with a deadline, and adding one is not an acceptable fix.

This is why the marker contract above exists: a case that blocks must still be identifiable. The last flushed marker names the case, and the phase it never left. Diagnosing means running that one case and observing which resource or peer it is waiting on - a listener, a worker process, the evaluation pool, a lock, or a file - until the actual defect is found and fixed.

## Cleanup

After a full run, every touched location must be back to intentional artifacts only.

In the repository:

```
Remove-Item -Recurse -Force build, build-debug, build-asan, _install, _consumer
```

Build trees, install prefixes and consumer scratch directories are disposable and are excluded by `.gitignore`; delete them when the run is finished rather than leaving them in the working tree. Keep them only while a proof is actively in progress.

Outside the repository, the run creates transient state on purpose: per-attempt workspaces, snapshots and child process logs live below the transient root reported by `autonomous_foundry::default_transient_root()`. The suites create their own unique directories there and remove them when a case ends, including on the failure path, and the coordinator removes its workspace root on shutdown with a bounded removal. After a crash, a killed process or an aborted run, check that root and remove any directory that belongs to a finished run.

Required result: no build tree, no install prefix, no consumer scratch directory, no leftover workspace, no snapshot file and no child process log remains after closure. A directory that is still there because a case hung is not cleanup work - it is the evidence for the defect, and it is removed once the defect is fixed and re-run.
