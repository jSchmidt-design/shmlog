#include <doctest/doctest.h>

#include "shmlog/LogContracts.h"
#include "shmlog/LogCollectorCore.h"

#include <ostream>  // doctest stringification
#include <array>
#include <cstring>
#include <string_view>

using namespace shmlog;

namespace {

LogEntry& MakeSlot(std::array<std::byte, sizeof(LogEntry)>& storage) {
    return *new (storage.data()) LogEntry{};
}

} // namespace

TEST_CASE("ReadSlot_validEntryCopiesAllFields") {
    std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);
    slot.sequence.store(2, std::memory_order_relaxed);
    slot.timestamp = 123;
    slot.thread_id = 456;
    slot.level = static_cast<uint8_t>(LogLevel::Info);
    slot.message_size = 5;
    std::memcpy(slot.message, "hello", 5);

    CopiedEntry e{};
    const auto res = detail::ReadSlot(slot, e, 7);

    CHECK(res == SlotReadResult::Valid);
    CHECK(e.timestamp == 123);
    CHECK(e.thread_id == 456);
    CHECK(e.source == 7);
    CHECK(e.level == LogLevel::Info);
    CHECK(e.message_size == 5);
    CHECK(std::string_view(e.message, e.message_size) == "hello");
    CHECK(e.sequence == 2);
}

TEST_CASE("ReadSlot_inProgressOddSequence") {
    std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);
    slot.sequence.store(1, std::memory_order_relaxed);

    CopiedEntry e{};
    const auto res = detail::ReadSlot(slot, e, 0);

    CHECK(res == SlotReadResult::InProgress);
}

TEST_CASE("ReadSlot_emptyMessage") {
    std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);
    slot.sequence.store(4, std::memory_order_relaxed);

    CopiedEntry e{};
    const auto res = detail::ReadSlot(slot, e, 0);

    CHECK(res == SlotReadResult::Empty);
}

TEST_CASE("ReadSlot_clampsOversizedMessageSize") {
    // A slot from a corrupt or foreign mapping can claim a length larger than
    // the message array.  Consumers build a string_view of message_size bytes
    // over CopiedEntry::message, so an unclamped length reads out of bounds.
    std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);
    slot.sequence.store(2, std::memory_order_relaxed);
    slot.message_size = 60000;

    CopiedEntry e{};
    const auto res = detail::ReadSlot(slot, e, 0);

    CHECK(res == SlotReadResult::Valid);
    CHECK(e.message_size == kMaxMessageBytes);
}

TEST_CASE("ReadSlot_reportsSequenceAsObserved") {
    // out.sequence carries the slot's sequence at the moment of the read, which
    // is what MergeBatch uses as its final tiebreak and what a drop marker is
    // stamped with.  It must be the observed value, not a recomputed one.
    std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);
    slot.sequence.store(4242, std::memory_order_relaxed);
    slot.message_size = 1;
    slot.message[0] = 'x';

    CopiedEntry e{};
    REQUIRE(detail::ReadSlot(slot, e, 0) == SlotReadResult::Valid);
    CHECK(e.sequence == 4242);
}

TEST_CASE("ReadSlot_ignoresTheOnWireSourceField") {
    // The reader assigns source ids from its own partition table; whatever the
    // writer stamped is deliberately discarded.
    std::array<std::byte, sizeof(LogEntry)> storage{};
    auto& slot = MakeSlot(storage);
    slot.sequence.store(2, std::memory_order_relaxed);
    slot.source = 200;
    slot.message_size = 1;
    slot.message[0] = 'x';

    CopiedEntry e{};
    REQUIRE(detail::ReadSlot(slot, e, 9) == SlotReadResult::Valid);
    CHECK(e.source == 9);
}

// SlotReadResult::Overwritten is deliberately not covered here.  Detecting it
// requires the sequence to change between ReadSlot's two loads, which cannot be
// arranged without a concurrent writer - and a timing-dependent test is worse
// than an honest gap.  The read-side arithmetic that acts on the result is
// covered by ReadPlan_test.cpp instead.
