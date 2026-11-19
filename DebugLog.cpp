// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/**
 * @file DebugLog.cpp
 * @brief Implementation of the bounded O(1) log ring buffer, category classifier, and lock-free rate limiter.
 */

#include "DebugLog.h"
#include <algorithm>
#include <cstring>

namespace DebugLog {

    namespace {
        /**
         * @brief Helper to test whether haystack contains any substring from a fixed list.
         * @param haystack The string view to inspect.
         * @param needles Null-terminated candidate substrings.
         * @param count Number of candidate substrings.
         * @return true if any needle is found in haystack, false otherwise.
         */
        bool ContainsAny(std::string_view haystack, const char* const* needles, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                if (haystack.find(needles[i]) != std::string_view::npos) {
                    return true;
                }
            }
            return false;
        }
    }

    /**
     * @brief Classifies a raw log message into a semantic category by strict precedence.
     * @param message The unformatted or formatted message text (excluding timestamp).
     * @return Category The resolved semantic category for UI color-coding.
     */
    Category ClassifyMessage(std::string_view message) {
        static constexpr const char* kSecurityTokens[] = {
            "[Security]", "SECURITY", "PIN", "Blocked"
        };
        if (ContainsAny(message, kSecurityTokens, sizeof(kSecurityTokens) / sizeof(kSecurityTokens[0]))) {
            return Category::Security;
        }

        static constexpr const char* kErrorTokens[] = {
            "ERROR", "Error", "FATAL", "CRITICAL", "WARNING",
            "failed", "Failed", "dropped", "Dropped", "OVERRUN",
            "depleted", "starvation", "invalidated", "aborted", "Aborted",
            "rejected", "Rejected", "desync", "Desync"
        };
        if (ContainsAny(message, kErrorTokens, sizeof(kErrorTokens) / sizeof(kErrorTokens[0]))) {
            return Category::Error;
        }

        static constexpr const char* kPerfTokens[] = {
            "[Audio Perf]", "[Network Perf]", "[Input Perf]", "[Session Summary]"
        };
        if (ContainsAny(message, kPerfTokens, sizeof(kPerfTokens) / sizeof(kPerfTokens[0]))) {
            return Category::Perf;
        }

        static constexpr const char* kNetworkTokens[] = {
            "[Network", "[KVM]"
        };
        if (ContainsAny(message, kNetworkTokens, sizeof(kNetworkTokens) / sizeof(kNetworkTokens[0]))) {
            return Category::Network;
        }

        static constexpr const char* kAudioTokens[] = {
            "[Audio", "Audio"
        };
        if (ContainsAny(message, kAudioTokens, sizeof(kAudioTokens) / sizeof(kAudioTokens[0]))) {
            return Category::Audio;
        }

        static constexpr const char* kUITokens[] = {
            "[KAM-Flow UI]", "[UI]"
        };
        if (ContainsAny(message, kUITokens, sizeof(kUITokens) / sizeof(kUITokens[0]))) {
            return Category::UI;
        }

        return Category::Info;
    }

    /**
     * @brief Attempts to acquire a rate-limited logging slot in a lock-free manner.
     * Tracks suppressed calls between successful acquisitions so summaries stay accurate.
     * @param nowMs Current monotonic millisecond tick (e.g., GetTickCount64()).
     * @param intervalMs Minimum interval in milliseconds between permitted logs.
     * @param outSuppressed Receives the number of suppressed calls since the previous permitted log.
     * @return true if the caller may emit the log now, false if throttled.
     */
    bool RateLimitState::TryAcquire(uint64_t nowMs, uint64_t intervalMs, uint32_t& outSuppressed) {
        const uint64_t normalizedNow = (nowMs == 0) ? 1 : nowMs;
        uint64_t last = lastLogTickMs.load(std::memory_order_relaxed);

        while (true) {
            if (last != 0 && (normalizedNow < last || (normalizedNow - last) < intervalMs)) {
                suppressedCount.fetch_add(1, std::memory_order_relaxed);
                outSuppressed = 0;
                return false;
            }
            if (lastLogTickMs.compare_exchange_weak(last, normalizedNow,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
                outSuppressed = suppressedCount.exchange(0, std::memory_order_relaxed);
                return true;
            }
        }
    }

    /**
     * @brief Constructs a bounded log ring buffer without eagerly allocating entry storage.
     * @param maxLines Maximum number of lines retained before oldest lines are overwritten.
     */
    LogRingBuffer::LogRingBuffer(size_t maxLines)
        : m_maxLines(maxLines < 1 ? 1 : maxLines) {
    }

    /**
     * @brief Appends a single-line log entry into the ring buffer in O(1) time.
     * Strips trailing line breaks, flattens embedded line breaks to spaces, clamps length,
     * and classifies category before acquiring the buffer mutex.
     * @param timestamp Pre-formatted timestamp prefix (e.g., "[12:00:00.000] ").
     * @param message Body of the log message.
     * @return void
     */
    void LogRingBuffer::Push(std::string_view timestamp, std::string_view message) {
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.remove_suffix(1);
        }

        const Category cat = ClassifyMessage(message);

        char buf[kMaxLineLength + 64];
        const size_t maxTsLen = sizeof(buf) - 1;
        const size_t tsLen = (std::min)(timestamp.size(), maxTsLen);
        if (tsLen > 0) {
            std::memcpy(buf, timestamp.data(), tsLen);
        }

        const size_t remaining = (sizeof(buf) - 1) - tsLen;
        const size_t bodyCap = (std::min)(kMaxLineLength, remaining);
        const bool truncated = message.size() > bodyCap;
        const size_t copyLen = truncated ? bodyCap : message.size();

        for (size_t i = 0; i < copyLen; ++i) {
            char ch = message[i];
            if (ch == '\r' || ch == '\n') {
                ch = ' ';
            }
            buf[tsLen + i] = ch;
        }

        if (truncated && copyLen >= 3) {
            buf[tsLen + copyLen - 3] = '.';
            buf[tsLen + copyLen - 2] = '.';
            buf[tsLen + copyLen - 1] = '.';
        }

        const size_t totalLen = tsLen + copyLen;
        buf[totalLen] = '\0';

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.empty()) {
            m_entries.resize(m_maxLines);
        }

        size_t writeIdx = 0;
        if (m_count < m_maxLines) {
            writeIdx = (m_head + m_count) % m_maxLines;
            ++m_count;
        } else {
            writeIdx = m_head;
            m_head = (m_head + 1) % m_maxLines;
        }

        m_entries[writeIdx].text.assign(buf, totalLen);
        m_entries[writeIdx].category = cat;
        ++m_generation;
    }

    /**
     * @brief Copies a chronological snapshot of stored entries if the buffer changed.
     * @param inOutGeneration Caller's last observed generation; updated on change.
     * @param outEntries Destination vector populated in chronological order when updated.
     * @return true if new entries were copied, false if generation matched.
     */
    bool LogRingBuffer::CopySnapshotIfChanged(uint64_t& inOutGeneration, std::vector<LogEntry>& outEntries) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_generation == inOutGeneration) {
            return false;
        }

        outEntries.resize(m_count);
        for (size_t i = 0; i < m_count; ++i) {
            outEntries[i] = m_entries[(m_head + i) % m_maxLines];
        }
        inOutGeneration = m_generation;
        return true;
    }

    /**
     * @brief Clears all stored entries while retaining allocated slot capacity.
     * @return void
     */
    void LogRingBuffer::Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_head = 0;
        m_count = 0;
        ++m_generation;
    }

    /**
     * @brief Releases all internal ring buffer memory back to the OS heap.
     * @return void
     */
    void LogRingBuffer::ReleaseMemory() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<LogEntry> empty;
        m_entries.swap(empty);
        m_head = 0;
        m_count = 0;
        ++m_generation;
    }

    /**
     * @brief Returns the number of entries currently stored in the ring buffer.
     * @return size_t Current entry count.
     */
    size_t LogRingBuffer::Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_count;
    }

    /**
     * @brief Returns the maximum line capacity configured for this ring buffer.
     * @return size_t Maximum number of entries.
     */
    size_t LogRingBuffer::Capacity() const {
        return m_maxLines;
    }

    /**
     * @brief Returns the monotonically increasing mutation counter for change detection.
     * @return uint64_t Current generation counter.
     */
    uint64_t LogRingBuffer::Generation() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generation;
    }

    /**
     * @brief Retrieves the singleton global debug log ring buffer instance.
     * @return Reference to the global LogRingBuffer.
     */
    LogRingBuffer& GetGlobalLog() {
        static LogRingBuffer s_globalLog(kDefaultMaxLines);
        return s_globalLog;
    }
}
