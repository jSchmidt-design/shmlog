#include <doctest/doctest.h>

#include "shmlog/LogContracts.h"
#include "shmlog/LoggerBackend.h"

#include <ostream>  // doctest stringification

using namespace shmlog;

namespace {

// Stamp `hdr` the way a compatible writer would.  Filled in place rather than
// returned: LogRingBufferHeader holds atomics and so is neither copyable nor
// movable.  Each test then mutates one field to prove that field is checked.
void StampValid(LogRingBufferHeader& hdr) {
    hdr.magic.store(kShmMagic, std::memory_order_relaxed);
    hdr.format_version = kFormatVersion;
    hdr.entry_size     = static_cast<uint16_t>(sizeof(LogEntry));
    hdr.capacity       = kRingCapacity;
}

} // namespace

TEST_CASE("HeaderCheck_validHeaderPasses") {
    LogRingBufferHeader hdr{};
    StampValid(hdr);
    CHECK(detail::ValidateHeader(hdr));
}

TEST_CASE("HeaderCheck_wrongMagicFails") {
    LogRingBufferHeader hdr{};
    StampValid(hdr);
    hdr.magic.store(0x1234, std::memory_order_relaxed);
    CHECK_FALSE(detail::ValidateHeader(hdr));
}

TEST_CASE("HeaderCheck_unstampedMagicFails") {
    // Zero magic means "mapping exists but the creator has not published the
    // header yet".  Validation must decline it rather than trust the other
    // fields, which are not yet guaranteed visible.
    LogRingBufferHeader hdr{};
    StampValid(hdr);
    hdr.magic.store(0, std::memory_order_relaxed);
    CHECK_FALSE(detail::ValidateHeader(hdr));
}

TEST_CASE("HeaderCheck_wrongVersionFails") {
    LogRingBufferHeader hdr{};
    StampValid(hdr);
    hdr.format_version = 99;
    CHECK_FALSE(detail::ValidateHeader(hdr));
}

TEST_CASE("HeaderCheck_wrongEntrySizeFails") {
    LogRingBufferHeader hdr{};
    StampValid(hdr);
    hdr.entry_size = static_cast<uint16_t>(sizeof(LogEntry) + 1);
    CHECK_FALSE(detail::ValidateHeader(hdr));
}

TEST_CASE("HeaderCheck_wrongCapacityFails") {
    LogRingBufferHeader hdr{};
    StampValid(hdr);
    hdr.capacity = kRingCapacity - 1;
    CHECK_FALSE(detail::ValidateHeader(hdr));
}
