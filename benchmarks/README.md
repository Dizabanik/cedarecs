# Cedar ECS benchmark results

Published 2026-11-05. Measurements were recorded on 2026-11-04 UTC.
The engine code is frozen as `adaptive-ecs-v121`, SHA-256
`179e69865c33f154d4e7cbb3e30336f81fb74a2f235843af4e8efbe8f34400b4`.
This publication changes documentation/assets only; the implementation is unchanged.

## Entity suite

| Peer | Cedar median wins |
| --- | ---: |
| EnTT | **27 / 28** |
| pico_ecs | **27 / 28** |
| Flecs | **28 / 28** |

All 28 cases and all 32 process rounds are included. Cedar leads 82/84 median
matchups (97.6%). Empty-world lifecycle is slower than EnTT; adding a component
at 65,536 entities is 0.48% slower than pico_ecs, with overlapping ranges.
The source is abeimler/ecs_benchmark's unchanged entity template. Creation,
destruction and component reads have the upstream timing boundaries; addition
includes timer pause/resume overhead. Existing guarantees/features differ.
The Cedar facade uses checked full-generation/owner access, not the optional
known-live contract. Inputs and component/entity counters match across peers.

Entity process positions are balanced at every one of eight environment
paddings. Executable and output argument paths have equal length. There is no
combined timing score, best-pass selection or removal of unfavorable samples.

## Native C suites

| Suite | Cedar wins vs Flecs | Cases |
| --- | ---: | ---: |
| Original | 47 | 50 |
| Packed | 345 | 386 |
| Indexed | 304 | 386 |

Every original/expanded input is retained. Before/current Cedar and Flecs run
serially with shared frozen kernels and the same generated high-resolution
clock object. A bounded expanded iteration limit of 20,000,000 allows very
short queries to reach their duration target; all other input checks remain.
Adaptive untimed calibration selects the same iterations for all three.
Single-pass creation/deletion remain single-pass. Two warmup rounds are
discarded; all 24 measured rounds are retained. Three execution positions are
balanced at each of eight environment paddings. Checksums, operation counts
and inputs match across implementations and repetitions.

The C refresh also includes an exact v107 Cedar control in the same session.
The original suite has no medians above the 4% before/after limit. Packed has
10 and indexed has 5; the complete gate is therefore **not met**. The largest
ratios are 1.1376 packed and 1.2315 indexed. This is an experimental source,
and the accepted v53 reference is unchanged. Earlier adverse runs are retained.

## Concurrent registry

All 24 fresh automatic-registry cases beat the v107 control. Automatic selection,
registration and promotion are timed. Eight-thread/two-world value enqueueing
improves 3.13×. Four policies are shown: automatic, v107 control, forced
ordinary and forced concurrent. Same-world cache hits do no selection work;
most ordinary ECS workloads need no concurrent registry lookup after caching.
Other world maps retain their structural-write exclusion contract. A concurrent
map does not make arbitrary structural writes thread-safe. Shards have a memory
cost and are allocated only at promotion; producer buffers retain their locks.

Auto beats forced ordinary mode in 20/24 cases and forced concurrent mode in
9/24. It is the fastest of the three policies in 9/24 cases. The largest
measured penalty versus concurrent mode is 60.6% (22.19 versus 13.81 ns per
operation, two threads, eight worlds, empty command scopes). Automatic selection
trades peak lookup throughput for avoiding shard allocations before promotion.
See the [registry policy](../docs/REFERENCE.md#registry-policy) for configuration
and first-use measurements.

## Memory

The fresh full memory suite validates **960 processes / 5,865 checkpoints**,
five backends, 21 scenario types, three populations and three repetitions.
It separates accounted storage (malloc plus page-rounded mapped logical
capacity), physical footprint, RSS, peaks and virtual reservation. Reserved
address space is not substituted for RAM. Full payload checksums, presence,
liveness and analytical counts match. Baseline, registered, populated, removal,
clear/recycling and destroyed states are all retained in the atlas.

Packed/indexed Cedar are separate policies. Indexed high-slot sparse data can
use much more memory; these results do not establish full EnTT/pico memory
parity. The separate query probe contains 96 states, showing 65,536 two-field
binding allocations becoming one; query metadata adds 64 allocated bytes on
this host. Those are query-owned allocations, not whole-process savings.

## Presentation

The layout follows Kama's README: white backgrounds, horizontal zero-origin
throughput bars and Mop/s, ns/op and signed Δ tables. A positive Δ is
`(peer ns / Cedar ns - 1) * 100`. Mop/s is `1000 / median ns/op`, not the
median of reciprocal samples. Entity results divide by actual visited counters;
the empty-world row is per world. Query passes, entity updates and component
transitions are different operations and must be compared within their case.
Grid/complete-poster cases have independent zero-origin scales. Read/lifecycle
overviews and the concurrent overview use one shared scale within each figure.
Whiskers convert observed timing-quartile endpoints; they are not confidence
intervals. Memory uses linear baseline-delta MiB, where lower is better.

The compact subset was defined by input before results: every one of the nine
entity operations at 65,536 entities. The full image contains all 850 timing
cases; the 142-page atlas adds every memory state and registry configuration.
No cases are selected by whether Cedar wins.

## Data and dependency versions

[All entity Mop/s and ns/op tables](entity-tables.md),
[complete native tables](complete-timings.md).

Raw measurements, metadata, source archives, plotting scripts and PDF exports
are kept locally and excluded from Git. The tracked tables and images summarize
those measurements. Compiler commands, exact pins and hashes remain recorded
locally; rebuild dependencies for another host. Primary pins: EnTT
`78213075654a688e9da6bc49f7f873d25c26d12c`, pico
`cfd880ec0804c060ad624a19a02a39b0deeb5ba4`, native Flecs
`75fa09fa2f0874cb26bda8626ffdc0bce41a4f9e`, entity/memory Flecs
`02c8c2666b22ccce5706c8f16efaf813704fe31e`, and Kama
`e838e2a4a1b0ea20f98007fbae4b1a2e723ae97d`. Native flags are `-O3 -DNDEBUG -march=native`;
runtime safety checks stay enabled. Apple M4 Pro/macOS, one host; affinity,
frequency, thermal conditions and background activity are uncontrolled.

### Packed before/after medians above 4%

| Case | v121 / v107 |
| --- | ---: |
| `frame/entities=100000,threads=4,grain=64` | 1.0546 |
| `recycle/entities=10000,threads=1,shuffled=1` | 1.1376 |
| `churn/entities=10000,threads=1,width=2,shuffled=1` | 1.0546 |
| `churn/entities=100000,threads=1,width=2,shuffled=1` | 1.0656 |
| `query_build/entities=1000,threads=1,bits=6,terms=4` | 1.0465 |
| `churn/entities=10000,threads=1,width=2,percent=50,shuffled=1` | 1.0601 |
| `toggle_entity/entities=1000,threads=1,width=32,bytes=0` | 1.0406 |
| `churn/entities=100000,threads=1,width=2,percent=50,shuffled=1` | 1.0831 |
| `set_payload/entities=10000,threads=1,bytes=64` | 1.0436 |
| `churn/entities=100000,threads=1,width=2,percent=1,shuffled=1` | 1.0855 |

### Indexed before/after medians above 4%

| Case | v121 / v107 |
| --- | ---: |
| `delete/entities=10000,threads=1,width=8,bytes=256,shuffled=1` | 1.0492 |
| `frame/entities=100000,threads=4,bits=6` | 1.0540 |
| `frame/entities=1000000,threads=4,grain=64` | 1.0534 |
| `recycle/entities=1000,threads=1,width=8,bytes=256,shuffled=1` | 1.2315 |
| `frame/entities=100000,threads=4,bits=10` | 1.0700 |
