# Platform findings for storage I/O

Measurements and mechanisms that strata found while tuning its index on top of this
library, recorded here for every consumer (Detent's journal and authority, strata,
Xapiand). Each entry names the platforms measured, the numbers, what this library now
does about it, and what a consumer should do. Raw probes live in the strata checkout
under `.scratch/`; summaries of record are in strata's `docs/perf/`.

Platforms: macOS 15.8 x86_64 APFS (i9-9980HK, internal SSD), Linux 6.8 x86_64 ext4
(QEMU/HVF gate VM, virtio, `cache=none`), FreeBSD 14.4 x86_64 UFS with soft updates
(VM, virtio), and macOS 27 arm64 APFS (M1 Pro) where noted.

## 1. Make many files durable with one flush per device: `IO::sync_files`

A durable commit that syncs each new file separately pays one device flush per file.
On macOS `File::sync` is `F_FULLFSYNC`, which flushes the drive's whole cache, so the
cost is the flush count, not the bytes.

`sync_probe.cc` (strata `.scratch/commitcost/`): N new files of 200 KB written, made
durable, then a directory barrier; median of 15 rounds.

| 9 files | One full sync each | Batched | Batched form |
|---|---:|---:|---|
| macOS x86_64 APFS | 214 ms | 46 ms | `fsync` each, then one `F_FULLFSYNC` |
| macOS arm64 APFS (M1 Pro) | 45 ms | 12 ms | `fsync` each, then one `F_FULLFSYNC` |
| Linux ext4 | 31.9 ms | 9.5 ms | `sync_file_range(SYNC_FILE_RANGE_WRITE)` on all, then `fsync` each |
| FreeBSD UFS | 13.4 ms | 11.1 ms | `aio_fsync` all (`fdatasync` each: 8.6 ms) |

Also measured: concurrent `fsync` threads (APFS 107 ms, ext4 14.7 ms, UFS 10.7 ms) and
one `syncfs` on Linux (10.1 ms, but it flushes every file on the filesystem).

**In this library:** `IO::sync_files(std::span<File* const>)`. The default calls
`File::sync` on each. `PosixIO` batches POSIX files from any backend: on macOS, `fsync`
each, then one `F_FULLFSYNC` per distinct `st_dev`; on Linux, start writeback on every
file, then `fsync` each; elsewhere (FreeBSD), `fsync` each, unchanged for now. A file
from another backend falls back to its own `sync`. After a failure, which files are
durable is unspecified.

The measured batches above reside on one device. The cross-device regression models
separate device caches, checks that every file is staged before any cache is flushed,
and injects interrupted and failed stat, staging and full-flush calls. It exercises the
same grouping code on all platforms; actual power-loss behavior is outside that test.

**In strata** (commit of flushed segments, live-docs files, and metadata as one batch;
the soak workload, 48 commits per side, interleaved): macOS commit phase 187 → 106 ms
median (p90 275 → 120 ms); Linux ext4 24.8 → 16.2 ms (p90 31.4 → 22.1 ms).

**For a consumer:** wherever several files must be durable before one ordering point (a
manifest rename, a checkpoint publication, a snapshot install), write them all, call
`sync_files` once, then take the directory barrier. Do not flip in-memory state (for
example, "this file is written") until the batch returns: a failure part-way leaves no
way to know which files are durable, so the next attempt must write them again.

**Not yet done:** the directory barrier on macOS is `fsync(dir)` plus its own
`F_FULLFSYNC`, so a commit still pays about four device flushes (data batch, directory,
manifest temporary, directory). Folding the first directory barrier into the batch, and
an ordering-only barrier (`F_BARRIERFSYNC`, measured at 10.7 ms for the same 9 files
plus the final directory flush) for the manifest temporary, are the next candidates.

## 2. Freeing large files costs 7–130 ms per GiB and disturbs other I/O

`unlink` of a 2 GiB synced file: macOS x86_64 118–171 ms, Linux ext4 256–269 ms,
FreeBSD UFS 91–105 ms, M1 APFS 14–19 ms. On ext4, a 4 GiB unlink raises other files'
random 4 KiB read p99 from 0.26 ms to 10–28 ms; truncating from the end in 16 MiB steps
first cuts that to 4–5 ms (p99.9 still 12–31 ms). Truncation in steps costs up to 1.9×
one unlink's total time but bounds each pause. Truncating a mapped file raises SIGBUS on
a read past the new end, so truncate only after the last mapping is released.

**For a consumer:** reclaim obsolete journal segments, snapshots, and images from a
queue, in bounded steps, off the latency-critical path; never unlink a large file inline
in a commit or an election-sensitive turn. Details: strata `docs/perf/reclamation.md`.

## 3. Errors after the commit point

A failed `truncate`, `unlink`, or directory sync during reclamation that runs after a
durable commit must not report the commit as failed or fence the writer: keep the file
queued and retry. If the manifests' removal could not be followed by a directory sync,
owe that sync before queuing their data files. `PosixIO::remove` treats a missing file as
success.

## 4. Space from a removed file is free only once the removal is durable

Filesystems release an unlinked file's blocks when the unlink reaches the journal or
transaction commit, and a crash before that leaves the file in place. Admission that
counts the bytes as free at the unlink admits work the disk then refuses (strata's
review, L8: a transaction admitted by draining a reclaim queue failed every commit with
ENOSPC until something synced the directory).

**For a consumer:** keep a removed file's bytes charged until the next directory barrier
returns; a drain that removes files to make room takes that barrier before admitting.
Likewise, an operation that reserved space and then drains must keep its own reservation
through the drain (strata's L5).

## 5. Unreliable reads: retry once, then report damage

Field studies of 1.5 million disks found latent sector errors on 3.45% of disks within 32
months and silent corruption on 0.86% of nearline disks within 17 months. Transient
faults (a read that fails or returns wrong bytes once) are also routine at scale.

strata now reads a file and its checksums again once when either fails: a transient
fault heals, and damage reports as corruption, a read error included, so the caller
repairs from a replica or an older generation. Under faultfs media faults at 782 offsets
of every file, every transient fault healed and no read ever returned a wrong answer.
Details: strata `docs/design/media-faults.md`.

**For a consumer:** a journal or checkpoint reader that fences on the first failed read
treats a transient fault as damage. Retrying the read once before fencing keeps the
fail-closed guarantee and removes those false positives. A truncated-looking tail in a
consensus log is not necessarily torn: protocol-aware recovery (Alagappan et al., FAST
2018) distinguishes a damaged acknowledged entry from an unacknowledged torn write.

## 6. macOS libc++ reads `std::cin` a character at a time

On macOS, libc++'s `std::cin` reads through `getc`, which locks stdin for every
character, and `std::ios::sync_with_stdio(false)` does not change it. Feeding 300,000
documents to strata's indexer through `std::getline(std::cin, ...)` took 64.5 s on the
M1; reading stdin with `read(2)`, up to 1 MiB at a time, took 7.9 s for byte-identical
output (strata `148eebb`). A profile shows the time in `__stdinbuf::__getchar`, `getc`,
and `pthread_mutex_lock`/`unlock`.

**For a consumer:** any macOS tool that streams input through `std::cin` (a bulk loader,
a replay, a benchmark driver) pays this per character. Read the file descriptor in large
blocks instead; `read(2)` returns what is available, so an interactive peer still gets
its answer.

## 7. An owner lock on the directory handle, warned on off local filesystems

strata's small-indexes review (`docs/reviews/small-indexes-design.md`, SI-R2 and SI-R18)
probed a directory `flock` and a directory-descriptor `F_FULLFSYNC` on macOS 15 x86_64:

- `flock(LOCK_EX | LOCK_NB)` on an `O_RDONLY` directory descriptor excludes a second
  description in one process and an unrelated process (`EWOULDBLOCK`) on APFS, HFS+, and
  exFAT; the lock follows the inode across a rename and a `SIGKILL`ed holder releases it.
  All three report `MNT_LOCAL`. ext4, tmpfs, and UFS rest on a designer's probe without
  kept output; this library's `dir_lock` suite is the evidence there (CI on Linux ext4 and
  tmpfs, FreeBSD, macOS APFS).
- `F_FULLFSYNC` on a directory descriptor is a device flush: 20.5 ms p50 after a staged
  write, against 20.4 ms on a regular file and 2 µs for `fsync(dir)`. No power cut has
  tested it.
- Over NFS, Linux emulates `flock` as a whole-file lock that needs the file open for
  writing, or keeps it local under `local_lock`; over SMB it is a mandatory byte-range
  lock (`flock(2)`). A directory cannot be opened for writing, so a directory lock there
  either fails or excludes no other client, and the call cannot tell which.

**In this library:** the directory lock (`OwnerLockMode::directory` and `both`, and the
legacy `IO::acquire_directory_owner()`; section 9) locks on any filesystem: the owner's ruling is that a user who runs on a network filesystem
gambles knowingly, so the library warns through the caller and does not refuse. A `flock`
reported unsupported throws `std::errc::not_supported`. `PosixIO::directory_filesystem()`
classifies the filesystem for that warning: `MNT_LOCAL` on macOS and the BSDs (FUSE
included), an `f_type` allow-list on Linux (FUSE is non-local there). On macOS
`sync_directory` flushes through the directory descriptor when no `owner.lock` is held,
where it used to throw `logic_error`. ZFS and overlayfs on Linux are classed local
unprobed.

**For a consumer:** one descriptor per open directory carries both the lock and the
barrier. Call `directory_filesystem()` and warn when it is not `local`: there the lock may
fail or exclude only local holders. A host that needs real exclusion on a network
filesystem needs its own above the library (one process owning the whole root); do not
fall back to a lock file.

## 8. A symlinked root, and how its refusal shows

strata's data volumes are commonly symlinks, and `PosixIO` refused a symlinked root. The
owner's rule is to warn, not babysit, so `PosixIOOptions::follow_root_symlink` opens
through a final symlink and `allow_shared_directory` admits a foreign owner or a
group/other-writable mode; the defaults refuse as before (Detent links this library).

- `open(O_DIRECTORY | O_NOFOLLOW)` on a symlink to a directory fails with `ENOTDIR` on
  macOS 15 (probed), not `ELOOP`: `O_DIRECTORY` is checked before the symlink. Other kernels
  differ (`EMLINK` is FreeBSD's for `O_NOFOLLOW`), so the constructor treats `ELOOP`,
  `EMLINK`, and `ENOTDIR` as "maybe a symlink" and confirms with `lstat` before reopening
  through it; the suite runs on Linux and FreeBSD in CI. A symlink to a file, a dangling one, and a loop still fail.
- The flags describe the descriptor that is held (`fstat` of it), so a symlink retargeted
  afterwards moves nothing. Whether the root was a symlink is the one fact read from the
  path; it is informational.

**For a consumer:** construct the `PosixIO` with both options, read `directory_trust()`,
and warn on `foreign_owner` or `shared_writable`; a symlinked root alone needs no warning.

## 9. Two owner locks that do not exclude each other

The consolidation audit (`cpp-libraries/docs/reviews/consolidation-2026-10-19.md`, CL-2)
held `acquire_owner(true)` in one `PosixIO` and `acquire_directory_owner()` in another on
one APFS directory, and both succeeded: `flock` excludes per inode, and `owner.lock` and
the directory are different inodes. Detent used the first and strata the second.

- `owner_lock_test` runs every pairing of `file`, `directory` and `both`, through the
  legacy and options calls, in both orders, across processes and through distinct and
  shared IO objects (25 pairings each). The legacy file/directory pairing acquires in all
  three (8 of 25, the unsafe control); the other 17 exclude. CI runs it on macOS, Linux
  and FreeBSD.
- macOS has distinct `ENOTSUP` (45) and `EOPNOTSUPP` (102); Linux defines them equal. Both
  map to `std::errc::not_supported`, which is `ENOTSUP`.
- Linux NFS without lockd returns `ENOLCK` from `flock`. It surfaces as a plain `system_error`,
  not `not_supported`, so it fails closed: a consumer that proceeds unlocked on
  `not_supported` (strata's warn policy) refuses there instead.
- The directory-first lock error text now reads "lock journal directory: ... does not lock
  directories" (the legacy text was "directory owner lock: ..."); the code is unchanged.
- A new call does not repair binaries already running: an old file-only process and an
  old directory-only process still do not exclude each other.

**In this library:** `IO::acquire_owner(OwnerLockOptions)` with `OwnerLockMode::both` takes
the directory lock, then `owner.lock`, and releases in reverse; a `both` contender against
a directory-only holder stops before opening or creating `owner.lock`, so it writes
nothing. An unsupported directory lock fails closed in `both`, so a filesystem where
`owner.lock` locked but the directory does not now refuses.

**For a consumer:** move mutable owners to `both`, and qualify the directory lock on each
deployment filesystem first. Until every process that can address a directory takes the
same protocol, keep it under one protocol: a directory-only owner is safe only where no
file-only owner can ever reach the directory, including during creation.
