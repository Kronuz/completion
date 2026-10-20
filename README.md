# completion

[![CI](https://github.com/Kronuz/completion/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/completion/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Owned, completion-driven file I/O for C++20: a hardened POSIX backend, and
operations that run synchronously, on Linux io_uring, on POSIX AIO (FreeBSD and
macOS), or under Asio coroutines, without ever losing track of an in-flight
write. The storage layer under [journal](https://github.com/Kronuz/journal) and
[strata](https://github.com/Kronuz/strata).

![completion: an owned operation issues one primitive at a time to the driver the host picks, and gets back the original's result](docs/completion.svg)

## ✨ Highlights

- **Nothing is ever faked.** An accepted operation keeps its buffers, names, and
  file leases until the original primitive completes; cancellation cannot
  fabricate a result or free memory the kernel is using.
- **One state machine, four drivers.** The same primitives run synchronously, on
  io_uring, on POSIX AIO, or through the `completion::asio` adapter.
- **A hardened directory backend.** `PosixIO` rejects symlinks, hard links,
  foreign-owned and world-writable files, and unsafe names; creation is exclusive. The
  root itself is trusted by default only when it is owned and private; two options let a
  host proceed and warn instead.
- **Durability done right.** `F_FULLFSYNC` on macOS, batched `sync_files` across
  devices, and separate file and directory barriers.
- **Zero-copy reads.** `File::map()` gives a read-only view of a whole file.
- **Allocation-aware.** Optional PMR contexts account every control block,
  queue, and name, so a service can admit its memory up front.

## 📦 Installation

Requires a C++20 compiler and CMake 3.16 or newer; it fetches
[metrics](https://github.com/Kronuz/metrics) at a pinned commit. The Asio adapter
needs standalone Asio from the host. CI builds it on Linux (x86_64, arm64), macOS
(arm64, x86_64), and FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(completion
  GIT_REPOSITORY https://github.com/Kronuz/completion.git
  GIT_TAG        main)
FetchContent_MakeAvailable(completion)

target_link_libraries(my_app PRIVATE completion::completion)   # or completion::asio
```

## 🚀 Quick start

Write a file and make it durable through the trusted-directory backend:

```cpp
#include "completion/posix.h"

#include <string_view>

namespace j = kronuz::journal;

int main() {
	j::PosixIO io("data");                         // an existing private directory
	// Exclusive across processes: the directory, then owner.lock, created once.
	auto owner = io.acquire_owner(j::OwnerLockOptions{j::OwnerLockMode::both, j::OwnerFile::create_exclusive});
	auto file = io.create_exclusive("record");
	std::string_view bytes = "durable bytes\n";
	for (std::size_t at = 0; at < bytes.size();) {
		auto n = file->write_at(at, bytes.substr(at));   // writes may be short
		if (n == 0) { return 1; }
		at += n;
	}
	file->sync();                                  // F_FULLFSYNC on macOS
	io.sync_directory();                           // the name is durable too
}
```

The owner lock has three modes. `both` (the default) locks the directory handle and then
`owner.lock`, and excludes an owner in any mode. `file` and `directory` are what the
legacy `io.acquire_owner(bool)` and `io.acquire_directory_owner()` take: they lock
different inodes, so a file-only owner and a directory-only owner do not exclude each
other. Recovery opens the existing `owner.lock` (`OwnerFile::open_existing`, the default);
a directory-only lock writes nothing. Busy is `EWOULDBLOCK` in every mode. Pass the
options by name: `io.acquire_owner({})` does not compile (it would take the legacy file-only
bool overload), while `OwnerLockOptions{}` is `both`. A failed `create_exclusive` may leave
a zero-length `owner.lock`; recover with `open_existing`. Acquisitions through one
`PosixIO` are safe from several threads: one wins and the others get `logic_error` or
busy, and a loser never releases the winner's lock.

Locks are tried on any filesystem. `io.directory_filesystem()` reports whether it is known
local (`local`, plus its `name`), so a host can warn before it relies on the lock over NFS,
SMB, or the like. A `flock` the filesystem does not support fails with
`std::errc::not_supported` in every mode that needs it, with no fallback to another mode;
the host decides whether to run unlocked.

By default `PosixIO` refuses a root that is a symlink, is owned by another user, or is
writable by group or others. `PosixIOOptions` relaxes that, for a host that would rather warn:
`PosixIO io(dir, 0600, {.follow_root_symlink = true, .allow_shared_directory = true})`
opens through a final symlink (the descriptor is then held, so a later retarget moves
nothing) and admits a foreign owner or a shared-writable mode. Either way
`io.directory_trust()` reports what was found (`symlinked_root`, `foreign_owner`,
`shared_writable`; `trusted()` is true when neither of the last two is set). An IO that
does not inspect its directory reports `known == false`, which is not trusted. Entries
inside the directory stay no-follow under every option.

[`examples/demo.cc`](examples/demo.cc) makes files in two directories durable with
one `sync_files` call.

## 🎯 When to use it

- ✅ Storage engines and logs that need real durability and asynchronous I/O
  without losing ownership of in-flight work.
- ❌ General file utilities or network I/O: this is a narrow, durability-first
  storage layer.
- ❌ Shared or untrusted directories: the backend requires a private directory it
  owns.

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): the ownership contract, the backend, image
  jobs, scanning, leases, reusable controls, and the drivers.
- [docs/PLATFORM_FINDINGS.md](docs/PLATFORM_FINDINGS.md): what durability and
  reclamation cost on macOS, Linux, and FreeBSD.
- [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md): qualification runs and object
  sizes.
- [AGENTS.md](AGENTS.md): repository map and the rules for changing I/O code.

## 🛠️ Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Add `-DASIO_INCLUDE_DIR=/path/to/asio/include` to also build the coroutine
adapter's test.

## 🤝 Contributing

Issues and pull requests are welcome. I/O changes need tests that hold an original
in flight and drive it after the facade is gone; read [AGENTS.md](AGENTS.md)
first. CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE).

## Owning allocation contexts

`completion::AllocationContext` and `completion::OwnedAllocator<T>` remain available through `completion/allocation.h` as aliases of the shared `allocators::resources` types. The resource lifetime and weak-control-block contract are unchanged. Completion links the header-only resource target, which neither builds nor links global allocation overrides or the optional cleanup runtime.

The shared aliases preserve C++ source names, but their canonical type identities and some internal object layouts change. Rebuild C++ dependents together; do not mix precompiled objects from before and after this extraction. Persisted formats are unchanged by these allocation/accounting moves.
