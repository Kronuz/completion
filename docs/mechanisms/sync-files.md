> Status: CURRENT (2026-11-03) `SyncFiles` and `OrderFiles` on branch `phase/sync-files`.

# SyncFiles and OrderFiles: group syncs as asynchronous primitives

A caller that publishes many files by name needs every file and the directory's entries on stable storage before a pointer to them is. `IO::sync_files_and_directory` and `IO::order_files_and_directory` already do that for a group (`io.h`, `posix.h`). The two primitives run them from the asynchronous driver, one primitive for the whole group, instead of one `Sync` primitive per file.

## How it works

- **Two kinds.** `PrimitiveKind::SyncFiles` and `OrderFiles` append after `CreateInto` (ordinals 23 and 24). `MutationRequest::files` is a view of `std::shared_ptr<File>` that the operation owns until the original completes, as `bytes` and `source` are.
- **Front ends only.** `detail::execute_primitive` (`operation.h`) collects the raw `File*` in request order and calls `io.sync_files_and_directory(group)` or `io.order_files_and_directory(group)` on the operation's `IO&`. There is no second implementation: a wrapper over `IO` (a counting or fault-injecting backend) sees the same virtual call the synchronous API makes. A null element throws `std::invalid_argument` before any sync.
- **One directory.** The group syncs the IO's own directory. Files outside it are not supported.
- **Read-only or read-write.** `IO::open_existing_read_only(name)` opens without write access. `PosixIO` opens `O_RDONLY`, so adopting files from a lower overlayfs layer copies nothing up. fsync, `F_FULLFSYNC` and `F_BARRIERFSYNC` work on such a descriptor; `write_at` and `truncate` fail. A file of another backend syncs itself (`File::sync`) before the group's flush.
- **Drivers.** Neither native queue has an opcode for a group: both run these kinds on the fallback worker and count them in `fallback_extensions` (`SyncFiles` index 11, `OrderFiles` index 12). The Asio adapter dispatches them like any kind, with the extension metrics arrays sized from the stats arrays.
- **Seam.** `PosixIO::replace_sync_calls` swaps the system calls the sync paths issue on every platform: macOS `fsync` and `fcntl`; Linux `fsync` and the `sync_file_range` writeback hint; FreeBSD `fsync`.

## What reaches stable storage when the call returns

| Platform | `SyncFiles` (durable) | `OrderFiles` (ordered) | Calls, in order |
|---|---|---|---|
| macOS | Every file's data and the directory's entries, through one `F_FULLFSYNC` per device. | Every file and the directory are staged ahead of any later write by one `F_BARRIERFSYNC` on the directory's device; not necessarily durable. Other devices are flushed in full; `PosixIOOptions::full_flush_ordering` makes it a full flush. | `fsync` each file, `fsync` the directory, then the device flush through the first file on the device. |
| Linux | Every file (fsync: data and the metadata to read it back) and the directory entries, each as the filesystem's fsync makes them durable. On ext4 and XFS with write barriers that is a device flush per fsync: N+1 flushes, nothing amortized (unmeasured). | The same as `SyncFiles`; durable is stronger than ordered. | `sync_file_range(WRITE)` hint on each file, `fsync` each file, `fsync` the directory. |
| FreeBSD | As Linux: the filesystem's fsync per file, then the directory. | The same as `SyncFiles`. | `fsync` each file, `fsync` the directory. |

An empty group syncs the directory alone (macOS: `fsync`, then `F_FULLFSYNC` or `F_BARRIERFSYNC` through the directory).

## What an error means

The first failing call throws `std::system_error` (the primitive's completion carries it) and nothing after it runs: files later in the list are not synced, the directory is not synced, and on macOS no device flush is issued. Files before the failure may be staged in a drive cache or in the page cache. The whole group is not durable; no prefix is promised. Do not retry the call and trust the retry: after a failed Linux fsync the kernel may have dropped the dirty pages and a second fsync can succeed with the data unwritten. Rewrite the files and sync again. A barrier error other than "unsupported" on macOS fails the call; "unsupported" falls back to a full flush (`commit-barriers.md`).

## Tests

`test/sync_primitives_test.cc` commits the expected call sequences for each platform (`#ifdef` per platform) and checks them through the `IO` method, `drive_synchronously` and the native queue: a mixed group of read-write and read-only files, a read-only file alone, an empty group, and an injected `EIO` on the second file. It also holds an original in flight after the caller let go. Mutants `s1` to `s6` in `tools/mutants.py` break the device flush, the barrier, the directory sync, the file fsync and the primitive's choice of call.
