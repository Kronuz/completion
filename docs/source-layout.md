# Source layout

Where the code lives, for the file that was split from one large header. `docs/SPLITTING.md` in
cpp-libraries is the rule: a piece is a unit someone opens alone, and a file over 1,000 lines
stays whole only when it is one coherent unit. No hand-written file here is over 1,000 lines.

## posix.h and posix_*.h

`posix.h` keeps its path and includes its pieces, so no user changes an include. It holds
`PosixIOOptions`, `PosixIO` and `make_posix_io`; `PosixIO` is one class whose members only make
sense together, so it stays whole in `posix.h`. The pieces sit next to it with its name as prefix,
as the other headers here do (`bsd_completion.h`, `linux_completion.h`). Each includes the one it
builds on and compiles alone.

| File | Holds |
|---|---|
| `posix.h` | `PosixIOOptions`, `PosixIO` (owner locks, opens, scans, renames, the sync entry points and their test seams) and `make_posix_io` |
| `posix_support.h` | the `detail` helpers every piece shares: `ManagedPosixControl`, `next_posix_instance`, `system_failure`, `validate_name` and `PosixName`, `validate_file`, `inspect_status` |
| `posix_sync.h` | `FlushMode`, `SyncSite`, `SyncCall` and `sync_call`, the one table from a flush mode and a sync site to the call; the sync system calls a test replaces (`SyncCalls`), `file_sync`, `SyncMode`, `files_sync_devices`, `files_sync` |
| `posix_volume.h` | the filesystem class (`linux_filesystem`, `mount_filesystem`, `classify_filesystem`, the `statfs` platform macros) and the barrier warning's types and macOS storage probe (`BarrierStorage`, `BarrierWarning`, `BarrierWarningCall`, `AppleRegistry`, `barrier_storage`, `barrier_volumes`) |
| `posix_file.h` | `PosixCursor` and `PosixFile` with its mappings, and their helpers `checked_offset`, `extended_acl`, `reservation_unsupported` |

Include order: `posix_support.h` ← `posix_sync.h` ← `posix_file.h`; `posix_volume.h` needs only
`io.h`. `posix.h` includes all four.
