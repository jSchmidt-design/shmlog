# shmlog wire format

The on-wire contract between a writer process and a reader process sharing one
shared-memory partition. Everything here is defined by
`include/shmlog/LogContracts.h`; this document explains what the fields mean
and how the two sides must use them.

Scope: the shared-memory format and its access protocols only. Library API,
macros, build options and collector usage are out of scope.

## Partition

One partition per writer process: a Windows named file mapping (`CreateFileMappingA` / `OpenFileMappingA`) holding a fixed-size header followed by a fixed-size array of slots.

```
offset 0     [ LogRingBufferHeader   64 bytes ]
offset 64    [ LogEntry slot 0      320 bytes ]
offset 384   [ LogEntry slot 1      320 bytes ]
             ...
             [ LogEntry slot N-1    320 bytes ]   // N = kRingCapacity = 4000
```

Total size is `kShmSize = sizeof(LogRingBufferHeader) + kRingCapacity *
sizeof(LogEntry)` = 64 + 4000 × 320 ≈ **1.25 MB**. Both structs are standard
layout and are placed directly over the raw mapping by `GetRingHeader()` /
`GetSlots()`.

Windows zero-fills the pages of a newly created mapping, so every slot starts
with `sequence == 0` - even, meaning "valid and empty" - with no explicit initialisation pass.

### LogRingBufferHeader (64 bytes, cache-line aligned)

```cpp
struct alignas(64) LogRingBufferHeader {
    std::atomic<uint32_t> head_index;     // free-running claim counter
    uint32_t              capacity;       // kRingCapacity = 4000
    std::atomic<uint16_t> magic;          // kShmMagic = 0x5348 ('SH'), released last
    uint16_t              format_version; // kFormatVersion = 1
    uint16_t              entry_size;     // sizeof(LogEntry) = 320
    char                  reserved[50];
};
```

`head_index` is a **free-running counter, not a slot index**: writers
`fetch_add` it and take the result modulo `capacity`. It is allowed to wrap
around `2^32`; readers must compare it against their own read counter with
unsigned arithmetic (see [Lap detection](#lap-detection)). A writer attaching
to an existing partition must leave `head_index` alone - resetting it would corrupt the read position of an already-connected reader.

`reserved` is zero on a freshly created mapping and must be ignored by readers - it is the space future format versions grow into.

### LogEntry (320 bytes = 5 × 64-byte cache lines)

```cpp
struct alignas(64) LogEntry {
    std::atomic<uint32_t> sequence;  // odd = write in progress, even = valid
    uint64_t timestamp;              // µs since the steady_clock (QPC) epoch
    uint32_t thread_id;              // writer's OS thread id
    uint8_t  source;                 // writer-assigned source id
    uint8_t  level;                  // LogLevel value, 0=Trace … 4=Error
    uint16_t message_size;           // bytes used in message[] (≤ 240)
    char     message[240];           // message text, not null-terminated
};
```

The natural layout is 264 bytes; `alignas(64)` rounds `sizeof` up to 320 so a
slot never shares a cache line with its neighbours.

- **`timestamp`** is microseconds since the `steady_clock` epoch, which on Windows is QPC. Every process on the machine shares that epoch, so entries from different partitions are directly comparable without any additional synchronisation. It is *not* a wall-clock time - converting to wall clock is the reader's job and is inherently approximate.
- **`source`** is assigned by the writer at initialisation. A reader that already knows which partition it is draining may substitute its own id - `detail::ReadSlot` does exactly that and ignores the on-wire value.
- **`message`** carries no terminator. Text longer than 237 bytes is cut at 237 with `"..."` appended, giving `message_size == 240`. The cut is by **byte count, not by code point**: a message truncated in the middle of a multi-byte UTF-8 sequence leaves that sequence incomplete, and a consumer decoding the field as UTF-8 must tolerate a trailing partial character. The format stores bytes and takes no position on their encoding.
- **`message_size` is untrusted.** See [Reading a slot](#reading-a-slot).

Both structs `static_assert` that `std::atomic<uint32_t>` and
`std::atomic<uint16_t>` are always lock-free. A non-lock-free atomic would
fall back to a process-local mutex, which synchronises nothing across process
boundaries and would break both the seqlock and header publication silently
rather than loudly.

## Header publication

`magic` doubles as the publication flag for the header, which is why it is
atomic.

- A **creator** writes `head_index`, `capacity`, `format_version` and
  `entry_size` first, then release-stores `magic`.
- An **attacher** acquire-loads `magic` first, and only reads the plain fields
  once it is non-zero.

Without that ordering, a process attaching while the creator was still stamping could observe a partly-written header and reject a perfectly good partition.

Three states follow from this:

| `magic` | Meaning | Attacher's response |
|---|---|---|
| `0` | Mapping exists, not yet stamped | Wait/retry - not an error |
| `0x5348` | Published | Validate the remaining fields |
| anything else | Foreign or corrupt | Reject immediately |

A writer waits briefly (50 ms) for publication before giving up; that path is
only reached when two processes race to create the same name. A reader simply
retries on its next poll.

## Compatibility rule

A writer and a reader are compatible only if they agree on `magic`,
`format_version`, `entry_size` and `capacity`. All four are checked by
`detail::ValidateHeader()`, and **both sides validate**:

- A **writer** stamps these fields when it creates a *new* mapping; when it
  attaches to an *existing* one it validates them instead and refuses to
  attach on any mismatch, leaving logging disabled.
- A **reader** validates on attach, skips the partition on mismatch, and
  retries on each poll - which also covers arriving mid-creation.

This check is load-bearing because `CreateFileMappingA` on an existing kernel
object name **does not fail if the requested size differs** from the mapping's
actual size; it silently returns a handle to the original mapping. Without
validation, a process built against a different header layout - a stale writer, a stale reader, or two writers sharing a partition name - would read or write through a mismatched struct with no error of any kind.

**Whenever `LogEntry` or `LogRingBufferHeader` changes, bump `kFormatVersion`**
in `LogContracts.h`. That constant is the entire compatibility contract
between independently built processes.

## Write protocol (lock-free seqlock)

Many threads in the writer process share one partition. Safety comes from
atomic slot claiming plus per-slot odd/even sequencing; there is no
buffer-level lock and no allocation.

```
1. idx = head_index.fetch_add(1, relaxed) % capacity   // claim a slot
2. prevSeq = slot.sequence.load(relaxed)
   if (prevSeq & 1) ++prevSeq                          // round up after a dead writer
3. slot.sequence.store(prevSeq + 1, release)           // mark in progress (odd)
4. write timestamp, thread_id, source, level, message_size, message
5. slot.sequence.store(prevSeq + 2, release)           // mark valid (even)
```

Step 1 is relaxed: ordering for the payload comes entirely from the per-slot
sequence stores. The release in step 3 keeps the payload writes from being
hoisted above it; the release in step 5 publishes them to any reader that
acquire-loads the sequence.

The odd-value rounding in step 2 is what makes the format **crash-consistent**:
a writer that died between steps 3 and 5 leaves an odd sequence behind, which
readers skip, and the next writer to claim that slot rounds up so its own pair
is again odd → even. No separate validity flag is needed.

Sequence numbers are per slot and increase by 2 per write. They are also the
generation counter a reader uses to notice it was lapped.

## Reading a slot

```
1. seq1 = slot.sequence.load(acquire)
   if (seq1 & 1) → InProgress   (a writer holds the slot; retry later)
2. copy timestamp, thread_id, level, message_size, message
   clamping message_size to sizeof(message)
3. seq2 = slot.sequence.load(acquire)
   if (seq1 != seq2) → Overwritten   (torn read; discard the copy)
4. message_size == 0 → Empty; otherwise → Valid
```

Two consequences worth stating explicitly:

- **`message_size` must be clamped before it is published to a consumer.** A
  slot from a foreign or corrupt mapping, or a torn read that happens to pass
  the sequence check, can present a length larger than the array. Consumers
  build a `string_view` of `message_size` bytes over `message`, so an unclamped
  length reads out of bounds. `detail::ReadSlot` clamps to the number of bytes
  it actually copied, so the value it hands back is always safe.
- **A zero-length message is indistinguishable from a never-written slot.**
  Both read as `Empty`. A deliberate empty log message therefore cannot survive
  the round trip.

An `InProgress` slot should stop the current drain rather than be skipped over,
otherwise later entries are emitted ahead of it.

## Lap detection

The ring overwrites its oldest entries - it is a flight recorder, and a slow reader loses data rather than blocking the writer. A reader keeps its own
free-running `readIndex` in the same counter space as `head_index` and detects
loss by distance, using unsigned wraparound:

```
gap = head_index - readIndex          // unsigned, wraps with the counters
if (gap > capacity):
    lost      = gap - capacity        // entries overwritten before being read
    readIndex = head_index - capacity // resynchronise to the oldest live slot
```

Per-slot laps are caught separately: if a slot is rewritten during the copy,
step 3 above reports `Overwritten` and that single entry is lost.

A reader that attaches to a partition starts its `readIndex` at 0, so the rule
above also covers the backlog: if the writer has already claimed more than
`capacity` slots, the first sample reports `head - capacity` entries as lost and
resynchronises to the oldest slot still present. That is the correct count -
those entries have genuinely been overwritten - but it describes when the reader
arrived, not that it failed to keep up. A reader that would rather ignore
history than report it should seed `readIndex` from `head_index` on attach
instead of from 0.

## Format version history

| `kFormatVersion` | Change |
|---|---|
| 1 | Initial versioned format: `magic` (`0x5348`, `'SH'`), `format_version` and `entry_size` claimed from `LogRingBufferHeader`'s padding; validated on attach by both sides. |

`magic` later became `std::atomic<uint16_t>`, which is byte-identical to the
plain `uint16_t` it replaced - same size, same alignment, lock-free on every supported target. The change is invisible on the wire, so the version stays at 1.
