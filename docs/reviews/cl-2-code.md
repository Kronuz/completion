# CL-2 code review: one owner-lock API

Branch `cl-2-owner-lock` of Kronuz/completion: code at `91d62cb`, docs and ledger through
`616a8c1` (equal to `origin/cl-2-owner-lock`), based on `main` `7734544`. Catalog: cpp-libraries
branch `cl-2-owner-lock` at `5d41c2d`, whose parent is on `main`. Reviewed 2026-10-20 in a
detached worktree, `~/code/Kronuz/completion-wt/review-cl2`.

Specs: the consolidation audit (`cpp-libraries/docs/reviews/consolidation-2026-10-19.md`: CL-2,
C1 to C4, Responses), Detent's `docs/CONSOLIDATION_RESPONSE.md` ("CL-2: one ownership protocol"
and the strata follow-up), and the ledger `docs/status/cl-2.md`. Completion has no
`calibration.md`, so I applied strata's (`strata/docs/reviews/calibration.md`). Its checks that
apply here: CI on every platform for the reviewed commit, `HEAD` against `origin`, and every
mechanism a claim relies on found in the code.

## Verdict

**REJECT**, with one narrow blocking item. The locking itself is correct, and the evidence
for it is strong. On three local builds (Debug, ASan+UBSan, TSan) the 25-pairing matrix gives
17 exclusions and 8 unsafe controls in every topology. 12 of my 14 mutants fail the suite,
including all six the ledger names, and the 2 survivors are unobservable by design. The legacy
calls give the same bytes and refusals as main except for one changed message. CI run
39380284199 is green on all eight jobs at `91d62cb`.

What blocks landing is CL2-1: `acquire_owner({})` compiles and quietly takes the legacy
file-only lock. That is the unsafe mode this change exists to retire, and it ships in a
shared API that four consumers will adopt next. The fix is one declaration plus a compile
test. The re-check can be scoped to it and to the conditions below.

## Findings

### CL2-1. `acquire_owner({})` resolves to the legacy file-only lock (blocking)

`include/completion/io.h:172` adds `acquire_owner(const OwnerLockOptions&)` beside the virtual
`acquire_owner(bool)` at `io.h:187`. An empty braced list is an identity conversion to `bool`
([over.ics.list], empty list to a non-class type). To the aggregate `OwnerLockOptions` it is a
user-defined conversion. So `io.acquire_owner({})` picks `acquire_owner(false)`: a file-only
open of an existing `owner.lock`, with no directory lock.

The documented default (`OwnerLockOptions{}` is `both`, `io.h:109`; README "`both` (the
default)") therefore can't be reached through the most natural spelling. The fallback is the
one mode that does not exclude a directory-only owner. The branch documents the trap
(`io.h:170`, `README.md:83`) instead of closing it.

Probe `.scratch/probes/braced_init_probe.cc`, run against the branch headers:

```
IO&.acquire_owner({})                -> bool(false)
IO&.acquire_owner(OwnerLockOptions{}) -> options (mode both: 1)
IO&.acquire_owner({both})            -> options
Hiding.acquire_owner({})             -> bool
```

The same file judges the candidate fixes with `static_assert`s over requires-expressions,
covering every call shape the consumers use (journal, consensus, strata, Detent: literal
`true`/`false`, a `bool` variable, and `0`, which none of them uses but the probe checks
anyway). All of these compile, so the assertions hold:

| Fix | `{}` | Legacy calls | Verdict |
|---|---|---|---|
| 1. `void acquire_owner(braced_owner_options_need_a_type) = delete;` with a scoped enum declared for it | ambiguous through `IO&` and `PosixIO`, so it fails to compile | `true`, `false`, a `bool`, `0`, named, `{mode}` and designated forms all compile | **Recommended** |
| 1b. The same with `std::nullptr_t` | fails to compile | `acquire_owner(0)` and `NULL` become ambiguous | Rejected |
| 2. A tag type, `acquire_owner(owner_options, opts)` | still the bool overload | unchanged | Rejected: it adds a spelling and fixes nothing |
| 3. A non-overloaded name, such as `acquire_owner_lock(opts = {})` | the documented default (`both`, open existing), including through derived types | unchanged | Acceptable alternative |

Fix 1 keeps the name that the audit, Detent and strata agreed on. Because no implicit
conversion reaches a scoped enum, no existing call changes meaning. It leaves one hole:
`{}` still means `bool` on a derived type that overrides only the bool call and has no
`using IO::acquire_owner;` (journal `MemoryIO`, strata `faultfs`). That is no worse than
today, and such a type can't spell the options call at all until it adds the
using-declaration, which brings the deleted overload with it.

Fix 3 closes that hole too, and costs nothing while nothing has adopted the new call. It does
rename what the consumers planned to call. Either fix lands with:

1. A compile-time test in the suite (a requires-expression `static_assert`, as in the probe)
   that `{}` does not compile through `IO&` and `PosixIO`, or selects `both` under fix 3.
2. `io.h:170-171` and `README.md:83` rewritten to say that `{}` is refused (fix 1) or is the
   default (fix 3).
3. The cpp-libraries catalog entry updated in the same series (C4).

### CL2-2. A failed create leaves `owner.lock`, and the contract doesn't say so (low)

`posix.h:604-607`: with `create_exclusive`, the file is created before its `flock` and
`file_sync`, so a failure at either step leaves a zero-length `0600` `owner.lock`. The test
asserts this on purpose (`test/owner_lock_test.cc:394`, "a failed create leaves owner.lock
for recovery"), and the legacy transcript shows main does the same, so this is not a
regression.

I agree with keeping it. Unlinking is unsafe whenever another process could have opened the
just-created file and won its `flock`. In `both` mode the directory lock excludes
`both`/directory owners, but not a legacy file-only opener, which is the exact mixed-protocol
window. The cost is that a retried `create_exclusive` then fails with `file_exists`, and the
caller must recover with `open_existing` (journal does, at `journal.h:605`).

Neither `io.h:163-171` nor README nor ARCHITECTURE states this. The prompt's expectation of
"no stray owner.lock" is therefore met only for the directory step (verified at
`owner_lock_test.cc:408-415` and by mutant m1).

**Condition:** state in `io.h` and ARCHITECTURE that a failed create may leave `owner.lock`
for `open_existing`.

### CL2-3. Concurrent acquisition through one `PosixIO`: `both`'s unwind releases the winner's directory lock (medium)

`posix.h:595-596` reads the held flags as plain fields before taking anything. A `flock` on the
shared directory descriptor `directory_` succeeds again for its own open file description. So
two threads acquiring through one IO can both pass the checks and both lock the directory.

In `both` mode the loser then fails at the file step, and its `Owner` destructor
(`posix.h:575-580`) runs `flock(io_.directory_, LOCK_UN)` on the shared description. That
releases the directory lock the winner believes it holds. The winner still holds
`owner.lock`, so file and `both` competitors stay excluded, but a directory-only owner (strata's
protocol) gets in.

Probe `.scratch/probes/shared_io_race.cc`, 2000 iterations each:

- `both`: in 1993 iterations a winner held the lock while a third `PosixIO` took the directory
  lock.
- `directory`: in 1990 iterations both threads held independently releasable handles.

TSan reports 5 races, in `lock_owner` and in `~Owner` called from `lock_owner`'s unwind
(`.scratch/probes/shared_io_race_tsan.log`).

Attribution: the `directory` alias is pre-existing. `shared_io_race_legacy.cc` with
`acquire_directory_owner()` gives 1994/2000 on main headers and 1997/2000 on the branch. The
dropped-lock outcome is new, because the unwind path is new.

Every consumer acquires once, at startup, on one thread, and no thread-safety contract for
`IO` is written down, so this does not block. It is still a lock whose own error path can
silently release another holder's lock.

**Condition:** either document "one thread acquires through an IO at a time", or make the
claim atomic: claim the held flags with an `exchange` before the `flock`, and clear only what
this `Owner` claimed. Prefer the second, folded into the CL2-1 rework.

The same plain fields are also written by `~Owner` on whatever thread drops the last
reference, for example an image job settling on a completion worker, while `sync_directory`
reads `owner_fd_` on macOS (`posix.h:542`). That pattern is pre-existing (main's `Lock` and
`DirectoryLock` did the same); I note it for the same fix.

### CL2-4. The ledger's Detent attribution was wrong; the failure is a pre-existing Detent flake (condition: correct the ledger)

The ledger (`docs/status/cl-2.md:35-37`) says `test_local_paged_snapshot_plan` "waits at most
5 s for heap reclamation and calls no owner-lock code" and calls the failure unattributed
timing. Both statements are wrong:

1. The first run aborted at 1.20 s (`~/.scratch/cl-2/detent-ctest.log:330`), too soon for the
   5-second deadline at `test_paged_snapshot_plan.cc:159-163`.
2. That check throws `runtime_error`, not the exception that actually fires.
3. The test does take the owner lock: `make_disk_store` reaches `make_owned_storage_session`,
   which calls `acquire_owner(create_owner)` (`storage_session.cc:107`) through the changed
   path.

So 20/20 isolated plus one full rerun was not enough evidence, and the stated cause did not
fit it. I measured it instead (`.scratch/detent-flake/`). Both sides use the same Detent
source (`3d05a4f`), Release, and the developer's AppleClang, with only the completion
override differing (`7a18c07` = main's headers, `5bd697f` = the branch's headers, each
verified by `diff -r`). Runs are 150 per block at 4 in parallel, blocks interleaved:

| Completion | Failures |
|---|---|
| base (main) | 2 of 300 |
| CL-2 | 1 of 300 |
| CL-2, developer's binary | 2 of 200 |

Every failure is the same uncaught `detent::distribution::StoragePressure` ("distribution
storage pressure"). It reproduces without CL-2, so it is pre-existing and not caused by this
branch. The Detent result therefore stands (166/167 plus a clean rerun, with the one failure
attributed).

The flake itself is Detent's: a `StoragePressure` escapes somewhere other than the guarded
`PagedPlanBuilder blocked` at `:153` under parallel load. The lead should pass it to Detent.

**Condition:** correct the ledger's explanation.

### CL2-5. The six mutation probes the ledger cites weren't kept (low, process)

`docs/status/cl-2.md:20` names six mutants that fail the suite, but none exists in the
developer's worktree or `.scratch`. I rebuilt them, plus eight more, in
`.scratch/mutants/mutate.py` (exact-anchor replacements) and `run.sh`, Homebrew clang 23,
`-O1`:

| Mutant | Result |
|---|---|
| m1, file before directory | killed (`:254`, a competing process wrote `owner.lock`) |
| m2, no EINTR retry | killed (uncaught EINTR) |
| m3, file-only fallback on unsupported directory | killed (`:447`) |
| m4, no held checks | killed (shared topology busy, not refused) |
| m5, directory not unwound | killed (EWOULDBLOCK escapes) |
| m6, EOPNOTSUPP unmapped | killed (`:399`, macOS) |
| m7, `both` treated as file-only | killed (directory then `both` acquired) |
| m10, fd leaked on failure | killed (`:392`) |
| m11, legacy file preset becomes `both` | killed (the unsafe control stopped acquiring) |
| m12, no directory held check | killed (`:268`) |
| m13, busy reported as not supported | killed (`:257`) |
| m14, `owner_fd_` not recorded | killed |
| m8, release the directory before the file | **survived**: release order is liveness only (a `both` contender sees a spurious busy), so it is not a safety property, and nothing observes it |
| m9, no `file_sync` after create | **survived**: durability is unobservable without a sync seam. Main synced, and the branch still does (`posix.h:607`) |

m6 is equivalent on Linux, where `EOPNOTSUPP == ENOTSUP`. I reran m1 to m6 once more, with
the same results.

### CL2-6. C1 compatibility: same bytes and refusals, one message text changed (info)

`.scratch/probes/legacy_transcript.cc`, built against main's and the branch's headers by
`legacy_diff.sh`, records outcome, error category and value, `what()`, the directory listing
(mode, size, link count) and descriptor leaks for 21 legacy steps under umask 0. The only
differences are:

1. The directory not-supported text changed from "directory owner lock: apfs does not lock
   directories" to "lock journal directory: apfs does not lock directories". The code is
   unchanged (`generic:45`), and no consumer matches the text (grep of journal, consensus,
   strata and Detent).
2. The file step now goes through `replace_flock` (intended, `posix.h:443-445`), and with it the
   file lock's EINTR retry and `not_supported` mapping that the audit asked for. On macOS, a
   file `flock` failing with EOPNOTSUPP (102) now surfaces as ENOTSUP (45). That is a refusal
   change, but one the CL-2 spec asked for ("unify ... unsupported/busy errors").
3. A symlink size differs because the state paths differ, which is a probe artifact.

Everything else is identical: `owner.lock` is 0600 and 0 bytes, `file_exists` and ENOENT
match, busy is 35, the `logic_error` texts match, the symlink, hard-link, group-writable and
directory refusals match, and nothing leaks.

The suite's `bootstrap` (`owner_lock_test.cc:289-321`) covers the single empty 0600
`owner.lock` under umask 0. journal's suite (Debug, built by the developer against this
branch, headers identical to `616a8c1`) passes 8/8 on my rerun.

The base `lock_owner` throws `not_supported` without recursing (`io.h:226`, test `base()`,
`legacy.calls == 0`). Existing test IOs still compile: strata's `faultfs_test`,
`directory_test`, `index_test` and `background_merge_test` pass a syntax-only compile with
`-Wall -Woverloaded-virtual -Werror` against the branch (`.scratch/probes/consumer_syntax.sh`).
Hiding the new non-virtual overload raises no `-Woverloaded-virtual` warning.

### CL2-7. Ordering, errno mapping and the matrix: verified (no finding)

**Ordering and unwind.** The directory is locked before the file (`posix.h:598-608`), and
`~Owner` closes the file before the directory (`:575-580`; order not tested, see m8). Every
failure unwinds through `~Owner`. The held checks run before anything is taken
(`:595-596`).

**Error mapping.** EINTR is retried at both steps; EAGAIN is folded into EWOULDBLOCK (`:617`);
ENOTSUP and EOPNOTSUPP map to `not_supported`. There is no fallback to file-only (m3 is
killed). ENOLCK, which Linux NFS returns without lockd, surfaces as a plain `system_error`.
That fails closed, which is acceptable, but it is not `not_supported`, so a warn-policy
consumer (strata's `index.hh:2350`) refuses rather than proceeding. Worth one line in
PLATFORM_FINDINGS §9.

**The matrix.** The 17/8 split is the right expectation: five holders (file through the
legacy and options calls, directory through the legacy and options calls, and `both`) give 25
ordered pairings. Only file against directory in either order fails to exclude, which is
2 × 2 × 2 = 8. Every pairing involving `both` excludes in all three topologies.

The unsafe control really holds both locks at once. After the competitor acquires, a fresh
attempt in the holder's own mode is still busy (`:257`, `:268`), and m11 shows that turning
the legacy file call into `both` fails the matrix. In the shared topology, "excluded" means
refused with a `logic_error` before any `flock`: no aliases through one IO in single-threaded
use (`aliases()`; the concurrent case is CL2-3).

**Lifetime.** `lifetime()` drops the caller's IO and owner handles, keeps only the image job,
finds every mode busy while the original is undelivered, and then free once the job is gone.
It does not drop the job facade while the original is in flight. That retention is
`ImagePreparation`'s existing lease behavior, which this branch does not change, so I accept
it.

### Catalog (cpp-libraries `5d41c2d`)

The entry is accurate to the code: the three modes, the order, the unsafe legacy pairing,
EWOULDBLOCK and `not_supported` with no fallback. It needs one more line once CL2-1 is fixed,
in the same series (C4).

## Blocking

1. CL2-1: make `acquire_owner({})` fail to compile (fix 1) or mean `both` (fix 3), with a
   compile-time test, the README and `io.h` text, and the catalog line.

## Conditions

1. CL2-2: document that a failed create may leave `owner.lock` for `open_existing`.
2. CL2-3: make the held-flag claim atomic, or document single-threaded acquisition per IO.
   Prefer the atomic claim, in the same rework.
3. CL2-4: correct the ledger's Detent explanation, and pass the `StoragePressure` flake to
   Detent.
4. CL2-7: add ENOLCK's handling to PLATFORM_FINDINGS §9.

## Probes (kept in `.scratch/`, not to be deleted)

- `build.sh debug|asan|tsan`: the suite under Homebrew clang 23, with Debug, ASan+UBSan and TSan
  each 15/15. Logs are `build-*.ctest.log`, with 0 sanitizer reports. The binaries were checked
  for `__asan_report`/`__tsan_read` symbols.
- `probes/braced_init_probe.cc` (CL2-1), `probes/build_probes.sh`.
- `probes/shared_io_race.cc`, `shared_io_race_legacy.cc` and `shared_io_race_tsan.log` (CL2-3).
- `probes/legacy_transcript.cc`, `legacy_diff.sh`, `legacy-{main,branch}.txt` and `legacy.diff`
  (CL2-6).
- `probes/consumer_syntax.sh`, with `syntax-*.log` (CL2-6).
- `probes/readme_quickstart.cc`: the README quick start passes a syntax-only compile.
- `mutants/mutate.py`, `mutants/run.sh`, and the `m*.run.log` and `m*.build.log` files (CL2-5).
- `detent-flake/build_ab.sh` and `repeat.sh`, with `runs-{build-base,build-cl2,detent-build}/`
  (CL2-4).

## Not run

- Linux, FreeBSD and Linux tmpfs locally. I relied on CI run 39380284199, whose `owner_lock`
  passed on all eight jobs at `91d62cb`. The branch adds no tmpfs `owner_lock` case.
- strata's and consensus's suites against the branch. strata was only syntax-compiled.
  consensus's `worker_test` fails to compile against both main's and the branch's headers
  (stale `journal` in its build's deps), so it is unverified and the failure is not
  attributable to this branch.
- Detent's full suite. I reran only `test_local_paged_snapshot_plan`, as an A/B.
- Network, FUSE and SMB filesystems.
- A thread-safety contract for `IO` beyond the probe.
- A test that drops the image-job facade while its original is outstanding.
- The journal suite rebuilt from source. I reran the developer's journal build (it wrote
  `Testing/` in `~/.scratch/cl-2/journal-build`).
