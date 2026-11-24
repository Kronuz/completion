> Status: CURRENT (2026-11-24) the flush mode every `PosixIO` sync follows. Source chapter for the Architecture and Design book, beside section 5.1 (`commit-barriers.md`).

# Flush modes: one choice for every sync

On macOS `fsync` hands data to the drive and returns while it sits in the drive's cache; only `F_FULLFSYNC` asks the drive to write its cache to the media, at about 20 ms a call. Linux's and FreeBSD's `fsync` already ask for that flush. So on macOS a backend must choose, per process or per store, between surviving a power loss and paying a full flush per sync. `PosixIOOptions::flush_mode` is that choice, made once by whoever builds the `PosixIO`.

## How it works

- **Required.** `FlushMode` is a class over `plain` and `full` with no default constructor, so `PosixIOOptions{}`, `PosixIOOptions{true, true}` and `PosixIO(directory)` do not compile, and there is no process-wide switch (`test/sync_primitives_test.cc` asserts it with `static_assert`).
- **One place.** The mode lives in `detail::SyncCalls` (`posix_sync.h`), beside the system calls the test seam replaces. `file_sync`, `file_sync_data` and `files_sync_devices` read it; `PosixIO::sync_directory` and `order_files_and_directory` read it from the backend's copy.
- **Files carry it.** Each `PosixFile` copies its backend's `SyncCalls` when it opens, because files may outlive their backend. `File::sync` and `File::sync_data` follow it, which is how the turn barrier's `SyncData`, run by a queue worker straight on the file, follows the mode. A file opened before `replace_sync_calls` keeps the calls it opened with.
- **macOS full.** Every durable sync and every ordering point is `F_FULLFSYNC`: a single file directly; a batch `fsync`s each file (and the directory) and then flushes each distinct device once; `sync_directory` flushes through `owner.lock` when held.
- **macOS plain.** A durable sync is `fsync` alone, a batch included, with no device lookup; an ordering point `fsync`s every file and the directory, then issues one `F_BARRIERFSYNC` on the directory's device. A file on another device keeps its `fsync`: a barrier orders only its own device, and plain mode promises nothing against power loss.
- **Linux and FreeBSD.** Both modes make the same calls: `fsync` (Linux: `fdatasync` for `sync_data`, and a writeback hint before a batch). The io_uring and AIO drivers issue their own `fsync`/`fdatasync` and never read the mode.

## Never degrade

Full mode has no fallback. An `F_FULLFSYNC` that fails, with `ENOTSUP`, `ENOTTY` or any other errno, fails the call with that errno, and no `fsync` follows it (`full_refusal` in `test/sync_calls_test.cc`, every call site and the creation of `owner.lock`). Plain mode's barrier falls back to `F_FULLFSYNC` where the filesystem lacks the call (`commit-barriers.md`, "Fail closed"): stronger, never weaker.

## What survives

| Event | Full mode | Plain mode |
|---|---|---|
| Application or kernel crash | Every returned sync | Every returned sync |
| Power loss, macOS | Every returned sync | Nothing promised; on an Apple SSD, barrier order holds |
| Power loss, Linux and FreeBSD | As the filesystem's `fsync` | The same |

Plain mode matches Xapiand's write-ahead log on macOS (Xapiand PLAN D177); Detent passes full.

## Not measured

Either mode's commit cost (full: two full flushes per commit; plain: one barrier), and power loss on hardware.

## Code

`include/completion/posix_sync.h`: `FlushMode`, `SyncCalls`, `file_sync`, `file_sync_data`, `files_sync_devices`. `include/completion/posix.h`: `PosixIOOptions::flush_mode`, `sync_directory`, `order_files_and_directory`, `lock_owner`. `include/completion/posix_file.h`: `PosixFile`. Tests: `test/sync_primitives_test.cc` (`sequences`, `call_sites`, both modes, every platform), `test/sync_calls_test.cc` (`plain_protocol`, `full_protocol`, `full_refusal`; on Linux and FreeBSD, both modes' identical sequence), `test/sync_files_test.cc` (`plain_batches`).
