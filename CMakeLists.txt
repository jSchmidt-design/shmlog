cmake_minimum_required(VERSION 3.20)
project(shmlog VERSION 1.0.0 LANGUAGES CXX)

# Whether shmlog is the project being built rather than one add_subdirectory()'d
# into a consumer's tree.  Spelled as a directory comparison rather than
# PROJECT_IS_TOP_LEVEL to stay within the declared 3.20 minimum.  Both the
# warning set and the test suite default off when embedded.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SHMLOG_IS_TOP_LEVEL ON)
else()
    set(SHMLOG_IS_TOP_LEVEL OFF)
endif()

set(SHMLOG_BACKEND shm CACHE STRING "shmlog output backend: shm, stdout, or null")
set_property(CACHE SHMLOG_BACKEND PROPERTY STRINGS shm stdout null)

set(SHMLOG_SHORT_MACROS ON CACHE BOOL "Define unprefixed LOG_*/LOG_LEVEL_* aliases alongside SHMLOG_*")

# --- Warnings ---
# On for our own builds, off when embedded: a consumer's warning policy is
# theirs to set, and a compiler that starts diagnosing something new must not
# break their build over our sources.  Applied PRIVATE, so nothing here reaches
# a target that merely links shmlog.
option(SHMLOG_WARNINGS "Build shmlog's own targets with the project warning set"
       ${SHMLOG_IS_TOP_LEVEL})

function(shmlog_set_warnings target)
    if(NOT SHMLOG_WARNINGS)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Matched exhaustively rather than defaulting to shm on the way out: a typo
# ("-DSHMLOG_BACKEND=stdotu") would otherwise configure and build clean while
# quietly giving you a backend you did not ask for, and the mistake would only
# surface as missing output at runtime.
if(SHMLOG_BACKEND STREQUAL "shm")
    set(SHMLOG_BACKEND_SOURCE src/Logger_Shm.cpp)
elseif(SHMLOG_BACKEND STREQUAL "stdout")
    set(SHMLOG_BACKEND_SOURCE src/Logger_Stdout.cpp)
elseif(SHMLOG_BACKEND STREQUAL "null")
    set(SHMLOG_BACKEND_SOURCE src/Logger_Null.cpp)
else()
    message(FATAL_ERROR
        "SHMLOG_BACKEND must be one of: shm, stdout, null (got '${SHMLOG_BACKEND}')")
endif()

# --- shmlog::logger - the writer path ---
# What almost every consumer needs: the SHMLOG_*/LOG_* macros. Deliberately excludes LogCollectorCore's <filesystem>/<format>-heavy merge/persist code, which a writer-only app never calls.
add_library(shmlog_logger STATIC
    # Backend-independent pure seams (FormatToBuffer/WriteSlot/ValidateHeader/ComposeStdoutLine).
    src/Logger.cpp
    ${SHMLOG_BACKEND_SOURCE}
    # Unconditional, even for the stdout/null backends: shmlog::collector maps partitions through ShmMapping regardless of which backend the writers were built with.
    src/ShmMapping.cpp
)
add_library(shmlog::logger ALIAS shmlog_logger)

target_compile_features(shmlog_logger PUBLIC cxx_std_20)

# Lets consumers detect the active backend, e.g. to skip a test that only
# makes sense against the shm backend: #ifdef SHMLOG_BACKEND_shm
target_compile_definitions(shmlog_logger PUBLIC
    SHMLOG_BACKEND_${SHMLOG_BACKEND}
    SHMLOG_SHORT_MACROS=$<BOOL:${SHMLOG_SHORT_MACROS}>
)

target_include_directories(shmlog_logger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

shmlog_set_warnings(shmlog_logger)

# --- shmlog::collector - the reader/exporter path ---
# Only needed by a process that drains SHM partitions to a file or console (e.g. a log-collector binary). PUBLIC-links shmlog::logger for LogContracts/ShmMapping, not for the writer macros.
add_library(shmlog_collector STATIC
    src/LogCollectorCore.cpp
)
add_library(shmlog::collector ALIAS shmlog_collector)

target_link_libraries(shmlog_collector PUBLIC shmlog::logger)
target_compile_features(shmlog_collector PUBLIC cxx_std_20)

shmlog_set_warnings(shmlog_collector)

# --- Tests ---
# Defaults ON for a top-level build and OFF when shmlog is add_subdirectory()'d
# into a consumer's tree, so embedding the library never drags in doctest.
option(SHMLOG_BUILD_TESTING "Build the shmlog test suite" ${SHMLOG_IS_TOP_LEVEL})

if(SHMLOG_BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
