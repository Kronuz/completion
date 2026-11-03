# Code review: the completion driver on task::task

> Reviewer: code-reviewer, 2026-11-02. Branch `phase/task-driver`, completion:bf69026 (head
> completion:e5472a3), base completion main e2c6474, task pinned at task:d3d94f3.

## Verdict: APPROVE WITH CONDITIONS

The driver does what the ledger says, and the four risks the lead named probe clean under
ASan+UBSan and TSan, including against task's C3 branch. One condition: the `cancel` section
and the consensus cancel request prove nothing (CR1), so the ledger, CHANGELOG and test comment
must stop claiming they do. No blocker in the code.

## Findings

### CR1 (condition): the cancel section passes without the code it tests

`test/asio_extension_test.cc` `cancel_mid_drive` / `uncancelled_drive` (and consensus
`test/completion_asio_test.cc` `exercise`, `cancellation.request()`) create a
`task::cancel_source` that nothing in the driver can observe. `task::task` has no ambient token
(`task.hh`: tokens are explicit arguments to `await_asio(init, token)`, `lock(token)`, budgets);
`drive()` takes no token and none exists on the awaiter chain. So the section passes on any
implementation of the driver, including one that reaches an `await_asio(..., token)`-style
cancellable wait only if a token parameter is added (and then the test would still not hand it
over). Under the old code the same invariant was real: `asio::cancellation_signal` was bound to
the co_spawn, and `disable_cancellation` was the only thing stopping it. Mutant M3 does not
isolate the section: it fails `core`, `homes` and `cancel` because it breaks the drive, not
because it honours a request. The proof gap is real: nothing in the suite fails if a future
change makes the drive cancellable through a token.

Condition: state it as structural (the driver has no token parameter, so a request above it
cannot reach it), drop the "the cancel section proves it" wording from the ledger, CHANGELOG and
the consensus commit message, and keep the section only as a smoke test that a long held drive
completes all 21 steps. If the owner wants it proven, the way is a driver overload that takes a
`cancel_token` and a test that it is ignored (design change; architect). Not a code blocker: the
behavior (uncancellable drive) is by construction correct.

### CR2 (non-blocking): downstream Detent still on asio::awaitable

`Detent/include/detent_delivery/completion_scheduler.hh:16,36` and `coordinator.hh:81,89` return
`asio::awaitable<void>` around the driver and `tests/detent_authority/test_transport_peers.cc:52`
uses it. Detent pins completion by hash, so nothing breaks until the W2 re-pin; the header no
longer includes `<asio.hpp>` (CR3), so those files must include what they use. A follow-up for
the W2 migration, not for this branch.

### CR3 (note, no action): `asio_completion.h` is self-contained

`.scratch/review-td/selfcontained.cc` (a TU of only `#include "completion/asio_completion.h"`,
`-fsyntax-only`) compiles on the branch against task d3d94f3 and against task C3. It gets
`asio::steady_timer` and `asio::error` through `task_asio.hh`, so it depends on task_asio.hh
including them; an include-what-you-use hygiene gap only (`<stdexcept>`, `<system_error>`,
`<memory>`, `<cerrno>` come in transitively).

### CR4 (note): `order[0] == 1 && timer_error` tightens nothing

The old `||` gave `std::variant` and threw on a timer error through `use_awaitable`; the new
code throws `system_error(timer_error)` only when the timer finishes first with an error, and
ignores the timer's `operation_aborted` when the notification wins. Equivalent for every
reachable state. The notification error handling (`!= operation_aborted` sets
`notification_failed_`) matches the old code.

## Probes (all in `.scratch/review-td/`; `run.sh asan|tsan|c3` builds a separate dir and runs
the 19-test suite, `probes.cc`, `selfcontained.cc`)

1. P1 race (`probes.cc` `p1`, 300 iterations, both orders). Standalone `await_asio` over
   `make_parallel_group(fd.async_wait(wait_read, deferred), timer.async_wait(deferred))
   .async_wait(wait_for_one(), handler)`, no token. Checks at resume: winner index, loser's
   error is `operation_aborted`, and `timer.cancel() == 0` (the loser is already reaped; the
   frame never resumes with an operation pending). The group completes its handler only after
   both operations finished, so no handler touches the frame or awaiter afterwards;
   `asio_awaiter::handler` carries no cancellation slot, so the drive stays uncancellable by
   construction, as `disable_cancellation` made it. ASan+UBSan: clean. TSan: clean.
2. P2 owner check (`p2`). `task::sync_wait(driver->run(op))` (no current scheduler) throws
   `std::logic_error` with no state change; a second `run` on the owner while the first is
   suspended is refused and the first completes with the queue not busy. The `homes` test (two
   schedulers, two threads) passes in the branch's suite on ASan, TSan and under task C3
   (task-wt/cancellation-c3 at task:20f1a6a, copied to `.scratch/review-td/task-c3`, full suite
   19/19 and all probes). The 16-step yield uses `schedule_on{owner_}`, which is an
   unconditional `post` (`task.hh:799`), so it does not depend on the inline-resume rule; the
   owner check happens at the first resume of the lazy task, where `current()` is the awaiter's
   scheduler (symmetric transfer from a same-scheduler awaiter; a different-scheduler awaiter is
   refused, as `homes` shows). I found no path where `run()` continues on another thread:
   `await_asio` resumes through `resume_on(sched_, h_)` with the scheduler recorded at suspend.
3. P3 lifetime (`p3_wait`, `p3_yield`). Drive suspended on the 100 ms held queue wait, loop
   stopped, then the documented teardown: `close()`, `destroy_roots()`, driver released,
   scheduler, io_context; once with the test's driver reference dropped first so the frame's
   `shared_from_this` is the last owner (the driver, its timer and descriptor die inside
   `destroy_roots()` with the parallel group's handler still queued; the queued handler is
   dropped unrun). Same for a drive suspended in the 16-step yield. ASan+UBSan and TSan: clean,
   `finished` stays false (no completion callback runs on teardown).
4. P4 build surface. `-DCOMPLETION_ASIO=OFF` configures with no task or Asio fetched
   (`build-off/_deps` held only metrics and allocators; the test target is absent), so
   `completion::completion` is unchanged. ON: `completion::asio` links `task::asio`.
   `asio_completion.h` is self-contained (CR3). Default is ON for a top-level build only; a
   consumer that wants the driver must set `COMPLETION_ASIO` before FetchContent (consensus
   does, `CMakeLists.txt:9`).
5. P5 consensus. Test diff reviewed (`git show consensus:c7bf941`): the reactor socket/timer
   coroutines moved to `await_asio`, same sequence, the same 20-tick and early-publish checks,
   `task::spawn` replaces `co_spawn`; the held 100 ms drive still has to run concurrently with
   `observe` for `ticks >= 20` to hold, so executor progress is still tested. The cancel
   request is the CR1 gap. Run result below.

## Test mutant table

M1 (owner check removed) fails `homes` only: isolating. M2 (yield removed) fails `yield` only:
isolating. M3 fails `core`, `homes`, `cancel`: not isolating for `cancel` (CR1). I did not
rerun the mutants.

## Scope

Within design: API change plus tests, docs, CMake option. The `run_one`/`drive` split, metrics
and polling are unchanged from main.

## Consensus run

`consensus_completion_asio` built (Debug, macOS arm64, consensus:c7bf941 with its pins) and passed 1/1, 1.5 s.

## Not run

Linux ASan/TSan locally (the build lease had no slot, exit 3; macOS arm64 only, so no LeakSanitizer; consensus ran without sanitizers);
FreeBSD, x86_64, gcc (left to CI, 8/8 green); the ledger's mutants; consensus bench
(`worker_completion_bench`, not built); journal; Detent.
