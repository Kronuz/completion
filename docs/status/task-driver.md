# task-driver: the completion driver on task::task

> Status: CURRENT (2026-11-02) Detent W2 Q6: `AsioCompletionDriver` returns `task::task<void>`.

Branch `phase/task-driver`, base `main` e2c6474, task pinned at `d3d94f3`. Owner rule: one shape
everywhere, `task::task` is the family's only coroutine type; no shim, no second return type.

## State

Implementation, test and docs written; macOS Debug build and suite green; sanitizers, Linux and
the mutants are recorded below as they finish. Not merged; the lead lands it.

## Done

- `asio_completion.h`: `run`, `run_one`, `drive` return `task::task<void>`; the constructor takes
  `task::asio_scheduler&`. The owner check compares `task::current()` with the scheduler the
  driver was built with (explicit, injected once, the same identity the I/O objects' executor
  comes from); a `task::yield()` would use the thread-local current scheduler instead of that
  identity, so the 16-step yield uses `schedule_on{owner_}`. The queue wait is one
  `await_asio` over `make_parallel_group(wait_read, timer).async_wait(wait_for_one())`, the same
  race `operator||` made; no token is passed, so the drive stays uncancellable.
- `CMakeLists.txt`: `COMPLETION_ASIO` (default on for a top-level build) fetches task and links
  `task::asio`; the core target is unchanged.
- `test/asio_extension_test.cc`: ported; new `scheduler_homes`, `yields_to_owner`,
  `cancel_mid_drive`.
- `CHANGELOG.md`, README, ARCHITECTURE, AGENTS updated.

## Next

Sanitizers, Linux, CI; then journal and consensus branches pinning this head.

## Blocked on

Nothing.
