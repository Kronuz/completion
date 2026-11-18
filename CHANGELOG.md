# Changelog

All notable changes to `completion`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

Upgrading a pin from before 2026-10-30: [the consumer upgrade note](https://github.com/Kronuz/cpp-libraries/blob/main/docs/UPGRADING-2026-10.md#completion) lists what changes for callers, how to find affected inputs, the commits not to pin, and the speedup.

## 2026-11-17

### Added

- **`IO::sync_data_files(std::span<File* const>)`**, the data-only counterpart of `sync_files`; the
  default runs `File::sync_data` on each file. Adding a virtual with a default breaks no backend.

### Changed

- The executor's `Sync` and `SyncData` primitives call `IO::sync_files` and `IO::sync_data_files`
  on the primitive's file instead of `File::sync` and `File::sync_data`, so a host that overrides
  the IO sees every sync run by `drive_synchronously`, `InlineCompletion`, `HeldCompletion` and either
  queue's worker. A `Sync` or `SyncData` without a file fails with `std::invalid_argument`. On macOS
  a `PosixFile`'s `Sync` run by the worker is `sync_files` of one file: an `fsync`, then the
  `F_FULLFSYNC` it did before.

## 2026-11-15

### Added

- **`SyncData`**, a primitive for `File::sync_data()` (the data and the metadata to read it back, not
  the timestamps), on every backend: `drive_synchronously`, the worker of either queue, the io_uring
  ring (`IORING_FSYNC_DATASYNC`), FreeBSD's `aio_fsync(O_DSYNC)` and the Asio driver. The statistics
  extension arrays grow from 13 to 14 entries, so `completion_extension_count` is 14; the legacy
  arrays are unchanged. A turn log written in place needs it per commit instead of `Sync`.
- **The seam** (`completion/seam.h`, target `completion::task`, over `task.hh` alone):
  `completion_ref`, a non-owning reference to any driver whose
  `run(std::shared_ptr<IOOperation>, task::cancel_token)` returns `task::task<void>`;
  `InlineCompletion`, which runs every primitive on the caller's thread; and `HeldCompletion`, which
  holds each primitive until its owner executes, delivers, fails or drops it, in any order. The token
  is read before each primitive is issued; an issued primitive completes into its operation first.
- `drive_synchronously(operation, proceed)`: the one synchronous loop, stopping before a primitive
  when `proceed()` is false. The one-argument form is unchanged, and is the loop
  `InlineCompletion` runs.
- `AsioCompletionDriver::run` and `run_one` take an optional token. Operations may now run at once:
  they share the queue's one slot a primitive at a time. `IOOperation::lane()` (default
  `IOLane::Bulk`) admits a `Turn` operation's primitive ahead of every waiting Bulk one; one Turn
  operation runs per driver at a time (asserted in Debug). A token requested while an operation
  waits for the slot releases its caller at once with `task::cancelled`.
- `COMPLETION_TASK` (on for a top-level build) builds `completion::task` without Asio;
  `completion::asio` links it. `bench/lane_probe.cc` times a turn's wait behind Bulk writes.

### Changed

- `AsioCompletionDriver` no longer refuses a second concurrent `run`. `test/asio_extension_test.cc`
  gains a `SyncData` step so each of the 14 extension slots is still exercised once.

## 2026-11-02

### Changed

- **Breaking.** `AsioCompletionDriver::run()` and `run_one()` return `task::task<void>`, not
  `asio::awaitable<void>`; `task::task` is the family's only coroutine type. The constructor takes
  the owner `task::asio_scheduler&` instead of an `asio::any_io_executor`, and the scheduler must
  outlive the driver. A task awaiting `run()` must be running on that scheduler, as the
  awaitable had to run on the owner executor; any other gets `std::logic_error`, thrown when it
  awaits. Spawn the call with `task::spawn(task::scheduler_ref::of(scheduler), ...)` where
  `asio::co_spawn(executor, ...)` was. Behavior, statistics and cancellation semantics are
  unchanged: `run()` takes no cancel token, so nothing cancels a drive and it completes its original, and the 16-step yield
  and the 1 ms poll fallback stay.
- `completion::asio` links `task::asio` and builds when `COMPLETION_ASIO` is on (the default for a
  top-level build; a dependent sets it before `FetchContent_MakeAvailable`). It fetches
  [task](https://github.com/Kronuz/task) at `6558c2e`, which fetches Asio 1.36.0 unless
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
