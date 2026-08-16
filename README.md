# shmlog

Ultra-low-latency shared-memory logging for Windows. A writer process calls
`Logger::Initialize()` once at startup to create its own shared-memory ring
buffer partition; `SHMLOG_*` / `LOG_*` macros then write into that partition
with zero heap allocation and zero locking. A separate reader process (built
on `LogCollectorCore`) consumes one or more partitions, merges them by
timestamp, and persists to disk or stdout - writer processes never touch the
filesystem.

This library is **identity-free and dependency-free**: it knows nothing
about any particular application, and links no other library. A partition is
identified by an opaque SHM name plus a caller-assigned `uint8_t` source id;
the name/id table for a given family of writer processes lives in the
consuming application, not here.

```cpp
#include <shmlog/Logger.h>

if (!shmlog::Logger::Initialize("MyApp_Log", /*sourceId=*/0)) {
    // Mapping unavailable or built against an incompatible layout;
    // every write macro below is a no-op.
}
LOG_INFO("Processing {} channels", count);
shmlog::Logger::Shutdown();   // optional; disables writes, releases the mapping
```

`Initialize` returns false - leaving logging disabled rather than writing
through a mismatched layout - if the mapping cannot be created or fails
validation. Calling it again while already enabled is a no-op returning true.
`Logger::IsEnabled()` reports the current state.

After `Shutdown()`, every write macro becomes a no-op. This is best-effort, not a barrier - a thread already inside a write may still hold a pointer into the mapping when it is unmapped, so quiesce your logging threads first if that matters.

## Two targets

Almost every consumer only needs to *write* logs. `shmlog::collector` (the
merge/persist/CLI machinery a reader process needs) is a separate target
from `shmlog::logger` (the writer) so that writer apps don't pay for
`<filesystem>`/`<format>`-heavy code they never call:

| Target | Contains | Link this if you... |
|---|---|---|
| `shmlog::logger` | `Logger`, the three backends, `ShmMapping` | ...call `LOG_*` / `SHMLOG_*` macros anywhere in your app (the common case). |
| `shmlog::collector` | `LogCollectorCore` (`PUBLIC`-links `shmlog::logger`) | ...are building a reader/exporter process that drains one or more partitions to a file or console. |

## Consuming this library

Via CMake `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(shmlog
    GIT_REPOSITORY https://github.com/jSchmidt-design/shmlog.git
    GIT_TAG        v1.0.0)   # pin a tag - `main` moves under you
FetchContent_MakeAvailable(shmlog)

target_link_libraries(my_app    PRIVATE shmlog::logger)     # writer
target_link_libraries(my_reader PRIVATE shmlog::collector)  # reader/exporter
```

For local co-development against a checked-out copy of this repo instead of
fetching from git, most consumers offer a local-directory escape hatch
(e.g. `-DSHMLOG_LOCAL_DIR=/path/to/shmlog`) that does
`add_subdirectory(${...})` instead.

## Layout

| Path | Role |
|---|---|
| `include/shmlog/LogContracts.h` | Wire format only - `LogEntry`, `LogRingBufferHeader`, `LogLevel`, `LevelName`, capacity/magic/version constants, navigation helpers. No macros, no app identity. |
| `include/shmlog/Logger.h` | `Logger` class (public API) + `SHMLOG_*` macros and `LOG_*` compatibility aliases. |
| `include/shmlog/LoggerBackend.h` | Internal pure seams shared by all backends: `FormatToBuffer`, `FormatWallClock`, `ComposeLine`, `WriteSlot`, `ValidateHeader`. |
| `include/shmlog/ShmMapping.h` | Vendored Windows named-shared-memory helper (create/open/close). |
| `include/shmlog/LogCollectorCore.h` | Reader-side core: partition polling, seqlock read, merge, formatting, rotating file output. Also declares the pure SHM-decoding seams (`detail::ReadSlot`, `SlotReadResult`, `detail::ComputeReadPlan`). |
| `src/Logger.cpp` | Backend-independent implementation of the pure seams. Always compiled into `shmlog::logger`. |
| `src/Logger_Shm.cpp` | **shm** backend - SHM creation, header validation, slot claiming, seqlock write. |
| `src/Logger_Stdout.cpp` | **stdout** backend - `system_clock` timestamp, one `fwrite` per line. |
| `src/Logger_Null.cpp` | **null** backend - discards every entry without formatting it. |
| `src/ShmMapping.cpp` | Windows named-shared-memory mapping implementation. |
| `src/LogCollectorCore.cpp` | `shmlog::collector`'s only source: partition polling/merge/format/rotation, and the `ReadSlot` seam. |
| `CMakeLists.txt` | Builds `shmlog::logger` and `shmlog::collector`. Neither links any third-party library. |
| `tests/` | doctest suite over the pure seams and the two OS-resource helpers. Configurable standalone; see [Tests](#tests). |

## Backend selection

```
cmake -B build                          # shm (default)
cmake -B build_stdout -DSHMLOG_BACKEND=stdout
cmake -B build_null   -DSHMLOG_BACKEND=null
```

Exactly one `Logger_*.cpp` is compiled per configuration, into
`shmlog::logger`. The chosen backend is published to consumers as a
`PUBLIC` compile definition - `SHMLOG_BACKEND_shm`, `SHMLOG_BACKEND_stdout`, or `SHMLOG_BACKEND_null`.

`ShmMapping.cpp` is compiled unconditionally into `shmlog::logger`,
including in stdout/null builds: `shmlog::collector` maps partitions
through it regardless of which backend the writers were built with.

## Shared memory layout

One partition per writer process: a 64-byte header followed by 4000 × 320-byte slots, ≈ **1.25 MB** total. Writers claim a slot with an atomic counter and publish it with a per-slot odd/even seqlock - no buffer-level lock, no allocation. The ring overwrites its oldest entries, so a slow reader loses data
rather than stalling the writer.

The header carries a layout stamp (magic, format version, entry size,
capacity). **Both sides validate it on attach**: `Initialize` refuses to write
through a mismatched layout, leaving logging disabled, and the collector skips
a mismatched partition rather than decoding it through the wrong struct.
**Bump `kFormatVersion` whenever `LogEntry` or `LogRingBufferHeader` changes** - that constant is the compatibility contract between
independently-built writer and reader processes.

`spec/format.md` is the authoritative wire-format spec: struct definitions and
field semantics, header publication, the seqlock write and read protocols, lap
detection, and version history. The definitions themselves live in
`include/shmlog/LogContracts.h`.

## Log levels & compile-time filtering

Disabled macros expand to `(void)0` at the preprocessor stage - arguments are **never evaluated**, guaranteed zero cost even in debug builds. (For an
*enabled* macro the arguments are always evaluated, including under the null
backend, which then discards them without formatting.)

| Macro | Alias | Default: Release | Default: Debug |
|---|---|---|---|
| `SHMLOG_TRACE` | `LOG_TRACE` | compiled out | compiled out |
| `SHMLOG_DEBUG` | `LOG_DEBUG` | compiled out | active |
| `SHMLOG_INFO`  | `LOG_INFO`  | active | active |
| `SHMLOG_WARN`  | `LOG_WARN`  | active | active |
| `SHMLOG_ERROR` | `LOG_ERROR` | active | active |

`SHMLOG_*` are the canonical names. The short `LOG_*` / `LOG_LEVEL_*`
aliases are on by default and can be suppressed with
`-DSHMLOG_SHORT_MACROS=0` for consumers whose own headers collide (`LOG_ERROR`
is a common name in Windows-adjacent code).

Override the threshold at compile time: `-DSHMLOG_LEVEL=SHMLOG_LEVEL_TRACE`.

The macros pass the format string through `__VA_ARGS__` rather than naming
it separately, avoiding `__VA_OPT__` and the `/Zc:preprocessor` flag MSVC
would otherwise require.

## Collector (reader)

`LogCollectorCore` sits on top of a caller-supplied
`{shmName, sourceId, label}` descriptor array: attach/detach partitions,
header validation, seqlock read with torn-read discard, lap detection,
timestamp-ordered merge, monotonic→wall-clock baseline conversion, entry
formatting, rotating file output.

Three behaviours worth knowing:

- **Ordering is per-batch** - Each poll sorts only what the partitions held at poll time. Entries that land in a partition after it was polled appear in the next batch, so output is timestamp-ordered *within* a batch but not strictly across batch boundaries.
- **Zero-length messages are dropped** - A never-written slot and a deliberate `SHMLOG_INFO("")` are indistinguishable on the wire; both read as `Empty`.
- **Attaching to a running writer reports a backlog as dropped** - A reader starts
  its read counter at 0, so if the writer has already claimed more than
  `kRingCapacity` slots the first poll emits `[N messages dropped]` for the
  entries that scrolled out of the ring before the reader existed. The count is
  accurate - those entries really are unrecoverable - but it reflects when the
  reader started, not that it fell behind.

`LogCollectorCore` writes nothing to the console on its own. Setup progress
and errors go to `LogReaderOptions::statusSink` if the caller supplies one.

Partitions are configured with repeatable flags
(`--partition <shmName>[:<label>]`) rather than a config file, since the
library has no JSON/YAML dependency. `ParseArgs` returns a `ParsedArgs`
carrying the options, a `helpRequested` flag, and an `errors` list; unknown
flags, missing values and unparseable numbers are reported, not thrown or
ignored.

## Output line format

The collector and the stdout backend share `detail::ComposeLine`, so both emit
the same shape:

```
[2024-01-02 03:04:05.678901] [AUDIO  ] [INFO ] [Thread    42] hello
 └ local time, µs precision  └ source  └ level └ OS thread id  └ message
```

The source column is a partition label on the reader side and the numeric
source id on the writer side, since a writer has no label table. Columns are
minimum widths, not hard ones: a label longer than 7 characters widens that
column for its own lines rather than being truncated.

## Key design constraints

- **Timestamps**: `std::chrono::steady_clock` (QPC on Windows) for the shm
  backend. All processes on the same machine share the QPC epoch - cross-process ordering is valid without extra sync. The stdout backend
  uses `system_clock` instead, since it has no reader to align with.
- **Thread ID**: cached via `thread_local` - never call `GetCurrentThreadId()` on each log write.
- **Formatting**: `std::format_to_n` writes directly into a stack-allocated buffer - no heap allocation on the write path. `Logger::Write` is `noexcept` - a throwing user-defined formatter drops the entry rather than propagating into the caller.
- **Line format**: the stdout backend and the collector share
  `detail::FormatWallClock` and `detail::ComposeLine`, so a line means the same
  thing whichever produced it. A writer has no label table, so it prints the
  numeric source id where the collector prints a partition label.
- **Truncation**: messages longer than 237 bytes are truncated to 237 bytes and
  `"..."` appended (total 240). Truncation happens above the backend seam, so
  all three backends see identical `(level, msg, size)`.
- **Overflow policy**: ring buffer overwrites oldest entries
  (flight-recorder). The collector detects the loss and emits a
  `[N messages dropped]` marker - see `spec/format.md`.
- **Crash consistency**: a writer that dies mid-entry leaves the slot marked
  in-progress, and readers skip it. No separate validity flag needed.
- **Not thread-safe to initialize**: `Initialize()`/`Shutdown()` have no
  internal synchronization beyond the atomic ready flag; call `Initialize`
  before other threads start.

## Platform

Windows only (named file mappings via `CreateFileMappingA`/
`OpenFileMappingA`). `LogContracts.h` and `LoggerBackend.h` include only
standard headers; `<windows.h>` appears only via `ShmMapping.h` and the
backends that need `GetCurrentThreadId`.

Requires a toolchain with reasonably complete C++20 support - MSVC from
Visual Studio 2022 17.x or newer - since the write path relies on
`std::format_to_n` and `std::format_string`.

### Who can see a partition

`Logger::Initialize` passes the name you give it to `CreateFileMappingA`
verbatim, with default security attributes. Two consequences decide whether a
reader can attach at all:

- **Unqualified names are per-session.** `"MyApp_Log"` resolves inside the
  caller's session namespace, so a writer running as a service (session 0) is
  invisible to a desktop reader (session 1). Prefix *both* sides with
  `Global\` to place the partition in the global namespace - note that
  creating a `Global\` object requires `SeCreateGlobalPrivilege`, which
  services hold by default and ordinary desktop processes do not.
- **The default DACL admits the creating user.** A reader running as a
  different account cannot open the mapping. If you need that, create the
  partition yourself with an explicit `SECURITY_ATTRIBUTES` - shmlog does not
  expose a hook for it.

A failed attach is silent by design - the partition is simply retried on every
poll, since that same path is what lets a reader start before its writer. The
cost is that a permanently unreachable partition and a writer that just has
nothing to say look identical in the output. If a partition never produces a
line, check the namespace and the account before looking at the logging code.

## Dependencies

**None.** Neither target links a third-party library, deliberately -
getting a logger should never pull in a JSON or crypto dependency.

## Tests

doctest, fetched at configure time (`v2.4.11`) - the only third-party code
in the repository, and it never reaches a consumer's build.

```
cmake -B build
cmake --build build
ctest --test-dir build -C Debug
```

Tests are on for a top-level build and off under `add_subdirectory()`;
force either way with `-DSHMLOG_BUILD_TESTING=ON|OFF`. `SHMLOG_WARNINGS`
follows the same rule and adds `/W4 /permissive-` to shmlog's own targets -
`PRIVATE`, and off when embedded, so a consumer's warning policy stays theirs.
The `tests/` directory also configures standalone:

```
cmake -S tests -B build-tests && ctest --test-dir build-tests -C Debug
```

Every case is deterministic and single-threaded. Cases needing an OS
resource (`LogFile`, `ShmMapping`, the writer round-trip) use a
process-unique name and release it on scope exit.

`SlotReadResult::Overwritten` is left uncovered - observing it needs a
writer racing the reader between `ReadSlot`'s two sequence loads. The
arithmetic acting on it is covered via `detail::ComputeReadPlan`.

## Manual smoke checklist (pre-release)

- Cross-process round-trip: two or more writers plus a reader against
  distinct partitions; merged output timestamp-ordered, no drops under
  normal load. A version mismatch must be rejected by `ValidateHeader` on
  **both** sides.
- Multithreaded stdout: concurrent threads under the `stdout` backend must
  never interleave partial lines.
