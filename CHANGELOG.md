# Changelog

All notable changes to `completion`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

## 2026-11-01

### Added

- `bench/`: the four timing probes behind `docs/PLATFORM_FINDINGS.md` (`sync_probe.cc`,
  `commit_probe.cc`, `unlink_probe.cc`, `dirflush_probe.cc`), built with
  `-DCOMPLETION_BUILD_BENCH=ON`. CI builds them and runs each once on a small input.
- `tools/mutants.py`: the 14 owner-lock mutants of the CL-2 review (CL2-5) as exact-anchor
  replacements. Each is applied to a scratch copy, rebuilt and run against the suite. The CTest
  test `completion_mutants_anchors` checks the anchors; the `mutants` workflow runs the script.

## 2026-10-30

### Changed

- `completion::AllocationContext` and `completion::OwnedAllocator<T>`, from
  `completion/allocation.h`, are aliases of the shared `allocators::resources` types. The
  resource lifetime and weak-control-block contract are unchanged. Completion links the
  header-only resource target, which neither builds nor links global allocation overrides or
  the optional cleanup runtime.
- The aliases keep the C++ source names, but the canonical type identities and some internal
  object layouts change. Rebuild C++ dependents together; do not mix precompiled objects from
  before and after this change. Persisted formats are unchanged.
