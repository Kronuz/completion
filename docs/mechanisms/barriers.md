> Status: CURRENT (2026-11-09) the macOS ordering barrier of `PosixIO`, its warning and its opt-in full flush. Source chapter for the Architecture and Design book, beside section 5.1 (`commit-barriers.md`).

# The ordering barrier: what it guarantees, and where it warns

`order_files_and_directory` issues `F_BARRIERFSYNC` on macOS where a durable batch would issue `F_FULLFSYNC` (`commit-barriers.md`). The barrier orders; it is not a flush. Its order is guaranteed only on Apple SSDs. `PosixIO` reports each volume where it ordered on other storage, once per process, and `full_flush_ordering` replaces the barrier with a full flush for hosts that cannot vouch for their storage. The default stays the barrier (owner, 2026-10-29, D10; lead, 2026-10-30, D16).

Linux and FreeBSD issue no barrier: there `order_files_and_directory` is `sync_files_and_directory`, every call is a durable `fsync`, and the warning is never called.

## What the barrier guarantees

- **It orders.** `fcntl(2)` documents `F_BARRIERFSYNC` as: I/O `fsync`'d before it persists before I/O issued after it, and "no assumption should be made on what has been persisted or not when this call returns". When it returns, nothing is promised to be on the media.
- **It is not a flush.** Durability comes from the `F_FULLFSYNC` in the `sync_directory` that ends every acknowledged commit. An acknowledged commit is durable on any storage that honors a cache flush, whatever the barrier did.
- **The order holds only on Apple SSDs.** Apple documents the call as implemented on HFS and APFS and requiring hardware support "which Apple SSDs are guaranteed to provide". On external or third-party drives, Fusion or AppleRAID sets, disk images and virtual disks, what the call does is undocumented. If it degrades to a plain `fsync`, a power loss before the final flush can leave a renamed manifest on the media whose files are not: strata then refuses to open the index rather than serve wrong data.

What is durable after each call of one commit, on any storage:

| Call | Durable when it returns |
|---|---|
| `order_files_and_directory(files + temporary)` | Nothing promised. On an Apple SSD, everything it fsynced reaches the media before anything issued after it. |
| `replace(temporary, manifest)` | Nothing promised. |
| `sync_directory()` | Every file, the temporary and the rename: `fsync` and `F_FULLFSYNC` on the directory. |
| The same, with `full_flush_ordering` | `order_files_and_directory` is already durable: one `F_FULLFSYNC` where the barrier would be. |

## How the warning works

1. **Only a barrier that ordered without flushing counts.** `SyncCalls::barrier` (`posix.h`) returns 0 when `F_BARRIERFSYNC` succeeded, 1 when the filesystem lacked the call and the full flush ran instead, -1 on failure. `files_sync_devices` reports whether the directory's device got a 0. A fallback, a failure, a durable batch or `full_flush_ordering` never warn.
2. **Each backend checks once.** After its first such barrier, a `PosixIO` with a `barrier_warning` callback claims the directory's `st_dev` in a process-wide list. Every later ordering call costs one relaxed atomic load. Without a callback nothing is classified or claimed.
3. **Each volume is classified once per process.** The first backend to claim a device reads `fstatfs` (mount point, source) and asks IOKit what lies under it. Later backends on that volume, warned or not, find it claimed and return.
4. **IOKit, loaded at run time.** `detail::AppleRegistry` loads IOKit and CoreFoundation with `dlopen` at the first classification, so consumers link nothing more. It matches the BSD name of a `/dev/disk...` mount (`IOBSDNameMatching`), walks every ancestor in the service plane, and reads each entry that carries "Device Characteristics". The volume is an Apple SSD when there is at least one such entry and each has a "Product Name" starting with `APPLE SSD ` and a "Physical Interconnect Location" of `Internal`. Anything else warns: another drive, a disk image, a Fusion set with one non-Apple member, a mount that is not a disk (SMB, NFS, devfs), or IOKit not loading.
5. **The callback runs on the ordering call's thread**, after the barrier and before the caller's rename, with a `BarrierWarning`: the volume's `st_dev`, its mount point, its source and the storage found (`"Disk Image (Virtual Interface, File)"`). If it throws, the ordering call fails with that exception (the caller then does not rename), the claim is released, and the volume warns again at its next barrier. A warning is delivered once: a callback that returned.

On this host's internal SSD the classification reads `APPLE SSD AP1024M (PCI-Express, Internal)` and nothing warns; on APFS, HFS+, exFAT and FAT32 disk images it reads `Disk Image (Virtual Interface, File)` and the first barrier warns (`PLATFORM_FINDINGS.md`, "Where the barrier warns").

## The opt-in full flush

`PosixIOOptions::full_flush_ordering` (default `false`) makes `order_files_and_directory` the durable sequence: every file and the directory fsynced, then one `F_FULLFSYNC` per device. It costs one full flush per commit, about 20 ms on this host's SSD (`commit-barriers.md`, "Numbers"). strata exposes it as an index option and passes the warning to its host (quest `barrier-warning-strata`); Xapiand names its switch in the warning it logs (quest `barrier-warning`).

## Invariants

- The default sequence is unchanged: `SyncFiles`, `OrderFiles` and `PreparationFilePolicy::Stage` issue the same calls with or without a callback; the warning adds no sync call.
- The warning fires only after a barrier returned 0 on the directory's device; never on Linux or FreeBSD, never with `full_flush_ordering`.
- A volume is classified at most once per process while its warning is delivered or it is an Apple SSD.
- Classification fails toward warning: whatever cannot be identified as an internal Apple SSD warns.

## Limits

- "Once per process" is per copy of the header's inline state: two shared libraries that each compile `posix.h` with hidden visibility warn once each.
- A volume is keyed by `st_dev`. The system and data volumes of an APFS volume group report the same `st_dev` and warn as one. A device number reused by a later mount in the same process is not classified again.
- The rule trusts IOKit's product name and location. An Apple SSD in an external enclosure warns (location `External`); a non-Apple drive reporting itself as `APPLE SSD` and `Internal` would not.
- Not measured: power loss on any of this storage; macOS arm64 internal storage's IOKit keys (expected `APPLE SSD` and `Internal`; a mismatch only warns).

## Code

`include/completion/posix.h`: `BarrierStorage`, `BarrierWarning`, `PosixIOOptions::barrier_warning` and `full_flush_ordering`, `SyncCalls::barrier`, `files_sync_devices`, `detail::AppleRegistry`, `detail::barrier_storage`, `PosixIO::note_barrier_volume`, the seam `PosixIO::replace_barrier_storage`. Tests: `test/sync_calls_test.cc` (`barrier_warning`, `real_storage` with `COMPLETION_EXPECT_STORAGE`, `full_flush_option`, and on Linux and FreeBSD the never-warns run).
