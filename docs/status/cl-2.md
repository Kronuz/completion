# CL-2: one owner-lock API

Consolidation audit item CL-2 (`cpp-libraries/docs/reviews/consolidation-2026-10-19.md`),
conditions C1 (additive, old refusals unchanged) and C4 (catalog in the same series), to
the spec in Detent's `docs/CONSOLIDATION_RESPONSE.md` ("CL-2: one ownership protocol").
Branch `cl-2-owner-lock`, worktree `~/code/Kronuz/completion-wt/cl-2`, base `main` 7734544.
Catalog: cpp-libraries branch `cl-2-owner-lock`, worktree
`~/code/Kronuz/completion-wt/cpp-libraries-cl-2`.

## State

Implemented, tested and documented; ready for code review. Not merged. Completion CI
green on all eight jobs (run 39380284199, at 91d62cb; later commits are docs only).

## Done

- `acquire_owner(OwnerLockOptions)` (file, directory, both; open-existing or
  create-exclusive), one PosixIO implementation with legacy presets forwarding to it;
  suite `completion_owner_lock` (matrix, partial failure, EINTR, unsupported, create and
  recover, aliases, lifetime through an image job, refusals). Six mutation probes (order,
  EINTR, file-only fallback, held checks, unwind, EOPNOTSUPP mapping) each fail it.
- Docs: README (three modes, quick start on `both`, compiled), ARCHITECTURE, AGENTS
  (two traps), PLATFORM_FINDINGS section 9 (dcf4b94).
- 91d62cb the API and `completion_owner_lock`; CI run 39380284199 passed it on
  freebsd-x86_64, linux-x86_64-gcc, linux-x86_64-clang, linux-arm64-gcc, linux-asan-ubsan,
  linux-tsan, macos-x86_64 and macos-arm64. Each topology's matrix asserts 17 excluding
  pairings and 8 acquiring legacy file/directory controls (separate processes, distinct
  IOs, one shared IO).
- Local: macOS x86_64 Apple clang, Debug 15/15 and ASan+UBSan 15/15.
- cpp-libraries 5d41c2d: the completion catalog entry (C4).
- Detent replay: local branch `cl-2-detent-replay` (worktree
  `~/code/Kronuz/completion-wt/cl-2-detent-replay`, head 5bd697f) is 5251cf9 plus the eight
  commits to 7734544 plus 91d62cb, headers identical to this branch. Detent 3d05a4f (Release,
  every default target, its own pins, completion overridden) built; ctest 166/167, then
  167/167 on a full rerun. The one failure, `test_local_paged_snapshot_plan` (aborted), waits
  at most 5 s for heap reclamation and calls no owner-lock code; it passed 20/20 isolated.
  Unattributed timing under the light hold, not reproduced.
- journal dbf753f against this branch: Debug, 8/8.

## Consumer test IOs (changes come later, in those repos)

Each must model exclusion or leave `lock_owner` unoverridden (the options call then reports
`not_supported`); none may return a lock that excludes nothing.

- journal `include/journal/memory_io.h:126`: `acquire_owner(bool)` returns a bare
  `OwnerLock`, a no-op success, and never adds `owner.lock` to the model. Needs held flags
  for the file and the directory (separately, so the legacy pairing still acquires), busy
  while held, `owner.lock` created on create, and a `lock_owner` taking the directory then
  the file and unwinding on failure.
- journal `test/journal_test.cc:119`: models file exclusion and create/exists; add
  `lock_owner` when Journal carries `OwnerLockOptions` (`journal.h:253, 277, 305, 605, 621`).
- strata `include/strata/faultfs.hh:400-415`: models file and directory exclusion apart;
  needs `lock_owner` (both: directory then file, unwound) before strata calls
  `acquire_owner(OwnerLockOptions{directory})`. The forwarding wrappers in
  `test/directory_test.cc:136`, `test/index_test.cc:105` and
  `test/background_merge_test.cc:175` need a `lock_owner` forwarding to
  `inner_->acquire_owner(options)`. Note: `index.hh:2348-2352` treats `not_supported` under
  `warn` as proceeding unlocked, so a test IO without `lock_owner` would warn rather than
  fail once strata calls the options form.
- Detent `tests/detent_local/test_inventory.cc:40` returns a no-op lock and
  `tests/detent_delivery/test_completion_scheduler.cc:17` returns null; both need to model
  exclusion or report `not_supported` before `StorageSession` (`storage_session.cc:19,
  :107`) moves to `both`.

## Next

- Code review of both branches; land after it.
- Consumer migrations: Detent to `both` (StorageSession and Journal-owned locks), journal
  `MemoryIO`, strata `faultfs` and wrappers (above). Then deprecate the bool form.
- Not run here: strata's suite against the branch; Linux tmpfs for `owner_lock`.

## Blocked on

Nothing.
