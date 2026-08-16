#include <doctest/doctest.h>

#include "shmlog/LogCollectorCore.h"

#include <ostream>  // doctest stringification
#include <format>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace shmlog;

namespace {

// ParseArgs takes char* argv[]; the literals here are only ever read.
ParsedArgs Parse(std::initializer_list<const char*> args, LogReaderOptions defaults = {}) {
    std::vector<char*> argv;
    for (const char* a : args) argv.push_back(const_cast<char*>(a));
    return ParseArgs(static_cast<int>(argv.size()), argv.data(), std::move(defaults));
}

} // namespace

// --- Happy path ---

TEST_CASE("ReaderCli_defaultsAreUsed") {
    const auto parsed = Parse({"shmlog_reader"});

    CHECK(parsed.errors.empty());
    CHECK_FALSE(parsed.helpRequested);
    CHECK(parsed.options.logFile == "shmlog.log");
    CHECK(parsed.options.maxSizeBytes == 10 * 1024 * 1024);
    CHECK(parsed.options.maxFiles == 5);
    CHECK(parsed.options.useConsole == false);
    CHECK(parsed.options.partitions.empty());
}

TEST_CASE("ReaderCli_overridesDefaults") {
    const auto parsed = Parse({"shmlog_reader",
                               "--log-file", "custom.log",
                               "--max-size", "2048",
                               "--max-files", "3",
                               "--console"});

    CHECK(parsed.errors.empty());
    CHECK(parsed.options.logFile == "custom.log");
    CHECK(parsed.options.maxSizeBytes == 2048);
    CHECK(parsed.options.maxFiles == 3);
    CHECK(parsed.options.useConsole == true);
}

TEST_CASE("ReaderCli_zeroMaxFilesIsAccepted") {
    // 0 is meaningful, not a rejected value: it selects truncate-in-place
    // instead of rotation (see LogFile::Rotate).
    const auto parsed = Parse({"shmlog_reader", "--max-files", "0"});

    CHECK(parsed.errors.empty());
    CHECK(parsed.options.maxFiles == 0);
}

// --- Partitions ---

TEST_CASE("ReaderCli_appendsPartitionWithLabel") {
    LogReaderOptions defaults;
    defaults.partitions.push_back({.shmName = "shmA", .label = "LABELA", .sourceId = 0});

    const auto parsed = Parse({"shmlog_reader",
                               "--partition", "CustomShm:CUSTOM",
                               "--partition", "NoLabel"},
                              std::move(defaults));

    CHECK(parsed.errors.empty());
    REQUIRE(parsed.options.partitions.size() == 3);
    CHECK(parsed.options.partitions[0].shmName == "shmA");
    CHECK(parsed.options.partitions[0].label == "LABELA");
    CHECK(parsed.options.partitions[0].sourceId == 0);

    CHECK(parsed.options.partitions[1].shmName == "CustomShm");
    CHECK(parsed.options.partitions[1].label == "CUSTOM");
    CHECK(parsed.options.partitions[1].sourceId == 1);

    CHECK(parsed.options.partitions[2].shmName == "NoLabel");
    CHECK(parsed.options.partitions[2].label == "NoLabel");
    CHECK(parsed.options.partitions[2].sourceId == 2);
}

TEST_CASE("ReaderCli_sourceIdContinuesPastNonContiguousDefaults") {
    // Source ids are not assumed to be 0..n-1: the next one is max+1.
    LogReaderOptions defaults;
    defaults.partitions.push_back({.shmName = "a", .label = "A", .sourceId = 6});

    const auto parsed = Parse({"shmlog_reader", "--partition", "b"},
                              std::move(defaults));

    CHECK(parsed.errors.empty());
    REQUIRE(parsed.options.partitions.size() == 2);
    CHECK(parsed.options.partitions[1].sourceId == 7);
}

TEST_CASE("ReaderCli_trailingColonFallsBackToTheShmName") {
    // "Name:" would otherwise produce an empty label, formatting every line from
    // this partition with a blank source column.
    const auto parsed = Parse({"shmlog_reader", "--partition", "Name:"});

    CHECK(parsed.errors.empty());
    REQUIRE(parsed.options.partitions.size() == 1);
    CHECK(parsed.options.partitions[0].shmName == "Name");
    CHECK(parsed.options.partitions[0].label == "Name");
}

TEST_CASE("ReaderCli_labelMayContainFurtherColons") {
    // Only the first colon separates; the rest belong to the label.
    const auto parsed = Parse({"shmlog_reader", "--partition", "Name:A:B"});

    REQUIRE(parsed.options.partitions.size() == 1);
    CHECK(parsed.options.partitions[0].shmName == "Name");
    CHECK(parsed.options.partitions[0].label == "A:B");
}

TEST_CASE("ReaderCli_rejectsEmptyShmName") {
    // A partition with no name can never be opened.
    const auto parsed = Parse({"shmlog_reader", "--partition", ":LABEL"});

    CHECK(parsed.errors.size() == 1);
    CHECK(parsed.options.partitions.empty());
}

TEST_CASE("ReaderCli_rejectsPartitionsPastTheSourceIdSpace") {
    // sourceId is a uint8_t, so 256 partitions is the hard ceiling.
    LogReaderOptions defaults;
    defaults.partitions.push_back({.shmName = "a", .label = "A", .sourceId = 255});

    const auto parsed = Parse({"shmlog_reader", "--partition", "b"}, std::move(defaults));

    CHECK(parsed.errors.size() == 1);
    CHECK(parsed.options.partitions.size() == 1);  // The default survives; "b" is refused.
}

// --- Error reporting ---

TEST_CASE("ReaderCli_reportsUnparseableNumber") {
    const auto parsed = Parse({"shmlog_reader", "--max-size", "abc"});
    CHECK(parsed.errors.size() == 1);
}

TEST_CASE("ReaderCli_reportsTrailingJunkInNumber") {
    const auto parsed = Parse({"shmlog_reader", "--max-files", "3x"});
    CHECK(parsed.errors.size() == 1);
}

TEST_CASE("ReaderCli_leavesOptionUntouchedWhenParsingFails") {
    // from_chars consumes the leading "3" of "3x" before the trailing-junk check
    // rejects it.  A rejected value must not be half-applied.
    const auto parsed = Parse({"shmlog_reader", "--max-files", "3x", "--max-size", "9y"});

    CHECK(parsed.errors.size() == 2);
    CHECK(parsed.options.maxFiles == 5);                 // Default, not 3.
    CHECK(parsed.options.maxSizeBytes == 10 * 1024 * 1024);  // Default, not 9.
}

TEST_CASE("ReaderCli_rejectsZeroMaxSize") {
    // Unlike --max-files 0, a zero size cap is not a mode: LogFile would rotate
    // after every line written.
    const auto parsed = Parse({"shmlog_reader", "--max-size", "0"});

    CHECK(parsed.errors.size() == 1);
    CHECK(parsed.options.maxSizeBytes == 10 * 1024 * 1024);  // Default preserved.
}

TEST_CASE("ReaderCli_rejectsNegativeMaxSize") {
    // maxSizeBytes is unsigned, so "-1" must be refused rather than wrapping to
    // a cap of 18 exabytes.
    const auto parsed = Parse({"shmlog_reader", "--max-size", "-1"});

    CHECK(parsed.errors.size() == 1);
    CHECK(parsed.options.maxSizeBytes == 10 * 1024 * 1024);
}

TEST_CASE("ReaderCli_rejectsNegativeMaxFiles") {
    const auto parsed = Parse({"shmlog_reader", "--max-files", "-3"});

    CHECK(parsed.errors.size() == 1);
    CHECK(parsed.options.maxFiles == 5);  // Default preserved.
}

TEST_CASE("ReaderCli_reportsMissingValue") {
    const auto parsed = Parse({"shmlog_reader", "--log-file"});
    CHECK(parsed.errors.size() == 1);
}

TEST_CASE("ReaderCli_reportsMissingPartitionValue") {
    const auto parsed = Parse({"shmlog_reader", "--partition"});
    CHECK(parsed.errors.size() == 1);
    CHECK(parsed.options.partitions.empty());
}

TEST_CASE("ReaderCli_reportsUnknownFlag") {
    const auto parsed = Parse({"shmlog_reader", "--partiton", "typo"});
    // Two errors: the misspelled flag, then "typo" as a stray positional.
    CHECK(parsed.errors.size() == 2);
}

TEST_CASE("ReaderCli_accumulatesEveryError") {
    // Parsing does not stop at the first problem - a user fixing their command
    // line should see all of them at once.
    const auto parsed = Parse({"shmlog_reader", "--nope", "--max-size", "x", "--also-nope"});
    CHECK(parsed.errors.size() == 3);
}

TEST_CASE("ReaderCli_keepsParsingAfterAnError") {
    const auto parsed = Parse({"shmlog_reader", "--max-size", "x", "--console"});

    CHECK(parsed.errors.size() == 1);
    CHECK(parsed.options.useConsole == true);
}

// --- Help ---

TEST_CASE("ReaderCli_helpIsRequested") {
    const auto parsed = Parse({"shmlog_reader", "--help"});
    CHECK(parsed.helpRequested);
    CHECK(parsed.errors.empty());
}

TEST_CASE("ReaderCli_shortHelpFlagIsAccepted") {
    const auto parsed = Parse({"shmlog_reader", "-h"});
    CHECK(parsed.helpRequested);
    CHECK(parsed.errors.empty());
}

TEST_CASE("ReaderCli_usageTextDocumentsEveryFlag") {
    const std::string_view usage = UsageText();
    REQUIRE_FALSE(usage.empty());

    for (const auto* flag : {"--partition", "--log-file", "--max-size",
                             "--max-files", "--console", "--help"}) {
        CAPTURE(flag);
        CHECK(usage.find(flag) != std::string_view::npos);
    }
}
