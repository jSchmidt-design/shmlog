// FormatTimestamp converts a monotonic µs reading into wall-clock text by
// offsetting it from a (wallBase, monoBaseUs) pair captured once at startup.
//
// The offset arithmetic is the whole point of the function, so these assert on
// the rendered seconds field rather than merely checking that a timestamp-shaped
// string came back.  Every real timezone is a whole number of minutes from UTC,
// so the seconds and microseconds fields are timezone-invariant and can be
// pinned exactly without fixing TZ for the test run.
#include <doctest/doctest.h>

#include "shmlog/LogCollectorCore.h"

#include <ostream>  // doctest stringification
#include <chrono>
#include <cstring>
#include <string>

using namespace shmlog;

namespace {

// 2023-11-14 ..:..:30.123456 UTC - deliberately mid-minute, so ±1 s stays inside
// the same minute and only the seconds field moves.
inline constexpr uint64_t kBaseUs = 1'700'000'010'123'456ULL;

inline constexpr size_t kWallClockLength = 26;  // "YYYY-MM-DD HH:MM:SS.uuuuuu"
inline constexpr size_t kSecondsOffset   = 17;  // Index of "SS.uuuuuu"

std::chrono::system_clock::time_point WallBase() {
    return std::chrono::system_clock::time_point{} + std::chrono::microseconds(kBaseUs);
}

} // namespace

TEST_CASE("FormatTimestamp_zeroOffsetRendersTheBaseInstant") {
    const std::string ts = FormatTimestamp(kBaseUs, WallBase(), kBaseUs);

    REQUIRE(ts.size() == kWallClockLength);
    CHECK(ts.substr(kSecondsOffset) == "30.123456");
}

TEST_CASE("FormatTimestamp_oneSecondLaterAdvancesTheSecondsField") {
    const std::string base  = FormatTimestamp(kBaseUs, WallBase(), kBaseUs);
    const std::string later = FormatTimestamp(kBaseUs + 1'000'000, WallBase(), kBaseUs);

    CHECK(later.substr(kSecondsOffset) == "31.123456");
    // Same minute: everything ahead of the seconds field is untouched.
    CHECK(later.substr(0, kSecondsOffset) == base.substr(0, kSecondsOffset));
}

TEST_CASE("FormatTimestamp_subSecondOffsetLandsInMicroseconds") {
    const std::string ts = FormatTimestamp(kBaseUs + 500'000, WallBase(), kBaseUs);
    CHECK(ts.substr(kSecondsOffset) == "30.623456");
}

TEST_CASE("FormatTimestamp_negativeOffsetGoesBackwards") {
    // An entry timestamped before the base is legitimate: the collector captures
    // its base after writers have already been running.
    const std::string ts = FormatTimestamp(kBaseUs - 1'000'000, WallBase(), kBaseUs);
    CHECK(ts.substr(kSecondsOffset) == "29.123456");
}

TEST_CASE("FormatTimestamp_clampsBelowTheEpoch") {
    // A wildly out-of-range monotonic reading would otherwise offset the base to
    // a negative time_t, which localtime_s rejects.  It is clamped to the epoch.
    const std::string ts = FormatTimestamp(0, WallBase() - std::chrono::seconds(1), kBaseUs);

    REQUIRE(ts.size() == kWallClockLength);
    CHECK(ts.substr(kSecondsOffset) == "00.000000");
}

TEST_CASE("FormatTimestamp_entryComposesTimestampWithTheOtherColumns") {
    CopiedEntry e{};
    e.timestamp    = kBaseUs;
    e.thread_id    = 1234;
    e.level        = LogLevel::Info;
    e.message_size = 5;
    std::memcpy(e.message, "hello", 5);

    // The timestamp column is checked exactly by the tests above; here it is
    // taken as given so the rest of the line can be pinned regardless of the
    // machine's timezone.
    const std::string ts = FormatTimestamp(e.timestamp, WallBase(), kBaseUs);

    CHECK(FormatEntry(e, "AUDIO", WallBase(), kBaseUs)
          == "[" + ts + "] [AUDIO  ] [INFO ] [Thread  1234] hello\n");
}

TEST_CASE("FormatTimestamp_entryEmitsOnlyMessageSizeBytes") {
    // CopiedEntry::message is not null-terminated; the line must stop at
    // message_size and not run into the rest of the buffer.
    CopiedEntry e{};
    e.timestamp    = kBaseUs;
    e.level        = LogLevel::Warn;
    e.message_size = 3;
    std::memcpy(e.message, "abcXXXX", 7);

    const std::string line = FormatEntry(e, "S", WallBase(), kBaseUs);

    CHECK(line.substr(line.size() - 6) == "] abc\n");
}
