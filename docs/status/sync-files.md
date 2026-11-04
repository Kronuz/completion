# S1: SyncFiles and OrderFiles

Detent step S1 (`Detent/docs/status/namespace-design.md`, "S1 (completion `SyncFiles` and `OrderFiles`)"; design D11 in `Detent/docs/design/namespace-delivery.md`; reviews ND-R5 and ND-R6). Branch `phase/sync-files`, base main `e2c6474`. Lead ruling L1: one directory; each primitive takes a list of read-only or read-write files plus the IO's directory. Shape constraint from the Xapiand lead: the primitives are async front ends over `IO::sync_files_and_directory` and `IO::order_files_and_directory`, not a second syscall implementation, so strata's counted IO and faultfs see them unchanged. The `IO` interface lives in completion's `io.h` (namespace `kronuz::journal`); completion includes nothing from journal, so the dependency does not invert.

## State

Implemented; local macOS arm64 suite green. Not merged, pin not moved (the lead lands it).

## Done

(filled in with commits below)

## Design vs code

1. D11 says each group syncs "the way `IO::sync_files` does". The code runs `sync_files_and_directory`, which also syncs the directory (macOS: inside the same device flush), as the design's own library-change paragraph states.
2. ND-R5 offered a primitive spanning directories or D8 in one directory; L1 chose one directory. Files outside the IO's directory are unsupported.
3. ND-R6 asked for read-only descriptors; added `IO::open_existing_read_only` (default throws `logic_error`; `PosixIO` opens `O_RDONLY`). Wrapper IOs (strata's counted IO, faultfs) do not forward it until they add the override.
4. D11 expects batching on Linux and FreeBSD to amortize; it does not. They fsync per file and then the directory (N+1 filesystem fsyncs, each a device flush on ext4/XFS with barriers). Unmeasured; T2 decides whether concurrent fsyncs are needed.
5. The sync seam existed on macOS only; it now exists on Linux (`fsync`, `sync_file_range` hint) and FreeBSD (`fsync`) so each platform records its own sequence. `detail::DeviceSyncCalls` is renamed `detail::SyncCalls`.
6. Neither native queue has an opcode for a group: both run these kinds on the fallback worker (`fallback_extensions`).

## Next

1. Lead: review, land, move Detent's pin.

## Blocked on

Nothing.
