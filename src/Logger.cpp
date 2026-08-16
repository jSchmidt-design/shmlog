#include "shmlog/LoggerBackend.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <format>
#include <string>
#include <string_view>

namespace shmlog::detail {

bool ValidateHeader(const LogRingBufferHeader& hdr) noexcept {
    // Acquire-load magic first: the creator releases it last, so a non-zero
    // magic makes the remaining plain fields safe to read.  Zero means the
    // mapping exists but is still being stamped.
    if (hdr.magic.load(std::memory_order_acquire) != kShmMagic) {
        return false;
    }
    return hdr.format_version == kFormatVersion
        && hdr.entry_size == static_cast<uint16_t>(sizeof(LogEntry))
        && hdr.capacity == kRingCapacity;
}

void WriteSlot(LogEntry& slot,
               uint32_t prevSeq,
               uint64_t timestamp,
               uint32_t threadId,
               uint8_t source,
               LogLevel level,
               std::string_view message) noexcept
{
    // An odd value means a previous writer died mid-write; round up so our own
    // pair is still odd → even.
    if (prevSeq & 1u) {
        ++prevSeq;
    }

    // 1. Mark write-in-progress (odd).  Release keeps the field writes below
    //    from being hoisted above this store.
    slot.sequence.store(prevSeq + 1, std::memory_order_release);

    // 2. Payload.
    const uint16_t msgSize =
        static_cast<uint16_t>(std::min(message.size(), kMaxMessageBytes));
    slot.timestamp    = timestamp;
    slot.thread_id    = threadId;
    slot.source       = source;
    slot.level        = static_cast<uint8_t>(level);
    slot.message_size = msgSize;
    std::memcpy(slot.message, message.data(), msgSize);

    // 3. Mark valid (even).  Release makes the field writes visible to a reader
    //    that acquire-loads this sequence value.
    slot.sequence.store(prevSeq + 2, std::memory_order_release);
}

std::string FormatWallClock(uint64_t timestampUs) {
    const auto secs = static_cast<std::time_t>(timestampUs / 1'000'000);
    const auto micros = static_cast<uint32_t>(timestampUs % 1'000'000);

    std::tm tm{};
    ::localtime_s(&tm, &secs);

    char dateBuf[32];
    const size_t len = std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::format("{}.{:06}", std::string_view(dateBuf, len), micros);
}

std::string ComposeLine(std::string_view timestamp,
                        std::string_view sourceLabel,
                        LogLevel level,
                        uint32_t threadId,
                        std::string_view message)
{
    return std::format("[{}] [{:<7}] [{:<5}] [Thread {:5}] {}\n",
                       timestamp, sourceLabel, LevelName(level), threadId, message);
}

} // namespace shmlog::detail
