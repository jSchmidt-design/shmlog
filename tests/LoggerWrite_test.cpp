// Logger::Write is the writer entry point behind the SHMLOG_* macros.  These
// cover the two guarantees its documentation makes - that it never lets an
// exception escape into the caller, and that a write lands in the partition in a
// form the reader seam decodes - plus the enable/disable state machine.
//
// SHMLOG_BACKEND_* is defined by the parent CMakeLists precisely so a test can
// skip what only makes sense for one backend.
#include <doctest/doctest.h>

#include "shmlog/LogCollectorCore.h"
#include "shmlog/Logger.h"
#include "shmlog/ShmMapping.h"

#include <ostream>  // doctest stringification
#include <atomic>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace shmlogtest {

// A type whose formatter throws, standing in for a user-supplied formatter that
// fails at a logging callsite.
struct Boom {};

} // namespace shmlogtest

template <>
struct std::formatter<shmlogtest::Boom> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    std::format_context::iterator format(const shmlogtest::Boom&, std::format_context&) const {
        throw std::runtime_error("formatter threw");
    }
};

using namespace shmlog;

namespace {

std::string UniqueName() {
    static std::atomic<unsigned> counter{0};
    return "shmlog_test_logger_" + std::to_string(::GetCurrentProcessId()) + "_" +
           std::to_string(counter.fetch_add(1));
}

// Logger's state is process-global; make sure a test cannot leave it enabled for
// whichever case runs next in the same binary.
struct LoggerGuard {
    ~LoggerGuard() { Logger::Shutdown(); }
};

} // namespace

// --- Exception safety ---

TEST_CASE("LoggerWrite_throwingFormatterDoesNotPropagate") {
    // A log statement must never take down the code it was meant to observe.
    CHECK_NOTHROW(Logger::Write(LogLevel::Info, "{}", shmlogtest::Boom{}));
}

TEST_CASE("LoggerWrite_survivesAThrowingFormatterWhileEnabled") {
    LoggerGuard guard;
    REQUIRE(Logger::Initialize(UniqueName().c_str(), 0));

    CHECK_NOTHROW(Logger::Write(LogLevel::Error, "before {} after", shmlogtest::Boom{}));
    // The failed entry is dropped, but logging stays usable afterwards.
    CHECK(Logger::IsEnabled());
    CHECK_NOTHROW(SHMLOG_INFO("still working"));
}

TEST_CASE("LoggerWrite_isANoOpBeforeInitialize") {
    CHECK_FALSE(Logger::IsEnabled());
    CHECK_NOTHROW(SHMLOG_INFO("discarded {}", 1));
}

// --- Enable / disable ---

TEST_CASE("LoggerWrite_initializeEnablesAndShutdownDisables") {
    LoggerGuard guard;

    REQUIRE(Logger::Initialize(UniqueName().c_str(), 0));
    CHECK(Logger::IsEnabled());

    Logger::Shutdown();
    CHECK_FALSE(Logger::IsEnabled());
}

TEST_CASE("LoggerWrite_repeatedInitializeIsANoOp") {
    LoggerGuard guard;

    const std::string name = UniqueName();
    REQUIRE(Logger::Initialize(name.c_str(), 0));
    // Documented: a second call while already enabled succeeds without
    // re-creating anything.
    CHECK(Logger::Initialize("a completely different name", 9));
    CHECK(Logger::IsEnabled());
}

TEST_CASE("LoggerWrite_shutdownIsSafeWhenNeverInitialized") {
    CHECK_NOTHROW(Logger::Shutdown());
    CHECK_FALSE(Logger::IsEnabled());
}

// --- Round trip through the wire format ---

#ifdef SHMLOG_BACKEND_shm
TEST_CASE("LoggerWrite_roundTripsThroughTheRingBuffer") {
    LoggerGuard guard;

    const std::string name = UniqueName();
    REQUIRE(Logger::Initialize(name.c_str(), /*sourceId=*/3));

    SHMLOG_ERROR("value={}", 42);

    // Attach the way the collector does and decode the slot the writer claimed.
    ShmMapping reader;
    REQUIRE(reader.open(name, kShmSize, ShmMapping::Mode::ReadOnly));

    const void* base = reader.getPtr();
    REQUIRE(detail::ValidateHeader(*GetRingHeader(base)));

    const auto* hdr = GetRingHeader(base);
    CHECK(hdr->head_index.load() == 1);  // Exactly one slot claimed.

    CopiedEntry entry{};
    REQUIRE(detail::ReadSlot(GetSlots(base)[0], entry, /*sourceId=*/3) == SlotReadResult::Valid);

    CHECK(entry.level == LogLevel::Error);
    CHECK(entry.source == 3);
    CHECK(entry.thread_id != 0);
    CHECK(std::string_view(entry.message, entry.message_size) == "value=42");
}

TEST_CASE("LoggerWrite_truncatedMessageRoundTripsWithEllipsis") {
    LoggerGuard guard;

    const std::string name = UniqueName();
    REQUIRE(Logger::Initialize(name.c_str(), 0));

    SHMLOG_WARN("{}", std::string(1000, 'x'));

    ShmMapping reader;
    REQUIRE(reader.open(name, kShmSize, ShmMapping::Mode::ReadOnly));

    CopiedEntry entry{};
    REQUIRE(detail::ReadSlot(GetSlots(static_cast<const void*>(reader.getPtr()))[0], entry, 0)
            == SlotReadResult::Valid);

    CHECK(entry.message_size == kMaxMessageBytes);
    CHECK(std::string_view(entry.message, entry.message_size)
          == std::string(kTruncateAtBytes, 'x') + "...");
}

TEST_CASE("LoggerWrite_writesAfterShutdownAreDropped") {
    const std::string name = UniqueName();

    // Keep the mapping alive independently so the name survives Shutdown.
    ShmMapping keepAlive;
    {
        LoggerGuard guard;
        REQUIRE(Logger::Initialize(name.c_str(), 0));
        REQUIRE(keepAlive.open(name, kShmSize, ShmMapping::Mode::ReadOnly));
        SHMLOG_INFO("kept");
    }  // Shutdown here.

    SHMLOG_INFO("dropped");

    const auto* hdr = GetRingHeader(static_cast<const void*>(keepAlive.getPtr()));
    CHECK(hdr->head_index.load() == 1);  // Still one - the second write was a no-op.
}
#endif  // SHMLOG_BACKEND_shm
