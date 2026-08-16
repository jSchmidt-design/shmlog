// LogCollectorCore::FormatBatch maps each entry's source id back to its
// partition label.  Constructing the core does not touch shared memory -
// partitions attach lazily on the first poll - so this exercises the label table
// without any SHM involved.
#include <doctest/doctest.h>

#include "shmlog/LogCollectorCore.h"

#include <ostream>  // doctest stringification
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace shmlog;

namespace {

inline constexpr uint64_t kBaseUs = 1'700'000'010'123'456ULL;

std::chrono::system_clock::time_point WallBase() {
    return std::chrono::system_clock::time_point{} + std::chrono::microseconds(kBaseUs);
}

CopiedEntry MakeEntry(uint8_t source, std::string_view message) {
    CopiedEntry e{};
    e.timestamp    = kBaseUs;
    e.level        = LogLevel::Info;
    e.source       = source;
    e.message_size = static_cast<uint16_t>(message.size());
    std::memcpy(e.message, message.data(), message.size());
    return e;
}

LogCollectorCore MakeCore() {
    LogReaderOptions options;
    options.partitions.push_back({.shmName = "shmA", .label = "AUDIO", .sourceId = 1});
    options.partitions.push_back({.shmName = "shmB", .label = "NET",   .sourceId = 4});
    return LogCollectorCore(std::move(options));
}

} // namespace

TEST_CASE("FormatBatch_labelsEachEntryByItsSourceId") {
    LogCollectorCore core = MakeCore();

    const std::vector<CopiedEntry> batch{MakeEntry(1, "a"), MakeEntry(4, "b")};
    const auto lines = core.FormatBatch(batch, WallBase(), kBaseUs);

    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("[AUDIO  ]") != std::string::npos);
    CHECK(lines[1].find("[NET    ]") != std::string::npos);
}

TEST_CASE("FormatBatch_unmappedSourceFallsBackToUnknown") {
    // A source id with no configured partition must still format, not index into
    // a hole in the table.
    LogCollectorCore core = MakeCore();

    const auto lines = core.FormatBatch({MakeEntry(9, "orphan")}, WallBase(), kBaseUs);

    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("[UNKNOWN]") != std::string::npos);
    CHECK(lines[0].find("orphan") != std::string::npos);
}

TEST_CASE("FormatBatch_highestSourceIdIsInRange") {
    // The label table is 256 entries; source is a uint8_t, so 255 must be valid.
    LogCollectorCore core = MakeCore();

    const auto lines = core.FormatBatch({MakeEntry(255, "edge")}, WallBase(), kBaseUs);

    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("[UNKNOWN]") != std::string::npos);
}

TEST_CASE("FormatBatch_emptyBatchProducesNoLines") {
    LogCollectorCore core = MakeCore();
    CHECK(core.FormatBatch({}, WallBase(), kBaseUs).empty());
}

TEST_CASE("FormatBatch_preservesBatchOrder") {
    // FormatBatch formats; ordering is MergeBatch's job and must not be redone
    // or undone here.
    LogCollectorCore core = MakeCore();

    const std::vector<CopiedEntry> batch{
        MakeEntry(4, "second"), MakeEntry(1, "first"), MakeEntry(4, "third")};
    const auto lines = core.FormatBatch(batch, WallBase(), kBaseUs);

    REQUIRE(lines.size() == 3);
    CHECK(lines[0].find("second") != std::string::npos);
    CHECK(lines[1].find("first")  != std::string::npos);
    CHECK(lines[2].find("third")  != std::string::npos);
}

TEST_CASE("FormatBatch_pollingWithNoPartitionsYieldsNothing") {
    LogCollectorCore core{LogReaderOptions{}};
    CHECK(core.PollAllOnce().empty());
}

// --- Duplicate source ids ---

TEST_CASE("FormatBatch_duplicateSourceIdIsReportedToTheStatusSink") {
    // The label table has one slot per source id, so a caller that assembles
    // descriptors itself can silently make one partition's entries carry
    // another's label.  Construction says so rather than letting it pass.
    std::vector<std::string> reported;

    LogReaderOptions options;
    options.statusSink = [&](std::string_view m) { reported.emplace_back(m); };
    options.partitions.push_back({.shmName = "shmA", .label = "AUDIO", .sourceId = 2});
    options.partitions.push_back({.shmName = "shmB", .label = "NET",   .sourceId = 2});

    LogCollectorCore core{std::move(options)};

    REQUIRE(reported.size() == 1);
    CHECK(reported[0].find("shmA") != std::string::npos);
    CHECK(reported[0].find("shmB") != std::string::npos);

    // The documented consequence: the last label wins for both.
    const auto lines = core.FormatBatch({MakeEntry(2, "x")}, WallBase(), kBaseUs);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("[NET    ]") != std::string::npos);
}

TEST_CASE("FormatBatch_distinctSourceIdsReportNothing") {
    std::vector<std::string> reported;

    LogReaderOptions options;
    options.statusSink = [&](std::string_view m) { reported.emplace_back(m); };
    options.partitions.push_back({.shmName = "shmA", .label = "AUDIO", .sourceId = 1});
    options.partitions.push_back({.shmName = "shmB", .label = "NET",   .sourceId = 4});

    LogCollectorCore core{std::move(options)};

    CHECK(reported.empty());
}
