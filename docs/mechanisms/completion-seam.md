> Status: CURRENT (2026-11-16) the seam the coroutine drivers run through: `SyncData`, `completion_ref`, `InlineCompletion`, `HeldCompletion`, and the Asio driver's shared slot.

# The completion seam

A coroutine driver (journal's turn driver, consensus's driver) issues its IO through one handle and does not know which engine runs it: the loop's Asio queue in production, the caller's own thread in a blocking entry, a test or simulator that holds each primitive. `completion_ref` is that handle. The three engines behind it run the same primitives and the same operations.

```
task awaits   completion_ref::run(operation, token)
                 |
                 +-- AsioCompletionDriver   native queue, one slot, Turn lane ahead of Bulk   (asio_completion.h)
                 +-- InlineCompletion       execute_primitive on the caller's thread          (seam.h, drive_synchronously)
                 +-- HeldCompletion         held until the owner executes, fails or drops it  (seam.h)
```

## `SyncData`

`PrimitiveKind::SyncData` (ordinal 25, appended) runs `File::sync_data()`: the file's data and the metadata needed to read it back, not the timestamps. A preallocated turn log, where no write changes the size, needs nothing more per commit. `PosixFile::sync_data` is `fdatasync` on Linux and FreeBSD and `F_FULLFSYNC` on macOS, whose `fdatasync` leaves the data in the drive's cache; any other `File` runs its own `sync_data`, which defaults to `sync`.

| Backend | Where it runs |
|---|---|
| `drive_synchronously`, `InlineCompletion`, `HeldCompletion` | `detail::execute_primitive` on the calling thread |
| Linux queue, a `PosixFile` | the ring: `IORING_OP_FSYNC` with `IORING_FSYNC_DATASYNC` |
| FreeBSD queue, a `PosixFile` | `aio_fsync(O_DSYNC)`, notified through kqueue |
| macOS queue; either queue for another `File`, or no ring | the fallback worker, which calls `sync_data` |
| `AsioCompletionDriver` | through whichever queue it was built over; counted in its own latency slot |

The statistics extension arrays grow from 13 to 14 entries (`SyncData` is index 13), so `completion_extension_count` and the driver's per-kind histograms grow with them. The legacy arrays are untouched.

## `completion_ref`

```cpp
class completion_ref {                       // non-owning, like task::scheduler_ref
	template <completion_driver D> static completion_ref of(D&) noexcept;
	task::task<void> run(std::shared_ptr<IOOperation>, task::cancel_token = {}) const;
};
```

A driver is any type whose `run(std::shared_ptr<IOOperation>, task::cancel_token)` returns `task::task<void>`; `AsioCompletionDriver`, `InlineCompletion` and `HeldCompletion` are three. The call drives the operation to `done()`. An IO error is the operation's to handle (its `complete` receives the completion with the error); `run` throws only for misuse (an operation already in flight, a driver on the wrong scheduler) and for `task::cancelled`.

**The token is read before each primitive is issued.** Requested, the primitive is not issued and the call throws `task::cancelled`; the operation is left idle and unfinished. A primitive already issued completes into the operation first, so the operation's buffers are never released under the kernel and a durable step is never torn: a caller blocked on its in-flight primitive is released when that primitive completes. A token requested after the last primitive completed does not fail the call. A caller waiting for the Asio driver's slot is released at once: a `cancel_callback` on the token claims its waiter (the slot's hand-over claims it too; the first claim wins) and posts its resume to the owner, where it leaves the queue unissued and throws `task::cancelled`. The callback may run on any thread; the claim is one atomic, and everything else stays on the owner.

## `InlineCompletion` and `drive_synchronously`

`drive_synchronously(operation, proceed)` is the one loop: ask `proceed()`, mark submitted, run `execute_primitive`, hand the result to `operation.complete`, until `done()`. The one-argument form is `proceed = true` and is every existing caller's. `InlineCompletion::run` has no loop: it calls `drive_synchronously` with `proceed = !token.requested()` and throws `task::cancelled` when that stopped it. Nothing suspends, so the operation is done when the call returns. `test/check_seam.py` (CTest `completion_seam_shape`) greps that `execute_primitive` is reached from `drive_synchronously`, the two queues' workers and `HeldCompletion::execute` only.

## `HeldCompletion`

Every primitive an operation issues is held, in a queue of tickets, until its owner chooses. One thread: the one that runs the scheduler the awaiting tasks run on.

| Call | Effect |
|---|---|
| `held()`, `tickets()`, `request(t)`, `holds(t)`, `executed(t)` | inspect what is held, in issue order |
| `execute(t)` | the primitive runs on its backend now; its completion stays held (applied, not acknowledged) |
| `deliver(t)` | the held completion reaches the operation; the task resumes on its scheduler, never inside the call |
| `complete(t)` | `execute` if not yet, then `deliver` |
| `fail(t, error)` | an error completion is delivered in place of the result; the backend is untouched unless already executed |
| `drop(t)` | the primitive is forgotten, a crash: never delivered, the operation stays in flight |

A ticket names one primitive and stays valid while it is held, so a test releases B's primitive before A's, or stops between `execute` and `deliver` to crash with the bytes written and unacknowledged. A multi-step operation holds one primitive at a time: its next step is held under a new ticket once the previous one is delivered and the scheduler has run. `HeldCompletion` posts the resume to the scheduler the task was running on (`task::current()` at the call; none is refused). A frame destroyed while its primitive is held (a root destroyed at teardown) leaves the queue; a queue destroyed first leaves its frames alone.

## The Asio driver's shared slot

One loop has one native queue and one `AsioCompletionDriver`, and many groups' tasks share them. The queue has one slot (one original in flight), so `run` now admits any number of operations at once and each *primitive* takes the slot in turn: a primitive holds it from submission to reaping, and at the release the next waiter inherits it by a post to the owner, so the slot is never idle while anyone waits. A multi-step operation re-queues between primitives; two overlapping operations alternate. An uncontended `run` takes the slot without suspending and issues exactly what it did before.

Waiting is in two lanes by `IOOperation::lane()`: `IOLane::Turn` (a turn's write and `SyncData`) is admitted ahead of every waiting `IOLane::Bulk` one (the default: artifact chunks, snapshot IO, everything that existed before), first come first served within a lane. Nothing preempts a primitive in flight, so a Turn waits at most one Bulk primitive. Measured: `docs/MEASUREMENTS.md`, "Turn lane".

One Turn source per driver: at most one `IOLane::Turn` operation runs on a driver at a time, as the design names turn logs by (generation, loop) and one loop has one driver. Debug builds assert it when a second Turn operation starts. Strict priority relies on it: between two turns the slot passes to the Bulk head, so Bulk gets at least one primitive per turn; two Turn sources alternating would starve Bulk.

At teardown, a primitive still in flight when the roots are destroyed stays in the native queue; the host drains the queue (polls it until idle) before destroying it, as `~BsdCompletionQueue` terminates on an undrained queue.
