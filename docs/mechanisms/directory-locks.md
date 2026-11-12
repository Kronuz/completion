# Directory locks: the exclusive owner lock and the shared reader lock

An `IO` locks its directory two ways. `acquire_directory_owner()` (and `acquire_owner` in `directory` or `both` mode) takes the exclusive lock a writer holds. `acquire_directory_reader()` takes the shared lock a reader holds while it reads the directory without writing it. Both are held while the returned `OwnerLock` lives and write nothing to the directory (`io.h`, `posix.h`).

## The contract

| Held \ wanted | shared (reader) | exclusive (directory, both) | owner.lock (file) |
|---|---|---|---|
| shared, another IO | acquires | busy | acquires |
| exclusive, another IO | busy | busy | acquires beside `directory`, busy beside `both` |
| either, the same IO | refused (`std::logic_error`) | refused | acquires beside `directory` or shared, refused beside `both` |

- **Busy** is a `std::system_error` with `EWOULDBLOCK` (`errc::operation_would_block`); neither call waits. A busy attempt writes nothing and leaves every other holder's lock in place.
- **Not supported.** A filesystem whose `flock` answers `ENOTSUP` or `EOPNOTSUPP` is `errc::not_supported`, naming the filesystem; there is no fallback to another lock. Any other errno is thrown as it is. `EINTR` is retried.
- **Any filesystem is tried.** The lock never inspects the filesystem; `directory_filesystem()` classifies it for a caller to warn on.
- **The base `IO`** throws `std::logic_error` from both calls. A wrapper over another `IO` forwards each call it supports; one that forwards only the exclusive lock refuses the shared one rather than taking the exclusive lock in its place.
- **owner.lock** (file mode) is a different inode: it excludes other `owner.lock` holders and neither directory lock.

## The upgrade is two steps the caller owns

One `IO` holds one directory lock of either kind: the shared and the exclusive lock take the same claim, so acquiring either while the IO holds one is refused before any `flock`, and the held lock is unchanged. An upgrade from shared to exclusive is therefore:

1. Release the reader's `OwnerLock`.
2. Call `acquire_directory_owner()`.

Between the two, the directory is unlocked by this IO: another IO or process may take either lock, and the upgrade is busy. On busy the caller takes the shared lock back (itself busy while a writer holds the exclusive lock) and decides what it read is still valid. Two shared holders upgrading at once cannot both win: the first to release is busy beside the other, which is still shared.

`flock` itself converts a held lock by dropping it first, so a single-call conversion would have the same window without saying so; the claim keeps the IO from issuing one (a second `flock` on its one open file description would convert the lock, and one release would free both holders).

## How PosixIO does it

- `PosixIO` opens the directory once (`directory_`). Each lock is a `flock` on that descriptor: `LOCK_EX | LOCK_NB` for the owner, `LOCK_SH | LOCK_NB` for the reader. Locks belong to the open file description, so two `PosixIO` objects on one directory, in one process or two, contend like two processes.
- **The claim.** `directory_claimed_` (an atomic exchange) admits one directory lock per IO; threads racing through one IO end with exactly one winner. A failed `flock` unwinds through the `Owner`'s destructor: `LOCK_UN` on the IO's own descriptor, then the claim cleared, so a loser never releases what another IO holds.
- **Errors.** `lock_exclusive` and `lock_shared` differ only in the `flock` operation; both report through `lock_failed`, the one errno mapping.
- **Death and renames.** The kernel releases a dead process's locks; the lock follows the directory's inode across a rename.

## Evidence

- `test/owner_lock_test.cc`: the reader in the pairing matrix (every mode against every mode, in both orders, across processes, distinct IO objects and one IO), `readers` (coexistence, the last release freeing the exclusive lock), `upgrade` (the refusal, the window, two upgrading readers), `concurrent_readers` (two threads, one IO or two, against the sequential outcome), and the EINTR, unsupported, partial-failure and base cases.
- `test/mutants/reader_shared_as_exclusive.sh`: the reader taking `LOCK_EX` fails the matrix's reader-beside-reader pairing.
