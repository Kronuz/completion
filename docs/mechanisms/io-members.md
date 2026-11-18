# File and IO members: preallocate, advise, map_shared, link, ACLs, creation mode

Copyright © 2026 Germán Méndez Bravo (Kronuz)

The members faultlab's seam needs (cpp-libraries `docs/design/faultlab.md`, F8), how `PosixFile`
and `PosixIO` implement them on each platform, and what `test/map_test.cc` observes.
`test/io_members_mutant.sh NAME` breaks one override and requires that test to fail.

## preallocate

| Platform | Call | Notes |
|---|---|---|
| Linux | `fallocate(fd, 0, offset, length)` | Reserves the whole range, holes included, and grows the size. |
| macOS | `F_PREALLOCATE` then `ftruncate` | Reserves only the growth past the current size. |
| FreeBSD and others | `posix_fallocate` | Returns its error instead of setting `errno`. |

`F_PREALLOCATE` with `F_PEOFPOSMODE` counts from the file's *allocated* end and never moves the
size, so `PosixFile` asks for `end - size()` and then sets the size with `ftruncate`. `F_VOLPOSMODE`
takes a volume position, not a file offset, so it cannot reserve a range inside the file. The call
asks for `F_ALLOCATECONTIG | F_ALLOCATEALL` first and falls back to `F_ALLOCATEALL`. On APFS,
`F_ALLOCATECONTIG` alone returned success with 0.6 MiB of an 8 MiB request reserved
(`fst_bytesalloc`), and storage's `storage_io.h` makes that call, so it can reserve only part of
the range. With `F_ALLOCATEALL` the request is all-or-nothing.

Where the filesystem cannot reserve (`EOPNOTSUPP`, `ENOTSUP`, `ENOSYS`, `EINVAL`; FreeBSD ZFS returns
`EINVAL`), the override calls `File::preallocate`, the default, which grows the size without a
reservation. `ENOSPC`, `EIO` and every other error throw. The test reads `allocated_bytes` as the
reservation: it must cover the range wherever a raw reservation call on a probe file succeeds.

## advise

| Advice | Linux, FreeBSD | macOS |
|---|---|---|
| `Normal`, `Sequential`, `Random` | `posix_fadvise` | `F_RDAHEAD` on, on, off |
| `WillNeed` | `POSIX_FADV_WILLNEED` | `F_RDADVISE` over the range (`int` count, clamped) |
| `DontNeed` | `POSIX_FADV_DONTNEED` | nothing |

A closed handle, an empty range or an offset past `off_t` does nothing, and every error is ignored.
The test writes the file past the page cache (macOS `F_NOCACHE`; elsewhere `POSIX_FADV_DONTNEED`),
checks with `mincore` that no page is resident, and requires `WillNeed` to read the range in (and
on Linux `DontNeed` to drop it). Where the pages stay resident, it reports that it observed nothing.

## map_shared

`mmap(PROT_READ | PROT_WRITE, MAP_SHARED)` over `[0, length)`, refusing a length past `size()`
with `std::invalid_argument` instead of a later `SIGBUS`. A read-only handle fails with `EACCES`.
The mapping outlives its handle and the name, as `map()`'s does.

Linux's `fsync` and FreeBSD's `kern_fsync` write back pages dirtied through a mapping. On macOS the
kernel delegates `fsync` to the filesystem: HFS's `hfs_fsync` pushes the cluster layer
(`cluster_push`), not pages dirtied through a mapping, and APFS's source is closed. `msync(MS_SYNC)`
writes those pages, and `PosixFile` does not issue it. So the contract promises visibility to
readers of the file everywhere and durability after `sync` everywhere but macOS.

## link

`linkat(directory, source, directory, destination, 0)`: it never replaces and fails with `EEXIST`
when the destination exists, and it does not follow a symlinked source. While both names exist the
inode has two links, so `open_existing` refuses either name (`validate_file` requires one link):
a publisher links, then removes the temporary name.

## extended_acl

`File::inspect` reports an ACL beyond the mode bits from the held descriptor:

- macOS: `acl_get_fd_np(ACL_TYPE_EXTENDED)` returns any entry (`ENOENT` when none).
- FreeBSD: a non-trivial NFSv4 ACL, else a non-trivial POSIX.1e access ACL (`acl_is_trivial_np`).
- Linux: the `system.posix_acl_access` attribute exists; the kernel removes it when an ACL says no
  more than the mode bits. Detent's `mode_only_acl` accepts every Linux file; this reports the ACL.

A filesystem without ACLs reports false. `IO::inspect_entry` opens nothing and reports false.

## Creation mode

`create_exclusive` and `create_exclusive_into` `fchmod` the new file to the IO's artifact mode
(`owner.lock` to 0600) before validating it, so the process umask cannot narrow it.
