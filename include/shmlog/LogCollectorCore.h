#pragma once

#include "shmlog/LogContracts.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shmlog {

// Reader-side copy of a log entry - no atomics, can be sorted/merged freely.
struct CopiedEntry {
    uint64_t  timestamp;
    uint32_t  thread_id;
    uint32_t  sequence;
    uint8_t   source;
    LogLevel  level;
    uint16_t  message_size;
    char      message[kMaxMessageBytes];
};

// Descriptor for one SHM partition the reader should consume.
struct PartitionDescriptor {
    std::string shmName;
    std::string label;
    uint8_t     sourceId;
};

struct LogReaderOptions {
    std::string logFile = "shmlog.log";
    size_t      maxSizeBytes = 10 * 1024 * 1024;
    int         maxFiles = 5;
    bool        useConsole = false;
    std::vector<PartitionDescriptor> partitions;

    // Where LogCollectorCore reports setup progress and errors. Empty by default - a library should not write to a console its caller owns.
    std::function<void(std::string_view)> statusSink;
};

enum class SlotReadResult { InProgress, Overwritten, Empty, Valid };

namespace detail {

// Pure seam: read one ring-buffer slot.
//
//   InProgress  - the writer has marked the slot as being modified (odd seq).
//   Overwritten - the sequence changed during the read (torn/corrupted).
//   Empty       - read succeeded but the entry carries no message bytes.
//   Valid       - a complete entry was copied into `out`.
//
// `sourceId` is the partition id assigned by the reader - the on-wire `slot.source` is ignored so the seam can be driven with synthetic data.
// `out.message_size` is clamped to the copied byte count, so it is always a safe length for `out.message` even if the slot claims otherwise.
SlotReadResult ReadSlot(const LogEntry& slot, CopiedEntry& out, uint8_t sourceId);

// What a reader should do with the claim counters it just sampled.
struct ReadPlan {
    uint32_t startIndex;  // Claim counter to resume reading from.
    uint32_t lostCount;   // Entries the writer overwrote before the reader reached them; 0 when none were lost.
};

// Pure seam: decide where to resume reading, given the writer's current
// `head` claim counter and the reader's own `readIndex`.
//
// Both are free-running counters rather than slot indices, so their difference
// is computed with deliberate unsigned wraparound - the arithmetic stays correct
// across the 2^32 boundary. A difference larger than the ring capacity means the
// writer lapped the reader and the oldest unread entries are already gone.
ReadPlan ComputeReadPlan(uint32_t head, uint32_t readIndex) noexcept;

} // namespace detail

// Convert a monotonic µs timestamp to an approximate wall-clock string.
// `wallBase` / `monoBaseUs` are captured once at startup so all entries are
// consistently offset from the same reference.
std::string FormatTimestamp(uint64_t monoUs,
                            std::chrono::system_clock::time_point wallBase,
                            uint64_t monoBaseUs);

// Format a single merged entry as one log line.
std::string FormatEntry(const CopiedEntry& e,
                        std::string_view sourceLabel,
                        std::chrono::system_clock::time_point wallBase,
                        uint64_t monoBaseUs);

// Order one polled batch by (timestamp, source, sequence). This orders entries *within* a batch only - see LogCollectorCore::Run for the cross-batch caveat.
void MergeBatch(std::vector<CopiedEntry>& batch);

// Result of parsing a reader command line.
struct ParsedArgs {
    LogReaderOptions         options;
    std::vector<std::string> errors;          // Empty when the command line is valid
    bool                     helpRequested = false;
};

// Usage text for a reader binary built on LogCollectorCore.
std::string_view UsageText();

// Parse CLI arguments over `defaults`: known flags override, and each `--partition <shmName>[:<label>]` appends to the partition list with the next unused source id. Unknown flags, missing values and unparseable numbers are reported in `errors` rather than thrown or ignored.
ParsedArgs ParseArgs(int argc, char* argv[], LogReaderOptions defaults);

// Rotating log file helper.
class LogFile {
public:
    // `files` is the number of rotated generations to keep - values below 1 mean the base file is truncated in place instead of rotated.
    bool Open(const std::string& path, size_t maxBytes, int files);
    void Write(const std::string& line);
    void Flush();
    void Rotate();

    [[nodiscard]] bool IsOpen() const { return m_stream.is_open(); }
    size_t CurrentSize() const { return m_currentSize; }
    const std::string& BasePath() const { return m_basePath; }

private:
    std::string   m_basePath;
    size_t        m_maxSizeBytes = 10 * 1024 * 1024;
    int           m_maxFiles = 5;
    std::ofstream m_stream;
    size_t        m_currentSize = 0;
};

// Log reader core: polls SHM partitions, merges entries, and writes formatted output to a log file or console.
class LogCollectorCore {
public:
    explicit LogCollectorCore(LogReaderOptions options);
    ~LogCollectorCore();

    LogCollectorCore(const LogCollectorCore&) = delete;
    LogCollectorCore& operator=(const LogCollectorCore&) = delete;

    // Main loop.  Returns 0 on normal shutdown, 1 on fatal setup error.
    int Run(std::atomic<bool>& running);

    // Poll every partition once and return the combined, unordered results.
    // Usable without Run(): partitions are attached lazily on first poll.
    std::vector<CopiedEntry> PollAllOnce();

    // Format a merged batch into lines.  Exposed for tests.
    std::vector<std::string> FormatBatch(
        const std::vector<CopiedEntry>& batch,
        std::chrono::system_clock::time_point wallBase,
        uint64_t monoBaseUs) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shmlog
