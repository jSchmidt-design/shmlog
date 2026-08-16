#pragma once

#include "shmlog/LogContracts.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace shmlog::detail {

// Format a message into `buf`. Returns the number of bytes used (≤ kMaxMessageBytes). Text longer than kTruncateAtBytes is cut there and the final three bytes are replaced with "...".
template<typename... Args>
uint16_t FormatToBuffer(std::array<char, kMaxMessageBytes>& buf,
                        std::format_string<Args...> fmt,
                        Args&&... args)
{
    const auto result =
        std::format_to_n(buf.data(), kTruncateAtBytes, fmt, std::forward<Args>(args)...);

    if (std::cmp_greater(result.size, kTruncateAtBytes)) {
        std::memcpy(buf.data() + kTruncateAtBytes, "...", kEllipsisBytes);
        return static_cast<uint16_t>(kMaxMessageBytes);
    }
    return static_cast<uint16_t>(result.size);
}

// Render a wall-clock microsecond timestamp as "YYYY-MM-DD HH:MM:SS.uuuuuu" in local time. The single implementation shared by the stdout backend and the collector, so their output cannot drift.
std::string FormatWallClock(uint64_t timestampUs);

// Compose one output line. Shared by the stdout backend and the collector so that a line means the same thing whichever produced it. `sourceLabel` is a partition label on the reader side and the numeric source id on the writer side, since a writer has no label table.
std::string ComposeLine(std::string_view timestamp,
                        std::string_view sourceLabel,
                        LogLevel level,
                        uint32_t threadId,
                        std::string_view message);

// Write one entry into a ring-buffer slot using the seqlock protocol. `prevSeq` is the slot's current sequence number - an odd value (a writer that died mid-write) is rounded up here, so callers pass it through unmodified.
void WriteSlot(LogEntry& slot,
               uint32_t prevSeq,
               uint64_t timestamp,
               uint32_t threadId,
               uint8_t source,
               LogLevel level,
               std::string_view message) noexcept;

// True if a partition header carries the magic, version, entry size and capacity this build expects. A false result means the mapping was written by an incompatible build and must not be decoded - see spec/format.md.
bool ValidateHeader(const LogRingBufferHeader& hdr) noexcept;

} // namespace shmlog::detail
