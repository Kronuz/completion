# AGENTS.md — completion

Working notes for anyone changing this repository. [README.md](README.md) is for
users; [ARCHITECTURE.md](ARCHITECTURE.md) explains the design. Header-only, in
`include/completion/`. journal, consensus, strata, and Detent build on it, so a
change here reaches all of them.

## Repo map

```
include/completion/
  io.h                  IO, File, Mapping, OwnerLock(Options), DirectoryCursor: the backend interfaces.
  posix.h               PosixIO: the trusted-directory POSIX backend.
  operation.h           Requests, tokens, completions, and drive_synchronously().
  completion.h          Shared completion values and statistics.
  native_completion.h   Selects the platform queue.
  linux_completion.h    LinuxCompletionQueue: io_uring.
  bsd_completion.h      BsdCompletionQueue: POSIX AIO (FreeBSD kqueue, macOS polled).
  asio_completion.h     The optional coroutine driver (completion::asio).
  image.h               ImagePreparation and ImagePublication jobs.
  allocation.h          AllocationContext and OwnedAllocator.
  api.h                 The kronuz::io::completion entry point.
test/                   One suite per feature (core, allocation, scan, inspection, reuse, map, owner_lock, ...).
examples/demo.cc        Batched durability across two directories.
docs/                   PLATFORM_FINDINGS.md, MEASUREMENTS.md, mechanisms/, completion.d2 (the README diagram's source).
```

## Verification and benchmarks

The shared method is the catalog's
[PERFORMANCE.md](https://github.com/Kronuz/cpp-libraries/blob/main/docs/PERFORMANCE.md). From a
fresh clone (configure fetches `metrics` and `allocators` at the commits `CMakeLists.txt` pins):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2 && ctest --test-dir build --output-on-failure
```

- **Suites.** One CTest test per feature on a real directory in the build tree (17, plus
  `completion_dir_lock_tmpfs` on Linux). `sync_calls_test` records the `fsync` and `fcntl` calls
  `PosixIO` issues on macOS and asserts the protocol; `sync_files_test` models separate device
  caches and injects failed calls; `owner_lock_test` runs all 25 holder pairings in three topologies
  (17 exclude, 8 are the unsafe legacy control); `completion_owner_braced_refused` fails if the
  `= delete` overload leaves `io.h`. The Asio adapter's test needs standalone Asio, which nothing
  fetches: `git clone --depth 1 https://github.com/chriskohlhoff/asio ../asio`, then configure
  with `-DASIO_INCLUDE_DIR=$PWD/../asio/asio/include`.
- **Sanitizers.** CI (`.github/workflows/ci.yml`, the catalog's shared
  [`cmake.yml`](https://github.com/Kronuz/cpp-libraries/blob/main/.github/workflows/cmake.yml))
  runs Linux x86_64 (GCC, Clang) and arm64 (GCC), macOS arm64 and x86_64, FreeBSD, and these two
  jobs, on Linux with Clang. Allocation-counting checks compile out under TSan (see Traps).

  ```sh
  CC=clang CXX=clang++ cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS" \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
  cmake --build build-asan && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
  CC=clang CXX=clang++ cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g -fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
  cmake --build build-tsan && TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" ctest --test-dir build-tsan --output-on-failure
  ```

- **Compatibility gate.** journal's suites against this checkout, before landing:
  `git clone https://github.com/Kronuz/journal ../journal && cmake -S ../journal -B build-journal -DFETCHCONTENT_SOURCE_DIR_COMPLETION=$PWD && cmake --build build-journal && ctest --test-dir build-journal`.
- **Not here.** No mutation script: `docs/reviews/cl-2-code.md` (CL2-5) lists 14 owner-lock
  mutants and the 12 the suite kills, and their script was not kept. No benchmark target:
  `docs/MEASUREMENTS.md` has each feature's qualification runs and object sizes, and
  `docs/PLATFORM_FINDINGS.md` and `docs/mechanisms/commit-barriers.md` the platform timings
  (batched syncs, barriers, unlink cost) with each probe's method; the probes were not kept.

## Rules for I/O code

- **The original owns its resources until it completes.** Never release a buffer,
  name, or file lease, refund admission, or fabricate a result because a consumer
  cancelled.
- **Every driver runs the same primitive set.** A new primitive gets the
  synchronous path, both native queues, and the Asio adapter, with tests that hold
  an original in flight and complete it after the facade is gone.
- **Additive by default.** New primitives append ordinals; the twelve legacy
  statistics arrays and existing request and result layouts stay unchanged, and
  new capabilities default to unsupported. Header consumers rebuild together.
- **Fallback is not native.** The bounded fallback worker keeps a backend working;
  it does not qualify the native path. Report which ran.
- **Durability claims need a barrier.** A file `sync` does not make a new name
  durable; that takes `sync_directory`.
- **Record platform costs** in `docs/PLATFORM_FINDINGS.md` when a finding changes
  what consumers should do.

## Traps

- **`file` and `directory` owner locks do not exclude each other.** They lock different
  inodes; only `both` excludes every owner. Keep a directory under one protocol, and
  keep `owner_lock_test`'s legacy pairing acquiring: it is the unsafe control.
- **`acquire_owner({})` must stay refused.** The deleted scoped-enum overload in `io.h`
  keeps `{}` off the legacy bool overload; `owner_braced_refused` proves it.
- **One owner-lock implementation.** A backend overrides `lock_owner`; its legacy
  `acquire_owner(bool)` and `acquire_directory_owner()` forward to it, never the reverse,
  and a mode it cannot exclude in throws `not_supported` rather than returning a lock that
  excludes nothing.
- **Close is never retried.** A failed close leaves the descriptor number free for
  reuse; retrying could close someone else's file.
- **Settle cursor and file originals before closing them.** Retaining a control
  does not serialize different queues.
- **Allocation-counting tests cannot run under TSan**, whose runtime replaces the
  global `operator new`; those assertions are compiled out there.
