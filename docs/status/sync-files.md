# S1: SyncFiles and OrderFiles

Detent step S1 (`Detent/docs/status/namespace-design.md`, "S1 (completion `SyncFiles` and `OrderFiles`)"; design D11 in `Detent/docs/design/namespace-delivery.md`; reviews ND-R5 and ND-R6). Branch `phase/sync-files`, base main `e2c6474`. Lead ruling L1: one directory; each primitive takes a list of read-only or read-write files plus the IO's directory. Shape constraint from the Xapiand lead: the primitives are async front ends over `IO::sync_files_and_directory` and `IO::order_files_and_directory`, not a second syscall implementation, so strata's counted IO and faultfs see them unchanged. The `IO` interface lives in completion's `io.h` (namespace `kronuz::journal`); completion includes nothing from journal, so the dependency does not invert.

## State

Implemented; local macOS arm64 suite green. Not merged, pin not moved (the lead lands it).

## Done

All four S1 criteria are met, pending the lead's review. Evidence per criterion:

1. Criteria 1 and 1a: `SyncFiles` and `OrderFiles` (`operation.h`), `MutationRequest::files`. For 1a, `MutationRequest::read_only` makes `Open`, `OpenInto`, `OpenCandidate` and `OpenCandidateInto` open `O_RDONLY` through `IO::open_existing_read_only`, `open_existing_read_only_into`, `open_reclaim_candidate_read_only` and `open_reclaim_candidate_read_only_into`; the existing sync `open_existing` and the three other opens are unchanged, and the candidate checks are shared (`reclaimable`). `read_only_opens` in `sync_primitives_test` proves each refuses writes, reads, and takes a lease when read-only; mutants s7 and s8 break the flag and `O_RDONLY`. `sync_primitives_test` drives a mixed read-write and read-only group through the IO method, `drive_synchronously` and the native queue's fallback worker; `asio_extension_test` (built against standalone Asio) drives both kinds through `AsioCompletionDriver` and the extension metrics.
2. Criterion 2: `test/sync_primitives_test.cc` commits the expected sequences per platform as strings (macOS: `fsync:a fsync:d fsync:b fsync:dir full:a`, and `barrier:a` for OrderFiles; Linux: `writeback:*` then `fsync:*` then `fsync:dir`; FreeBSD: `fsync:*` then `fsync:dir`), plus read-only alone, empty group, injected EIO. Green on CI run 41738797330 for linux-x86_64 gcc and clang, linux-arm64, macos-arm64, macos-x86_64 and freebsd-x86_64, and locally on macOS arm64.
3. Criterion 3: `docs/mechanisms/sync-files.md` (table per platform and "What an error means"); pointers in `ARCHITECTURE.md`, `docs/PLATFORM_FINDINGS.md`, `docs/mechanisms/commit-barriers.md`.
4. Criterion 4 (completion side): mutants `s1` to `s6` (`tools/mutants.py`). macOS local run: s1, s2, s3, s4, s5 killed, s6 survives as expected (code not compiled on macOS). Linux workflow run 41738803943 found s1 and s2 killed by `completion_sync_files` where this ledger first expected them to survive; expectations corrected. Sanitizers in separate builds on macOS arm64 with clang: ASan+UBSan (`build-asan`, `detect_leaks=0` because macOS ASan has no leak checker) and TSan (`build-tsan`), 20 of 20 tests each; CI linux-asan-ubsan (with leak detection) and linux-tsan green. The Detent pin move is the lead's.

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
