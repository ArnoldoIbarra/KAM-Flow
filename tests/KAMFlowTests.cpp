// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/**
 * @file KAMFlowTests.cpp
 * @brief Lightweight deterministic CTest unit test suite for KAM-Flow core utilities.
 */

#include "DebugLog.h"
#include "NetworkMessages.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

    struct TestCase {
        const char* name;
        std::function<bool()> fn;
    };

    std::vector<TestCase>& GetRegistry() {
        static std::vector<TestCase> s_tests;
        return s_tests;
    }

    struct Registrar {
        Registrar(const char* name, std::function<bool()> fn) {
            GetRegistry().push_back({name, std::move(fn)});
        }
    };

#define KAM_TEST(name) \
    static bool Test_##name(); \
    static Registrar Reg_##name(#name, Test_##name); \
    static bool Test_##name()

#define EXPECT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return false; \
        } \
    } while (false)

#define EXPECT_EQ(a, b) \
    do { \
        auto _va = (a); \
        auto _vb = (b); \
        if (_va != _vb) { \
            std::printf("  [FAIL] %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            return false; \
        } \
    } while (false)

    // --- MILESTONE A TESTS ---

    KAM_TEST(LogRingBuffer_LazyAllocAndReleaseMemory) {
        DebugLog::LogRingBuffer ring(16);
        EXPECT_EQ(ring.Size(), static_cast<size_t>(0));
        EXPECT_EQ(ring.Capacity(), static_cast<size_t>(16));

        std::vector<DebugLog::LogEntry> snap;
        uint64_t gen = 0;
        EXPECT_TRUE(!ring.CopySnapshotIfChanged(gen, snap));

        ring.Push("[00:00:00.001] ", "Hello KAM-Flow");
        EXPECT_EQ(ring.Size(), static_cast<size_t>(1));
        EXPECT_TRUE(ring.CopySnapshotIfChanged(gen, snap));
        EXPECT_EQ(snap.size(), static_cast<size_t>(1));
        EXPECT_EQ(snap[0].text, std::string("[00:00:00.001] Hello KAM-Flow"));

        ring.ReleaseMemory();
        EXPECT_EQ(ring.Size(), static_cast<size_t>(0));
        EXPECT_TRUE(ring.CopySnapshotIfChanged(gen, snap));
        EXPECT_EQ(snap.size(), static_cast<size_t>(0));
        return true;
    }

    KAM_TEST(LogRingBuffer_WrapAroundPreservesWholeLines) {
        DebugLog::LogRingBuffer ring(4);
        for (int i = 0; i < 7; ++i) {
            ring.Push("[T] ", "Line " + std::to_string(i));
        }
        EXPECT_EQ(ring.Size(), static_cast<size_t>(4));

        std::vector<DebugLog::LogEntry> snap;
        uint64_t gen = 0;
        EXPECT_TRUE(ring.CopySnapshotIfChanged(gen, snap));
        EXPECT_EQ(snap.size(), static_cast<size_t>(4));
        EXPECT_EQ(snap[0].text, std::string("[T] Line 3"));
        EXPECT_EQ(snap[1].text, std::string("[T] Line 4"));
        EXPECT_EQ(snap[2].text, std::string("[T] Line 5"));
        EXPECT_EQ(snap[3].text, std::string("[T] Line 6"));
        return true;
    }

    KAM_TEST(LogRingBuffer_ClampsLongAndMultilineMessages) {
        DebugLog::LogRingBuffer ring(8);
        ring.Push("[TS] ", "First\nSecond\r\nThird\r\n");

        std::string huge(DebugLog::kMaxLineLength + 100, 'X');
        ring.Push("[TS] ", huge);

        std::vector<DebugLog::LogEntry> snap;
        uint64_t gen = 0;
        EXPECT_TRUE(ring.CopySnapshotIfChanged(gen, snap));
        EXPECT_EQ(snap.size(), static_cast<size_t>(2));
        EXPECT_EQ(snap[0].text, std::string("[TS] First Second  Third"));
        EXPECT_EQ(snap[1].text.size(), std::string("[TS] ").size() + DebugLog::kMaxLineLength);
        EXPECT_TRUE(snap[1].text.ends_with("..."));
        return true;
    }

    KAM_TEST(LogRingBuffer_ClassifyMessageCategories) {
        using Cat = DebugLog::Category;
        EXPECT_EQ(DebugLog::ClassifyMessage("[Security] Auth rejected: wrong Master PIN"), Cat::Security);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Network Server] WARNING: failed to bind"), Cat::Error);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Audio Perf] 30s Buf: min=40"), Cat::Perf);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Network Perf] Server UDP 30s"), Cat::Perf);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Input Perf] Mouse 30s"), Cat::Perf);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Session Summary] ended after 10.0s"), Cat::Perf);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Network Client] Connected"), Cat::Network);
        EXPECT_EQ(DebugLog::ClassifyMessage("[KVM] Focus -> LOCAL"), Cat::Network);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Audio] Render started"), Cat::Audio);
        EXPECT_EQ(DebugLog::ClassifyMessage("[KAM-Flow UI] Emergency Override"), Cat::UI);
        EXPECT_EQ(DebugLog::ClassifyMessage("Engine Booting..."), Cat::Info);
        return true;
    }

    KAM_TEST(LogRingBuffer_SnapshotGenerationAndClear) {
        DebugLog::LogRingBuffer ring(8);
        ring.Push("[T] ", "Msg A");

        std::vector<DebugLog::LogEntry> snap;
        uint64_t gen = 0;
        EXPECT_TRUE(ring.CopySnapshotIfChanged(gen, snap));
        EXPECT_EQ(snap.size(), static_cast<size_t>(1));
        EXPECT_TRUE(!ring.CopySnapshotIfChanged(gen, snap));

        ring.Clear();
        EXPECT_EQ(ring.Size(), static_cast<size_t>(0));
        EXPECT_TRUE(ring.CopySnapshotIfChanged(gen, snap));
        EXPECT_EQ(snap.size(), static_cast<size_t>(0));
        return true;
    }

    KAM_TEST(LogRingBuffer_ConcurrentMultiThreadStress) {
        DebugLog::LogRingBuffer ring(64);
        std::atomic<bool> stopReader{false};
        std::atomic<bool> badLine{false};

        std::thread reader([&]() {
            uint64_t gen = 0;
            std::vector<DebugLog::LogEntry> snap;
            while (!stopReader.load(std::memory_order_acquire)) {
                if (ring.CopySnapshotIfChanged(gen, snap)) {
                    for (const auto& entry : snap) {
                        if (entry.text.find('\n') != std::string::npos ||
                            entry.text.find('\r') != std::string::npos ||
                            entry.text.empty()) {
                            badLine.store(true, std::memory_order_relaxed);
                        }
                    }
                }
            }
        });

        std::vector<std::thread> writers;
        for (int t = 0; t < 8; ++t) {
            writers.emplace_back([&, t]() {
                for (int i = 0; i < 200; ++i) {
                    ring.Push("[MT] ", "Thread " + std::to_string(t) + " entry " + std::to_string(i));
                }
            });
        }
        for (auto& w : writers) {
            w.join();
        }
        stopReader.store(true, std::memory_order_release);
        reader.join();

        EXPECT_TRUE(!badLine.load());
        EXPECT_EQ(ring.Size(), static_cast<size_t>(64));
        return true;
    }

    KAM_TEST(RateLimitState_DeterministicWindowAndSuppressedCount) {
        DebugLog::RateLimitState rl;
        uint32_t suppressed = 99;

        EXPECT_TRUE(rl.TryAcquire(0, 5000, suppressed));
        EXPECT_EQ(suppressed, static_cast<uint32_t>(0));

        EXPECT_TRUE(!rl.TryAcquire(100, 5000, suppressed));
        EXPECT_TRUE(!rl.TryAcquire(4999, 5000, suppressed));

        EXPECT_TRUE(rl.TryAcquire(5001, 5000, suppressed));
        EXPECT_EQ(suppressed, static_cast<uint32_t>(2));

        EXPECT_TRUE(rl.TryAcquire(10001, 5000, suppressed));
        EXPECT_EQ(suppressed, static_cast<uint32_t>(0));
        return true;
    }

    KAM_TEST(NetworkMessages_TypeNamesAndHighFrequencyFilter) {
        using MT = Network::MessageType;
        EXPECT_TRUE(std::strcmp(Network::MessageTypeToString(MT::EVENT_KEYBOARD), "KEYBOARD") == 0);
        EXPECT_TRUE(std::strcmp(Network::MessageTypeToString(MT::EVENT_STATE), "STATE") == 0);
        EXPECT_TRUE(std::strcmp(Network::MessageTypeToString(MT::EVENT_AUTH_REJECTED), "AUTH_REJECTED") == 0);
        EXPECT_TRUE(std::strcmp(Network::MessageTypeToString(MT::EVENT_FILE_DECLINE), "FILE_DECLINE") == 0);

        EXPECT_TRUE(Network::IsHighFrequencyMessage(MT::EVENT_MOUSE));
        EXPECT_TRUE(Network::IsHighFrequencyMessage(MT::EVENT_KEYBOARD));
        EXPECT_TRUE(Network::IsHighFrequencyMessage(MT::EVENT_AUDIO_DATA));
        EXPECT_TRUE(Network::IsHighFrequencyMessage(MT::EVENT_MIC_DATA));
        EXPECT_TRUE(Network::IsHighFrequencyMessage(MT::EVENT_HEARTBEAT));

        EXPECT_TRUE(!Network::IsHighFrequencyMessage(MT::EVENT_STATE));
        EXPECT_TRUE(!Network::IsHighFrequencyMessage(MT::EVENT_CLIPBOARD));
        EXPECT_TRUE(!Network::IsHighFrequencyMessage(MT::EVENT_FILE_OFFER));
        EXPECT_TRUE(!Network::IsHighFrequencyMessage(MT::EVENT_AUDIO_FORMAT));
        return true;
    }

    // --- MILESTONE B TESTS ---

    KAM_TEST(DebugLog_MilestoneBMessageCategories) {
        using Cat = DebugLog::Category;
        EXPECT_EQ(DebugLog::ClassifyMessage("[KVM] Focus -> Client 'Laptop' (-1,0) (entryEdge=1)."), Cat::Network);
        EXPECT_EQ(DebugLog::ClassifyMessage("[KVM] WARNING: RouteCursorTransition failed for exitEdge=0; staying LOCAL."), Cat::Error);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Config] Loaded C:\\KAM-Flow\\kamflow.ini (DefaultRole=0, Jitter=80ms)"), Cat::Info);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Config] WARNING: Failed to write config file (err=5)."), Cat::Error);
        EXPECT_EQ(DebugLog::ClassifyMessage("[UI] Initialized (DX11 Hardware, DPI scale: 125%)."), Cat::UI);
        EXPECT_EQ(DebugLog::ClassifyMessage("[UI] WARNING: Failed to register Emergency Hotkey Ctrl+Alt+M (err=1409)."), Cat::Error);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Security] BCryptEncrypt failed (status=0xC000000D, size=32)."), Cat::Security);
        EXPECT_EQ(DebugLog::ClassifyMessage("[FileTransfer] ERROR: Disk write failed for transfer 1001 (err=112)."), Cat::Error);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Audio] Render started: 48000Hz 2ch 32-bit (HW buffer: 480 frames / 10.0ms)"), Cat::Audio);
        return true;
    }

    KAM_TEST(RateLimitState_MultiThreadBurst) {
        DebugLog::RateLimitState rl;
        std::atomic<int> acquiredCount{0};
        constexpr int kThreads = 8;
        constexpr int kCallsPerThread = 250;

        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < kCallsPerThread; ++i) {
                    uint32_t sup = 0;
                    if (rl.TryAcquire(1000 + static_cast<uint64_t>(i), 5000, sup)) {
                        acquiredCount.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }

        // All timestamps lie inside [1000, 1249], so exactly 1 caller can acquire in the 5000ms window.
        EXPECT_EQ(acquiredCount.load(), 1);

        // Next acquisition past the window must account for every suppressed call.
        uint32_t finalSuppressed = 0;
        EXPECT_TRUE(rl.TryAcquire(10000, 5000, finalSuppressed));
        EXPECT_EQ(finalSuppressed, static_cast<uint32_t>(kThreads * kCallsPerThread - 1));
        return true;
    }

    // --- MILESTONE C TESTS ---

    KAM_TEST(VerifyUdpReplayWindow_SequentialZeroLoss) {
        uint32_t maxSeq = 0;
        uint64_t mask = 0;
        Network::UdpReplayStats stats = {};
        for (uint32_t s = 1; s <= 100; ++s) {
            EXPECT_TRUE(Network::VerifyUdpReplayWindow(s, maxSeq, mask, &stats));
        }
        EXPECT_EQ(maxSeq, static_cast<uint32_t>(100));
        EXPECT_EQ(stats.packetsAccepted, static_cast<uint64_t>(100));
        EXPECT_EQ(stats.packetsOutOfOrder, static_cast<uint64_t>(0));
        EXPECT_EQ(stats.sequenceGaps, static_cast<uint64_t>(0));
        EXPECT_EQ(stats.staleDrops, static_cast<uint64_t>(0));
        EXPECT_EQ(stats.duplicateDrops, static_cast<uint64_t>(0));
        return true;
    }

    KAM_TEST(VerifyUdpReplayWindow_OutOfOrderCancelsGap) {
        uint32_t maxSeq = 0;
        uint64_t mask = 0;
        Network::UdpReplayStats stats = {};
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(1, maxSeq, mask, &stats));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(2, maxSeq, mask, &stats));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(5, maxSeq, mask, &stats));
        EXPECT_EQ(stats.sequenceGaps, static_cast<uint64_t>(2));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(3, maxSeq, mask, &stats));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(4, maxSeq, mask, &stats));

        EXPECT_EQ(stats.packetsAccepted, static_cast<uint64_t>(5));
        EXPECT_EQ(stats.packetsOutOfOrder, static_cast<uint64_t>(2));
        EXPECT_EQ(stats.sequenceGaps, static_cast<uint64_t>(0));
        EXPECT_EQ(stats.staleDrops, static_cast<uint64_t>(0));
        EXPECT_EQ(stats.duplicateDrops, static_cast<uint64_t>(0));
        return true;
    }

    KAM_TEST(VerifyUdpReplayWindow_TrueLossAndStaleDrop) {
        uint32_t maxSeq = 0;
        uint64_t mask = 0;
        Network::UdpReplayStats stats = {};
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(1, maxSeq, mask, &stats));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(2, maxSeq, mask, &stats));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(70, maxSeq, mask, &stats));
        EXPECT_EQ(stats.packetsAccepted, static_cast<uint64_t>(3));
        EXPECT_EQ(stats.sequenceGaps, static_cast<uint64_t>(67));

        // Seq 2 now has diff = 68 >= 64 -> must be rejected as stale.
        EXPECT_TRUE(!Network::VerifyUdpReplayWindow(2, maxSeq, mask, &stats));
        EXPECT_EQ(stats.staleDrops, static_cast<uint64_t>(1));
        return true;
    }

    KAM_TEST(VerifyUdpReplayWindow_DuplicateAndZeroRejected) {
        uint32_t maxSeq = 0;
        uint64_t mask = 0;
        Network::UdpReplayStats stats = {};
        EXPECT_TRUE(!Network::VerifyUdpReplayWindow(0, maxSeq, mask, &stats));
        EXPECT_EQ(stats.duplicateDrops, static_cast<uint64_t>(1));

        EXPECT_TRUE(Network::VerifyUdpReplayWindow(1, maxSeq, mask, &stats));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(2, maxSeq, mask, &stats));
        EXPECT_TRUE(!Network::VerifyUdpReplayWindow(2, maxSeq, mask, &stats));
        EXPECT_EQ(stats.duplicateDrops, static_cast<uint64_t>(2));
        EXPECT_EQ(stats.packetsAccepted, static_cast<uint64_t>(2));
        return true;
    }

    KAM_TEST(VerifyUdpReplayWindow_LargeForwardJumpResetsWindow) {
        uint32_t maxSeq = 0;
        uint64_t mask = 0;
        Network::UdpReplayStats stats = {};
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(1, maxSeq, mask, &stats));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(100, maxSeq, mask, &stats));
        EXPECT_EQ(stats.sequenceGaps, static_cast<uint64_t>(98));
        EXPECT_EQ(mask, 1ULL);

        // Seq 100 already received -> duplicate drop; seq 99 within window -> accepted out-of-order.
        EXPECT_TRUE(!Network::VerifyUdpReplayWindow(100, maxSeq, mask, &stats));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(99, maxSeq, mask, &stats));
        EXPECT_EQ(stats.packetsOutOfOrder, static_cast<uint64_t>(1));
        EXPECT_EQ(stats.sequenceGaps, static_cast<uint64_t>(97));
        EXPECT_EQ(stats.duplicateDrops, static_cast<uint64_t>(1));
        return true;
    }

    KAM_TEST(VerifyUdpReplayWindow_NullStatsBackwardCompat) {
        uint32_t maxSeq = 0;
        uint64_t mask = 0;
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(1, maxSeq, mask));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(2, maxSeq, mask));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(4, maxSeq, mask));
        EXPECT_TRUE(Network::VerifyUdpReplayWindow(3, maxSeq, mask));
        EXPECT_TRUE(!Network::VerifyUdpReplayWindow(3, maxSeq, mask));
        return true;
    }

    KAM_TEST(PerfSummaryGating_AndPerfCategory) {
        EXPECT_TRUE(!Network::ShouldEmitPerfSummary(0));
        EXPECT_TRUE(Network::ShouldEmitPerfSummary(1));

        EXPECT_EQ(DebugLog::ClassifyMessage("[Audio Perf] Render 30s: min=45ms avg=80ms max=98ms conceal=1 starv=0 overrun=0 herm=-1/+1 wakeErr=0"),
                  DebugLog::Category::Perf);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Network Perf] Server UDP 30s: rx=1000 lost=2 ooo=4 stale=0 dup=0 decFail=0 txErr=0"),
                  DebugLog::Category::Perf);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Input Perf] Mouse 30s: peakBatch=20, slowSends(>15ms)=0, overflows=0, dropClicks=0, teleports=0"),
                  DebugLog::Category::Perf);
        EXPECT_EQ(DebugLog::ClassifyMessage("[Session Summary] 'LAPTOP' (192.168.1.5) ended after 120.0s | Transport=UDP | UDP rx=500 lost=0 ooo=0 stale=0 dup=0 | TCP stalls=0"),
                  DebugLog::Category::Perf);
        return true;
    }

} // namespace

/**
 * @brief Executes all registered KAMFlowTests unit tests and reports pass/fail totals.
 * @return int 0 if all tests pass, 1 if any test fails.
 */
int main() {
    const auto& tests = GetRegistry();
    int passed = 0;
    int failed = 0;

    std::printf("Running %zu KAMFlowTests...\n", tests.size());
    for (const auto& t : tests) {
        if (t.fn()) {
            std::printf("[PASS] %s\n", t.name);
            ++passed;
        } else {
            std::printf("[FAIL] %s\n", t.name);
            ++failed;
        }
    }
    std::printf("Summary: %d passed, %d failed.\n", passed, failed);
    return (failed == 0) ? 0 : 1;
}
