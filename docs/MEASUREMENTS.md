# completion measurements

Qualification runs and object-size measurements, each recorded when its feature landed. Sizes are object capacity on the stated toolchain, not process RSS or execution time.

These features were built in [Kronuz/cluster](https://github.com/Kronuz/cluster) before completion stood alone, so the baseline commits named here (`5754b66`, `8d4c200`, `38cbadb`) are cluster's. The standalone qualification runs are this repository's CTest suites, under the builds in `AGENTS.md` ("Verification and benchmarks"); the journal and Detent runs are those projects' suites built against these headers. The PMR capacities are printed by `completion_create_into` and `completion_reuse` (`ctest -V`). The type sizes have no probe in the repository; check one by printing its `sizeof` against each commit's headers, with the same toolchain.

## Opaque image jobs

The managed POSIX fixture qualifies admission before file/lock creation, placement-constructor rollback, resource retention after backend retirement, cross-thread final release and an actual image/POSIX/queue completion chain. Four standalone suites and journal compatibility suites pass on macOS, Linux and FreeBSD, including required-native Linux namespace operations with 128-byte names and rejection at 129 bytes. Standalone and Detent publication suites pass under macOS ASan/UBSan; the managed POSIX/native chain passes a fully instrumented ThreadSanitizer run. Measured Intel macOS/libc++ `PosixIO` grows from 24 to 40 bytes while the queue stays 656 bytes. Linux/GCC 13 `PosixIO` grows from 32 to 48 bytes and the queue from 1176 to 1376 bytes, replacing dynamic namespace storage with fixed capacity. These sizes exclude ownership headers and native/runtime baselines and establish no throughput or process-RSS improvement.

The standalone resource fixture qualifies overflow, alignment, allocation and constructor failures, container growth, and last-owner release on another thread. All three standalone suites pass on macOS, Linux and FreeBSD and under macOS ASan/UBSan. The resource fixture also passes a separate fully instrumented ThreadSanitizer run.

The same jobs run through `drive_synchronously()` on a worker or through `NativeQueue`. Callers must retain the owner/admission lease and drive accepted originals through settlement even after consumer cancellation. The standalone test verifies held originals, rejection of stale tokens, multi-chunk writes, native preparation, and the barrier before durable publication. This is primitive/job qualification, not the complete application generation lifecycle acceptance.

The allocation fixture observes one Create-adoption allocation against the previous implementation and zero after this change. It also checks file and admission retention. Standalone completion and Detent publication suites pass on macOS, Linux and FreeBSD and under macOS ASan/UBSan. This allocation count establishes no latency or whole-process memory improvement.

With 120-byte candidate and destination names, the fixture observes three managed preparation allocations and two managed publication allocations, with zero unaccounted global allocations throughout successful job construction and completion. Every observed allocation is fault-injected, including a successful publication retry after both publication allocation failures. Weak controls remain allocated until their final release. Standalone and Detent publication suites pass on all three supported operating systems and under macOS ASan/UBSan. On the measured Intel macOS/libc++ toolchain, `ImagePreparation` remains 256 bytes and `PreparedImage` 16 bytes; `ImagePublication` grows from 240 to 264 bytes for dual legacy/managed name storage. Private state and allocator-control capacity are additional resource-observed allocations, not included in those object sizes. Complete storage lifecycle remains outside these generic job gates.

## Caller-buffer directory scanning

Five standalone suites pass on macOS, Linux and FreeBSD and under macOS ASan/UBSan. The scan fixture also passes fully instrumented ThreadSanitizer. It observes zero global allocations for a 255-byte POSIX name and EOF, retains cursor/backend/scratch through an original after initiating facades disappear, and completes with subsequent PMR allocation refused. Existing journal suites pass on all three platforms, including required-native Linux tests; Detent publication compatibility passes on macOS. These checks qualify the scanning primitive, not application census or disk reclamation.

## Managed metadata, leases and handle settlement

Six standalone suites pass on macOS (1.73 seconds), Linux (0.15 seconds), FreeBSD (0.11 seconds) and macOS ASan/UBSan (1.99 seconds). The inspection/lease/close fixture passes fully instrumented TSan. It checks no-follow symlink/special-file observations, stable held/named identity, hard-link preservation, shared/exclusive contention, forbidden conversion, fresh-handle retry, retained originals, stale-token rejection and invalid/default operands. Journal compatibility passes on all three platforms, including required-native Linux qualification.

On Intel macOS with LLVM 23.1.2/libc++, `PosixFile` remains 16 bytes; result storage grows from 104 to 184 bytes, statistics from 288 to 368 bytes and `NativeQueue` from 672 to 832 bytes relative to cluster `5754b66`. Those queue/control sizes are admitted by managed factories. These object measurements are neither process RSS nor a latency or power improvement. Application census, ownership validation, publication settlement and reclamation remain separate work.

## Coroutine extension dispatch

`run_one(operation)` submits and authentically reaps one primitive before returning to the owner scheduler. It retains the same original ownership and cancellation protection as `run()`, whose complete-operation behavior is unchanged. This permits coordinators to stop at explicit paused operation boundaries, including collection slices, without submitting a paused operation. The coroutine fixture checks one-step progress, retained operation lifetime, subsequent completion and existing metrics behavior. It passes on macOS, Linux and FreeBSD, under ASan/UBSan and fully instrumented TSan. This additive header API changes neither journal bytes nor cluster wire formats; consumers rebuild their header instantiations.

Supplying `-DASIO_INCLUDE_DIR=/path/to/asio/include` to the standalone build adds a dependency-free-host integration test; the module still fetches no Asio. The fixture drives caller-buffer scanning, entry/file metadata, both lease modes and file closure through a coroutine after dropping the initiating operation facade. It qualifies metrics enabled/disabled, invalid kinds, authentic IO errors and each extension histogram. This caught a gap in the newly added primitive integration: native queues accepted extension kinds while the coroutine adapter rejected them. The fixture fails at `NextInto` against the preceding adapter.

All seven standalone suites pass on macOS, Linux and FreeBSD and under macOS ASan/UBSan; the coroutine fixture also passes fully instrumented TSan. Existing journal coroutine qualification passes on macOS. On the measured Intel macOS toolchain, optional metrics and snapshot objects each grow from 3,600 to 5,328 bytes for the six extension histograms in each success/error set. Metrics allocation occurs only when requested; coroutine/runtime and telemetry resources remain separate baselines. This is a capacity measurement, not an execution-time or RSS improvement.

## Explicit directory-cursor settlement

Eight standalone suites pass on macOS (2.23 seconds), Linux (0.15 seconds), FreeBSD (0.14 seconds) and macOS ASan/UBSan (2.63 seconds). The focused cursor fixture also passes fully instrumented TSan. It verifies worker thread identity, cancellation and detached facade retention across a held original, stale-token rejection, authentic close errors and descriptor reuse after wrapper destruction. Journal compatibility passes on all three platforms, with required-native Linux and native legacy coroutine checks. The coroutine extension fixture includes cursor closure with metrics enabled and disabled.

## Preadmitted reusable collector controls

Nine standalone suites pass on macOS (2.57 seconds), Linux, FreeBSD and macOS ASan/UBSan (4.20 seconds). The reusable-control fixture and expanded coroutine fixture pass fully instrumented TSan. Nine closed file controls and one cursor perform 100 repeated open/lease/close/scan cycles with subsequent resource allocation refused, retaining constant addresses and admitted capacity. The fixture also exercises detached native originals, stale tokens, missing/unsafe candidates, legacy and wrong-backend destinations, and reuse of the exact backend object address. Journal compatibility passes on all three platforms, including required-native Linux and native legacy coroutine gates; Detent census/publication passes natively and under ASan/UBSan.

Measured Intel macOS native statistics grow from 384 to 432 bytes and queues from 848 to 896 bytes relative to cluster `8d4c200`; result storage remains 184 bytes and legacy `PosixFile` 16 bytes. Optional coroutine metrics/snapshots grow from 5,616 to 6,480 bytes. The fixture's admitted backend, ten control objects, state and job occupy 1,112 bytes on this toolchain, excluding application-ledger headers and native/runtime baselines. The twelve legacy counter/latency arrays, unique-return APIs, journal bytes and wire formats remain unchanged; header consumers rebuild together. Complete collector preallocation and reuse scheduling belong to the application lifecycle, not these primitive gates.

## Managed image-preparation file settlement

Ten standalone suites pass on macOS (3.10 seconds), Linux (0.32 seconds), FreeBSD (0.23 seconds) and macOS ASan/UBSan (7.59 seconds). The managed preparation/native-driver fixture passes fully instrumented TSan. It covers empty bodies, positive short writes, failures before/after Create/write/sealing, returned-file-plus-error, consumed close failure, first-error preservation, cancellation across every held phase, detached native originals, stale tokens, retained capabilities, unsupported close capability, token bounds and unchanged legacy file retention. Journal compatibility passes on all three platforms; Detent census/publication passes natively and under ASan/UBSan.

On the measured Intel macOS toolchain, preparation grows from 256 to 280 bytes for policy and error state; publication remains 264 bytes and the prepared capability 16 bytes. Nested state, names, native/file controls and runtime resources remain separately admitted or reported. This supplies an opt-in generic lifecycle; application managed publication, logical debt settlement and session final retirement remain distinct integration work.

## Preadmitted exclusive creation

Eleven standalone suites pass on macOS (1.11 seconds), Linux (0.25 seconds), FreeBSD (0.36 seconds) and macOS ASan/UBSan (1.80 seconds). Creation and coroutine fixtures pass fully instrumented TSan; journal suites pass on all three systems, including required-native Linux tests and the native legacy coroutine test. The fixture preconstructs three preparations, refuses further resource allocation and verifies sealing, closure, collision handling, wrong-backend rejection, held originals and failure before/after creation. Its measured PMR capacity is 2,744 bytes on Intel macOS, including the two backends, queue, controls and preparations, excluding caller payload/admission and native/runtime baselines.

Relative to cluster `38cbadb`, measured Intel macOS statistics grow from 432 to 448 bytes and native queues from 896 to 912 bytes. Optional coroutine metrics and snapshots grow from 6,480 to 6,768 bytes. Result storage remains 184 bytes, preparation 280 bytes, publication 264 bytes and the prepared capability 16 bytes. The twelve legacy arrays, primitive ordinals, journal bytes and wire formats remain unchanged; header consumers rebuild together. These are capacity measurements, not RSS or execution-time improvements.

## Turn lane

The Asio driver admits a Turn's primitive ahead of waiting Bulk ones (`IOOperation::lane()`); before, the one queue slot was a plain FIFO. `bench/lane_probe.cc` times a turn (a 4 KiB write, then SyncData) while Bulk tasks keep the slot busy with 64 KiB chunk writes on one loop, one native queue and one `AsioCompletionDriver`. `bench/lane_ab.py` interleaves whole runs of two builds (alternating order, a fresh directory per run) and reports, per column, the median over nine runs and its [min, max] interval. A difference counts only when it clears the A/A band.

Base `completion:8656f12` (FIFO slot, `lane()` present, unused) against head `completion:f579c4e` (Turn lane), 120 turns, 64 KiB chunks, Intel i9-9980HK (16 threads) with macOS 15.8.1 and clang/libc++, `quiet exclusive`. The probe's turns run back to back, so this is the worst case for Bulk. Rows: job 261115-230734-0b0a3e (smoke 261115-225743-cc7228); controls: job 261115-230745-462b77.

| Bulk tasks | Column | FIFO slot (base), median [min, max] | Turn lane (head), median [min, max] |
|---|---|---|---|
| 16 | admission wait p50 (ms) | 21.233 [18.330, 25.397] | 0.502 [0.474, 0.522] |
| 16 | admission wait p99 (ms) | 48.896 [44.441, 50.364] | 0.965 [0.855, 1.112] |
| 16 | turn total p50 (ms) | 42.765 [39.043, 46.997] | 21.957 [21.647, 22.272] |
| 16 | turn total p99 (ms) | 74.166 [70.075, 78.360] | 25.015 [23.917, 38.607] |
| 16 | Bulk MiB/s | 44.1 [42.9, 47.0] | 6.7 [6.1, 7.5] |
| 4 | admission wait p50 (ms) | 7.596 [5.677, 9.808] | 0.544 [0.512, 0.606] |
| 4 | admission wait p99 (ms) | 12.015 [11.761, 12.917] | 1.301 [1.067, 1.415] |
| 4 | turn total p50 (ms) | 28.947 [26.752, 31.602] | 21.932 [21.062, 22.027] |
| 4 | turn total p99 (ms) | 37.029 [35.052, 40.153] | 24.573 [24.009, 37.009] |
| 4 | Bulk MiB/s | 21.3 [19.6, 23.4] | 8.7 [7.3, 10.8] |

Controls (job 261115-230745-462b77, same nine-run protocol):

| Control | Column | Arm a median [min, max] | Arm b median [min, max] |
|---|---|---|---|
| A/A, base against base, 16 Bulk | wait p50 (ms) | 25.992 [18.650, 36.404] | 22.856 [20.303, 30.058] |
| | total p50 (ms) | 49.815 [39.695, 58.626] | 43.576 [41.970, 50.733] |
| | Bulk MiB/s | 40.8 [39.7, 45.4] | 42.1 [40.8, 45.4] |
| Head with the lane off (`LANE_PROBE_FLAT=1`) against base, 16 Bulk | wait p50 (ms) | 30.734 [19.759, 35.734] | 25.085 [21.011, 30.897] |
| | total p50 (ms) | 52.766 [41.721, 66.365] | 46.902 [42.826, 54.084] |
| | Bulk MiB/s | 40.1 [34.2, 43.9] | 41.1 [39.4, 43.5] |
| No Bulk tasks, head against base | wait p50 (ms) | 0.002 [0.001, 0.003] | 0.002 [0.001, 0.003] |
| | total p50 (ms) | 21.679 [21.454, 22.139] | 21.813 [21.587, 22.262] |

Reading. The A/A band is wide: with 16 Bulk tasks the same binary's wait p50 median differs by 3.1 ms (12%) between arms and one arm's runs span 18.7 to 36.4 ms. The lane's effect is far outside it: the turn's admission wait p50 falls 42-fold (21.2 to 0.50 ms) at 16 Bulk tasks and 14-fold (7.6 to 0.54 ms) at 4, and the intervals do not touch. Turn total latency falls to the unloaded floor, 21.7-21.8 ms with no Bulk tasks, from 42.8 ms (p99 74.2 to 25.0 ms) at 16 Bulk tasks and from 28.9 ms at 4. The lane off matches base inside the band, and with no Bulk tasks the head costs nothing (total p50 +0.6%, inside the intervals), so the mechanism, not the build, makes the difference.

The turn lane helps turn latency and hurts Bulk throughput. Bulk writes fall from 44.1 to 6.7 MiB/s with 16 Bulk tasks (-85%) and from 21.3 to 8.7 MiB/s with 4 (-59%), intervals disjoint in both rows. The turns saturate the slot in this probe, so Bulk runs only in the gaps between a turn's primitives; that is the designed trade, and a loop whose turns leave the slot idle between them would see less of it. The probe measures one macOS loop with the native write queue and a worker-run SyncData; Linux (io_uring) and FreeBSD (`aio_fsync`) are not measured, and these numbers establish no throughput or power improvement beyond the turn-latency rows above.
