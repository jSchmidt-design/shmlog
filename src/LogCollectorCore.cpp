#include "shmlog/LogCollectorCore.h"
#include "shmlog/LoggerBackend.h"
#include "shmlog/ShmMapping.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace shmlog {

namespace {

uint64_t CurrentTimeUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Fill `out` from a whole string, rejecting trailing junk ("10abc").
//
// Parses into a scratch value and assigns only on success: std::from_chars
// consumes the leading digits of "3x" before the trailing-junk check rejects it,
// so writing through directly would leave `out` half-updated alongside a
// reported error.
template<typename T>
bool ParseNumber(std::string_view text, T& out) {
    const auto* first = text.data();
    const auto* last  = text.data() + text.size();

    T value{};
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }

    out = value;
    return true;
}

} // namespace

// --- Pure slot-read seam ---

SlotReadResult detail::ReadSlot(const LogEntry& slot, CopiedEntry& out, uint8_t sourceId) {
    out = {};

    const uint32_t seq1 = slot.sequence.load(std::memory_order_acquire);
    if (seq1 & 1u) {
        return SlotReadResult::InProgress;
    }

    // Clamp before publishing the length - a slot from a corrupt or foreign mapping can claim a size larger than the array, and every consumer builds a string_view of message_size bytes over `message`.
    const uint16_t copied = static_cast<uint16_t>(
        std::min(static_cast<size_t>(slot.message_size), sizeof(out.message)));

    out.timestamp    = slot.timestamp;
    out.thread_id    = slot.thread_id;
    out.source       = sourceId;
    out.level        = static_cast<LogLevel>(slot.level);
    out.message_size = copied;
    out.sequence     = seq1;
    std::memcpy(out.message, slot.message, copied);

    const uint32_t seq2 = slot.sequence.load(std::memory_order_acquire);
    if (seq1 != seq2) {
        return SlotReadResult::Overwritten;
    }

    // A zero-length entry is indistinguishable from a never-written slot, so both are reported as Empty and dropped - including a deliberate SHMLOG_INFO("").
    return copied == 0 ? SlotReadResult::Empty : SlotReadResult::Valid;
}

detail::ReadPlan detail::ComputeReadPlan(uint32_t head, uint32_t readIndex) noexcept {
    // Unsigned wraparound is intentional: head and readIndex are free-running
    // claim counters, not slot indices.
    const uint32_t gap = head - readIndex;

    // A gap of exactly the capacity is still fully readable - every claimed slot
    // is present. Only past that has the writer overwritten unread entries.
    if (gap > kRingCapacity) {
        return {.startIndex = head - kRingCapacity, .lostCount = gap - kRingCapacity};
    }
    return {.startIndex = readIndex, .lostCount = 0};
}

// --- Formatting helpers ---

std::string FormatTimestamp(uint64_t monoUs,
                            std::chrono::system_clock::time_point wallBase,
                            uint64_t monoBaseUs)
{
    using namespace std::chrono;

    const int64_t offsetUs = static_cast<int64_t>(monoUs) - static_cast<int64_t>(monoBaseUs);
    const auto entryWall = wallBase + microseconds(offsetUs);
    const int64_t sinceEpochUs =
        duration_cast<microseconds>(entryWall.time_since_epoch()).count();

    return detail::FormatWallClock(static_cast<uint64_t>(std::max<int64_t>(sinceEpochUs, 0)));
}

std::string FormatEntry(const CopiedEntry& e,
                        std::string_view sourceLabel,
                        std::chrono::system_clock::time_point wallBase,
                        uint64_t monoBaseUs)
{
    return detail::ComposeLine(FormatTimestamp(e.timestamp, wallBase, monoBaseUs),
                               sourceLabel,
                               e.level,
                               e.thread_id,
                               std::string_view(e.message, e.message_size));
}

void MergeBatch(std::vector<CopiedEntry>& batch) {
    // stable_sort, not sort: entries that tie on all three keys keep the order
    // the partitions were polled in, so a given input always formats to the
    // same output.
    std::stable_sort(batch.begin(), batch.end(), [](const CopiedEntry& a, const CopiedEntry& b) {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        if (a.source    != b.source)    return a.source    < b.source;
        return a.sequence < b.sequence;
    });
}

// ─── Log file with rotation ───────────────────────────────────────────────────

bool LogFile::Open(const std::string& path, size_t maxBytes, int files) {
    m_basePath     = path;
    m_maxSizeBytes = maxBytes;
    m_maxFiles     = files;
    m_stream.open(path, std::ios::app | std::ios::binary);
    if (!m_stream) return false;
    // `app` positions writes at the end but leaves the put pointer at 0 until
    // the first write, so seek explicitly to measure what is already there.
    m_stream.seekp(0, std::ios::end);
    m_currentSize = static_cast<size_t>(m_stream.tellp());
    return true;
}

void LogFile::Write(const std::string& line) {
    // is_open(), not operator bool: a default-constructed ofstream has no error
    // flags set, so `!m_stream` is false on a stream that was never opened and
    // the guard would let the write through - silently advancing m_currentSize
    // against a file that does not exist.
    if (!m_stream.is_open()) return;
    // A line longer than the cap would otherwise rotate on every write.
    if (m_currentSize > 0 && m_currentSize + line.size() > m_maxSizeBytes) Rotate();
    m_stream << line;
    m_currentSize += line.size();
}

void LogFile::Flush() {
    if (m_stream.is_open()) m_stream.flush();
}

void LogFile::Rotate() {
    m_stream.close();

    std::error_code ec;
    for (int i = m_maxFiles - 1; i >= 1; --i) {
        const auto from = std::format("{}.{}", m_basePath, i);
        const auto to   = std::format("{}.{}", m_basePath, i + 1);
        std::filesystem::remove(to, ec);
        std::filesystem::rename(from, to, ec);
    }

    if (m_maxFiles >= 1) {
        std::filesystem::rename(m_basePath, std::format("{}.1", m_basePath), ec);
    }

    // Truncating open: with no generations kept, this is what discards the old
    // content; after a rotate the base path no longer exists anyway.
    m_stream.open(m_basePath, std::ios::out | std::ios::binary | std::ios::trunc);
    m_currentSize = 0;
}

namespace {

// ─── Partition reader ────────────────────────────────────────────────────────

class PartitionReader {
public:
    explicit PartitionReader(PartitionDescriptor desc) : m_desc(std::move(desc)) {}

    PartitionReader(const PartitionReader&) = delete;
    PartitionReader& operator=(const PartitionReader&) = delete;

    std::vector<CopiedEntry> Poll() {
        if (!TryConnect()) return {};

        const void* base  = m_shm.getPtr();
        const auto* hdr   = GetRingHeader(base);
        const auto* slots = GetSlots(base);

        const uint32_t head = hdr->head_index.load(std::memory_order_acquire);

        std::vector<CopiedEntry> result;

        const auto plan = detail::ComputeReadPlan(head, m_readIndex);
        m_readIndex = plan.startIndex;
        if (plan.lostCount > 0) {
            result.push_back(MakeMarker(CurrentTimeUs(), 0,
                                        std::format("[{} messages dropped]", plan.lostCount)));
        }

        while (m_readIndex != head) {
            const LogEntry& slot = slots[m_readIndex % kRingCapacity];

            CopiedEntry e{};
            const auto status = detail::ReadSlot(slot, e, m_desc.sourceId);

            // A writer is mid-write: stop here and retry on the next poll so
            // the entry is not emitted out of order.
            if (status == SlotReadResult::InProgress) break;

            ++m_readIndex;

            switch (status) {
                case SlotReadResult::Overwritten:
                    result.push_back(MakeMarker(e.timestamp, e.sequence, "[1 message dropped]"));
                    break;
                case SlotReadResult::Empty:
                    break;
                default:
                    result.push_back(e);
                    break;
            }
        }
        return result;
    }

private:
    bool TryConnect() {
        if (m_shm.isOpen()) return true;
        if (!m_shm.open(m_desc.shmName, kShmSize, ShmMapping::Mode::ReadOnly)) return false;

        // Refuse a partition written by an incompatible build rather than
        // decoding it through a mismatched struct layout - the reason the
        // header carries a layout stamp at all (spec/format.md).  Retried on
        // the next poll, which also covers attaching mid-creation.
        if (!detail::ValidateHeader(*GetRingHeader(m_shm.getPtr()))) {
            m_shm.close();
            return false;
        }

        m_readIndex = 0;
        return true;
    }

    CopiedEntry MakeMarker(uint64_t timestamp, uint32_t sequence, std::string_view text) const {
        CopiedEntry marker{};
        marker.timestamp    = timestamp;
        marker.source       = m_desc.sourceId;
        marker.level        = LogLevel::Warn;
        marker.sequence     = sequence;
        marker.message_size = static_cast<uint16_t>(
            std::min(text.size(), sizeof(marker.message)));
        std::memcpy(marker.message, text.data(), marker.message_size);
        return marker;
    }

    PartitionDescriptor m_desc;
    ShmMapping          m_shm;
    uint32_t            m_readIndex = 0;
};

} // namespace

// ─── LogCollectorCore implementation ────────────────────────────────────────────

class LogCollectorCore::Impl {
public:
    explicit Impl(LogReaderOptions options) : m_options(std::move(options)) {
        m_labels.fill("UNKNOWN");
        m_readers.reserve(m_options.partitions.size());

        // shmName of the first descriptor to claim each source id, so a second
        // claim can be named in the warning below.  Empty means unclaimed.
        std::array<std::string_view, 256> claimedBy{};

        for (const auto& p : m_options.partitions) {
            // Both partitions are still drained, but the label table has one
            // slot per source id, so the last label wins and entries from the
            // earlier partition are attributed to the later one.  ParseArgs
            // hands out ids sequentially and cannot produce this; a caller
            // assembling descriptors itself can.
            if (!claimedBy[p.sourceId].empty()) {
                Report(std::format(
                    "source id {} is claimed by both '{}' and '{}'; entries from both will be labelled '{}'",
                    static_cast<unsigned>(p.sourceId), claimedBy[p.sourceId], p.shmName, p.label));
            } else {
                claimedBy[p.sourceId] = p.shmName;
            }

            m_labels[p.sourceId] = p.label;
            m_readers.push_back(std::make_unique<PartitionReader>(p));
        }
    }

    int Run(std::atomic<bool>& running) {
        const auto wallBase   = std::chrono::system_clock::now();
        const auto monoBaseUs = CurrentTimeUs();

        if (m_options.useConsole) {
            Report("writing to console");
        } else {
            if (!m_file.Open(m_options.logFile, m_options.maxSizeBytes, m_options.maxFiles)) {
                Report(std::format("failed to open log file: {}", m_options.logFile));
                return 1;
            }
            Report(std::format("writing to {} (max {} KB, {} rotated files)",
                               m_options.logFile,
                               m_options.maxSizeBytes / 1024,
                               m_options.maxFiles));
        }

        using namespace std::chrono_literals;
        constexpr auto kMinSleep = 100us;
        constexpr auto kMaxSleep = 1000us;
        auto sleepDuration = kMinSleep;

        // Ordering caveat: each iteration sorts only what the partitions held at
        // poll time.  Entries that arrive in a partition after it was polled are
        // emitted in the next batch, so timestamps are ordered within a batch
        // but not strictly across batch boundaries.
        while (running.load(std::memory_order_relaxed)) {
            std::vector<CopiedEntry> batch = PollAllOnce();

            if (batch.empty()) {
                std::this_thread::sleep_for(sleepDuration);
                sleepDuration = std::min(sleepDuration * 2, kMaxSleep);
                continue;
            }

            sleepDuration = kMinSleep;
            MergeBatch(batch);

            for (const auto& line : FormatBatch(batch, wallBase, monoBaseUs)) {
                if (m_options.useConsole) {
                    std::fwrite(line.data(), 1, line.size(), stdout);
                } else {
                    m_file.Write(line);
                }
            }
            if (m_options.useConsole) {
                std::fflush(stdout);
            } else {
                m_file.Flush();
            }
        }

        return 0;
    }

    std::vector<CopiedEntry> PollAllOnce() {
        std::vector<CopiedEntry> batch;
        for (const auto& r : m_readers) {
            auto entries = r->Poll();
            batch.insert(batch.end(), entries.begin(), entries.end());
        }
        return batch;
    }

    std::vector<std::string> FormatBatch(
        const std::vector<CopiedEntry>& batch,
        std::chrono::system_clock::time_point wallBase,
        uint64_t monoBaseUs) const
    {
        std::vector<std::string> lines;
        lines.reserve(batch.size());
        for (const auto& e : batch) {
            lines.push_back(FormatEntry(e, m_labels[e.source], wallBase, monoBaseUs));
        }
        return lines;
    }

private:
    void Report(std::string_view message) const {
        if (m_options.statusSink) m_options.statusSink(message);
    }

    LogReaderOptions m_options;
    // Source id → label, so formatting a batch does not rescan the partition
    // list once per line.
    std::array<std::string_view, 256> m_labels;
    std::vector<std::unique_ptr<PartitionReader>> m_readers;
    LogFile m_file;
};

LogCollectorCore::LogCollectorCore(LogReaderOptions options)
    : m_impl(std::make_unique<Impl>(std::move(options))) {}

LogCollectorCore::~LogCollectorCore() = default;

int LogCollectorCore::Run(std::atomic<bool>& running) {
    return m_impl->Run(running);
}

std::vector<CopiedEntry> LogCollectorCore::PollAllOnce() {
    return m_impl->PollAllOnce();
}

std::vector<std::string> LogCollectorCore::FormatBatch(
    const std::vector<CopiedEntry>& batch,
    std::chrono::system_clock::time_point wallBase,
    uint64_t monoBaseUs) const
{
    return m_impl->FormatBatch(batch, wallBase, monoBaseUs);
}

// ─── CLI argument parsing ─────────────────────────────────────────────────────

std::string_view UsageText() {
    return
        "Options:\n"
        "  --partition <shmName>[:<label>]  Consume a partition (repeatable)\n"
        "  --log-file <path>                Output file (default: shmlog.log)\n"
        "  --max-size <bytes>               Rotate above this size\n"
        "  --max-files <n>                  Rotated generations to keep\n"
        "  --console                        Write to stdout instead of a file\n"
        "  --help                           Show this message\n";
}

ParsedArgs ParseArgs(int argc, char* argv[], LogReaderOptions defaults) {
    ParsedArgs parsed{std::move(defaults), {}, false};

    // Continue past the defaults rather than assuming they are numbered 0..n-1.
    unsigned nextSourceId = 0;
    for (const auto& p : parsed.options.partitions) {
        nextSourceId = std::max(nextSourceId, static_cast<unsigned>(p.sourceId) + 1);
    }

    const auto requireValue = [&](std::string_view flag, int& i) -> const char* {
        if (i + 1 >= argc) {
            parsed.errors.push_back(std::format("{} requires a value", flag));
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            parsed.helpRequested = true;
        } else if (arg == "--console") {
            parsed.options.useConsole = true;
        } else if (arg == "--log-file") {
            if (const char* value = requireValue(arg, i)) parsed.options.logFile = value;
        } else if (arg == "--max-size") {
            if (const char* value = requireValue(arg, i)) {
                // Parsed into a local for the same reason as --max-files below.
                size_t bytes = 0;
                if (!ParseNumber(value, bytes))
                    parsed.errors.push_back(std::format("--max-size: not a size: {}", value));
                else if (bytes == 0)
                    // LogFile rotates whenever the next line would carry it past
                    // the cap, so a cap of zero rotates after every single line -
                    // never what someone typing it meant.
                    parsed.errors.push_back("--max-size: must be greater than 0");
                else
                    parsed.options.maxSizeBytes = bytes;
            }
        } else if (arg == "--max-files") {
            if (const char* value = requireValue(arg, i)) {
                // Parsed into a local so a rejected value leaves the option at
                // its default rather than at a negative count.
                int files = 0;
                if (!ParseNumber(value, files) || files < 0)
                    parsed.errors.push_back(std::format("--max-files: not a count: {}", value));
                else
                    parsed.options.maxFiles = files;
            }
        } else if (arg == "--partition") {
            const char* value = requireValue(arg, i);
            if (!value) continue;
            if (nextSourceId > 255) {
                parsed.errors.push_back("--partition: more than 256 partitions");
                continue;
            }
            const std::string_view spec = value;
            const size_t colon = spec.find(':');
            std::string shmName(spec.substr(0, colon));
            std::string label = (colon == std::string_view::npos)
                                    ? shmName
                                    : std::string(spec.substr(colon + 1));
            // An unnamed partition can never be opened, so reject it here rather
            // than letting every poll fail silently.
            if (shmName.empty()) {
                parsed.errors.push_back(std::format("--partition: empty shm name: {}", value));
                continue;
            }
            // "Name:" - a colon with nothing after it. Fall back to the same
            // label the no-colon form gets, rather than formatting every line
            // from this partition with a blank label column.
            if (label.empty()) {
                label = shmName;
            }
            parsed.options.partitions.push_back({
                .shmName  = std::move(shmName),
                .label    = std::move(label),
                .sourceId = static_cast<uint8_t>(nextSourceId++)});
        } else {
            parsed.errors.push_back(std::format("unknown argument: {}", arg));
        }
    }

    return parsed;
}

} // namespace shmlog
