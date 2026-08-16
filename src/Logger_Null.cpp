#include "shmlog/Logger.h"

#include <atomic>

namespace shmlog {

// Null backend: Logger::Write short-circuits before formatting when
// SHMLOG_BACKEND_null is defined, so WriteToBuffer is never reached.  Only the
// enable flag is kept, so that IsEnabled() still answers truthfully.

namespace {

std::atomic<bool> g_ready{false};

} // namespace

bool Logger::Initialize(const char* /*shmName*/, uint8_t /*sourceId*/) {
    g_ready.store(true, std::memory_order_release);
    return true;
}

void Logger::Shutdown() {
    g_ready.store(false, std::memory_order_relaxed);
}

bool Logger::IsEnabled() noexcept {
    return g_ready.load(std::memory_order_relaxed);
}

void Logger::WriteToBuffer(LogLevel /*level*/, const char* /*msg*/, uint16_t /*msgSize*/) noexcept {
}

} // namespace shmlog
