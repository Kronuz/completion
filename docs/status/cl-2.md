# CL-2: one owner-lock API

Consolidation audit item CL-2 (`cpp-libraries/docs/reviews/consolidation-2026-10-19.md`),
conditions C1 (additive, old refusals unchanged) and C4 (catalog in the same series), to
the spec in Detent's `docs/CONSOLIDATION_RESPONSE.md` ("CL-2: one ownership protocol").
Branch `cl-2-owner-lock`, worktree `~/code/Kronuz/completion-wt/cl-2`, base `main` 7734544.
Catalog: cpp-libraries branch `cl-2-owner-lock`, worktree
`~/code/Kronuz/completion-wt/cpp-libraries-cl-2`.

## State

Code review (`docs/reviews/cl-2-code.md`, REJECT on CL2-1) folded in; ready for the scoped
re-check. Not merged. Completion CI for the review fixes: see the run id at the end of
this section's Done list. Earlier green run 39380284199 was at 91d62cb.

## Done

- `acquire_owner(OwnerLockOptions)` (file, directory, both; open-existing or
  create-exclusive), one PosixIO implementation with legacy presets forwarding to it;
  suite `completion_owner_lock` (matrix, partial failure, EINTR, unsupported, create and
  recover, aliases, lifetime through an image job, refusals). Six mutation probes (order,
  EINTR, file-only fallback, held checks, unwind, EOPNOTSUPP mapping) each fail it; the
  reviewer rebuilt them and eight more (`review-cl2/.scratch/mutants`, 12 of 14 killed, the
  2 survivors unobservable by design).
- Docs: README (three modes, quick start on `both`, compiled), ARCHITECTURE, AGENTS
  (traps), PLATFORM_FINDINGS section 9.
- 91d62cb the API and `completion_owner_lock`; CI run 39380284199 passed it on
  freebsd-x86_64, linux-x86_64-gcc, linux-x86_64-clang, linux-arm64-gcc, linux-asan-ubsan,
  linux-tsan, macos-x86_64 and macos-arm64. Each topology's matrix asserts 17 excluding
  pairings and 8 acquiring legacy file/directory controls (separate processes, distinct
  IOs, one shared IO).
- cpp-libraries 5d41c2d: the completion catalog entry (C4).
- Detent replay: local branch `cl-2-detent-replay` (head 5bd697f), Detent 3d05a4f Release
  with completion overridden: ctest 166/167, then 167/167 on a full rerun.
- journal dbf753f against this branch: Debug, 8/8.

Review fixes (`docs/reviews/cl-2-code.md`):

- CL2-1: `IO::acquire_owner(braced_owner_options_need_a_type) = delete` (a scoped enum), so
  `acquire_owner({})` is ambiguous and fails to compile through `IO&` and `PosixIO`; `true`,
  `false`, a `bool`, `0`, `OwnerLockOptions{}`, `{mode}` and designated forms still compile.
  CTest `completion_owner_braced_refused` (`test/owner_braced_check.cmake`) compiles
  `test/owner_braced_refused.cc` once per case: the refusals must fail naming the deleted
  overload, the control must build. With the overload removed the refusals compile. The
  reviewer's probe's `{}` assertions now hold (`BracedCompiles<IO>` and `<PosixIO>` false).
  Residual, as the review notes: a derived type overriding only the bool call without
  `using IO::acquire_owner;` still takes `{}` as bool.
- CL2-3: the held flags are atomic claims (`exchange` before any `flock`); an `Owner`
  releases only what it claimed, and a claim clears after its lock is gone;
  `owner_fd_` is atomic. The reviewer's `shared_io_race` probe, 2000 iterations each for
  `directory` and `both`: 0 double holds and 0 droppable locks (1990 and 1993 bad on the
  branch before), and 0 TSan reports. New `concurrent()` in `completion_owner_lock`
  (300 iterations in each mode) fails on the previous `posix.h` and passes now.
- CL2-4: corrected below.
- CL2-2: README, ARCHITECTURE and `io.h` state that a failed create may leave a zero-length
  `owner.lock`; recover with `open_existing` (a retried create is `file_exists`).
- CL2-7: PLATFORM_FINDINGS section 9 says ENOLCK is a plain `system_error`, failing closed.
- CL2-6: PLATFORM_FINDINGS section 9 notes the directory not-supported text now reads
  "lock journal directory: ... does not lock directories"; the code is unchanged.
- Local, Homebrew clang 23 Debug: Debug, ASan+UBSan and TSan, 16/16 each (the suite plus
  the braced-refusal test), 0 sanitizer reports.

## Detent (CL2-4, corrected)

`test_local_paged_snapshot_plan` is a pre-existing Detent flake, not timing and not CL-2.
It fails with an uncaught `detent::distribution::StoragePressure`, and it does take the
owner lock (`make_disk_store` reaches `acquire_owner(create_owner)` through the changed
path), so the earlier claim that it calls no owner-lock code was wrong. The reviewer's A/B
(same Detent source, only the completion headers differing): 2 of 300 failures on main's
headers, 1 of 300 on CL-2's, 2 of 200 on the developer's binary. The lead passes the
`StoragePressure` escape to Detent. The Detent result stands (166/167, then 167/167).

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

- Scoped re-check of CL2-1 to CL2-4, then land.
- Consumer migrations: Detent to `both` (StorageSession and Journal-owned locks), journal
  `MemoryIO`, strata `faultfs` and wrappers (above). Then deprecate the bool form.
- Not run here: strata's suite against the branch; Linux tmpfs for `owner_lock`.

## Blocked on

Nothing.
