> Status: CURRENT (2026-11-24) the flush mode every `PosixIO` sync follows. Source chapter for the Architecture and Design book, beside section 5.1 (`commit-barriers.md`).

# Flush modes: one choice for every sync

On macOS `fsync` hands data to the drive and returns while it sits in the drive's cache; only `F_FULLFSYNC` asks the drive to write its cache to the media, at about 20 ms a call. Linux's and FreeBSD's `fsync` already ask for that flush. So on macOS a backend must choose, per process or per store, between surviving a power loss and paying a full flush per sync. `PosixIOOptions::flush_mode` is that choice, made once by whoever builds the `PosixIO`.

## How it works

- **Required.** `FlushMode` is a class over `plain` and `full` with no default constructor, so `PosixIOOptions{}`, `PosixIOOptions{true, true}` and `PosixIO(directory)` do not compile, and there is no process-wide switch (`test/sync_primitives_test.cc` asserts it with `static_assert`). Its `Value` has no zero (`plain = 1`, `full = 2`): a `FlushMode` built from a value-initialized or out-of-range `Value` throws `std::invalid_argument`, and so does a `PosixIO` whose options' bytes hold one, so no mode is ever chosen by default (`flush_mode_values`).
- **One place.** `sync_call(site, mode, platform)` (`posix_sync.h`) is the one table from a flush mode and a sync site (`SyncSite::file`, `data`, `directory`, `order`) to the call (`SyncCall::fsync`, `fdatasync`, `full_fsync`, `barrier_fsync`); `platform` defaults to the host. `file_sync`, `files_sync_devices` (which reads the macOS row: it is macOS's batch) and `PosixIO::sync_directory` issue what it names and nothing else; `order_files_and_directory` is always the ordered batch, and the table makes it a full flush in full mode. The mode lives in `detail::SyncCalls`, two bytes: the mode and the index of the backend's seam. faultlab's crash model can adopt the same table.
- **Files carry it.** Each `PosixFile` copies its backend's `SyncCalls` when it opens, because files may outlive their backend; a reusable file gets it from `make_closed_file`, and no open into it changes it. `File::sync` and `File::sync_data` follow it, which is how the turn barrier's `SyncData`, run by a queue worker straight on the file, follows the mode. A file opened before `replace_sync_calls` keeps the calls it opened with. A `PosixFile` is 16 bytes on 64-bit targets: the vtable pointer, the descriptor, `SyncCalls` and the lease flag (`static_assert` in `posix_file.h`).
- **A batch never weakens a file.** A backend batches another backend's `PosixFile`s too. A file the batch would sync with a weaker call than its own (`batch_weakens`: on macOS, a `full` file in a `plain` batch) syncs itself through `File::sync` first, as a file of a non-POSIX backend does; a `plain` file in a `full` batch shares the batch's full flush, which is stronger. This holds on every batch path: `sync_files`, `sync_files_and_directory`, `order_files_and_directory`, and the `Sync`, `SyncFiles` and `OrderFiles` primitives, which call them (`cross_backend`).
- **macOS full.** Every durable sync and every ordering point is `F_FULLFSYNC`: a single file directly; a batch `fsync`s each file (and the directory) and then flushes each distinct device once; `sync_directory` flushes through `owner.lock` when held.
- **macOS plain.** A durable sync is `fsync` alone, a batch included, with no device lookup; an ordering point `fsync`s every file and the directory, then issues one `F_BARRIERFSYNC` on the directory's device. A file on another device keeps its `fsync`: a barrier orders only its own device, and plain mode promises nothing against power loss.
- **Linux and FreeBSD.** Both modes make the same calls: `fsync` (Linux: `fdatasync` for `sync_data`, and a writeback hint before a batch). The io_uring and AIO drivers issue their own `fsync`/`fdatasync` and never read the mode.

## Never degrade

Full mode has no fallback. An `F_FULLFSYNC` that fails, with `ENOTSUP`, `ENOTTY` or any other errno, fails the call with that errno, and no `fsync` follows it (`full_refusal` in `test/sync_calls_test.cc`, every call site and the creation of `owner.lock`). Plain mode's barrier falls back to `F_FULLFSYNC` where the filesystem lacks the call (`commit-barriers.md`, "Fail closed"): stronger, never weaker.

## The seam table

The test seam (`replace_sync_calls`) is a `SyncSeam` of system-call pointers. `detail::sync_seams` holds each distinct seam once, process-wide, so a file names its seam by a one-byte index instead of copying the pointers.

| Entry state | `replace_sync_calls` with this seam | A file opens | A sync | Process exit |
|---|---|---|---|---|
| Index 0 | the default seam: index 0, nothing written | copies the backend's index | reads the entry, no lock | stays |
| Unused (count to 255) | written under the mutex, count bumped, index returned; the 256th distinct seam throws `std::length_error` | not reachable: no backend holds its index | not reachable | stays |
| Written | found under the mutex, the same index returned | copies the backend's index | reads the entry, no lock: it never changes | stays |

A file reads only an entry written before its backend held the index, and an entry is never rewritten or freed, so a file outliving its backend still reads its seam.

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

`include/completion/posix_sync.h`: `FlushMode`, `SyncSite`, `SyncCall`, `sync_call`, `SyncCalls::issue`, `file_sync`, `files_sync_devices`. `include/completion/posix.h`: `PosixIOOptions::flush_mode`, `sync_directory`, `order_files_and_directory`, `lock_owner`. `include/completion/posix_file.h`: `PosixFile`. Tests: `test/sync_primitives_test.cc` (`sequences`, `call_sites`, `cross_backend`, `reusable`, both modes, every platform; `flush_mode_values`, `table_rows`), `test/sync_calls_test.cc` (`plain_protocol`, `full_protocol`, `full_refusal`; on Linux and FreeBSD, both modes' identical sequence), `test/sync_files_test.cc` (`plain_batches`).
