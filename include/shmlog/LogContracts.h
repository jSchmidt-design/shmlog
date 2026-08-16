#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace shmlog {

// --- Log levels ---

// Values must stay in sync with the SHMLOG_LEVEL_* preprocessor constants in
// Logger.h.  They are spelled as literals here so that this header stays free
// of macros - reader-side code includes it for the wire format alone.
enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
};

// Unpadded level name.  Callers pad to a column width; both line formatters use
// `{:<5}`.  Shared from the wire-format header so the writer and reader targets
// cannot drift into printing different names for the same level.
constexpr std::string_view LevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

// --- Ring buffer geometry ---

// 4 000 slots × 320 bytes ≈ 1.25 MB per partition.
inline constexpr uint32_t kRingCapacity = 4000;

// Bytes available for message text in one slot.  Longer messages are cut at
// kTruncateAtBytes and given a trailing "..." (detail::FormatToBuffer).
inline constexpr size_t kMaxMessageBytes = 240;
inline constexpr size_t kEllipsisBytes   = 3;
inline constexpr size_t kTruncateAtBytes = kMaxMessageBytes - kEllipsisBytes;

// --- Log entry ---
// Natural layout (with compiler padding) is 264 bytes; alignas(64) rounds
// sizeof up to 320 (5 × 64-byte cache lines).
//
// Seqlock protocol:
//   writer: sequence → odd (in progress), write fields, sequence → even (valid)
//   reader: read seq; if odd skip; copy fields; re-read seq; if changed, discard

#ifdef _MSC_VER
#  pragma warning(push)
// C4324: padded due to alignment specifier. That padding is the point - it is what rounds LogEntry to a whole number of cache lines. Suppressed here so consumers building at /W4 do not inherit the warning from this header.
#  pragma warning(disable : 4324)
#endif

struct alignas(64) LogEntry {
    std::atomic<uint32_t> sequence;  // Odd = write in progress, even = valid
    uint64_t timestamp;              // µs since steady_clock epoch (cross-process on Windows)
    uint32_t thread_id;              // Cached GetCurrentThreadId()
    uint8_t  source;                 // Caller-assigned source id (see Logger::Initialize)
    uint8_t  level;                  // LogLevel value
    uint16_t message_size;           // Bytes used in `message` (≤ kMaxMessageBytes)
    char     message[kMaxMessageBytes];  // Message text, not null-terminated
};
static_assert(sizeof(LogEntry) == 320, "LogEntry must be 320 bytes (5 cache lines)");
static_assert(alignof(LogEntry) == 64, "LogEntry must be cache-line aligned");
static_assert(std::is_standard_layout_v<LogEntry>,
              "GetSlots() places LogEntry over a raw mapping");

// --- Ring buffer header ---
// First 64 bytes of each shared-memory partition.

inline constexpr uint16_t kShmMagic      = 0x5348;  // 'SH'
inline constexpr uint16_t kFormatVersion = 1;

// `magic` is atomic because it doubles as the publication flag: a creator
// stamps the plain fields first and release-stores `magic` last, so anyone who
// acquire-loads a non-zero magic is guaranteed to see the rest of the header
// (Logger::Initialize / detail::ValidateHeader).
struct alignas(64) LogRingBufferHeader {
    std::atomic<uint32_t> head_index;     // Next slot to claim; wraps mod capacity
    uint32_t              capacity;       // kRingCapacity
    std::atomic<uint16_t> magic;          // kShmMagic; released last
    uint16_t              format_version; // kFormatVersion
    uint16_t              entry_size;     // sizeof(LogEntry)
    char                  reserved[50];
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

static_assert(sizeof(LogRingBufferHeader) == 64, "Header must occupy one cache line");
static_assert(std::is_standard_layout_v<LogRingBufferHeader>,
              "GetRingHeader() places the header over a raw mapping");

// The seqlock and the header publication flag span process boundaries.  A
// non-lock-free atomic would fall back to a process-local mutex, which does not
// synchronise anything between processes and would break both protocols
// silently rather than loudly.
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "cross-process seqlock requires lock-free 32-bit atomics");
static_assert(std::atomic<uint16_t>::is_always_lock_free,
              "cross-process header publication requires lock-free 16-bit atomics");

// Total size of one shared-memory partition.
inline constexpr size_t kShmSize =
    sizeof(LogRingBufferHeader) + sizeof(LogEntry) * kRingCapacity;

// --- Shared memory navigation helpers ---

inline LogRingBufferHeader* GetRingHeader(void* base) noexcept {
    return static_cast<LogRingBufferHeader*>(base);
}
inline const LogRingBufferHeader* GetRingHeader(const void* base) noexcept {
    return static_cast<const LogRingBufferHeader*>(base);
}
inline LogEntry* GetSlots(void* base) noexcept {
    return reinterpret_cast<LogEntry*>(
        static_cast<char*>(base) + sizeof(LogRingBufferHeader));
}
inline const LogEntry* GetSlots(const void* base) noexcept {
    return reinterpret_cast<const LogEntry*>(
        static_cast<const char*>(base) + sizeof(LogRingBufferHeader));
}

} // namespace shmlog
