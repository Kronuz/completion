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
## Next

- Docs; catalog; Detent replay; journal against the branch; CI.

## Blocked on

Nothing.
