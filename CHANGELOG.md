# Changelog

All notable changes to `completion`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

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
