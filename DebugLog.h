// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/// @file DebugLog.h
/// Bounded O(1) ring-buffer logger, category classifier, and lock-free rate limiter.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace DebugLog {

    /// Semantic categories used to color-code entries once at insertion time.
    enum class Category : uint8_t {
        Info = 0,
        Security,
        Error,
        Perf,
        Network,
        Audio,
        UI
    };

    /// Single formatted log line paired with its pre-classified category.
    struct LogEntry {
        std::string text;
        Category category = Category::Info;
    };

    /// Default maximum number of retained lines in the ring buffer.
    constexpr size_t kDefaultMaxLines = 1500;

    /// Maximum character length of a single formatted message body before clamping.
    constexpr size_t kMaxLineLength = 512;

    /// Classifies a raw log message into a semantic category by precedence.
    Category ClassifyMessage(std::string_view message);

    /// Lock-free rate-limiting state to prevent hot-path or retry-loop log spam.
    struct RateLimitState {
        std::atomic<uint64_t> lastLogTickMs{0};
        std::atomic<uint32_t> suppressedCount{0};

        /// Attempts to acquire permission to emit a log entry at nowMs given intervalMs.
        bool TryAcquire(uint64_t nowMs, uint64_t intervalMs, uint32_t& outSuppressed);
    };

    /// Thread-safe, lazily allocated bounded ring buffer of single-line log entries.
    class LogRingBuffer {
    public:
        /// Constructs a bounded ring buffer with the specified line capacity.
        explicit LogRingBuffer(size_t maxLines = kDefaultMaxLines);

        /// Appends a single-line log entry, overwriting the oldest line when full.
        void Push(std::string_view timestamp, std::string_view message);

        /// Copies entries chronologically only if the buffer generation has changed.
        bool CopySnapshotIfChanged(uint64_t& inOutGeneration, std::vector<LogEntry>& outEntries) const;

        /// Clears all retained log entries without deallocating storage.
        void Clear();

        /// Frees all internal log storage back to the heap when debug mode is disabled.
        void ReleaseMemory();

        /// Returns the current number of stored log entries.
        size_t Size() const;

        /// Returns the maximum line capacity of the ring buffer.
        size_t Capacity() const;

        /// Returns the monotonically increasing mutation counter.
        uint64_t Generation() const;

    private:
        mutable std::mutex m_mutex;
        std::vector<LogEntry> m_entries;
        size_t m_maxLines = kDefaultMaxLines;
        size_t m_head = 0;
        size_t m_count = 0;
        uint64_t m_generation = 0;
    };

    /// Returns the process-wide global debug log ring buffer instance.
    LogRingBuffer& GetGlobalLog();
}
