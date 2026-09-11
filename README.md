# Autonomous Foundry

Autonomous Foundry is an open-source, vendor-neutral C++20 runtime for coordinating production populations of autonomous coding and research workers through generation, execution, testing, evaluation, selection, retention, lineage and promotion.

## The question it answers

Given a population of autonomous workers producing competing artifacts and results, which candidates may execute, which outputs are valid, which candidates survive selection, which lineages remain worth retaining, and which result is authoritative enough to advance toward promotion?

## What it is not

Autonomous Foundry is not an agent loop, and it is not an agent runtime. It publishes narrow interfaces and leaves the concerns of the adjacent systems to those systems:

- **Agent Scheduler** - owns when persistent autonomous workers may run. The foundry's interface is that a worker connects when the scheduler lets it.
- **Agent Runtime** - owns one long-lived agent across models, tools, memory and budgets. The foundry only sees an accepted assignment and a published result.
- **Model Router** - owns model choice under policy, cost and capability. The foundry has no interface to it and never names a model.
- **Ensemble Fabric** - owns multiple model attempts, judges, arbitration and consensus. The foundry has no interface to it; an ensemble may sit behind one worker.
- **Critic Fabric** - owns reusable bounded critique workers. The foundry has no interface to it.
- **Experiment Fabric** - owns hypotheses, branches, metrics and rollback. The foundry has no interface to it.
- **Lab Scheduler** - owns experiments across models, GPUs, datasets and simulators. The foundry has no interface to it.
- **Research Ledger** - owns durable provenance and accounting for every call and artifact. The foundry emits digests and evidence summaries the ledger can re-verify.
- **Artifact Promotion** - owns the trust transition that approves an artifact. The foundry emits a `PromotionRequest` through `PromotionSink` and never claims promotion.

## The core principle

A candidate does not become authoritative because an agent produced it.

`UNKNOWN` never silently becomes `PASS`.

An agent claiming success is evidence, not authority.

A test invocation is not a test result until execution actually completes.

A candidate that once passed does not remain valid after a dependency, evaluation, policy, task, artifact, worker or authority generation changes.

## Systems boundary

What the foundry owns:

| Area | Owned concern |
| --- | --- |
| Population | identity and lifecycle |
| Candidate | identity, generation, execution state, production attempts and lineage |
| Dispatch | worker-to-candidate assignment |
| Evaluation | evaluation requirements and collected evidence |
| Selection | selection policy and the selection decision |
| Retention | retention policy and the retention decision |
| Lifecycle | candidate supersession and retirement, population closure |
| Promotion | promotion eligibility and the typed handoff request |
| Authority | generation-fenced foundry authority |
| Durability | durable foundry state, population-level accounting, recovery |
| Explainability | explainable selection and rejection |

The adjacent systems above are not absorbed. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full boundary table with the interface published to each one.

## Architecture

The runtime separates a pure state machine from everything that can block or fail:

```
apps/            af_cli, af_coordinator, af_worker
                 thin command-line front ends

src/coordinator  FoundryCoordinator: sockets, threads, evaluation pool,
                 persistence ordering, shutdown
src/worker_*     reference worker runtime: connects, produces, publishes
src/foundry_*    FoundryCore: the state machine. No I/O, no threads, no waits.
src/persistence  versioned, CRC-checked, bounded snapshot format
src/protocol     framed wire protocol and payload codec
src/transport    winsock/POSIX sockets, one reader and one writer per connection
src/process      child processes without a shell and without a console window
src/evaluator    real compiler + real test process evaluation
src/workspace    path validation, reparse-point refusal, atomic publication
src/{task,policy,candidate,lineage,population,evaluation,selection,...}
                 domain types, validation, transition tables, canonical digests
include/         the entire public API
```

`FoundryCore` performs no I/O, spawns no threads, calls no user hooks and never blocks. Exactly one `std::shared_mutex` guards all foundry state and is never held across anything that can block. The state machine is implemented across five translation units that share the private header `src/foundry_core_impl.hpp`, and the concurrency contract is audited in [docs/CONCURRENCY.md](docs/CONCURRENCY.md).

Full layering, state diagrams and the evidence realms are documented in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Identity and generation model

Every identity is a distinct C++ type: `CandidateId`, `WorkerBootId`, `AttemptId`, `AssignmentId`, `EvidenceGeneration` and the rest are separate types even where they are all 64-bit values, so two identities from different domains cannot be compared, assigned or passed to each other.

The raw 64-bit layout carries the domain, so an identity that arrives through an untyped channel is still checked:

```
bits 56..63 : IdKind      identity domain tag
bits 32..55 : salt        24-bit per-foundry-run salt
bits  0..31 : counter     monotonic allocation counter, never zero
```

Raw value 0 is the null identity for every domain and is never allocated. Serialized identities, wire frames and operator strings go through `strong_id_from_raw_checked`, which rejects the null identity and any identity whose domain tag does not match the expected field.

Generation counters are per-domain and never regress and never wrap. Generation 0 is the invalid generation, the first generation of a durable object is 1, and `Generation::next()` reports failure at the numeric limit instead of rolling over. `IdAllocator::restore_counter` refuses a restored counter lower than the current one, so durable identity allocation cannot re-issue an identity that already exists.

`CoordinatorEpoch` is the coordinator incarnation counter. It advances every time a coordinator process takes ownership of a durable foundry run, it is never derived from wall time, and it can only be preserved by durability. After recovery every authority issued under the previous epoch is dead.

## Population and candidate lifecycle

A population is an explicit runtime object, not a loop counter. It carries its own task contract, policy binding, budgets, lifecycle state and closure contract.

```
CREATED -> READY -> RUNNING <-> EVALUATING -> SELECTING -> ADVANCING
                       |            |            |
                       +------------+------------+--> REVALIDATION_REQUIRED
                                                          |
                    CLOSING -> CLOSED                     v
                    FAILED, CANCELLED                back to RUNNING
```

The closure contract is explicit: a population may close only when every mandatory evaluation is complete, no required attempt is in flight, selection is committed, retention is committed, promotion eligibility is resolved when the population requires it, and no budget reservation is still open. `FoundryCore::closure_blockers` returns the concrete reasons a population may not close, and `close_population` refuses while that list is non-empty.

A candidate is a durable object with a lifecycle, a producing incarnation, a lineage position, an evidence set and a decision history. "Candidate exists" and "candidate is valid" are different states.

```
REGISTERED -> PRODUCING -> PUBLISHED -> EVALUATING -> EVALUATED
                   |            |            |             |
                   v            v            v             v
             PRODUCTION_   PRODUCTION_   REVALIDATION_  SELECTED / RETAINED /
             FAILED        CANCELLED     REQUIRED       RETIRED / SUPERSEDED /
                                                        DISQUALIFIED
```

Candidate output becomes authoritative only after transactional publication: prepare the per-attempt workspace, execute, collect the declared artifacts, validate names and digests, publish into the candidate record, commit the candidate generation, and only then retire the temporary workspace. Legal transitions are a table, published as `candidate_transition_is_legal`.

## Lineage

Lineage is a first-class durable acyclic DAG, not a tree and not a log. `LineageGraph::insert` refuses self-parenting, unknown parents, depth regression, generation regression and re-parenting of an existing node, and `validate()` re-checks acyclicity with an iterative depth-bounded traversal. Depth is bounded by `kMaxLineageDepth`.

Retirement preserves history. A retired or superseded candidate keeps its node, its depth, its parents and its retirement reason; losing selection is not a reason to destroy history, and a retroactive ancestry change would silently invalidate every selection decision derived from it. Ancestor, descendant and root-path traversals are deterministic.

## Evaluation

An evaluation record is generation-bound evidence, not a boolean and not a score. Each record is bound to the candidate generation, the task generation, the evidence generation, the evaluator identity and the coordinator epoch that produced it.

The typed outcomes are `PASS`, `FAIL`, `UNKNOWN`, `UNSUPPORTED`, `ERROR` and `CANCELLED`. Only a complete record with outcome `PASS` from an evaluator kind that is allowed to be authoritative (`authoritative_for_mandatory()`) can satisfy a mandatory gate. `UNKNOWN` never becomes `PASS`, and a record that never reached a terminal state is not complete and is never counted as a pass.

Requirements are typed. A requirement is either `Mandatory` - it must be satisfied by a complete authoritative pass - or `Optional`, in which case it contributes to ranking only. Hard constraints are enforced through mandatory requirements; soft signals never gate anything.

A worker self report is recorded, and it is evidence, not authority. `EvaluatorKind::WorkerSelfReport` is not an authoritative evaluator kind, so no worker claim, however confident, can satisfy a mandatory gate. The reference evaluator launches a real compiler and a real test binary and checks the actual exit status, the produced output and the expected token; process creation alone is never treated as evaluation. When no usable toolchain resolves, the compile-backed evaluator reports `UNSUPPORTED` and never `PASS`.

## Selection

Selection is an authoritative state transition, not a query. It runs a fixed pipeline:

```
authority -> candidate lifecycle -> required evidence -> hard constraints
          -> task compatibility -> policy feasibility -> ranking
          -> stable tie-break -> decision
```

A candidate that fails any stage before ranking never enters ranking, however high its soft score is. Ranking uses only the factors the policy names, and the final factor is always the candidate identity compared exactly, so ranking is a total order and the tie-break is stable across runs and across shuffled input order.

A decision carries an explanation for every candidate: a `RankingEntry` with each factor's raw value, availability, weight and weighted contribution for those that reached ranking, and an `ExclusionEntry` naming the stage and the typed `ExclusionReason` for those that did not. No candidate disappears silently.

A prepared decision carries a SHA-256 over the canonical encoding of every input it used, and committing revalidates that digest against current state. Therefore

```
evaluate state N -> mutate candidate -> commit stale decision from N
```

cannot happen: the commit is refused as stale rather than applied to state it was never derived from.

## Retention

Retention is a different question with a different policy. Selection picks the candidate allowed to advance; retention decides which alternatives remain worth keeping, so a population may select one winner and still retain several alternatives or lineages.

Retention is bounded by policy: `retain_top_k`, a hard `max_retained` ceiling, `max_per_lineage`, and an explicit decision about the selected candidate. Under a deterministic policy the outcome is deterministic, and every entry carries a typed `RetentionOutcome` explaining whether a candidate was retained because it was selected, for lineage diversity, or retired below the cut, at the lineage cap, at the capacity limit, or as ineligible. Retention has its own canonical state digest, revalidated at commit, so retention cannot race retirement or supersession. Retiring a candidate never destroys provenance.

## Promotion eligibility

The foundry determines whether a selected candidate is eligible to be handed to an Artifact Promotion boundary, and it emits a typed `PromotionRequest`: foundry, run, epoch, population, task, candidate, lineage, selection generation, policy generation, evidence generation, artifact references and digests, mandatory evidence summaries, root-first ancestry and a canonical state digest.

Requested is not promoted. `PromotionEligibilityState` has no "promoted" value, `PromotionHandoffState` stops at "accepted for processing", and a receipt means the receiving system took responsibility for its own promotion decision - it does not mean the artifact was promoted. The foundry never reports otherwise.

## Distributed authority

A worker is addressed by two identities. `WorkerId` is durable and stable - the operator's name for "the thing that runs work". `WorkerBootId` identifies one process incarnation of that worker and is drawn from process entropy, never from the run allocator, so a restarted worker cannot replay the authority of the incarnation it replaced. Every authorization is bound to both, plus the run, the session, the session generation and every relevant object generation, so a stale but otherwise perfectly valid message is rejected outright rather than mutating current state.

When a worker dies, the foundry does not invent an outcome. An attempt that was dispatched and never resolved becomes `OUTCOME_UNKNOWN`, which is exactly the truth: the foundry cannot determine whether the work completed. It does not record success, and it does not record failure either. A reconnecting incarnation must complete a full revalidation before it may accept new work, and that revalidation issues a fresh session and session generation.

When the coordinator restarts, the epoch advances, every worker session is dead, every worker is offline, attempts that were never dispatched are cancelled, attempts that were in flight become `OUTCOME_UNKNOWN`, prepared-but-uncommitted decisions are discarded, and every live population moves to `REVALIDATION_REQUIRED`.

## Persistence and recovery

The snapshot format is versioned and integrity checked:

```
offset 0   magic            "AFSN"              4 bytes
offset 4   format version   little endian u32   4 bytes
offset 8   payload length   little endian u64   8 bytes
offset 16  payload CRC-32C  little endian u32   4 bytes
offset 20  reserved         0                   4 bytes
offset 24  payload          payload length bytes
```

Decoding is bounded on every length and count before anything is allocated from an untrusted size field, and every record is validated semantically after parsing: enum ranges, generation non-regression, reference integrity, lineage acyclicity and duplicate identity rejection. Loading rejects truncation, trailing garbage, an unsupported version, an integrity mismatch and content that parses but is not a valid foundry state, each with its own error code.

Writes are transactional. A snapshot is serialized under a shared lock, written to a temporary sibling, flushed, closed, then replaced atomically using Windows replacement semantics rather than an assumed POSIX rename. Shutdown writes a final snapshot and verifies that it reloads to an equal state, so the final durable image is proven readable rather than assumed.

Recovery rests on one ordering rule the coordinator follows every time it authorizes or acknowledges work:

```
mutate -> persist -> notify
```

An authorization that was never persisted is never sent, and work that was never durably dispatched is never mistaken for work that happened. A publication is acknowledged to a worker only after it is durable, so a worker that never receives an acknowledgement has not been told anything false.

## Failure semantics

Every operation returns either a value or a `Status` carrying a stable machine-readable `ErrorCode` plus a human-readable diagnostic. Distinct failures are never collapsed into a generic `false` or an exception string.

The codes are grouped by domain and the strings are part of the observable contract: identity (100s), arguments and encoding (200s), lifecycle (300s), authority and generations (400s), lineage (500s), evaluation, selection and retention (600s), budgets and resources (700s), persistence (800s), transport and protocol (900s), filesystem and workspaces (1000s), process and lifecycle (1100s), capability (1200s).

Authority rejections are specific rather than generic: `StaleCoordinatorEpoch`, `StaleWorkerBoot`, `StaleSession`, `StaleAuthority`, one code per stale generation, `SupersededCandidate`, `CancelledAttempt`, `LateCompletion`, `DuplicateResult`, `RevalidationRequired` and `GenerationRegression`. `is_stale_authority` and `is_persistence_rejection` let a caller implement deterministic revalidation without matching on individual codes.

`OUTCOME_UNKNOWN` exists as a first-class outcome and a first-class error code because "the foundry cannot tell" is a real answer. It is never recorded as success and never as failure.

## Security boundary

What is validated:

- **Untrusted frames.** Every frame carries magic, protocol version, message type, flags, a sequence number, a payload length and a CRC-32C over the payload. Payload length is bounded before allocation, and the decoder handles both split reads and coalesced reads. Every mutating message carries enough authority to be rejected outright when it is stale.
- **Untrusted snapshots.** Version, integrity, every length and every count are checked, then the records are validated semantically. Nothing is allocated directly from a size field.
- **Untrusted paths.** A path derived from worker-influenced input is canonicalized, checked to be inside its workspace root, and rejected on absolute paths, drive-relative paths, UNC prefixes, traversal components, control characters, non-printable bytes, trailing dots or spaces, over-long paths and Windows reserved device names.
- **Reparse points.** Every component below the workspace root is checked, so a symlink or junction cannot be used to escape the root.
- **Child processes.** Every process is launched through an explicit executable plus an argument vector. No shell is ever involved, so no argument can be reinterpreted as shell syntax. Children are created without a visible console window and without an inherited handle set.
- **Resource bounds.** Frame size, queued frames, snapshot size, records per kind, string lengths, artifact count and size, captured child output, connections, candidates and lineages all have explicit ceilings in `include/autonomous_foundry/limits.hpp`.
- **Artifact names.** A logical artifact name is restricted to a conservative character set so that it can never be interpreted as a path by a downstream consumer, even though it travels into logs, JSON and promotion systems.

No sandbox is implemented. Process separation is not a sandbox: the reference worker executes only the deterministic strategies it ships with, and the runtime does not claim to confine hostile code.

## Validation: REAL, SYNTHETIC, UNSUPPORTED

**REAL** - proven by the reference deployment and its suites:

- independent OS-process workers over real framed TCP on the loopback interface
- coordinator process restart with epoch advance and stale-authority rejection
- worker process kill and reincarnation under a fresh boot identity
- persistence corruption rejection across truncation, trailing garbage, bad integrity, unsupported version and semantically invalid content
- compiler-backed evaluation of generated candidate source when a C++ toolchain resolves, reported as `UNSUPPORTED` when it does not

**SYNTHETIC / REFERENCE** - deterministic, plainly labelled, never presented as more:

- the deterministic reference worker strategies
- the reference diversity key, which is a declared strategy label plus a deterministic structural bucket of the published source. It is a reference signal, not semantic novelty.

**UNSUPPORTED** - outside this boundary, not implemented and not claimed:

- external commercial model fleets
- hostile-code sandboxing
- physical multi-node deployment (all proofs are loopback on one host)
- accelerator execution

## Build

Configure and build with CMake and Ninja from a Visual Studio developer environment:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

For a debug build, either build the configured tree in the Debug configuration or use a separate directory:

```
cmake --build build --config Debug
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

CMake 3.20 or newer and a C++20 compiler are required. On MSVC the build uses `/W4` with `/permissive-`, `/utf-8`, `/guard:cf`, `/sdl` and `/WX`, so first-party warnings are errors; `AUTONOMOUS_FOUNDRY_WARNINGS_AS_ERRORS=OFF` relaxes only that last flag. AddressSanitizer is available through `-DAUTONOMOUS_FOUNDRY_ENABLE_ASAN=ON`.

CUDA is not required. A network model provider is not required. No other Summon Software Labs repository is required.

The tree contains the complete runtime: the library sources in `src/`, the public API in `include/`, the validation suites in `tests/`, the front ends in `apps/`, the examples in `examples/` and the benchmarks in `benchmarks/`. `CMakeLists.txt` is the authoritative list of the sources a build needs. See [docs/VALIDATION.md](docs/VALIDATION.md) for the per-step validation surface.

## Test

```
ctest --test-dir build --output-on-failure
```

The CTest suite registers `af_core_tests` for the core suite and, when the front ends are built, `af_distributed_tests` for the multiprocess suite. Both are the same harness with a different set of registered cases.

Run one case by its 1-based index or by its stable `suite::case` name:

```
af_tests --list
af_tests 12
af_tests lineage::retirement_preserves_history
af_distributed_tests --list
af_distributed_tests 3
af_distributed_tests distributed_worker_death::restarted_worker_registers_a_new_boot_and_the_old_boot_is_refused
```

The multiprocess suite registers seven cases across three suites: `distributed_worker_death` (a killed worker leaves an ambiguous attempt and a survivor retries the slot; a restarted worker registers a new boot and the old boot is refused), `distributed_coordinator_restart` (the restart advances the epoch and refuses authority from the previous incumbent; dispatched work is recorded as ambiguous rather than completed; a published but unevaluated candidate is marked `REVALIDATION_REQUIRED`), and `distributed_proof` (the framed-TCP pipeline commits a winner and a retention and then shuts down; an unevaluable mandatory gate stays `UNSUPPORTED` and never becomes a winner).

`--list` prints every registered case with its index and its source file and then stops. Running a case always prints the fully qualified `suite::case` name, so a marker can be pasted straight back onto the command line.

The harness prints unbuffered, immediately flushed markers: `BEGIN <suite>::<case>` before the body, `PHASE <suite>::<case> <PHASE>` at every blocking, concurrent or process boundary, and `PASS <suite>::<case>` or `FAIL <suite>::<case>: <reason>` when the case returns. The final line is `SUITES <n> CASES <n> PASSED <n> FAILED <n>`, and the process exits 0 only when every selected case passed. Every marker line is flushed in the same call that writes it, so the last marker produced survives a case that never returns; the failing case and the phase it was in are identifiable instead of leaving an empty result and an unknown verdict. A case that throws, or a case during which `std::terminate` is called, is still attributed to the right case name.

The harness entry point (`tests/test_main.cpp`) and its support header (`tests/test_support.hpp`) define the contract above, and every case in `tests/` is registered into it.

## Examples

The build declares seven runnable example programs, one per translation unit in `examples/`, each using the public API only:

- `ex_basic_population`
- `ex_selection_and_retention`
- `ex_lineage_evolution`
- `ex_stale_authority_rejection`
- `ex_promotion_handoff`
- `ex_persistence_recovery`
- `ex_cancellation`

`ex_basic_population` drives the reference task through one complete in-process population: define the reference policy and task, create and start a population, register three worker incarnations and mark them ready, then for every worker slot run `authorize_attempt`, `confirm_dispatch`, `acknowledge_attempt` and `publish_candidate`; evaluate every published candidate through the evaluator registry and record the evidence; then print the resulting candidate states. Dispatch and publication stay separate steps: dispatch records that an assignment was handed to a worker, publication records that output was committed, and only a complete authoritative evaluation record satisfies a mandatory gate. Exit code 0 means every step succeeded and the final state is self-consistent.

## Command-line tools

The build declares three front ends:

- **af_cli** - an operator command-line client that submits and inspects foundry state.
- **af_coordinator** - the process that owns durable foundry state, accepts framed TCP connections and drives the state machine.
- **af_worker** - the reference worker process that connects, produces a candidate inside its assigned workspace and publishes it.

The entry points are `apps/af_cli.cpp`, `apps/af_coordinator_main.cpp` and `apps/af_worker_main.cpp`; each is a thin front end over the library entry point it names.

## Installation and find_package

```
cmake --install build --prefix <prefix>
```

The install exports the static library, the entire public header tree, the command-line front ends when they are built, and a CMake package. A consumer uses it like this:

```cmake
find_package(AutonomousFoundry CONFIG REQUIRED)
target_link_libraries(app PRIVATE AutonomousFoundry::autonomous_foundry)
```

The package version is `1.0.0` with `SameMajorVersion` compatibility, the exported target namespace is `AutonomousFoundry::`, and the exported target set is defined in `AutonomousFoundryTargets.cmake` under `lib/cmake/AutonomousFoundry`. Threads are resolved by the package itself; on Windows the socket library is linked by the exported target.

## Benchmarks

`af_benchmarks` is the benchmark executable, built from `benchmarks/af_benchmarks.cpp` with `benchmarks/bench_support.cpp`. It reports measured timings and throughputs for the runtime's hot paths - identity allocation, canonical encoding and digests, snapshot serialization and decoding, transition application and selection over a candidate population - as plain text on stdout, and each line names the measurement it reports. It is a measurement tool, not a pass/fail gate.

## Known genuine limitations

- All distributed proofs run on the loopback interface of one host. There is no multi-node deployment, no RDMA and no cross-host clock or failure model.
- Candidate artifacts are carried and cached in memory by the reference coordinator. Durable artifact storage belongs to an adjacent system.
- The reference worker executes only the deterministic reference strategies it ships with. It never executes arbitrary generated code, and no sandbox is implemented or claimed.
- No execution deadline is imposed anywhere by design. No operation in this repository imposes a deadline, a watchdog or an execution-duration limit on itself, on a test, or on a child process it launches. A hang is a defect to diagnose.
- The reference diversity key is a declared strategy label plus a deterministic structural bucket. It is not a measure of semantic novelty, and no claim of novelty is made from it.
- The budgets the foundry enforces are only the quantities it can measure itself: candidate attempts, worker assignments, evaluation attempts, retained candidates, retries, candidates, workers, active attempts and evaluation concurrency. It does not fabricate wall-clock, token, dollar or accelerator budgets.
- The compile-backed evaluator requires a resolvable C++ toolchain. Without one it reports `UNSUPPORTED`, which is a truthful answer and not a pass.
- Promotion beyond the typed request is out of scope: the foundry stops at eligibility and handoff.
- Every proof in this repository runs on one host. Coordinator and worker processes are real OS processes and the transport is real framed TCP on the loopback interface, but no claim is made about a multi-host failure model, a cross-host clock or a network partition.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
