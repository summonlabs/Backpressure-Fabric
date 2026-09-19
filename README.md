# Backpressure Fabric

Open-source, vendor-neutral C++20 runtime for generation-bound propagation of network
resource pressure with bounded scope, damping, loop prevention, authority, and recovery
semantics.

Backpressure Fabric answers one question:

> Given authoritative downstream pressure, topology and dependency relationships, policy,
> damping limits, and current generations, where may backpressure propagate, by how much,
> along which dependencies, and when must propagation stop, decay, be fenced, or be
> rejected to avoid uncontrolled feedback loops?

The answer is a deterministic, bounded, fully explained decision. The fabric never
speculates: a statement that cannot be bound to the exact generations, epoch and
incarnation that justified it is refused, and UNKNOWN pressure never authorises anything.

## Boundary

The fabric owns **pressure-propagation authority, scope, damping, lineage and loop
prevention**. That is the whole product.

It deliberately does **not** own, implement or model:

* congestion detection;
* queue, buffer or memory management;
* rate enforcement or shaping;
* packet pacing;
* admission control;
* path computation or routing;
* flow scheduling;
* generic credit semantics;
* congestion recovery sequencing.

A resource in this model is an identity that pressure may be observed at and applied to.
It is not a queue, it performs no admission, and it enforces no rate.

## Building

Requirements: a C++20 compiler, CMake 3.20 or newer, and a threading library. The
project is warning-clean under MSVC `/W4 /WX` and under GCC/Clang
`-Wall -Wextra -Wpedantic -Wshadow -Werror`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Useful options: `BPFAB_BUILD_TESTS`, `BPFAB_BUILD_TOOLS`, `BPFAB_WARNINGS_AS_ERRORS`,
`BPFAB_ENABLE_ASAN`, `BPFAB_ENABLE_UBSAN`.

## Installing and consuming

```
cmake --install build --prefix /some/prefix
```

The install exports a versioned CMake package. An independent project consumes it with:

```cmake
find_package(BackpressureFabric 1.0 REQUIRED)
target_link_libraries(app PRIVATE BackpressureFabric::backpressure_fabric)
```

`tests/consumer` is exactly such a project. It shares no build tree with the fabric and is
built against the installed prefix only.

## Model

Strong identities are distinct C++ types, not interchangeable integers:

| Concept | Type | Meaning |
| --- | --- | --- |
| Resource | `ResourceId` | A place pressure is observed at or applied to |
| Dependency | `EdgeId` | "pressure at `from` is a reason for pressure at `to`" |
| Signal | `SignalId` | One authoritative pressure observation |
| Propagation | `PropagationId` | One bounded traversal derived from one signal |
| Policy | `PolicyId` + `Generation` | Versioned propagation rules |
| Source | `SourceId` | The authority that observed the pressure |
| Fence | `FenceId` | A hard refusal at a resource |
| Attempt | `AttemptId` | One durable mutation attempt |

Every authoritative statement is bound to:

* the **generation** of the resource definition or dependency relationship it describes;
* the **topology digest** (a canonical digest over every resource and edge, so it commits
  to every dependency generation at once);
* the **epoch** under which authority was granted;
* the **incarnation** (boot id, OS process id, participant index, sequence) that asserted it;
* a **provenance chain** of bounded length whose evicted prefix is folded into a running
  digest, so the whole history is still committed to;
* a **lineage digest** derived from the signal's evidentiary content.

Pressure magnitude is a Q0.16 fraction of capacity. Attenuation is a Q0.16 fraction.
Traversal potential is Q0.32, and it is the quantity that bounds every propagation.

## Propagation semantics

`PropagationEngine::propagate` is a pure, deterministic function of an immutable topology,
an immutable policy, an explicitly owned ledger and an explicitly owned fence table. It
starts no threads, takes no callbacks and holds no locks, so it cannot re-enter anything.

For each admitted signal the engine runs one bounded traversal:

1. The origin is settled with a potential derived from the observed magnitude.
2. A best-first worklist settles each resource **exactly once**, with the highest potential
   that reaches it. Ordering is total (potential, then hops, then resource identity), so
   the result never depends on hash iteration order.
3. Each settled resource expands at most `policy.max_fanout` edges. Every considered edge
   is either accepted or recorded with a specific refusal reason.
4. The traversal stops when the potential falls below `policy.min_propagatable`, when the
   hop bound is reached, when the expansion, settlement or record budgets are exhausted, or
   when a protection boundary or fence forbids it.

Contributions from simultaneous sources are combined once per resource under the policy's
aggregation rule (`Max`, `SaturatingSum`, `WeightedMax`, `PriorityFirst`) and clamped by
`policy.aggregation_ceiling`.

### Invariants and how they are enforced

| Invariant | Enforcement |
| --- | --- |
| Pressure cannot increase through propagation unless explicitly authorised and bounded | Attenuation is at most 1. Potentials are non-increasing unless the edge carries the `Amplifying` flag **and** the policy enables amplification **and** the signal's authority grants it **and** the cumulative gain stays inside the tighter of the policy and authority bounds. The absolute magnitude ceiling is 1.0 and the aggregate is additionally clamped by the policy ceiling. |
| Cycles cannot create infinite feedback | A resource is settled once per traversal. Any further edge into it is refused as `NodeAlreadySettled` and counted as a loop prevention. Cyclic topologies are accepted and remain bounded. |
| Hop count and fan-out are bounded | `max_hops`, per-edge hop overrides, the authority's granted hops, `max_fanout`, `max_expansions`, `max_visited`, and the topology's own degree limits. |
| Stale dependency generations invalidate propagation | The request must carry the live topology digest; optional per-edge pins must match the live dependency generation; a signal whose own topology digest or epoch differs is refused before any traversal. |
| UNKNOWN source pressure cannot authorise propagation | `PressureObservation::can_authorize_propagation()` is the single gate. A policy that would disable the rule fails validation, and the engine refuses an UNKNOWN observation with `UnknownPressure`. |
| Duplicate lineage is idempotent | Lineage is content addressed. A lineage already in the ledger produces an explanation with no hops and no state change. The ledger is bounded by FIFO capacity and reports its evictions. |

Two further rules that fall out of the same discipline: a declared severity that overstates
the measured magnitude is downgraded to the measured one, and the effective hop bound is the
minimum of policy, edge override and granted authority.

## Damping and recovery

* **Cooldown / hysteresis.** An edge may declare a cooldown. Inside the window a repeat is
  refused unless it exceeds the last accepted magnitude by `policy.hysteresis_delta`.
* **Recovery decay.** `PropagationEngine::recover` lowers pressure for one source according
  to `policy.recovery_decay`, bounded by `max_steps`, floored, and clamped so it can never
  raise pressure. Recovery is convergent: applying it repeatedly reaches the floor instead of
  oscillating.

## Explanation

Every accepted signal produces a `PropagationExplanation`:

* the source signal, origin resource, lineage and authority vector;
* the effective ceiling and the granted cumulative gain;
* one hop record per accepted edge, with attenuation, gain, potential before and after, and
  the verdict (propagated, amplified, suppressed, fenced, loop prevented, stale, unauthorised);
* one aggregated record per (resource, refusal reason), with the **exact** count of refused
  edges — so explanation size never grows with fan-out while the counters stay exact;
* per-resource outcomes: applied magnitude, severity, strongest single contribution, every
  contributing source, the winning predecessor and edge (which reconstructs the full path),
  and the governing fence when one applies;
* exact counters for settled resources, hops, suppressions, loop preventions, stale refusals,
  fence refusals, unauthorised refusals, budget exhaustions and amplifications;
* a fingerprint over the whole explanation and a fingerprint over the whole outcome.

When a caller tightens the record budget below the policy maximum, the traversal stops
before applying pressure it could not explain and reports `complete == false`. For a policy
that satisfies `max_records >= max_expansions + max_visited` the explanation is always
complete; a policy that cannot afford its own explanation is rejected at validation time.

## Durability

`DurableStore` is the only component that writes to disk. It persists configuration,
fences, the epoch and boot binding, propagation lineage, the revalidation footprint and
attempt outcomes.

* **Journal format.** Append-only, 8-byte magic, versioned header, CRC-32 protected,
  per-record sequence numbers, and a chained digest over every previous record. A torn or
  tampered tail is detected, reported and truncated back to the last intact record; state is
  never invented from damaged bytes.
* **Crash safety.** Single writer enforced by an operating-system exclusive lock on
  `fabric.lock`. Atomic replacement via temporary file, durability barrier and rename.
  Stale temporary files are removed on open.
* **Mutation protocol.** validate, bind authority, plan, journal intent, apply, verify,
  commit, retire. `Fabric::publish` runs the propagation once as a side-effect-free plan,
  journals the lineage, runs it again to commit, and requires the two outcome fingerprints to
  be identical before the attempt is committed.
* **Recovery distinguishes** durable configuration and history, committed authoritative state,
  unfinished attempts, ambiguous outcomes, stale live authority, and evidence that requires
  revalidation.
* **No liveness is restored.** After restart there are no publishers, no leases, no live
  pressure, no authority and no restored pressure magnitude. The epoch advances, which fences
  every statement made under the previous epoch; durable fences are explicitly re-bound to
  the new epoch; and the persisted pressure footprint becomes an explicit revalidation list.

## Runtime

`Fabric` owns the live authoritative state of one process: installed topology, active policy,
epoch, publisher registry, fences, ledger and the optional durable store.

* **Publishers** are registered with an exact incarnation. A signal from an unregistered
  source, a superseded incarnation or a retired publisher is refused. Registering a new
  incarnation supersedes the old one and the old incarnation can never publish again.
* **Epochs** advance on every durable open and fence every previous statement.
* **Cancellation is real.** A cancellation requested before the authoritative commit boundary
  causes the publish to return `Cancelled` with no ledger mutation and no success report. A
  ticket that already committed reports `completed` and a later cancellation cannot undo it.
* **Shutdown** stops accepting work, cancels every pending ticket, discards queued work,
  lets in-flight work reach its completion boundary, joins workers, persists the pressure
  footprint, retires every publisher and returns the in-memory ledger to an empty baseline.
  It is idempotent.

### Locking

* `Fabric::mutex_` guards the live state; `Fabric::ticket_mutex_` guards tickets;
  `WorkerPool::mutex_` guards the queue.
* The lock order is `ticket_mutex_` then nothing, and `mutex_` then nothing. The pool mutex
  is **never** held while a job runs, and no job holds the fabric lock while touching tickets.
* Workers are joined with no lock held.
* There are no read-write locks anywhere, so read-to-write re-entry is impossible.
* The API exposes no callbacks and emits no events, so no callback can re-enter a held lock
  and no event is emitted under a lock.
* The engine is pure and is invoked while the fabric lock is held; it never calls back into
  the fabric.

## Transport

`backpressure::Connection` and `backpressure::Listener` provide a loopback stream carrying
length-delimited, versioned, CRC-protected frames. Every decoder refuses truncated,
oversized, wrong-version and integrity-failing input. `backpressure::spawn_process` launches
real child processes; the multiprocess suite re-executes the test binary as a child and
exchanges real frames with it.

## Benchmark

`bpfab bench` runs a **SYNTHETIC** propagation benchmark over generated graphs. It measures
completed propagation work, never submission cost. The population is generated; no physical
network, switch, NIC or link is measured or implied.

```
name              nodes  edges  depth fanout src cycles  iters  props     settled    hops     suppr   loops   ms      prop/s
size-small          121    120      4      3   1      0     32       32      3872     3840        0       0     3.499        9145
size-large         4096   4095      6      4   1      0      8        8     32768    32760        0       0    29.775         269
depth-1               9      8      1      8   1      0     16       16       144      128        0       0     0.150      106667
depth-10           2047   2046     10      2   1      0      8        8     16376    16368        0       0    14.525         551
fanout-32          2048   2047      3     32   1      0      8        8     16384    16376        0       0    13.787         580
sources-8          2048   2040      5      3   8      0      8       64     16384    16320        0       0    14.593        4386
cycles-dense        364    875      5      3   1    512      8        8      2912     2904     4096    4096     2.591        3088
attenuation-decay  2048   2047      8      3   1      0      8        8       968      960     1944       0     1.177        6795
```

## Tests

`bpfab_tests` contains 117 cases across unit, integration, property, seeded randomised,
adversarial, concurrency, failure-injection, persistence/restart and real multiprocess
suites. No test declares a timeout: every test is run plainly and allowed to complete
naturally, because a test that does not terminate is a defect to diagnose rather than
something to hide behind a watchdog.

```
bpfab_tests              # everything
bpfab_tests --list       # list cases
bpfab_tests --filter=multiprocess
```

## Proof surface

**REAL**

* Propagation decisions, budgets, damping, loop prevention, staleness refusal and
  explanations, executed in-process on generated topologies.
* Durable state: real files, real CRC and chain verification, real truncation repair, real
  atomic replacement, real exclusive locking, real restart and epoch advancement.
* Real child OS processes exchanging real framed bytes over loopback TCP, including a
  hard-killed publisher, restart fencing and cross-process single-writer exclusion.
* Real worker threads, real cancellation boundaries and real shutdown.
* AddressSanitizer and `/W4 /WX` clean builds on this platform.

**SYNTHETIC**

* Every graph in the benchmark and in the randomised property suites.
* All throughput and latency figures above.

**UNSUPPORTED / NOT CLAIMED**

* No physical network, switch, NIC, DPU, RDMA, NVLink or optical validation of any kind.
* No multi-host validation. The multiprocess suite runs on one machine over loopback TCP.
* No congestion detection, rate enforcement, pacing, admission, scheduling or recovery
  sequencing — those are outside the boundary.
* GCC and Clang builds were not executed in the environment where this release was prepared;
  the warning profile for those compilers is configured but unverified here. MSVC was
  verified.
* No formal proof assistant verification; the invariants are enforced by construction and
  checked by tests.

## Limitations

* The propagation ledger is bounded. Lineage eviction is FIFO and counted; once a lineage is
  evicted, replay of that exact signal is no longer recognised by the in-memory ledger.
  Durable replay protection is bounded the same way and reports its eviction count.
* Recovery decay is driven by the logical tick supplied by the caller. The fabric has no
  wall-clock opinion and no background timers.
* Journal compaction rewrites the journal under the fabric state lock, so a compaction
  serialises concurrent publishes.
* A durable directory has exactly one writer. A second process is refused rather than
  coordinated with.
* The fabric is a library. It ships no daemon, no service wrapper and no configuration file
  format.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.