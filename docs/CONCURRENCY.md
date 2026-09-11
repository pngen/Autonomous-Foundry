# Concurrency and lock ownership

This document is the audit trail for the concurrency contract. It is meant to
be read before changing any synchronization in this repository.

## The single rule

> Exactly one lock guards all foundry state. It is never held across anything
> that can block, call out, wait, or re-enter.

That lock is `FoundryCore::Impl::mutex`, a `std::shared_mutex`.

- Read paths take `std::shared_lock` and return immutable copies. No pointer or
  reference into internal state escapes a read path.
- Mutating paths take `std::unique_lock` for one validated transition and
  release it before returning.
- No public method takes the lock twice. There is no read-to-write upgrade
  anywhere, which is what makes the "one lock" claim checkable by inspection.

Methods on `Impl` suffixed `_locked` assume the caller already holds the lock
in the required mode and never take it again. They are declared in
`src/foundry_core_impl.hpp` with that suffix precisely so an accidental nested
acquisition is visible at the call site.

## What is never done while holding the lock

- no socket send or receive
- no `accept`, `connect` or `select`
- no process creation, process wait or process kill
- no file read, write, flush or replacement
- no evaluation, compilation or command execution
- no user-supplied hook or virtual call into external code
- no thread join or thread creation
- no condition-variable wait

The coordinator is structured so that this is natural: it calls a
`FoundryCore` method, gets a value or a `Status` back, releases the lock by
construction, and only then performs I/O.

## Lock inventory

| Lock | Protects | Held across blocking work |
| --- | --- | --- |
| `FoundryCore::Impl::mutex` | all foundry state | never |
| `Connection::queue_mutex_` | one connection's outbound frame queue | never; the writer thread pops one frame under the lock, releases it, then calls `send` |
| `Connection::close_mutex_` | the recorded close reason | never |
| connection registry mutex | the connection map | never |
| evaluation pool mutex | the evaluation job queue | never; a worker pops a job, releases, then runs the job |
| artifact cache mutex | the reference in-memory artifact cache | never |
| pump mutex | the pump's condition variable | never |

Every one of these is a leaf lock. No lock is ever acquired while another is
held, so there is no lock ordering to get wrong.

## Audited anti-patterns

Each row was checked by inspection, and where a check is behavioural it is also
covered by the concurrency test suite.

| Anti-pattern | Finding |
| --- | --- |
| read-lock to write-lock upgrade on the same lock | none: mutating paths take the unique lock first |
| write lock held across a call that reacquires the same state | none: `_locked` helpers never acquire |
| mutex re-entry through a callback | none: the core has no callbacks |
| event emission beneath an internal lock | none: the core emits nothing; the coordinator sends after the call returns |
| network I/O beneath a state lock | none: `Connection::send` only enqueues |
| process wait beneath a state lock | none: all process work happens on the evaluation pool |
| worker shutdown while holding a worker-required lock | none: connections are closed after the core call returns |
| thread join while holding required state | none: joins happen in destructors and in `shutdown` outside any core call |
| reversed lock ordering | none: every lock is a leaf |
| cancellation lock inversion | none: `cancel_attempt` is one exclusive-lock transition |
| persistence callback re-entering state | none: serialization copies a snapshot under a shared lock and writes outside it |
| evaluation completion racing cancellation | resolved by state: a terminal attempt refuses a late publication, and a settled candidate refuses new evidence |
| selection racing candidate publication | resolved by the prepared-decision digest: commit revalidates and refuses a decision derived from state that has since changed |
| retention racing retirement | resolved the same way, with the retention digest |
| shutdown racing result publication | resolved by ordering: shutdown stops accepting, then drains the evaluation pool, then persists, then closes |

## Ordering guarantees

The coordinator performs, in this order, every time it authorizes or
acknowledges work:

```
mutate state  ->  persist snapshot  ->  send the frame
```

The consequences are deliberate:

- If the process dies between the mutation and the persist, the durable state
  never mentioned the authorization, so nothing is invented on recovery.
- If it dies between the persist and the send, the durable state says the
  attempt was dispatched but no worker ever saw it. Recovery turns that into
  `OUTCOME_UNKNOWN`, which is exactly the truth.
- A publication is acknowledged to a worker only after it is durable, so a
  worker that never receives an acknowledgement has not been told anything
  false.

## Shutdown

Graceful shutdown: stop accepting new connections; stop the pump so no new
assignment is authorized; ask every worker to close; drain the evaluation pool
by letting queued jobs finish rather than abandoning them; persist a final
snapshot and verify that it reloads to an equal state; remove the workspace
root with bounded removal; close sockets; join threads. Repeated start and
shutdown of the same process is exercised by the test suite.

## Failure semantics under concurrency

Cancellation is real. An attempt cancelled before publication can never later
become successful: its candidate moves to a terminal state and every later
publication, evaluation or selection attempt is refused with a typed rejection.
Cancellation at the boundaries is tested: before dispatch, after dispatch but
before acknowledgement, after acknowledgement but before a result, during
publication, between publication and evaluation, during evaluation, between
evaluation and selection, and during a population generation transition.

Timeout policy: none exists. No operation in this repository imposes a
deadline, a watchdog or an execution-duration limit on itself, on a test, or on
a child process it launches. A hang is a defect to diagnose. The test harness
emits an unbuffered marker before every blocking phase precisely so that a case
that never returns is identifiable from the last line it printed.
