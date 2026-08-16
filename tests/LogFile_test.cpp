// LogFile is the rotating-output helper.  These touch the filesystem, but stay
// deterministic: single-threaded, no timing dependence, and each case gets its
// own scratch directory which it removes again on the way out.
#include <doctest/doctest.h>

#include "shmlog/LogCollectorCore.h"

#include <ostream>  // doctest stringification
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace shmlog;

namespace {

namespace fs = std::filesystem;

// One scratch directory per test case, removed on scope exit.  The name mixes a
// steady_clock reading with a counter so that two cases - or two test binaries
// running side by side - cannot collide.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() /
                 ("shmlog_test_" + std::to_string(stamp) + "_" +
                  std::to_string(counter.fetch_add(1)));
        fs::create_directories(m_path);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    // LogFile takes std::string paths.
    std::string File(const std::string& name = "test.log") const {
        return (m_path / name).string();
    }

private:
    fs::path m_path;
};

std::string ReadWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// 11 bytes, so a 20-byte cap fits exactly one.
constexpr const char* kLine = "0123456789\n";
constexpr size_t kLineSize  = 11;

} // namespace

// --- Opening ---

TEST_CASE("LogFile_opensAndReportsItsPath") {
    ScratchDir dir;
    const std::string path = dir.File();

    LogFile file;
    REQUIRE(file.Open(path, 1024, 5));
    CHECK(file.IsOpen());
    CHECK(file.BasePath() == path);
    CHECK(file.CurrentSize() == 0);
}

TEST_CASE("LogFile_openFailsInsideAMissingDirectory") {
    ScratchDir dir;

    LogFile file;
    CHECK_FALSE(file.Open(dir.File("no_such_dir/test.log"), 1024, 5));
    CHECK_FALSE(file.IsOpen());
}

TEST_CASE("LogFile_measuresAnExistingFileOnOpen") {
    ScratchDir dir;
    const std::string path = dir.File();

    {
        LogFile first;
        REQUIRE(first.Open(path, 1024, 5));
        first.Write(kLine);
        first.Flush();
    }

    // Reopening appends: the size must reflect what is already on disk, or the
    // rotation threshold would be measured from zero every restart.
    LogFile second;
    REQUIRE(second.Open(path, 1024, 5));
    CHECK(second.CurrentSize() == kLineSize);

    second.Write(kLine);
    second.Flush();
    CHECK(ReadWholeFile(path) == std::string(kLine) + kLine);
}

// --- Writing ---

TEST_CASE("LogFile_writesAndTracksSize") {
    ScratchDir dir;
    const std::string path = dir.File();

    LogFile file;
    REQUIRE(file.Open(path, 1024, 5));
    file.Write("abc");
    file.Write("de");
    file.Flush();

    CHECK(file.CurrentSize() == 5);
    CHECK(ReadWholeFile(path) == "abcde");
}

TEST_CASE("LogFile_writeOnAClosedFileIsANoOp") {
    LogFile file;  // Never opened.
    CHECK_FALSE(file.IsOpen());

    file.Write("dropped");
    file.Flush();

    CHECK(file.CurrentSize() == 0);
}

// --- Rotation ---

TEST_CASE("LogFile_rotatesOnceTheCapIsExceeded") {
    ScratchDir dir;
    const std::string path = dir.File();

    LogFile file;
    REQUIRE(file.Open(path, /*maxBytes=*/20, /*files=*/5));

    file.Write("first\n");   // 6 bytes, fits.
    file.Write("second\n");  // 13 total, fits.
    file.Flush();
    CHECK_FALSE(fs::exists(path + ".1"));

    file.Write("third\n");   // Would reach 19... still fits.
    file.Flush();
    CHECK(file.CurrentSize() == 19);
    CHECK_FALSE(fs::exists(path + ".1"));

    file.Write("fourth\n");  // 19 + 7 > 20: rotate first.
    file.Flush();

    REQUIRE(fs::exists(path + ".1"));
    CHECK(ReadWholeFile(path + ".1") == "first\nsecond\nthird\n");
    CHECK(ReadWholeFile(path) == "fourth\n");
    CHECK(file.CurrentSize() == 7);
}

TEST_CASE("LogFile_keepsExactlyMaxFilesGenerations") {
    ScratchDir dir;
    const std::string path = dir.File();

    LogFile file;
    REQUIRE(file.Open(path, /*maxBytes=*/20, /*files=*/2));

    // Each write after the first rotates: 11 + 11 > 20.
    file.Write("aaaaaaaaaa\n");
    file.Write("bbbbbbbbbb\n");
    file.Write("cccccccccc\n");
    file.Write("dddddddddd\n");
    file.Flush();

    CHECK(ReadWholeFile(path)          == "dddddddddd\n");
    CHECK(ReadWholeFile(path + ".1")   == "cccccccccc\n");
    CHECK(ReadWholeFile(path + ".2")   == "bbbbbbbbbb\n");
    // The oldest generation is discarded rather than accumulating forever.
    CHECK_FALSE(fs::exists(path + ".3"));
}

TEST_CASE("LogFile_truncatesInPlaceWhenNoGenerationsAreKept") {
    ScratchDir dir;
    const std::string path = dir.File();

    LogFile file;
    REQUIRE(file.Open(path, /*maxBytes=*/20, /*files=*/0));

    file.Write("aaaaaaaaaa\n");
    file.Write("bbbbbbbbbb\n");
    file.Flush();

    CHECK(ReadWholeFile(path) == "bbbbbbbbbb\n");
    CHECK_FALSE(fs::exists(path + ".1"));
    CHECK(file.CurrentSize() == kLineSize);
}

TEST_CASE("LogFile_lineLongerThanTheCapDoesNotRotateAnEmptyFile") {
    ScratchDir dir;
    const std::string path = dir.File();

    LogFile file;
    REQUIRE(file.Open(path, /*maxBytes=*/5, /*files=*/5));

    // Without the "already has content" guard this would rotate on every single
    // write and never store the line anywhere.
    file.Write("a line well past the cap\n");
    file.Flush();

    CHECK_FALSE(fs::exists(path + ".1"));
    CHECK(ReadWholeFile(path) == "a line well past the cap\n");
}

TEST_CASE("LogFile_explicitRotateStartsAFreshFile") {
    ScratchDir dir;
    const std::string path = dir.File();

    LogFile file;
    REQUIRE(file.Open(path, 1024, 5));
    file.Write("before\n");
    file.Rotate();

    CHECK(file.CurrentSize() == 0);
    CHECK(file.IsOpen());
    CHECK(ReadWholeFile(path + ".1") == "before\n");
    CHECK(ReadWholeFile(path).empty());

    file.Write("after\n");
    file.Flush();
    CHECK(ReadWholeFile(path) == "after\n");
}
