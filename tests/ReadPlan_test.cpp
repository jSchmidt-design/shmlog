// detail::ComputeReadPlan is the drop-detection arithmetic lifted out of
// PartitionReader::Poll.  It decides where a reader resumes after the writer may
// have lapped it, using free-running 32-bit claim counters whose difference is
// computed with deliberate unsigned wraparound.
#include <doctest/doctest.h>

#include "shmlog/LogCollectorCore.h"
#include "shmlog/LogContracts.h"

#include <ostream>  // doctest stringification
#include <cstdint>

using namespace shmlog;

TEST_CASE("ReadPlan_caughtUpReaderHasNothingToDo") {
    const auto plan = detail::ComputeReadPlan(/*head=*/7, /*readIndex=*/7);
    CHECK(plan.startIndex == 7);
    CHECK(plan.lostCount == 0);
}

TEST_CASE("ReadPlan_readerBehindButWithinCapacityLosesNothing") {
    const auto plan = detail::ComputeReadPlan(/*head=*/10, /*readIndex=*/0);
    CHECK(plan.startIndex == 0);
    CHECK(plan.lostCount == 0);
}

TEST_CASE("ReadPlan_gapOfExactlyCapacityIsStillFullyReadable") {
    // The boundary case: every claimed slot is still present, so nothing is
    // lost and the reader resumes where it was.
    const auto plan = detail::ComputeReadPlan(kRingCapacity, 0);
    CHECK(plan.startIndex == 0);
    CHECK(plan.lostCount == 0);
}

TEST_CASE("ReadPlan_oneSlotPastCapacityDropsExactlyOne") {
    // The first gap that must lose an entry rather than merely fill the ring.
    const auto plan = detail::ComputeReadPlan(kRingCapacity + 1, 0);
    CHECK(plan.lostCount == 1);
    CHECK(plan.startIndex == 1);
}

TEST_CASE("ReadPlan_resumesAtTheOldestSurvivingSlot") {
    const uint32_t head = kRingCapacity * 3;
    const auto plan = detail::ComputeReadPlan(head, 0);

    CHECK(plan.lostCount == kRingCapacity * 2);
    CHECK(plan.startIndex == head - kRingCapacity);
    // The reader is left exactly one full ring behind the writer.
    CHECK(head - plan.startIndex == kRingCapacity);
}

TEST_CASE("ReadPlan_survivesCounterWraparoundWithoutFalseDrops") {
    // head has wrapped past 2^32 while the reader has not.  The unsigned
    // difference is the true distance (21), so nothing should be reported lost.
    const auto plan = detail::ComputeReadPlan(/*head=*/5, /*readIndex=*/0xFFFF'FFF0u);
    CHECK(plan.lostCount == 0);
    CHECK(plan.startIndex == 0xFFFF'FFF0u);
}

TEST_CASE("ReadPlan_detectsDropsAcrossCounterWraparound") {
    // Same wraparound, but the writer is now far enough ahead to have lapped the
    // reader: distance is 4352, of which 4352 - 4000 are gone.
    const uint32_t head      = 4096;
    const uint32_t readIndex = 0xFFFF'FF00u;
    const auto plan = detail::ComputeReadPlan(head, readIndex);

    CHECK(plan.lostCount == 4352 - kRingCapacity);
    CHECK(plan.startIndex == head - kRingCapacity);
}

TEST_CASE("ReadPlan_freshReaderAgainstALongRunningWriter") {
    // A reader attaching to a partition that has been written to for a while
    // starts at readIndex 0 and must not try to replay billions of entries.
    const uint32_t head = 5'000'000u;
    const auto plan = detail::ComputeReadPlan(head, 0);

    CHECK(plan.startIndex == head - kRingCapacity);
    CHECK(plan.lostCount == head - kRingCapacity);
}
