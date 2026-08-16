#include <doctest/doctest.h>

#include "shmlog/LogCollectorCore.h"

#include <vector>

using namespace shmlog;

TEST_CASE("MergeBatch_sortsByTimestampThenSourceThenSequence") {
    std::vector<CopiedEntry> batch;

    CopiedEntry a{}; a.timestamp = 100; a.source = 1; a.sequence = 2; batch.push_back(a);
    CopiedEntry b{}; b.timestamp = 100; b.source = 1; b.sequence = 1; batch.push_back(b);
    CopiedEntry c{}; c.timestamp = 100; c.source = 0; c.sequence = 5; batch.push_back(c);
    CopiedEntry d{}; d.timestamp =  50; d.source = 2; d.sequence = 0; batch.push_back(d);

    MergeBatch(batch);

    REQUIRE(batch.size() == 4);
    CHECK(batch[0].timestamp == 50);
    CHECK(batch[1].source == 0);
    CHECK(batch[2].sequence == 1);
    CHECK(batch[3].sequence == 2);
}

TEST_CASE("MergeBatch_stableForIdenticalKeys") {
    std::vector<CopiedEntry> batch;

    CopiedEntry a{}; a.timestamp = 1; a.source = 0; a.sequence = 0; a.thread_id = 1; batch.push_back(a);
    CopiedEntry b{}; b.timestamp = 1; b.source = 0; b.sequence = 0; b.thread_id = 2; batch.push_back(b);

    MergeBatch(batch);

    CHECK(batch[0].thread_id == 1);
    CHECK(batch[1].thread_id == 2);
}
