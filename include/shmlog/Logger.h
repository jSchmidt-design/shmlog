#pragma once

#include "shmlog/LogContracts.h"
#include "shmlog/LoggerBackend.h"

#include <array>
#include <format>
#include <string>

namespace shmlog {

// Logger - ultra-low-latency frontend for the shared memory ring buffer.
//
// The library carries no knowledge of who its writers are: a partition is
// identified by an opaque SHM name plus a caller-assigned source id.  Any
// app-specific name/id table lives outside this library, in the consuming
// application.
//
// Usage:
//   // Application startup (once per process):
//   shmlog::Logger::Initialize("MyApp_Log", /*sourceId=*/0);
//
//   // Callsite (prefer macros - they compile away entirely when disabled):
//   SHMLOG_INFO("Processing {} channels", count);
class Logger {
public:
    Logger() = delete;

    // Create or attach this process's SHM partition and enable writes.
    // Returns false - leaving logging disabled and every write macro a no-op - if the mapping cannot be created or carries an incompatible layout.
    // A second call while already enabled is a no-op that returns true.
    //
    // Not internally synchronised beyond the ready flag: call before starting
    // the threads that log.
    //
    // [[nodiscard]] because a dropped false is the one failure mode with no
    // other symptom: logging simply stays silent for the life of the process.
    [[nodiscard]] static bool Initialize(const char* shmName, uint8_t sourceId);

    // Disable writes and release the mapping.
    //
    // A thread already inside a write may still be holding a pointer into the
    // mapping, so this is best-effort rather than a hard barrier; quiesce your
    // logging threads first if that matters.
    static void Shutdown();

    // True while writes are being recorded.
    [[nodiscard]] static bool IsEnabled() noexcept;

    // Write a log entry.  Called by the SHMLOG_* / LOG_* macros; prefer those.
    // Formats directly into a stack buffer via std::format_to_n - no heap allocation. Messages longer than kTruncateAtBytes are truncated with "...". Never throws - a throwing user formatter drops the entry.
    template<typename... Args>
    static void Write(LogLevel level,
                      std::format_string<Args...> fmt,
                      Args&&... args) noexcept
    {
#ifdef SHMLOG_BACKEND_null
        // Discard without formatting. The arguments have already been evaluated by the caller - only a macro disabled at the preprocessor stage avoids that.
        (void)level;
        (void)fmt;
        ((void)args, ...);
#else
        try {
            std::array<char, kMaxMessageBytes> buf;
            const uint16_t msgSize =
                detail::FormatToBuffer(buf, fmt, std::forward<Args>(args)...);
            WriteToBuffer(level, buf.data(), msgSize);
        } catch (...) {
            // A log statement must not propagate an exception into its caller.
        }
#endif
    }

private:
    static void WriteToBuffer(LogLevel level, const char* msg, uint16_t msgSize) noexcept;
};

} // namespace shmlog

// --- Canonical SHMLOG level constants ---
// These are the primary names; LOG_LEVEL_* are kept as compatibility aliases
// below (gated by SHMLOG_SHORT_MACROS, default on).

#define SHMLOG_LEVEL_TRACE 0
#define SHMLOG_LEVEL_DEBUG 1
#define SHMLOG_LEVEL_INFO  2
#define SHMLOG_LEVEL_WARN  3
#define SHMLOG_LEVEL_ERROR 4

static_assert(static_cast<int>(shmlog::LogLevel::Trace) == SHMLOG_LEVEL_TRACE);
static_assert(static_cast<int>(shmlog::LogLevel::Debug) == SHMLOG_LEVEL_DEBUG);
static_assert(static_cast<int>(shmlog::LogLevel::Info)  == SHMLOG_LEVEL_INFO);
static_assert(static_cast<int>(shmlog::LogLevel::Warn)  == SHMLOG_LEVEL_WARN);
static_assert(static_cast<int>(shmlog::LogLevel::Error) == SHMLOG_LEVEL_ERROR);

// Compile-time log level: DEBUG in debug builds, INFO in release.
// Override by defining SHMLOG_LEVEL before including this header.
#ifndef SHMLOG_LEVEL
#  ifdef NDEBUG
#    define SHMLOG_LEVEL SHMLOG_LEVEL_INFO
#  else
#    define SHMLOG_LEVEL SHMLOG_LEVEL_DEBUG
#  endif
#endif

// --- Short compatibility aliases (default on) ---
#if !defined(SHMLOG_SHORT_MACROS) || SHMLOG_SHORT_MACROS
#  define LOG_LEVEL_TRACE SHMLOG_LEVEL_TRACE
#  define LOG_LEVEL_DEBUG SHMLOG_LEVEL_DEBUG
#  define LOG_LEVEL_INFO  SHMLOG_LEVEL_INFO
#  define LOG_LEVEL_WARN  SHMLOG_LEVEL_WARN
#  define LOG_LEVEL_ERROR SHMLOG_LEVEL_ERROR
#endif

// --- Canonical SHMLOG_* call macros ---
// Disabled-level macros expand to (void)0 at the preprocessor stage, so their
// arguments are never evaluated - guaranteed zero cost.
//
// The format string is passed through __VA_ARGS__ rather than named as its own
// macro parameter.  That keeps a no-argument call from producing a trailing
// comma, which would otherwise need __VA_OPT__ - and __VA_OPT__ only works on MSVC under /Zc:preprocessor, a flag no consumer should be forced to adopt.

#if SHMLOG_LEVEL <= SHMLOG_LEVEL_TRACE
#  define SHMLOG_TRACE(...) ::shmlog::Logger::Write(::shmlog::LogLevel::Trace, __VA_ARGS__)
#else
#  define SHMLOG_TRACE(...) (void)0
#endif

#if SHMLOG_LEVEL <= SHMLOG_LEVEL_DEBUG
#  define SHMLOG_DEBUG(...) ::shmlog::Logger::Write(::shmlog::LogLevel::Debug, __VA_ARGS__)
#else
#  define SHMLOG_DEBUG(...) (void)0
#endif

#if SHMLOG_LEVEL <= SHMLOG_LEVEL_INFO
#  define SHMLOG_INFO(...)  ::shmlog::Logger::Write(::shmlog::LogLevel::Info, __VA_ARGS__)
#else
#  define SHMLOG_INFO(...)  (void)0
#endif

#if SHMLOG_LEVEL <= SHMLOG_LEVEL_WARN
#  define SHMLOG_WARN(...)  ::shmlog::Logger::Write(::shmlog::LogLevel::Warn, __VA_ARGS__)
#else
#  define SHMLOG_WARN(...)  (void)0
#endif

#if SHMLOG_LEVEL <= SHMLOG_LEVEL_ERROR
#  define SHMLOG_ERROR(...) ::shmlog::Logger::Write(::shmlog::LogLevel::Error, __VA_ARGS__)
#else
#  define SHMLOG_ERROR(...) (void)0
#endif

// --- Short LOG_* compatibility aliases (default on) ---
#if !defined(SHMLOG_SHORT_MACROS) || SHMLOG_SHORT_MACROS
#  define LOG_TRACE SHMLOG_TRACE
#  define LOG_DEBUG SHMLOG_DEBUG
#  define LOG_INFO  SHMLOG_INFO
#  define LOG_WARN  SHMLOG_WARN
#  define LOG_ERROR SHMLOG_ERROR
#endif
