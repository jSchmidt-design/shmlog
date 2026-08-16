// The stdout backend and the collector share detail::FormatWallClock and
// detail::ComposeLine, so these pin the exact line shape both of them emit.
//
// Assertions here are whole-string equality rather than substring searches: the
// column order and widths are the library's user-visible contract, and a
// find()-based check passes just as happily when two columns are swapped.
#include <doctest/doctest.h>

#include "shmlog/LoggerBackend.h"

#include <ostream>  // doctest stringification
#include <format>
#include <string>
#include <string_view>
#include <utility>

using namespace shmlog;

namespace {

// 2023-11-14 ..:..:30.123456 UTC.  Every real timezone is a whole number of
// minutes from UTC, so the seconds and microseconds fields render identically
// everywhere - only the date, hour and minute shift.  That makes the tail of the
// string safe to assert on without pinning the test machine's timezone.
inline constexpr uint64_t kSampleUs = 1'700'000'010'123'456ULL;

// "YYYY-MM-DD HH:MM:SS.uuuuuu"
inline constexpr size_t kWallClockLength = 26;
inline constexpr size_t kSecondsOffset   = 17;  // Index of "SS.uuuuuu"
inline constexpr size_t kMicrosOffset    = 20;  // Index of "uuuuuu"

} // namespace

// --- ComposeLine: exact line shape ---

TEST_CASE("StdoutLine_composesExactLine") {
    CHECK(detail::ComposeLine("2024-01-02 03:04:05.678901", "AUDIO", LogLevel::Info, 42, "hello")
          == "[2024-01-02 03:04:05.678901] [AUDIO  ] [INFO ] [Thread    42] hello\n");
}

TEST_CASE("StdoutLine_levelColumnIsPaddedToFive") {
    const std::pair<LogLevel, std::string_view> cases[] = {
        {LogLevel::Trace, "TRACE"},
        {LogLevel::Debug, "DEBUG"},
        {LogLevel::Info,  "INFO "},
        {LogLevel::Warn,  "WARN "},
        {LogLevel::Error, "ERROR"},
    };

    for (const auto& [level, column] : cases) {
        CAPTURE(column);
        CHECK(detail::ComposeLine("T", "S", level, 0, "m")
              == std::format("[T] [S      ] [{}] [Thread     0] m\n", column));
    }
}

TEST_CASE("StdoutLine_overlongColumnsGrowRatherThanTruncate") {
    // {:<7} and {:5} are minimum widths.  A label or thread id wider than its
    // column must still be emitted in full - a truncated source label would make
    // two partitions indistinguishable in the log.
    CHECK(detail::ComposeLine("T", "VERYLONGLABEL", LogLevel::Warn, 1234567, "m")
          == "[T] [VERYLONGLABEL] [WARN ] [Thread 1234567] m\n");
}

TEST_CASE("StdoutLine_emptyMessageStillTerminatesLine") {
    CHECK(detail::ComposeLine("T", "S", LogLevel::Trace, 0, "")
          == "[T] [S      ] [TRACE] [Thread     0] \n");
}

TEST_CASE("StdoutLine_messageIsNotReformatted") {
    // The message arrives already formatted.  Braces inside it are payload, not
    // a format string, and must survive verbatim.
    CHECK(detail::ComposeLine("T", "S", LogLevel::Info, 0, "{} {0} {{literal}}")
          == "[T] [S      ] [INFO ] [Thread     0] {} {0} {{literal}}\n");
}

// --- FormatWallClock: fixed-width timestamp ---

TEST_CASE("StdoutLine_wallClockIsFixedWidth") {
    const std::string ts = detail::FormatWallClock(kSampleUs);

    REQUIRE(ts.size() == kWallClockLength);
    CHECK(ts[4]  == '-');
    CHECK(ts[7]  == '-');
    CHECK(ts[10] == ' ');
    CHECK(ts[13] == ':');
    CHECK(ts[16] == ':');
    CHECK(ts[19] == '.');
    // Timezone-invariant tail: seconds and microseconds.
    CHECK(ts.substr(kSecondsOffset) == "30.123456");
}

TEST_CASE("StdoutLine_includesMicroseconds") {
    CHECK(detail::FormatWallClock(123456u).substr(kMicrosOffset) == "123456");
}

TEST_CASE("StdoutLine_microsecondsAreZeroPadded") {
    // The sub-second field is a fixed six digits, so 42 µs must not render as
    // ".42" and sort out of order against ".123456".
    CHECK(detail::FormatWallClock(42u).substr(kMicrosOffset) == "000042");
}

TEST_CASE("StdoutLine_wholeSecondRendersZeroMicroseconds") {
    CHECK(detail::FormatWallClock(2'000'000u).substr(kMicrosOffset) == "000000");
}
