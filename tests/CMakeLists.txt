cmake_minimum_required(VERSION 3.20)
project(shmlog_tests LANGUAGES CXX)

# Built as part of a top-level build (SHMLOG_BUILD_TESTING, on by default), and
# ALSO standalone-configurable: point CMake at this directory and it configures
# against the parent library with nothing else.
#
#   cmake -S tests -B build-tests
#   cmake --build build-tests
#   ctest --test-dir build-tests
#
# Every test is deterministic and single-threaded: no threads, no sleeps, no
# timing dependence, nothing that can pass or fail depending on scheduling.
# Cases that need an OS resource (LogFile, ShmMapping, the writer round trip)
# create it under a name unique to the process and release it on scope exit.
#
# SlotReadResult::Overwritten is the one behaviour left uncovered - observing it
# requires a concurrent writer, and a racy test is worse than an honest gap.
#
# `shmlog::collector` PUBLIC-links `shmlog::logger`, so it alone pulls in every
# header under test (Logger.h, LoggerBackend.h, LogCollectorCore.h, ShmMapping.h).

include(FetchContent)

if(NOT TARGET shmlog::logger)
    # Standalone, so the parent sees itself as embedded and defaults its warning
    # set off.  This is still one of our own builds, though, so turn it back on.
    set(SHMLOG_WARNINGS ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. shmlog)
endif()

# doctest: the test framework itself. Guarded so a consumer that already
# fetched doctest at the same tag wins instead of colliding with a second copy.
if(NOT TARGET doctest::doctest)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE
        # doctest ships a ~140-character path under examples/, so checking it
        # out beneath a build directory that is itself deep exceeds Windows'
        # 260-character MAX_PATH and the clone dies with "Filename too long" -
        # taking the whole configure step with it.  Set per-clone rather than
        # asking every consumer to carry core.longpaths in their global config.
        GIT_CONFIG     core.longpaths=true)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON  CACHE BOOL "" FORCE)
    # doctest v2.4.11 declares cmake_minimum_required(VERSION 3.0); allow it to
    # configure under newer CMake that dropped compatibility with < 3.5.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_MakeAvailable(doctest)
endif()

enable_testing()

# Explicit source list, not GLOB, so a missing file fails loudly rather than
# silently dropping a test.
set(TEST_SOURCES
    doctest_main.cpp
    # Writer seams
    HeaderCheck_test.cpp
    SlotWrite_test.cpp
    LogFormat_test.cpp
    StdoutLine_test.cpp
    LoggerWrite_test.cpp
    # Reader seams
    ReadSlot_test.cpp
    ReadPlan_test.cpp
    MergeBatch_test.cpp
    FormatTimestamp_test.cpp
    FormatBatch_test.cpp
    ReaderCli_test.cpp
    # Owned OS resources
    LogFile_test.cpp
    ShmMapping_test.cpp
)

add_executable(shmlog_tests ${TEST_SOURCES})
target_link_libraries(shmlog_tests PRIVATE doctest::doctest shmlog::collector)

if(MSVC)
    target_compile_options(shmlog_tests PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(shmlog_tests PRIVATE -Wall -Wextra)
endif()

# doctest_discover_tests() registers each TEST_CASE as its own CTest test. Its
# module ships in the doctest checkout; fall back to one test per binary when
# doctest came from elsewhere (an installed package, a consumer's own fetch).
if(DEFINED doctest_SOURCE_DIR AND EXISTS "${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
    list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
    include(doctest)
    doctest_discover_tests(shmlog_tests)
else()
    add_test(NAME shmlog-tests COMMAND shmlog_tests)
endif()
