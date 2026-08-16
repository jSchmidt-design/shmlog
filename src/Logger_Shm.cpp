#include "shmlog/Logger.h"
#include "shmlog/LoggerBackend.h"
#include "shmlog/ShmMapping.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace shmlog {

// --- Module-level state ---

namespace {

ShmMapping g_shm;
uint8_t g_source{0};
std::atomic<bool> g_ready{false};

// How long Initialize waits for a concurrent creator to publish the header before giving up. Only reached when two processes race to create the same partition name - a header that is present but wrong fails immediately.
constexpr auto kHeaderPublishTimeout = std::chrono::milliseconds(50);

// Microseconds since the steady_clock epoch (backed by QPC on Windows). All processes on the same machine share the same QPC epoch, giving cross-process monotonic alignment without extra synchronisation.
uint64_t TimestampUs() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Cache the thread ID to avoid repeated kernel calls on the hot path.
uint32_t CachedThreadId() noexcept {
    thread_local const uint32_t id = static_cast<uint32_t>(::GetCurrentThreadId());
    return id;
}

// Stamp a freshly created mapping. Windows zero-fills new file-mapping pages, so every slot's sequence already reads 0 (even = valid/empty).
void StampHeader(LogRingBufferHeader& hdr) noexcept {
    hdr.head_index.store(0, std::memory_order_relaxed);
    hdr.capacity       = kRingCapacity;
    hdr.format_version = kFormatVersion;
    hdr.entry_size     = static_cast<uint16_t>(sizeof(LogEntry));
    // Released last: this is what makes the fields above visible to an attacher
    // (see LogRingBufferHeader and detail::ValidateHeader).
    hdr.magic.store(kShmMagic, std::memory_order_release);
}

// Accept an existing mapping only if it was written by a compatible build. Tolerates arriving mid-stamping, which is otherwise indistinguishable from a corrupt header.
bool AcceptExistingHeader(const LogRingBufferHeader& hdr) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + kHeaderPublishTimeout;
    while (hdr.magic.load(std::memory_order_acquire) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return detail::ValidateHeader(hdr);
}

} // namespace

// --- Public API ---

bool Logger::Initialize(const char* shmName, uint8_t sourceId) {
    if (g_ready.load(std::memory_order_relaxed)) return true;

    bool isNew = false;
    if (!g_shm.create(std::string(shmName), kShmSize, ShmMapping::Mode::ReadWrite, &isNew)) {
        return false;
    }

    auto& hdr = *GetRingHeader(g_shm.getPtr());
    if (isNew) {
        StampHeader(hdr);
    } else if (!AcceptExistingHeader(hdr)) {
        // Refuse rather than write through a mismatched struct layout. Note head_index is deliberately left alone on an existing mapping - resetting it would corrupt a connected reader's readIndex.
        g_shm.close();
        return false;
    }

    g_source = sourceId;
    g_ready.store(true, std::memory_order_release);
    return true;
}

void Logger::Shutdown() {
    if (!g_ready.load(std::memory_order_relaxed)) return;

    // Disable writes first so new callsites become no-ops, then release the mapping. A concurrent writer that already observed ready==true may still be holding a pointer into the view - that race is inherent without per-write synchronisation, so quiesce logging threads before calling.
    g_ready.store(false, std::memory_order_relaxed);
    g_shm.close();
}

bool Logger::IsEnabled() noexcept {
    return g_ready.load(std::memory_order_relaxed);
}

// --- Internal write ---

void Logger::WriteToBuffer(LogLevel level, const char* msg, uint16_t msgSize) noexcept {
    if (!g_ready.load(std::memory_order_relaxed)) return;

    void* base  = g_shm.getPtr();
    auto* hdr   = GetRingHeader(base);
    auto* slots = GetSlots(base);

    // Claim a slot. Ordering comes from the per-slot seqlock, so relaxed is sufficient here. Capacity is kRingCapacity by ValidateHeader/StampHeader.
    const uint32_t idx =
        hdr->head_index.fetch_add(1, std::memory_order_relaxed) % kRingCapacity;

    LogEntry& slot = slots[idx];
    detail::WriteSlot(slot,
                      slot.sequence.load(std::memory_order_relaxed),
                      TimestampUs(),
                      CachedThreadId(),
                      g_source,
                      level,
                      std::string_view(msg, msgSize));
}

} // namespace shmlog
