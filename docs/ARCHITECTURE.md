# Architecture

Autonomous Foundry coordinates production populations of autonomous workers. It
decides which candidates may execute, which outputs are valid, which candidates
survive selection, which lineages remain worth retaining, and which result is
authoritative enough to advance toward promotion.

It is not an agent loop, and it is not an agent runtime.

## Systems boundary

Autonomous Foundry **owns** the higher-order coordination of a production
population:

- population identity and lifecycle
- candidate identity, generation and lineage
- candidate execution state and production attempts
- worker-to-candidate assignment
- evaluation requirements and collected evidence
- selection policy and the selection decision
- retention policy and the retention decision
- candidate supersession and retirement
- promotion **eligibility** and the typed handoff request
- generation-fenced foundry authority
- durable foundry state and population-level accounting
- population closure
- explainable selection and rejection

Autonomous Foundry **does not own** the concerns of the adjacent runtimes it is
designed to sit above. It publishes narrow interfaces instead:

| Adjacent system | Owns | The foundry's interface |
| --- | --- | --- |
| Agent Scheduler | when persistent autonomous workers may run | a worker connects when the scheduler lets it |
| Agent Runtime | one long-lived agent across models, tools, memory and budgets | the foundry only sees an accepted assignment and a published result |
| Model Router | model choice under policy, cost and capability | none; the foundry never names a model |
| Ensemble Fabric | multiple model attempts, judges, arbitration, consensus | none; an ensemble may sit behind one worker |
| Critic Fabric | reusable bounded critique workers | none |
| Experiment Fabric | hypotheses, branches, metrics, rollback | none |
| Lab Scheduler | experiments across models, GPUs, datasets and simulators | none |
| Research Ledger | durable provenance and accounting for every call and artifact | the foundry emits digests and evidence summaries the ledger can re-verify |
| Artifact Promotion | the trust transition that approves an artifact | `PromotionSink`: the foundry emits a request and never claims promotion |

A candidate does not become authoritative because an agent produced it. It
becomes authoritative only after it survives validity, evidence, testing,
policy, selection, lineage, budget, freshness and authority gates.

## Layering

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

## The state machine

`FoundryCore` is a pure in-process state machine. It performs no I/O, spawns
no threads, calls no user hooks and never blocks. Every public method either
takes a shared lock and returns an immutable copy, or takes the exclusive lock
and applies one validated transition. That is what makes the concurrency
contract auditable: there is exactly one lock, and it is never held across
anything that could block.

The state machine is implemented across five translation units that share the
private header `src/foundry_core_impl.hpp`:

| File | Responsibility |
| --- | --- |
| `foundry_core.cpp` | read paths, canonical encodings, transition helpers, closure blockers |
| `foundry_lifecycle.cpp` | task/policy/population definitions, population lifecycle |
| `foundry_dispatch.cpp` | worker sessions, dispatch, publication, evaluation ingestion, cancellation |
| `foundry_select.cpp` | selection, retention, promotion eligibility, closure, evolution |
| `foundry_recovery.cpp` | coordinator restart recovery and the self audit |

## Populations

A population is an explicit runtime object, not a loop counter. It carries its
own task contract, policy binding, budgets, lifecycle state and closure
contract.

```
CREATED -> READY -> RUNNING <-> EVALUATING -> SELECTING -> ADVANCING
                       |            |            |
                       +------------+------------+--> REVALIDATION_REQUIRED
                                                          |
                    CLOSING -> CLOSED                     v
                    FAILED, CANCELLED                back to RUNNING
```

A population may close only when its closure contract holds: every mandatory
evaluation is complete, no required attempt is in flight, selection is
committed, retention is committed, promotion eligibility is resolved when the
population requires it, and no budget reservation is still open.

## Candidates

A candidate is a durable object with a lifecycle, a producing incarnation, a
lineage position, an evidence set and a decision history. "Candidate exists"
and "candidate is valid" are different states and the runtime never lets one
collapse into the other.

```
REGISTERED -> PRODUCING -> PUBLISHED -> EVALUATING -> EVALUATED
                   |            |            |             |
                   v            v            v             v
             PRODUCTION_   PRODUCTION_   REVALIDATION_  SELECTED / RETAINED /
             FAILED        CANCELLED     REQUIRED       RETIRED / SUPERSEDED /
                                                        DISQUALIFIED
```

Candidate output becomes authoritative only after transactional publication:
prepare the per-attempt workspace, execute, collect the declared artifacts,
validate names and digests, publish into the candidate record, commit the
candidate generation, and only then retire the temporary workspace.

## Lineage

Lineage is a first-class durable acyclic DAG. It survives retirement and
supersession, because losing selection is not a reason to destroy history. The
graph refuses self-parenting, unknown parents, depth regression and
re-parenting of a finalized node, and `validate()` re-checks acyclicity with an
iterative depth-bounded traversal.

## Evidence, selection and retention

Evaluation results are generation-bound evidence, not booleans and not scores.
A record is bound to the candidate generation, the task generation, the
evaluator identity and the coordinator epoch that produced it. Only a complete
record from an evaluator kind that is allowed to be authoritative can satisfy a
mandatory gate, and `UNKNOWN` never becomes `PASS`.

Selection runs a fixed pipeline:

```
authority -> candidate lifecycle -> required evidence -> hard constraints
          -> task compatibility -> policy feasibility -> ranking
          -> stable tie-break -> decision
```

A candidate that fails any stage before ranking never enters ranking, however
high its soft score is. Ranking uses only the factors the policy names; the
final factor is always the candidate identity, compared exactly, so ranking is
a total order and the tie-break is stable across runs and across shuffled input
order.

A prepared decision carries a SHA-256 over the canonical encoding of every
input it used. Committing revalidates that digest against current state, so

```
evaluate state N -> mutate candidate -> commit stale decision from N
```

cannot happen.

Retention is a different question with a different policy: a population may
select one winner and still keep several alternatives or lineages. Retention is
bounded, deterministic under a deterministic policy, and never destroys
provenance.

## Authority and generations

Authority is bound to every mutable source that can make a decision stale:

```
CoordinatorEpoch, FoundryRunId, WorkerId + WorkerBootId, SessionId +
WorkerSessionGeneration, PopulationGeneration, TaskGeneration,
CandidateGeneration, AttemptGeneration, AssignmentId, PolicyGeneration,
EvaluationGeneration, EvidenceGeneration
```

Four things that informal runtimes conflate are kept separate:

| | Meaning | Survives restart |
| --- | --- | --- |
| historical truth | what provably happened, recorded durably | yes |
| durable identity | which object a record is about | yes |
| recovered state | what a new coordinator believes after reading storage | yes, as state |
| live authority | the right to mutate current state, right now | **no** |

After a coordinator restart the epoch advances, every worker session is dead,
every worker is offline, attempts that were never dispatched are cancelled,
attempts that were in flight become `OUTCOME_UNKNOWN` rather than being
invented as complete or failed, prepared-but-uncommitted decisions are
discarded, and every live population moves to `REVALIDATION_REQUIRED`.

## Persistence and recovery

The snapshot format is versioned, integrity-checked with CRC-32C, bounded on
every length and count, and validated semantically after parsing. Writes are
transactional: serialize under a shared lock, write to a temporary sibling,
flush, close, then replace atomically (with Windows replacement semantics, not
an assumed POSIX rename).

Ordering rule the whole recovery story rests on:

```
mutate -> persist -> notify
```

An authorization that was never persisted is never sent, and work that was
never durably dispatched is never mistaken for work that happened.

## Connection lifetime

A framed connection owns a reader thread and a writer thread. Each of those
threads holds a reference to the connection for as long as it runs, because a
connection is regularly released by the very callback it is delivering: a peer
that disconnects is reported through the close callback, and the owner of that
callback drops its last reference there, on the reader thread. Holding the
reference means the destructor can only run once that thread has finished, so it
can detach the thread it is itself running on instead of leaving a joinable
thread behind -- destroying a joinable `std::thread` would terminate the process.
A disconnect is therefore an ordinary event: the worker's record becomes
`Offline`, an attempt that was in flight becomes `OutcomeUnknown`, the state is
persisted, and the coordinator keeps serving every other connection.

## Absent identities in the wire format

Every identity field that the domain allows to be absent is encoded and decoded
as optional, with raw zero meaning "no identity", exactly as the durable format
already did. Three examples carry real meaning rather than being a convenience:
an in-flight candidate has no producer yet, a candidate slot has no assignment
until dispatch is confirmed, and a published candidate's producer is recorded
separately from its evidence. Reading such a field strictly made a legitimate
state unrepresentable and turned it into an undecodable frame -- a stale-boot
refusal became a protocol violation.

## Realms of evidence

The reference deployment proves the boundary locally. It does not claim more.

| Capability | Label |
| --- | --- |
| independent OS-process workers over real framed TCP | REAL |
| coordinator process death, restart and epoch advance | REAL |
| worker process death and reincarnation under a fresh boot identity | REAL |
| compiler-backed evaluation of generated candidate source | REAL when a C++ toolchain resolves, otherwise UNSUPPORTED and reported as such |
| deterministic reference worker strategies | SYNTHETIC / REFERENCE |
| reference diversity key (declared strategy plus digest bucket) | SYNTHETIC / REFERENCE, not semantic novelty |
| external commercial model fleet | UNSUPPORTED |
| hostile-code sandboxing | UNSUPPORTED: process separation is not a sandbox |
| physical multi-node deployment | UNSUPPORTED: all proofs are loopback on one host |
| accelerator execution | not part of this boundary and not implemented |

## Genuine limitations

- All distributed proofs run on the loopback interface of one host. There is no
  multi-node deployment, no RDMA and no cross-host clock or failure model.
- Candidate artifacts are carried and cached in memory by the reference
  coordinator. Durable artifact storage belongs to an adjacent system.
- The reference worker executes only the deterministic reference strategies it
  ships with. It never executes arbitrary generated code, and no sandbox is
  implemented or claimed.
- Evaluation imposes no execution deadline by design. A hung evaluation is a
  defect to diagnose, not something to paper over.
