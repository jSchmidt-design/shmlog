#include <doctest/doctest.h>

#include "shmlog/LoggerBackend.h"

#include <ostream>  // doctest stringification
#include <array>
#include <string>
#include <string_view>

using namespace shmlog;

namespace {
using MessageBuffer = std::array<char, kMaxMessageBytes>;
}

TEST_CASE("LogFormat_shortMessageFits") {
    MessageBuffer buf{};
    const uint16_t n = detail::FormatToBuffer(buf, "hello {}", 42);
    CHECK(n == 8);
    CHECK(std::string_view(buf.data(), n) == "hello 42");
}

TEST_CASE("LogFormat_exactBoundaryNoTruncation") {
    const std::string message(kTruncateAtBytes, 'a');
    MessageBuffer buf{};
    const uint16_t n = detail::FormatToBuffer(buf, "{}", message);
    CHECK(n == kTruncateAtBytes);
    CHECK(std::string_view(buf.data(), n) == message);
}

TEST_CASE("LogFormat_oneByteOverBoundaryTruncates") {
    // The first length that must lose its tail rather than merely fit.
    const std::string message(kTruncateAtBytes + 1, 'a');
    MessageBuffer buf{};
    const uint16_t n = detail::FormatToBuffer(buf, "{}", message);

    CHECK(n == kMaxMessageBytes);
    // Whole buffer: the kept prefix must survive intact, not just the ellipsis
    // land in the right place.
    CHECK(std::string_view(buf.data(), n) == std::string(kTruncateAtBytes, 'a') + "...");
}

TEST_CASE("LogFormat_truncatesLongMessageWithEllipsis") {
    const std::string message(300, 'x');
    MessageBuffer buf{};
    const uint16_t n = detail::FormatToBuffer(buf, "{}", message);

    CHECK(n == kMaxMessageBytes);
    CHECK(std::string_view(buf.data(), n) == std::string(kTruncateAtBytes, 'x') + "...");
}

TEST_CASE("LogFormat_truncationKeepsTheHeadOfTheFormattedResult") {
    // Truncation happens after formatting, so the arguments are substituted
    // first and only the composed tail is lost.
    MessageBuffer buf{};
    const uint16_t n =
        detail::FormatToBuffer(buf, "id={} {}", 7, std::string(kMaxMessageBytes, 'y'));

    CHECK(n == kMaxMessageBytes);
    CHECK(std::string_view(buf.data(), 5) == "id=7 ");
    CHECK(std::string_view(buf.data() + kTruncateAtBytes, kEllipsisBytes) == "...");
}

TEST_CASE("LogFormat_emptyMessage") {
    MessageBuffer buf{};
    CHECK(detail::FormatToBuffer(buf, "") == 0);
}

TEST_CASE("LogFormat_doesNotWritePastTheBuffer") {
    // FormatToBuffer writes the ellipsis by offset rather than through the
    // format machinery, so the last byte of the array is the one at risk.
    MessageBuffer buf{};
    buf.fill('\xCC');

    const uint16_t n = detail::FormatToBuffer(buf, "{}", std::string(1000, 'a'));

    REQUIRE(n == kMaxMessageBytes);
    CHECK(buf[kMaxMessageBytes - 1] == '.');
}
