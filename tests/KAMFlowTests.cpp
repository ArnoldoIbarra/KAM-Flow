// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/**
 * @file KAMFlowTests.cpp
 * @brief Lightweight deterministic CTest unit test suite for KAM-Flow core utilities.
 */

#include "AudioManager.h"
#include "DebugLog.h"
#include "MouseCapture.h"
#include "NetworkMessages.h"
#include "UIManager.h"
#include <atomic>
#include <cmath>
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

    // --- AUDIO BURST & PACING TESTS (v3.5.4) ---

    KAM_TEST(Audio_BufferBoundsOrdering) {
        const float targets[] = { 20.0f, 40.0f, 80.0f, 200.0f };
        for (float t : targets) {
            const Audio::BufferBounds b = Audio::ComputeBufferBounds(t);
            EXPECT_TRUE(b.lowDeadbandMs < t);
            EXPECT_TRUE(t < b.releaseMs);
            EXPECT_TRUE(b.releaseMs < b.highDeadbandMs);
            EXPECT_TRUE(b.highDeadbandMs < b.maxBufferMs);
        }

        const Audio::BufferBounds b40 = Audio::ComputeBufferBounds(40.0f);
        EXPECT_TRUE(std::fabs(b40.lowDeadbandMs - 26.0f) < 0.001f);
        EXPECT_TRUE(std::fabs(b40.releaseMs - 52.0f) < 0.001f);
        EXPECT_TRUE(std::fabs(b40.highDeadbandMs - 79.0f) < 0.001f);
        EXPECT_TRUE(std::fabs(b40.maxBufferMs - 200.0f) < 0.001f);
        return true;
    }

    KAM_TEST(Audio_PlaybackRateDeadbandBitPerfect) {
        EXPECT_TRUE(Audio::ComputePlaybackRate(26.0f, 40.0f) == 1.0);
        EXPECT_TRUE(Audio::ComputePlaybackRate(52.0f, 40.0f) == 1.0);
        EXPECT_TRUE(Audio::ComputePlaybackRate(79.0f, 40.0f) == 1.0);
        EXPECT_TRUE(std::fabs(Audio::ComputePlaybackRate(25.9f, 40.0f) - 0.9980) < 1e-9);
        EXPECT_TRUE(std::fabs(Audio::ComputePlaybackRate(80.0f, 40.0f) - 1.0020) < 1e-9);
        EXPECT_TRUE(std::fabs(Audio::ComputePlaybackRate(119.0f, 40.0f) - 1.0020) < 1e-9);
        return true;
    }

    KAM_TEST(Audio_PlaybackRateTieredBacklogDrain) {
        const double midRate = Audio::ComputePlaybackRate(159.0f, 40.0f);
        EXPECT_TRUE(midRate > 1.0020);
        EXPECT_TRUE(midRate < 1.0075);
        EXPECT_TRUE(std::fabs(midRate - 1.00475) < 1e-6);

        const double maxEdge = Audio::ComputePlaybackRate(199.0f, 40.0f);
        const double maxClamped = Audio::ComputePlaybackRate(270.0f, 40.0f);
        EXPECT_TRUE(std::fabs(maxEdge - 1.0075) < 1e-9);
        EXPECT_TRUE(std::fabs(maxClamped - 1.0075) < 1e-9);
        return true;
    }

    KAM_TEST(Audio_IdleSilenceThresholdAdaptsToPacketDuration) {
        EXPECT_EQ(Audio::ComputeIdleSilenceThresholdMs(0), static_cast<int64_t>(30));
        EXPECT_EQ(Audio::ComputeIdleSilenceThresholdMs(10), static_cast<int64_t>(30));
        EXPECT_EQ(Audio::ComputeIdleSilenceThresholdMs(20), static_cast<int64_t>(35));
        EXPECT_EQ(Audio::ComputeIdleSilenceThresholdMs(22), static_cast<int64_t>(37));
        return true;
    }

    KAM_TEST(NetworkMessages_DefaultPortsValidAndDistinct) {
        constexpr uint16_t serverPort = Network::DEFAULT_SERVER_PORT;
        constexpr uint16_t discoveryPort = Network::DEFAULT_DISCOVERY_PORT;
        constexpr uint16_t transferPort = Network::DEFAULT_FILE_TRANSFER_PORT;

        EXPECT_EQ(serverPort, static_cast<uint16_t>(39240));
        EXPECT_EQ(discoveryPort, static_cast<uint16_t>(39241));
        EXPECT_EQ(transferPort, static_cast<uint16_t>(39242));

        EXPECT_TRUE(serverPort != discoveryPort);
        EXPECT_TRUE(serverPort != transferPort);
        EXPECT_TRUE(discoveryPort != transferPort);

        const uint16_t ports[] = { serverPort, discoveryPort, transferPort };
        for (uint16_t p : ports) {
            EXPECT_TRUE(p >= 1024 && p <= 49151);
            EXPECT_TRUE(p < 8000 || p > 8999);
        }

        Network::UDPBeaconPayload beacon{};
        beacon.tcpPort = serverPort;
        EXPECT_EQ(beacon.tcpPort, static_cast<uint16_t>(39240));

        Network::FileAcceptPayload accept{};
        accept.tcpPort = transferPort;
        EXPECT_EQ(accept.tcpPort, static_cast<uint16_t>(39242));
        return true;
    }

    // --- MILESTONE 1 TESTS (v3.5.5: UDP STARTUP & AUDIO SWITCHOVER) ---

    KAM_TEST(UdpHandshake_RetryScheduleRampsToHeartbeat) {
        EXPECT_EQ(Network::ComputeUdpHandshakeRetryDelayMs(0), static_cast<uint32_t>(150));
        EXPECT_EQ(Network::ComputeUdpHandshakeRetryDelayMs(1), static_cast<uint32_t>(200));
        EXPECT_EQ(Network::ComputeUdpHandshakeRetryDelayMs(2), static_cast<uint32_t>(350));
        EXPECT_EQ(Network::ComputeUdpHandshakeRetryDelayMs(3), static_cast<uint32_t>(600));
        EXPECT_EQ(Network::ComputeUdpHandshakeRetryDelayMs(4), static_cast<uint32_t>(2000));
        EXPECT_EQ(Network::ComputeUdpHandshakeRetryDelayMs(5), static_cast<uint32_t>(2000));
        EXPECT_EQ(Network::ComputeUdpHandshakeRetryDelayMs(6), static_cast<uint32_t>(2000));
        return true;
    }

    KAM_TEST(UdpHandshake_DeferTcpAudioFallbackOnlyDuringGraceWindow) {
        EXPECT_TRUE(Network::ShouldDeferTcpAudioFallback(false, true, 0));
        EXPECT_TRUE(Network::ShouldDeferTcpAudioFallback(false, true, 499));
        EXPECT_TRUE(!Network::ShouldDeferTcpAudioFallback(false, true, 500));
        EXPECT_TRUE(!Network::ShouldDeferTcpAudioFallback(false, true, 1000));
        EXPECT_TRUE(!Network::ShouldDeferTcpAudioFallback(true, true, 0));
        EXPECT_TRUE(!Network::ShouldDeferTcpAudioFallback(false, false, 0));
        return true;
    }

    KAM_TEST(ServerAudio_DropsTcpAudioOnceUdpActive) {
        EXPECT_TRUE(Network::ShouldAcceptTcpAudioPacket(false));
        EXPECT_TRUE(!Network::ShouldAcceptTcpAudioPacket(true));
        return true;
    }

    // --- MILESTONE 2 TESTS (v3.5.5: RECONNECT POLICY & PRE-AUTH TIMEOUT) ---

    KAM_TEST(ReconnectPolicy_WakeOverridesBackoffToOneSecond) {
        using RP = Network::ReconnectPolicy;
        // Stable session + wake -> 1s
        EXPECT_EQ(RP::NextDelayAfterDisconnect(20, true, 120000, true), 1);
        // Short-lived auto-reconnect session + wake -> 1s
        EXPECT_EQ(RP::NextDelayAfterDisconnect(10, true, 2700, true), 1);
        // Connect failure + wake -> 1s
        EXPECT_EQ(RP::NextDelayAfterConnectFailure(20, true), 1);
        return true;
    }

    KAM_TEST(ReconnectPolicy_EscalationAndCap) {
        using RP = Network::ReconnectPolicy;
        // Escalation across repeated short-lived disconnects: 1 -> 5 -> 10 -> 20 -> 30 -> 30
        int d = RP::NextDelayAfterDisconnect(1, true, 2700, false);
        EXPECT_EQ(d, 5);
        d = RP::NextDelayAfterDisconnect(d, true, 2700, false);
        EXPECT_EQ(d, 10);
        d = RP::NextDelayAfterDisconnect(d, true, 2700, false);
        EXPECT_EQ(d, 20);
        d = RP::NextDelayAfterDisconnect(d, true, 2700, false);
        EXPECT_EQ(d, 30);
        d = RP::NextDelayAfterDisconnect(d, true, 2700, false);
        EXPECT_EQ(d, 30);

        // Escalation across repeated connect failures: 1 -> 5 -> 10 -> 20 -> 30 -> 30
        int c = RP::NextDelayAfterConnectFailure(1, false);
        EXPECT_EQ(c, 5);
        c = RP::NextDelayAfterConnectFailure(c, false);
        EXPECT_EQ(c, 10);
        c = RP::NextDelayAfterConnectFailure(c, false);
        EXPECT_EQ(c, 20);
        c = RP::NextDelayAfterConnectFailure(c, false);
        EXPECT_EQ(c, 30);
        c = RP::NextDelayAfterConnectFailure(c, false);
        EXPECT_EQ(c, 30);

        // Stable session (>30s) resets backoff to initial 5s
        EXPECT_EQ(RP::NextDelayAfterDisconnect(30, true, 30001, false), 5);
        // Manual (non-auto) short session starts at 5s
        EXPECT_EQ(RP::NextDelayAfterDisconnect(30, false, 1500, false), 5);
        return true;
    }

    KAM_TEST(NetworkMessages_PreAuthTimeoutAccommodatesWifiRetransmit) {
        EXPECT_EQ(Network::PRE_AUTH_RECV_TIMEOUT_MS, static_cast<uint32_t>(5000));
        return true;
    }

    // --- MILESTONE 3 TESTS (v3.5.5: EDGE-RETURN DEBOUNCE) ---

    KAM_TEST(EdgeReturnDebounce_SuppressesBurstAndAllowsReset) {
        constexpr uint64_t t0 = 10000;
        EXPECT_TRUE(Network::CanSendReturnControl(t0, 0));
        EXPECT_TRUE(!Network::CanSendReturnControl(t0 + 27, t0));
        EXPECT_TRUE(!Network::CanSendReturnControl(t0 + 199, t0));
        EXPECT_TRUE(Network::CanSendReturnControl(t0 + 200, t0));
        // Resetting lastSentMs on EVENT_SYNC_CURSOR immediately re-arms edge return.
        EXPECT_TRUE(Network::CanSendReturnControl(t0 + 10, 0));
        return true;
    }

    // --- MOUSE TRANSITION & ANCHOR STABILITY TESTS ---

    KAM_TEST(MouseHook_PendingTransitionSwallowsAndPreservesFlag) {
        using Action = Input::MouseHookAction;

        // During async transition (isRemote=false, transitionPending=true),
        // the hook must swallow events and MUST NOT clear isTransitionedOut.
        EXPECT_EQ(Input::ResolveMouseHookAction(false, true), Action::SwallowOnly);
        EXPECT_EQ(Input::ResolveMouseHookAction(true, true), Action::SwallowOnly);
        EXPECT_EQ(Input::ResolveMouseHookAction(true, false), Action::ForwardRemote);
        EXPECT_EQ(Input::ResolveMouseHookAction(false, false), Action::PassThrough);

        EXPECT_TRUE(!Input::ShouldClearTransitionedOut(false, true, true));
        EXPECT_TRUE(!Input::ShouldClearTransitionedOut(true, true, true));
        EXPECT_TRUE(!Input::ShouldClearTransitionedOut(true, false, true));
        EXPECT_TRUE(!Input::ShouldClearTransitionedOut(false, false, false));
        EXPECT_TRUE(Input::ShouldClearTransitionedOut(false, false, true));
        return true;
    }

    KAM_TEST(MouseDelta_DriftedCursorHasZeroBiasAndTriggersReanchor) {
        // Exact reproduction: monitor center (960, 540), OS cursor drifted to (870, 532).
        const auto verticalMove = Input::EvaluateRemoteMouseDelta(870, 537, 870, 532, 960, 540);
        EXPECT_EQ(verticalMove.dx, 0);
        EXPECT_EQ(verticalMove.dy, 5);
        EXPECT_TRUE(!verticalMove.discardTeleport);
        EXPECT_TRUE(verticalMove.fromDrift);
        EXPECT_TRUE(verticalMove.needsReanchor);

        const auto rightMove = Input::EvaluateRemoteMouseDelta(880, 532, 870, 532, 960, 540);
        EXPECT_EQ(rightMove.dx, 10);
        EXPECT_EQ(rightMove.dy, 0);
        EXPECT_TRUE(!rightMove.discardTeleport);
        EXPECT_TRUE(rightMove.fromDrift);
        EXPECT_TRUE(rightMove.needsReanchor);

        // Normal centered movement stays within drift and flick thresholds.
        const auto normalMove = Input::EvaluateRemoteMouseDelta(975, 520, 960, 540, 960, 540);
        EXPECT_EQ(normalMove.dx, 15);
        EXPECT_EQ(normalMove.dy, -20);
        EXPECT_TRUE(!normalMove.discardTeleport);
        EXPECT_TRUE(!normalMove.fromDrift);
        EXPECT_TRUE(!normalMove.needsReanchor);

        // Fast flick (>150px, <=1500px) preserves delta and triggers re-anchor.
        const auto fastFlick = Input::EvaluateRemoteMouseDelta(1160, 540, 960, 540, 960, 540);
        EXPECT_EQ(fastFlick.dx, 200);
        EXPECT_EQ(fastFlick.dy, 0);
        EXPECT_TRUE(!fastFlick.discardTeleport);
        EXPECT_TRUE(!fastFlick.fromDrift);
        EXPECT_TRUE(fastFlick.needsReanchor);

        // External teleport (>1500px) discards delta and re-anchors.
        const auto teleport = Input::EvaluateRemoteMouseDelta(2500, 540, 960, 540, 960, 540);
        EXPECT_TRUE(teleport.discardTeleport);
        EXPECT_TRUE(!teleport.fromDrift);
        EXPECT_TRUE(teleport.needsReanchor);

        // Negative virtual-desktop monitor coordinates behave identically.
        const auto negMon = Input::EvaluateRemoteMouseDelta(-1038, -544, -1050, -540, -960, -540);
        EXPECT_EQ(negMon.dx, 12);
        EXPECT_EQ(negMon.dy, -4);
        EXPECT_TRUE(!negMon.discardTeleport);
        EXPECT_TRUE(negMon.fromDrift);
        EXPECT_TRUE(negMon.needsReanchor);
        return true;
    }

    KAM_TEST(MouseReanchor_VirtualDesktopCoordRoundingAndClamping) {
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(0, 0, 1920), 0);
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(960, 0, 1920), 32785);
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(1919, 0, 1920), 65535);

        // Secondary monitor to the left of primary (-1920..1919, width 3840).
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(-1920, -1920, 3840), 0);
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(-960, -1920, 3840), 16388);
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(1919, -1920, 3840), 65535);

        // Out-of-bounds clamping and degenerate sizes.
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(-500, 0, 1920), 0);
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(5000, 0, 1920), 65535);
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(100, 0, 1), 0);
        EXPECT_EQ(Input::ComputeAbsoluteVirtualCoord(100, 0, 0), 0);
        return true;
    }

    // --- UI WINDOW VISIBILITY & TRAY RECOVERY TESTS (v3.5.7) ---

    KAM_TEST(UI_WindowBoundsClampingAndTitleBarVisibility) {
        // Standard 1080p work area (40px bottom taskbar)
        constexpr int wL = 0, wT = 0, wR = 1920, wB = 1040;

        // 1. Normal on-screen window stays visible
        const UI::WindowBounds normal{100, 100, 841, 643};
        EXPECT_TRUE(UI::IsWindowTitleBarVisibleInWorkArea(normal, wL, wT, wR, wB));

        // 2. Windows iconic sentinel (-32000, -32000) is off-screen and clamped into work area
        const UI::WindowBounds iconic{-32000, -32000, 841, 643};
        EXPECT_TRUE(!UI::IsWindowTitleBarVisibleInWorkArea(iconic, wL, wT, wR, wB));
        const UI::WindowBounds clampedIconic = UI::ClampWindowBoundsToWorkArea(iconic, wL, wT, wR, wB);
        EXPECT_EQ(clampedIconic.x, 0);
        EXPECT_EQ(clampedIconic.y, 0);
        EXPECT_EQ(clampedIconic.width, 841);
        EXPECT_EQ(clampedIconic.height, 643);
        EXPECT_TRUE(UI::IsWindowTitleBarVisibleInWorkArea(clampedIconic, wL, wT, wR, wB));

        // 3. Unplugged secondary monitor to the right (x=2560)
        const UI::WindowBounds unpluggedRight{2560, 150, 600, 500};
        EXPECT_TRUE(!UI::IsWindowTitleBarVisibleInWorkArea(unpluggedRight, wL, wT, wR, wB));
        const UI::WindowBounds clampedRight = UI::ClampWindowBoundsToWorkArea(unpluggedRight, wL, wT, wR, wB);
        EXPECT_EQ(clampedRight.x, 1920 - 600);
        EXPECT_EQ(clampedRight.y, 150);
        EXPECT_TRUE(UI::IsWindowTitleBarVisibleInWorkArea(clampedRight, wL, wT, wR, wB));

        // 4. Title bar above top edge or below taskbar is rejected and clamped back
        EXPECT_TRUE(!UI::IsWindowTitleBarVisibleInWorkArea({200, -20, 600, 480}, wL, wT, wR, wB));
        EXPECT_TRUE(!UI::IsWindowTitleBarVisibleInWorkArea({200, 1020, 600, 480}, wL, wT, wR, wB));
        const UI::WindowBounds clampedBottom = UI::ClampWindowBoundsToWorkArea({200, 1020, 600, 480}, wL, wT, wR, wB);
        EXPECT_EQ(clampedBottom.y, 1040 - 480);
        EXPECT_TRUE(UI::IsWindowTitleBarVisibleInWorkArea(clampedBottom, wL, wT, wR, wB));

        // 5. Degenerate 0x0 size resets to 450x480 default
        const UI::WindowBounds clampedZero = UI::ClampWindowBoundsToWorkArea({50, 50, 0, 0}, wL, wT, wR, wB);
        EXPECT_EQ(clampedZero.width, 450);
        EXPECT_EQ(clampedZero.height, 480);

        // 6. Oversized window clamps to small work area (800x600)
        const UI::WindowBounds clampedSmall = UI::ClampWindowBoundsToWorkArea({50, 50, 1200, 900}, 0, 0, 800, 600);
        EXPECT_EQ(clampedSmall.x, 0);
        EXPECT_EQ(clampedSmall.y, 0);
        EXPECT_EQ(clampedSmall.width, 800);
        EXPECT_EQ(clampedSmall.height, 600);

        // 7. Negative-coordinate secondary monitor work area (-1920..0, 0..1040)
        const UI::WindowBounds negMonOn{-1600, 120, 500, 400};
        EXPECT_TRUE(UI::IsWindowTitleBarVisibleInWorkArea(negMonOn, -1920, 0, 0, 1040));
        const UI::WindowBounds negMonOff{-2400, 120, 500, 400};
        EXPECT_TRUE(!UI::IsWindowTitleBarVisibleInWorkArea(negMonOff, -1920, 0, 0, 1040));
        const UI::WindowBounds clampedNeg = UI::ClampWindowBoundsToWorkArea(negMonOff, -1920, 0, 0, 1040);
        EXPECT_EQ(clampedNeg.x, -1920);
        EXPECT_EQ(clampedNeg.y, 120);
        EXPECT_TRUE(UI::IsWindowTitleBarVisibleInWorkArea(clampedNeg, -1920, 0, 0, 1040));
        return true;
    }

    KAM_TEST(UI_TrayRecoveryActionFallbackPolicy) {
        using Act = UI::TrayRecoveryAction;
        // Not hidden in tray -> None regardless of Shell_NotifyIcon results
        EXPECT_EQ(UI::ResolveTrayRecoveryAction(false, false, false), Act::None);
        EXPECT_EQ(UI::ResolveTrayRecoveryAction(false, true, false), Act::None);
        EXPECT_EQ(UI::ResolveTrayRecoveryAction(false, false, true), Act::None);
        EXPECT_EQ(UI::ResolveTrayRecoveryAction(false, true, true), Act::None);

        // Hidden in tray with either NIM_MODIFY or NIM_ADD succeeding -> IconValid
        EXPECT_EQ(UI::ResolveTrayRecoveryAction(true, true, false), Act::IconValid);
        EXPECT_EQ(UI::ResolveTrayRecoveryAction(true, false, true), Act::IconValid);
        EXPECT_EQ(UI::ResolveTrayRecoveryAction(true, true, true), Act::IconValid);

        // Hidden in tray when both NIM_MODIFY and NIM_ADD fail -> RestoreWindow
        EXPECT_EQ(UI::ResolveTrayRecoveryAction(true, false, false), Act::RestoreWindow);
        return true;
    }

    KAM_TEST(Audio_ResolvePlaybackRateLiveSliderTargetSeek) {
        // Steady-state (not seeking): behaves identically to ComputePlaybackRate
        bool seeking = false;
        EXPECT_TRUE(Audio::ResolvePlaybackRate(52.0f, 80.0f, seeking) == 1.0);
        EXPECT_TRUE(Audio::ResolvePlaybackRate(143.0f, 80.0f, seeking) == 1.0);
        EXPECT_TRUE(std::fabs(Audio::ResolvePlaybackRate(51.9f, 80.0f, seeking) - 0.9980) < 1e-9);
        EXPECT_TRUE(std::fabs(Audio::ResolvePlaybackRate(143.1f, 80.0f, seeking) - 1.0020) < 1e-9);
        EXPECT_TRUE(!seeking);

        // Live upward slider change (40ms -> 80ms) when buffer sits at 52ms:
        // Must seek upward at 0.9940x even though 52ms is inside 80ms's steady-state deadband.
        seeking = true;
        EXPECT_TRUE(std::fabs(Audio::ResolvePlaybackRate(52.0f, 80.0f, seeking) - 0.9940) < 1e-9);
        EXPECT_TRUE(seeking);
        EXPECT_TRUE(std::fabs(Audio::ResolvePlaybackRate(79.9f, 80.0f, seeking) - 0.9940) < 1e-9);
        EXPECT_TRUE(seeking);
        EXPECT_TRUE(Audio::ResolvePlaybackRate(80.0f, 80.0f, seeking) == 1.0);
        EXPECT_TRUE(!seeking);

        // Live downward slider change (160ms -> 40ms):
        // Uses at least 1.0060x (or faster ComputePlaybackRate if >119ms bloated), then locks into [40, 55]ms.
        seeking = true;
        EXPECT_TRUE(std::fabs(Audio::ResolvePlaybackRate(110.0f, 40.0f, seeking) - 1.0060) < 1e-9);
        EXPECT_TRUE(seeking);
        EXPECT_TRUE(std::fabs(Audio::ResolvePlaybackRate(220.0f, 40.0f, seeking) - 1.0075) < 1e-9);
        EXPECT_TRUE(seeking);
        EXPECT_TRUE(Audio::ResolvePlaybackRate(55.0f, 40.0f, seeking) == 1.0);
        EXPECT_TRUE(!seeking);
        return true;
    }

    KAM_TEST(Audio_ConcealmentWarningClassifiedAsError) {
        using Cat = DebugLog::Category;
        EXPECT_EQ(DebugLog::ClassifyMessage(
                      "[Audio] WARNING: Transient jitter underrun concealed (buf=0ms, target=80ms) — raise Jitter Buffer if persistent."),
                  Cat::Error);
        EXPECT_EQ(DebugLog::ClassifyMessage(
                      "[Audio] WARNING: Transient mic jitter underrun concealed (target=80ms) — raise Jitter Buffer if persistent."),
                  Cat::Error);
        EXPECT_EQ(DebugLog::ClassifyMessage(
                      "[Audio Perf] 30s Buf: min=10 avg=82 max=94ms (target=80ms) | Conceal=1 Starv=0 Overrun=0 | Hermite=-1/+0 | Jitter>25ms=0 Clip=0"),
                  Cat::Perf);
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
