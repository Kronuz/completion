# task-driver: the completion driver on task::task

> Status: CURRENT (2026-11-02) Detent W2 Q6: `AsioCompletionDriver` returns `task::task<void>`.

Branch `phase/task-driver`, base `main` e2c6474, task pinned at `d3d94f3`. Owner rule: one shape
everywhere, `task::task` is the family's only coroutine type; no shim, no second return type.

## State

Done at completion:bf69026; ready for review and landing. CI run 41573992492 (workflow_dispatch;
`ci.yml` triggers only on main and pull requests) green on linux-x86_64-gcc, linux-x86_64-clang,
linux-arm64-gcc, linux-asan-ubsan, linux-tsan, macos-arm64, macos-x86_64 and freebsd-x86_64
(headers skipped by configuration). Not merged. Cascade branches pin this head:
journal:836c98b (CI 41574972922 green on all eight jobs) and consensus:c7bf941.

## Done

- completion:bf69026 `asio_completion.h`: `run`, `run_one`, `drive` return `task::task<void>`; the
  constructor takes `task::asio_scheduler&`. The owner check compares `task::current()` with the
  scheduler the driver was built with (explicit, injected once, the identity the I/O objects'
  executor comes from); `task::yield()` would use the thread-local current scheduler instead, so
  the 16-step yield uses `schedule_on{owner_}`. The queue wait is one `await_asio` over
  `make_parallel_group(wait_read, timer).async_wait(wait_for_one())`, the race `operator||` made;
  no token is passed, so the drive stays uncancellable.
- `CMakeLists.txt`: `COMPLETION_ASIO` (default on for a top-level build) fetches task and links
  `task::asio`; `completion::completion` is unchanged.
- `test/asio_extension_test.cc`: ported; new sections `homes` (two schedulers on two threads: a
  run awaited through the owner returns the awaiter to its own scheduler and thread; another
  scheduler is refused), `yield` (work posted at step 3 runs at step 15, via a queue that never
  reports "not yet"), `cancel` (a token requested 20 ms into a 100 ms held drive: the drive
  completes all 21 steps and the caller's token-bound await sees the request). A second argument
  runs one section (`core`, `homes`, `yield`, `cancel`).
- Proof, macOS arm64 (AppleClang 17): full suite 19 of 19 in ASan+UBSan (Debug) and in TSan
  (RelWithDebInfo), separate builds; the same on Linux in CI.
- Mutants (`~/.scratch/completion-task-driver/mutants.sh`), each rebuilt and run per section:
  M1 owner check removed: `homes` fails only. M2 16-step yield removed: `yield` fails only. M3
  drive abandons its first wait with the original in flight: `core`, `homes`, `cancel` fail.
- `CHANGELOG.md`, README, ARCHITECTURE, AGENTS updated.

## Next

The lead lands the branch; journal and consensus re-pin to completion main's hash.

## Blocked on

Nothing.
