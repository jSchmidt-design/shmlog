#include "shmlog/Logger.h"
#include "shmlog/LoggerBackend.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>

namespace shmlog {

// ─── Module-level state ───────────────────────────────────────────────────────

namespace {

uint8_t g_source{0};
std::atomic<bool> g_ready{false};

// Wall-clock rather than steady_clock: this backend has no reader to align
// with, and a human reading the console wants the time of day.
uint64_t SystemTimestampUs() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

uint32_t CachedThreadId() noexcept {
    thread_local const uint32_t id = static_cast<uint32_t>(::GetCurrentThreadId());
    return id;
}

} // namespace

// ─── Public API ───────────────────────────────────────────────────────────────

bool Logger::Initialize(const char* /*shmName*/, uint8_t sourceId) {
    if (g_ready.load(std::memory_order_relaxed)) return true;

    g_source = sourceId;
    g_ready.store(true, std::memory_order_release);
    return true;
}

void Logger::Shutdown() {
    g_ready.store(false, std::memory_order_relaxed);
}

bool Logger::IsEnabled() noexcept {
    return g_ready.load(std::memory_order_relaxed);
}

// ─── Internal write ───────────────────────────────────────────────────────────

void Logger::WriteToBuffer(LogLevel level, const char* msg, uint16_t msgSize) noexcept {
    if (!g_ready.load(std::memory_order_relaxed)) return;

    // Same line shape as the collector produces, except that a writer has no
    // partition label table and so prints the numeric source id.
    const std::string line = detail::ComposeLine(
        detail::FormatWallClock(SystemTimestampUs()),
        std::to_string(static_cast<unsigned>(g_source)),
        level,
        CachedThreadId(),
        std::string_view(msg, msgSize));

    // One fwrite per line: the CRT locks the stream, so concurrent threads
    // cannot interleave partial lines.
    std::fwrite(line.data(), 1, line.size(), stdout);
}

} // namespace shmlog
