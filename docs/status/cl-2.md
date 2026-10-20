# CL-2: one owner-lock API

Consolidation audit item CL-2 (`cpp-libraries/docs/reviews/consolidation-2026-10-19.md`),
conditions C1 (additive, old refusals unchanged) and C4 (catalog in the same series), to
the spec in Detent's `docs/CONSOLIDATION_RESPONSE.md` ("CL-2: one ownership protocol").
Branch `cl-2-owner-lock`, worktree `~/code/Kronuz/completion-wt/cl-2`, base `main` 7734544.
Catalog: cpp-libraries branch `cl-2-owner-lock`, worktree
`~/code/Kronuz/completion-wt/cpp-libraries-cl-2`.

## State

Implemented and tested locally (macOS x86_64, Apple clang, Debug and ASan+UBSan);
docs, catalog, Detent replay and CI next.

## Done

- `acquire_owner(OwnerLockOptions)` (file, directory, both; open-existing or
  create-exclusive), one PosixIO implementation with legacy presets forwarding to it;
  suite `completion_owner_lock` (matrix, partial failure, EINTR, unsupported, create and
  recover, aliases, lifetime through an image job, refusals). Six mutation probes (order,
  EINTR, file-only fallback, held checks, unwind, EOPNOTSUPP mapping) each fail it.
- Docs: README (three modes, quick start on `both`, compiled), ARCHITECTURE, AGENTS
  (two traps), PLATFORM_FINDINGS section 9.

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

- Catalog; Detent replay; journal against the branch; CI.

## Blocked on

Nothing.
