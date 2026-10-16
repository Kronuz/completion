# AGENTS.md — completion

Working notes for anyone changing this repository. [README.md](README.md) is for
users; [ARCHITECTURE.md](ARCHITECTURE.md) explains the design. Header-only, in
`include/completion/`. journal, consensus, strata, and Detent build on it, so a
change here reaches all of them.

## Repo map

```
include/completion/
  io.h                  IO, File, Mapping, OwnerLock, DirectoryCursor: the backend interfaces.
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
test/                   One suite per feature (core, allocation, scan, inspection, reuse, map, ...).
examples/demo.cc        Batched durability across two directories.
docs/                   PLATFORM_FINDINGS.md, MEASUREMENTS.md, completion.d2.
```

## Verify a change

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2 && ctest --test-dir build --output-on-failure
```

Add `-DASIO_INCLUDE_DIR=...` to build the coroutine adapter's test. Run the suites
under ASan and UBSan, and the scan, inspection, reuse, and creation suites under
TSan, before landing. Then build journal against the change: its suites are this
library's compatibility gate.

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

- **Close is never retried.** A failed close leaves the descriptor number free for
  reuse; retrying could close someone else's file.
- **Settle cursor and file originals before closing them.** Retaining a control
  does not serialize different queues.
- **Allocation-counting tests cannot run under TSan**, whose runtime replaces the
  global `operator new`; those assertions are compiled out there.
