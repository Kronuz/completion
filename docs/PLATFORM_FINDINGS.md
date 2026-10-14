# Platform findings for storage I/O

Measurements and mechanisms that strata found while tuning its index on top of this
library, recorded here for every consumer (Detent's journal and authority, strata,
Xapiand). Each entry names the platforms measured, the numbers, what this library now
does about it, and what a consumer should do. Raw probes live in the strata checkout
under `.scratch/`; summaries of record are in strata's `docs/perf/`.

Platforms: macOS 15.8 x86_64 APFS (i9-9980HK, internal SSD), Linux 6.8 x86_64 ext4
(QEMU/HVF gate VM, virtio, `cache=none`), FreeBSD 14.4 x86_64 UFS with soft updates
(VM, virtio), and macOS 27 arm64 APFS (M1 Pro) where noted.

## 1. Make many files durable with one device flush: `IO::sync_files`

A durable commit that syncs each new file separately pays one device flush per file.
On macOS `File::sync` is `F_FULLFSYNC`, which flushes the drive's whole cache, so the
cost is the flush count, not the bytes.

`sync_probe.cc` (strata `.scratch/commitcost/`): N new files of 200 KB written, made
durable, then a directory barrier; median of 15 rounds.

| 9 files | One full sync each | Batched | Batched form |
|---|---:|---:|---|
| macOS x86_64 APFS | 214 ms | 46 ms | `fsync` each, then one `F_FULLFSYNC` |
| Linux ext4 | 31.9 ms | 9.5 ms | `sync_file_range(SYNC_FILE_RANGE_WRITE)` on all, then `fsync` each |
| FreeBSD UFS | 13.4 ms | 11.1 ms | `aio_fsync` all (`fdatasync` each: 8.6 ms) |

Also measured: concurrent `fsync` threads (APFS 107 ms, ext4 14.7 ms, UFS 10.7 ms) and
one `syncfs` on Linux (10.1 ms, but it flushes every file on the filesystem).

**In this library:** `IO::sync_files(std::span<File* const>)`. The default calls
`File::sync` on each. `PosixIO` batches its own files: on macOS, `fsync` each, then one
`F_FULLFSYNC`; on Linux, start writeback on every file, then `fsync` each; elsewhere
(FreeBSD), `fsync` each, unchanged for now. A file from another backend falls back to its
own `sync`. After a failure, which files are durable is unspecified.

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
