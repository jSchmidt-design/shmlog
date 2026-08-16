// ShmMapping owns a Windows named file mapping.  These create real kernel
// objects, but stay deterministic: everything happens in-process, single
// threaded, under a name unique to this process and test case, and every
// mapping is released when its object leaves scope.
#include <doctest/doctest.h>

#include "shmlog/ShmMapping.h"

#include <ostream>  // doctest stringification
#include <atomic>
#include <cstring>
#include <string>

using namespace shmlog;

namespace {

// One page is plenty; these test ownership, not the ring layout.
constexpr std::size_t kSize = 4096;

std::string UniqueName() {
    static std::atomic<unsigned> counter{0};
    return "shmlog_test_map_" + std::to_string(::GetCurrentProcessId()) + "_" +
           std::to_string(counter.fetch_add(1));
}

// Wrapper so that a self-move can be exercised without writing `x = std::move(x)`
// at the call site, which compilers rightly diagnose.
void MoveInto(ShmMapping& dst, ShmMapping& src) {
    dst = std::move(src);
}

} // namespace

// --- create / open ---

TEST_CASE("ShmMapping_createReportsNewMapping") {
    ShmMapping map;
    bool isNew = false;

    REQUIRE(map.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite, &isNew));
    CHECK(isNew);
    CHECK(map.isOpen());
    CHECK(map.getPtr() != nullptr);
}

TEST_CASE("ShmMapping_secondCreateOfTheSameNameIsNotNew") {
    const std::string name = UniqueName();

    ShmMapping first;
    bool firstIsNew = false;
    REQUIRE(first.create(name, kSize, ShmMapping::Mode::ReadWrite, &firstIsNew));
    REQUIRE(firstIsNew);

    // This is what tells Logger::Initialize whether to stamp the header or to
    // validate one somebody else stamped.
    ShmMapping second;
    bool secondIsNew = true;
    REQUIRE(second.create(name, kSize, ShmMapping::Mode::ReadWrite, &secondIsNew));
    CHECK_FALSE(secondIsNew);

    // Two mappings of one name are two views of the same pages.
    std::memset(first.getPtr(), 0, kSize);
    static_cast<char*>(first.getPtr())[7] = 'Z';
    CHECK(static_cast<const char*>(second.getPtr())[7] == 'Z');
}

TEST_CASE("ShmMapping_createAcceptsANullIsNewOut") {
    ShmMapping map;
    CHECK(map.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite));
}

TEST_CASE("ShmMapping_openFindsAnExistingMapping") {
    const std::string name = UniqueName();

    ShmMapping writer;
    REQUIRE(writer.create(name, kSize, ShmMapping::Mode::ReadWrite));
    std::memset(writer.getPtr(), 0, kSize);
    std::memcpy(writer.getPtr(), "payload", 7);

    ShmMapping reader;
    REQUIRE(reader.open(name, kSize, ShmMapping::Mode::ReadOnly));
    CHECK(std::string(static_cast<const char*>(reader.getPtr()), 7) == "payload");
}

TEST_CASE("ShmMapping_openFailsWhenTheNameDoesNotExist") {
    ShmMapping map;

    CHECK_FALSE(map.open(UniqueName(), kSize, ShmMapping::Mode::ReadOnly));
    CHECK_FALSE(map.isOpen());
    CHECK(map.getPtr() == nullptr);
}

TEST_CASE("ShmMapping_openOnAnOpenObjectLeavesItAlone") {
    const std::string first  = UniqueName();
    const std::string second = UniqueName();

    ShmMapping other;
    REQUIRE(other.create(second, kSize, ShmMapping::Mode::ReadWrite));

    ShmMapping map;
    REQUIRE(map.create(first, kSize, ShmMapping::Mode::ReadWrite));
    void* original = map.getPtr();

    // Documented contract: on failure, and when already holding a mapping,
    // nothing is modified.
    CHECK_FALSE(map.open(second, kSize, ShmMapping::Mode::ReadOnly));
    CHECK(map.isOpen());
    CHECK(map.getPtr() == original);
}

TEST_CASE("ShmMapping_createOnAnOpenObjectLeavesItAlone") {
    ShmMapping map;
    REQUIRE(map.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite));
    void* original = map.getPtr();

    CHECK_FALSE(map.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite));
    CHECK(map.isOpen());
    CHECK(map.getPtr() == original);
}

// --- close ---

TEST_CASE("ShmMapping_closeReleasesAndIsIdempotent") {
    ShmMapping map;
    REQUIRE(map.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite));

    map.close();
    CHECK_FALSE(map.isOpen());
    CHECK(map.getPtr() == nullptr);

    map.close();  // Safe on an already-closed object.
    CHECK_FALSE(map.isOpen());
}

TEST_CASE("ShmMapping_reusableAfterClose") {
    ShmMapping map;
    REQUIRE(map.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite));
    map.close();

    // PartitionReader::TryConnect closes and retries on the next poll, so a
    // closed object has to be openable again.
    CHECK(map.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite));
}

TEST_CASE("ShmMapping_lastCloseDestroysTheName") {
    const std::string name = UniqueName();

    {
        ShmMapping owner;
        REQUIRE(owner.create(name, kSize, ShmMapping::Mode::ReadWrite));
    }  // Destructor releases the only handle.

    ShmMapping late;
    CHECK_FALSE(late.open(name, kSize, ShmMapping::Mode::ReadOnly));
}

// --- move semantics ---

TEST_CASE("ShmMapping_moveConstructionTransfersOwnership") {
    ShmMapping source;
    REQUIRE(source.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite));
    void* original = source.getPtr();

    ShmMapping moved(std::move(source));

    CHECK(moved.isOpen());
    CHECK(moved.getPtr() == original);
    CHECK_FALSE(source.isOpen());
    CHECK(source.getPtr() == nullptr);
}

TEST_CASE("ShmMapping_moveAssignmentReleasesTheTarget") {
    const std::string discarded = UniqueName();

    ShmMapping target;
    REQUIRE(target.create(discarded, kSize, ShmMapping::Mode::ReadWrite));

    ShmMapping source;
    REQUIRE(source.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite));
    void* original = source.getPtr();

    target = std::move(source);

    CHECK(target.getPtr() == original);
    CHECK_FALSE(source.isOpen());

    // The target's previous mapping was closed, not leaked - its name is gone.
    ShmMapping probe;
    CHECK_FALSE(probe.open(discarded, kSize, ShmMapping::Mode::ReadOnly));
}

TEST_CASE("ShmMapping_selfMoveAssignmentKeepsTheMapping") {
    ShmMapping map;
    REQUIRE(map.create(UniqueName(), kSize, ShmMapping::Mode::ReadWrite));
    void* original = map.getPtr();

    MoveInto(map, map);

    CHECK(map.isOpen());
    CHECK(map.getPtr() == original);
}
