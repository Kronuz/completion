# Changelog

All notable changes to `completion`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

Upgrading a pin from before 2026-10-30: [the consumer upgrade note](https://github.com/Kronuz/cpp-libraries/blob/main/docs/UPGRADING-2026-10.md#completion) lists what changes for callers, how to find affected inputs, the commits not to pin, and the speedup.

## 2026-11-02

### Changed

- **Breaking.** `AsioCompletionDriver::run()` and `run_one()` return `task::task<void>`, not
  `asio::awaitable<void>`; `task::task` is the family's only coroutine type. The constructor takes
  the owner `task::asio_scheduler&` instead of an `asio::any_io_executor`, and the scheduler must
  outlive the driver. A task awaiting `run()` must be running on that scheduler, as the
  awaitable had to run on the owner executor; any other gets `std::logic_error`, thrown when it
  awaits. Spawn the call with `task::spawn(task::scheduler_ref::of(scheduler), ...)` where
  `asio::co_spawn(executor, ...)` was. Behavior, statistics and cancellation semantics are
  unchanged: the drive ignores a cancel request and completes its original, and the 16-step yield
  and the 1 ms poll fallback stay.
- `completion::asio` links `task::asio` and builds when `COMPLETION_ASIO` is on (the default for a
  top-level build; a dependent sets it before `FetchContent_MakeAvailable`). It fetches
  [task](https://github.com/Kronuz/task) at `d3d94f3`, which fetches Asio 1.36.0 unless
  `ASIO_INCLUDE_DIR` names a checkout. The core `completion::completion` is unchanged and needs
  neither. `asio_completion.h` includes the Asio headers it uses, not `<asio.hpp>`.
- The driver's test now runs in CI: it covers the scheduler a `run()` resumes on, the owner yield
  and a request mid-drive.
- Pins metrics at `db490b4` and allocators at `07de45d`, their heads; both moves past `d7d4c4d`
  and `6eb7fb2` are docs only.

## 2026-11-01

### Added

- `bench/`: the four timing probes behind `docs/PLATFORM_FINDINGS.md` (`sync_probe.cc`,
  `commit_probe.cc`, `unlink_probe.cc`, `dirflush_probe.cc`), built with
  `-DCOMPLETION_BUILD_BENCH=ON`. CI builds them and runs each once on a small input.
- `tools/mutants.py`: the 14 owner-lock mutants of the CL-2 review (CL2-5) as exact-anchor
  replacements. Each is applied to a scratch copy, rebuilt and run against the suite. The CTest
  test `completion_mutants_anchors` checks the anchors; the `mutants` workflow runs the script.
- `completion_sync_calls` asserts which descriptor the publication's full flush goes through on
  macOS (the owner file while it is held, the directory after it is released). Mutant `m14`
  (`owner_fd_` unrecorded) survived the suite until it did.

## 2026-10-30

### Changed

- `completion::AllocationContext` and `completion::OwnedAllocator<T>`, from
  `completion/allocation.h`, are aliases of the shared `allocators::resources` types. The
  resource lifetime and weak-control-block contract are unchanged. Completion links the
  header-only resource target, which neither builds nor links global allocation overrides or
  the optional cleanup runtime.
- The aliases keep the C++ source names, but the canonical type identities and some internal
  object layouts change. Rebuild C++ dependents together; do not mix precompiled objects from
  before and after this change. Persisted formats are unchanged.
