// WriteSlot is the writer half of the seqlock.  Note that it *clamps* an
// oversized message at kMaxMessageBytes with no ellipsis - the truncate-with-"..."
// behaviour belongs to FormatToBuffer one layer up (see LogFormat_test.cpp).
#include <doctest/doctest.h>

#include "shmlog/LogContracts.h"
#include "shmlog/LoggerBackend.h"

#include <ostream>  // doctest stringification
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

using namespace shmlog;

namespace {

// LogEntry holds an atomic, so it is neither copyable nor movable; tests place
// one over aligned storage rather than declaring it by value.
LogEntry& MakeSlot(std::array<std::byte, sizeof(LogEntry)>& storage) {
    return *new (storage.data()) LogEntry{};
}

} // namespace

TEST_CASE("SlotWrite_roundTripsAllFields") {
    alignas(alignof(LogEntry)) std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);

    detail::WriteSlot(slot,
                      10,
                      123456789u,
                      0xDEADBEEFu,
                      7,
                      LogLevel::Warn,
                      "hello");

    CHECK(slot.sequence.load() == 12);
    CHECK(slot.timestamp == 123456789u);
    CHECK(slot.thread_id == 0xDEADBEEFu);
    CHECK(slot.source == 7);
    CHECK(slot.level == static_cast<uint8_t>(LogLevel::Warn));
    CHECK(slot.message_size == 5);
    CHECK(std::string_view(slot.message, slot.message_size) == "hello");
}

TEST_CASE("SlotWrite_leavesSequenceEvenSoReadersAcceptIt") {
    // The seqlock contract: a completed write ends on an even sequence, two
    // above where it started.  An odd value would make every reader skip the
    // slot as a write in progress.
    alignas(alignof(LogEntry)) std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);

    detail::WriteSlot(slot, 0, 1, 2, 3, LogLevel::Info, "x");

    CHECK(slot.sequence.load() == 2);
    CHECK((slot.sequence.load() % 2) == 0);
}

TEST_CASE("SlotWrite_roundsUpOddPrevSeq") {
    // An odd starting value means a previous writer died mid-write.
    alignas(alignof(LogEntry)) std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);

    detail::WriteSlot(slot, 11, 1, 2, 3, LogLevel::Info, "x");

    // 11 is odd -> rounded to 12, so final sequence is 12 + 2 = 14.
    CHECK(slot.sequence.load() == 14);
}

TEST_CASE("SlotWrite_clampsOversizedMessage") {
    alignas(alignof(LogEntry)) std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);

    const std::string longMessage(kMaxMessageBytes + 10, 'z');
    detail::WriteSlot(slot, 0, 0, 0, 0, LogLevel::Trace, longMessage);

    CHECK(slot.message_size == kMaxMessageBytes);
    CHECK(std::string_view(slot.message, slot.message_size) ==
          std::string(kMaxMessageBytes, 'z'));
}

TEST_CASE("SlotWrite_exactCapacityMessageIsNotClamped") {
    alignas(alignof(LogEntry)) std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);

    const std::string exact(kMaxMessageBytes, 'q');
    detail::WriteSlot(slot, 0, 0, 0, 0, LogLevel::Trace, exact);

    CHECK(slot.message_size == kMaxMessageBytes);
    CHECK(std::string_view(slot.message, slot.message_size) == exact);
}

TEST_CASE("SlotWrite_emptyMessageIsRecordedAsZeroLength") {
    alignas(alignof(LogEntry)) std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);

    detail::WriteSlot(slot, 0, 1, 2, 3, LogLevel::Info, "");

    CHECK(slot.message_size == 0);
    CHECK(slot.sequence.load() == 2);
}
