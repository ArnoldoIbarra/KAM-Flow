// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/**
 * @file AudioManager_Win.cpp
 * @brief Implementation of the AudioManager using Windows Core Audio (WASAPI).
 * Handles device enumeration, format negotiation, and stream initialization.
 */

#include "AudioManager.h"
#include "AudioCodec.h"
#include "DebugLog.h"
#include "StateManager.h"
#include "NetworkMessages.h"
#include "NetworkClient.h"
#include "NetworkServer.h"
#include "UIManager.h"
#include <windows.h>

// Prevent global namespace collision between Windows SDK 'Network' enum and KAM-Flow's 'Network' namespace
#define Network Win32_Network
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#undef Network

#include <iostream>
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <memory>
#include <shared_mutex>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

namespace Audio {

    /// PIMPL: The WASAPI device enumerator instance.
    IMMDeviceEnumerator* pEnumerator = nullptr;
    static bool g_comInitializedByAudio = false;

    /// RAII helper for safely initializing and unitializing COM on background threads.
    struct CoInitRAII {
        CoInitRAII() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
        ~CoInitRAII() { CoUninitialize(); }
    };

    /**
     * @brief Registers the calling thread with Windows MMCSS for guaranteed real-time scheduling.
     * MMCSS reserves a CPU time slice that no game or foreground app can steal.
     * Falls back to THREAD_PRIORITY_TIME_CRITICAL if MMCSS registration fails.
     * @param taskName The MMCSS task category (e.g., "Pro Audio").
     * @param callerName A debug label for logging which thread registered.
     * @return The MMCSS task handle (must be reverted on thread exit), or NULL on failure.
     */
    HANDLE RegisterMMCSS(const wchar_t* taskName, const char* callerName) {
        DWORD taskIndex = 0;
        HANDLE hTask = AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
        if (hTask) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] %s: MMCSS 'Pro Audio' registered (index=%u). Thread is scheduler-protected.", callerName, taskIndex);
        } else {
            // Fallback: MMCSS service may be disabled on exotic Windows configurations
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            if (State::globalDebugMode) UI::LogDebug("[Audio] %s: MMCSS registration failed (err=%lu). Falling back to TIME_CRITICAL.", callerName, GetLastError());
        }
        return hTask;
    }

    /**
     * @brief Emits a rate-limited WASAPI HRESULT diagnostic message when debug mode is enabled.
     * @param rate Per-site rate-limit state preventing device-recovery retry spam.
     * @param prefix Human-readable failure description.
     * @param hr Failing HRESULT code.
     * @return void
     */
    static void LogHResultRateLimited(DebugLog::RateLimitState& rate, const char* prefix, HRESULT hr) {
        if (!State::globalDebugMode) return;
        uint32_t suppressed = 0;
        if (rate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) {
                snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
            }
            UI::LogDebug("%s (0x%08X%s).", prefix, static_cast<uint32_t>(hr), supBuf);
        }
    }

    /**
     * @brief Atomically updates target to the maximum of its current value and value.
     * @param target Atomic maximum accumulator.
     * @param value Candidate value to incorporate.
     * @return void
     */
    static inline void AtomicUpdateMax(std::atomic<uint32_t>& target, uint32_t value) {
        uint32_t cur = target.load(std::memory_order_relaxed);
        while (value > cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
        }
    }

    /**
     * @brief Emits a 30-second WASAPI capture anomaly summary when discontinuities, slips, or send stalls occurred.
     * @param label Thread label for diagnostic output.
     * @param intervalStart Start timestamp of the current 30-second window (reset when elapsed).
     * @param stats Accumulated capture timing, packet-size, and discontinuity telemetry (reset when elapsed).
     * @return void
     */
    static void MaybeLogCapturePerf30s(const char* label,
                                      std::chrono::steady_clock::time_point& intervalStart,
                                      CaptureTimingStats& stats) {
        auto now = std::chrono::steady_clock::now();
        if ((now - intervalStart) < std::chrono::seconds(30)) {
            return;
        }
        if (State::globalDebugMode && stats.HasAnomaly()) {
            UI::LogDebug("[Audio Perf] %s 30s: tx=%u (real=%u sil=%u) | maxGap=%ums (>%ums=%u) | maxSend=%ums | maxPkt=%uB | disc=%u slips=%u",
                         label, stats.realPackets + stats.silencePackets, stats.realPackets, stats.silencePackets,
                         stats.maxSendGapMs, kCaptureSendGapSpikeMs, stats.sendGapSpikes,
                         stats.maxSendCallMs, stats.maxPacketBytes, stats.discontinuities, stats.silenceClockSlips);
        }
        intervalStart = now;
        stats.Reset();
    }

    // --- CLIENT CAPTURE THREAD ---
    /// The background thread that captures and streams client audio.
    std::thread g_captureThread;
    std::atomic<bool> g_isCaptureRunning(false);
    
    // --- CLIENT MUTE STATE ---
    /// Handle to the client's master volume control.
    IAudioEndpointVolume* g_pClientEndpointVolume = nullptr;
    BOOL g_bOriginalMuteState = FALSE;

    // --- SERVER RENDER THREAD ---
    /// The background thread that mixes and plays all incoming client audio.
    std::thread g_renderThread;
    std::atomic<bool> g_isRenderRunning(false);

    /// Handle to manually trigger the WASAPI event to unblock thread instantly on shutdown.
    HANDLE g_hRenderEvent = NULL;

    // --- SERVER RENDER STATE ---
    AudioFormat g_serverRenderFormat;
    std::atomic<bool> g_hasRenderFormat(false);
    std::atomic<uint32_t> g_serverRenderRate{0};
    std::atomic<uint16_t> g_serverRenderBits{0};
    std::atomic<uint16_t> g_serverRenderChannels{0};
    std::mutex g_formatBroadcastMutex;

    /**
     * @brief Serializes an internal AudioFormat struct into the packed 12-byte wire AudioFormatPayload.
     * @param fmt Source audio format and negotiated codec.
     * @return Packed AudioFormatPayload ready for EVENT_AUDIO_FORMAT transmission.
     */
    static Network::AudioFormatPayload MakeAudioFormatPayload(const AudioFormat& fmt) {
        Network::AudioFormatPayload payload = {};
        payload.sampleRate = fmt.sampleRate;
        payload.bitDepth = fmt.bitDepth;
        payload.channels = fmt.channels;
        payload.codec = fmt.codec;
        return payload;
    }

    /**
     * @brief A low-overhead circular buffer for audio PCM data.
     * Prevents O(N) std::vector erasure shifting on the high-priority render thread.
     */
    class ByteRingBuffer {
    private:
        std::vector<uint8_t> m_buffer;
        size_t m_capacity;
        size_t m_head;
        size_t m_tail;
        size_t m_size;

    public:
        ByteRingBuffer(size_t capacity = 1048576 * 2) // 2MB default capacity
            : m_buffer(capacity, 0), m_capacity(capacity), m_head(0), m_tail(0), m_size(0) {}

        void Clear() {
            m_head = 0;
            m_tail = 0;
            m_size = 0;
        }

        size_t Size() const { return m_size; }

        void Push(const uint8_t* data, size_t size) {
            if (size == 0 || size > m_capacity) return;
            
            if (m_size + size > m_capacity) {
                size_t excess = (m_size + size) - m_capacity;
                Drop(excess);
            }
            
            size_t firstPart = (std::min)(size, m_capacity - m_tail);
            std::memcpy(m_buffer.data() + m_tail, data, firstPart);
            
            if (firstPart < size) {
                std::memcpy(m_buffer.data(), data + firstPart, size - firstPart);
                m_tail = size - firstPart;
            } else {
                m_tail = (m_tail + firstPart) % m_capacity;
            }
            m_size += size;
        }

        void Peek(uint8_t* outData, size_t size) const {
            if (size == 0 || size > m_size) return;
            size_t firstPart = (std::min)(size, m_capacity - m_head);
            std::memcpy(outData, m_buffer.data() + m_head, firstPart);
            if (firstPart < size) {
                std::memcpy(outData + firstPart, m_buffer.data(), size - firstPart);
            }
        }

        void Drop(size_t size) {
            if (size == 0) return;
            if (size >= m_size) {
                Clear();
                return;
            }
            m_head = (m_head + size) % m_capacity;
            m_size -= size;
        }

        void Pop(uint8_t* outData, size_t size) {
            if (size == 0 || size > m_size) return;
            Peek(outData, size);
            Drop(size);
        }
    };

    /// A thread-safe queue for a single client's audio data.
    struct ClientAudioQueue {
        ByteRingBuffer buffer;
        std::mutex mtx;
        std::atomic<bool> isEnabled{true};
        std::atomic<float> volume{1.0f};
        std::atomic<uint32_t> overrunCount{0};
        std::atomic<uint32_t> rxMaxGapMs{0};
        std::atomic<uint32_t> pendingAdaptRxGapMs{0};
        std::atomic<uint32_t> rxGapSpikes{0};
        std::atomic<uint32_t> rxMaxPktBytes{0};
        std::atomic<uint32_t> rxPackets{0};
        std::atomic<uint32_t> opusFecCount{0};
        std::atomic<uint32_t> opusPlcCount{0};
        std::atomic<uint32_t> opusStaleCount{0};
        bool isBuffering = true;
        bool wasBuffering = false;  // Tracks previous buffering state for silence->audio fade-in
        bool needsFadeIn = false;   // Signals smooth crossfade ramp after partial fills, silence, or underflows
        int starvationCount = 0;
        float lastFrameFloat[8] = {0.0f};       // Last valid frame per channel for smooth underflow decay & crossfade
        int16_t lastFrameInt16[8] = {0};        // Int16 counterpart for ClientMicRenderThreadLoop if 16-bit
        double resamplePhase = 0.0;             // Fractional phase for edge-only 4-point Hermite drift correction
        float prevFramesFloat[3][8] = {{0.0f}}; // Prior 3 frames for C1-continuous 4-point Hermite interpolation
        bool hasHermiteHistory = false;
        float appliedTargetMs = -1.0f;          // Last observed jitter target (detects live slider adjustments)
        bool seekingTarget = false;             // True while converging smoothly onto an updated slider target
        std::chrono::steady_clock::time_point lastRxTime{};
        bool hasRxTime = false;
        uint8_t lastRxCodec = 255;
        AudioCodec::OpusStreamDecoder opusDecoder;
        std::vector<uint8_t> decodePcmScratch;

        void ResetStreamState() {
            buffer.Clear();
            isBuffering = true;
            wasBuffering = false;
            needsFadeIn = false;
            starvationCount = 0;
            resamplePhase = 0.0;
            hasHermiteHistory = false;
            appliedTargetMs = -1.0f;
            seekingTarget = false;
            hasRxTime = false;
            pendingAdaptRxGapMs.store(0, std::memory_order_relaxed);
            lastRxCodec = 255;
            opusDecoder.Reset(true);
            std::fill(std::begin(lastFrameFloat), std::end(lastFrameFloat), 0.0f);
            std::fill(std::begin(lastFrameInt16), std::end(lastFrameInt16), static_cast<int16_t>(0));
            for (int i = 0; i < 3; ++i) {
                std::fill(std::begin(prevFramesFloat[i]), std::end(prevFramesFloat[i]), 0.0f);
            }
        }
    };
    std::map<uintptr_t, std::shared_ptr<ClientAudioQueue>> g_clientAudioQueues;
    std::shared_mutex g_queuesMutex; // Protects the map itself during additions/removals.

    /**
     * @brief Encodes active audio into 10ms Opus frames or splits raw PCM into MTU-safe packets.
     * Both capture paths (client loopback and server mic) share this wire packet emitter.
     * @param format Active WASAPI capture stream format.
     * @param useOpus True when Opus encoding should be used (if format-supported).
     * @param isVoice True for VOIP microphone tuning, false for desktop audio music/multimedia tuning.
     * @param isSilence True when transmitting pure digital silence (header-only 8-byte fast path).
     * @param numFrames Number of PCM frames represented or contained in pcmData.
     * @param pcmData Interleaved PCM buffer (optional when isSilence is true).
     * @param encoder Per-thread Opus encoder and monotonic sequence generator.
     * @param activeOpusState Tracks whether the previous packet used Opus so live toggling resets FIFO cleanly.
     * @param encodedScratch Reusable vector holding newly produced Opus packets.
     * @param txScratch Reusable packet assembly buffer holding AudioPacketHeader + payload.
     * @param sendPacketFn Callable receiving (const void* packetBytes, UINT32 packetSize).
     * @return void
     */
    template <typename SendPacketFn>
    static void EmitAudioStreamFrames(const AudioFormat& format,
                                      bool useOpus,
                                      bool isVoice,
                                      bool isSilence,
                                      uint16_t numFrames,
                                      const uint8_t* pcmData,
                                      AudioCodec::OpusStreamEncoder& encoder,
                                      bool& activeOpusState,
                                      std::vector<AudioCodec::EncodedOpusPacket>& encodedScratch,
                                      std::vector<uint8_t>& txScratch,
                                      SendPacketFn&& sendPacketFn) {
        if (numFrames == 0) {
            return;
        }

        const bool wantOpus = AudioCodec::ResolveEffectiveAudioCodec(
            useOpus, format.sampleRate, format.channels, format.bitDepth) == AudioCodec::kAudioCodecOpus;
        if (wantOpus) {
            if (!encoder.IsInitialized() ||
                encoder.GetSampleRate() != format.sampleRate ||
                encoder.GetChannels() != format.channels ||
                encoder.GetBitDepth() != format.bitDepth) {
                encoder.Initialize(format.sampleRate, format.bitDepth, format.channels, isVoice);
            }
            activeOpusState = encoder.IsInitialized();
        } else {
            if (activeOpusState) {
                encoder.Reset(false);
                activeOpusState = false;
            }
        }

        if (activeOpusState) {
            if (isSilence) {
                encoder.NotifySilenceSkipped();
                Network::AudioPacketHeader hdr = {};
                hdr.codec = AudioCodec::kAudioCodecOpus;
                hdr.flags = Network::AUDIO_FLAG_SILENCE;
                hdr.frameCount = numFrames;
                hdr.audioSeq = encoder.NextSequence();
                sendPacketFn(&hdr, static_cast<UINT32>(sizeof(hdr)));
                return;
            }

            if (pcmData) {
                const size_t blockAlign = static_cast<size_t>(format.channels) * (format.bitDepth / 8);
                encodedScratch.clear();
                if (encoder.PushPcmAndEncode(pcmData, static_cast<size_t>(numFrames) * blockAlign, encodedScratch)) {
                    for (const auto& pkt : encodedScratch) {
                        Network::AudioPacketHeader hdr = {};
                        hdr.codec = AudioCodec::kAudioCodecOpus;
                        hdr.flags = pkt.flags;
                        hdr.frameCount = pkt.frameCount;
                        hdr.audioSeq = pkt.audioSeq;
                        txScratch.resize(sizeof(hdr) + pkt.opusBytes.size());
                        std::memcpy(txScratch.data(), &hdr, sizeof(hdr));
                        if (!pkt.opusBytes.empty()) {
                            std::memcpy(txScratch.data() + sizeof(hdr), pkt.opusBytes.data(), pkt.opusBytes.size());
                        }
                        sendPacketFn(txScratch.data(), static_cast<UINT32>(txScratch.size()));
                    }
                    return;
                }
            }
        }

        // Uncompressed PCM path: keep silence compact (8B header) and chunk active audio <= 1280B to eliminate IP fragmentation.
        if (isSilence) {
            Network::AudioPacketHeader hdr = {};
            hdr.codec = AudioCodec::kAudioCodecPcm;
            hdr.flags = Network::AUDIO_FLAG_SILENCE;
            hdr.frameCount = numFrames;
            hdr.audioSeq = encoder.NextSequence();
            sendPacketFn(&hdr, static_cast<UINT32>(sizeof(hdr)));
            return;
        }

        if (!pcmData) {
            return;
        }
        const uint32_t blockAlign = static_cast<uint32_t>(format.channels) * (format.bitDepth / 8);
        if (blockAlign == 0) {
            return;
        }
        const size_t totalBytes = static_cast<size_t>(numFrames) * blockAlign;
        const size_t chunkBytes = AudioCodec::ComputeMtuSafePcmChunkBytes(blockAlign);
        size_t offset = 0;
        while (offset < totalBytes) {
            const size_t sliceBytes = (std::min)(chunkBytes, totalBytes - offset);
            const uint16_t sliceFrames = static_cast<uint16_t>(sliceBytes / blockAlign);
            Network::AudioPacketHeader hdr = {};
            hdr.codec = AudioCodec::kAudioCodecPcm;
            hdr.flags = 0;
            hdr.frameCount = sliceFrames;
            hdr.audioSeq = encoder.NextSequence();
            txScratch.resize(sizeof(hdr) + sliceBytes);
            std::memcpy(txScratch.data(), &hdr, sizeof(hdr));
            std::memcpy(txScratch.data() + sizeof(hdr), pcmData + offset, sliceBytes);
            sendPacketFn(txScratch.data(), static_cast<UINT32>(txScratch.size()));
            offset += sliceBytes;
        }
    }

    /**
     * @brief Decodes one incoming AudioPacketHeader-prefixed network packet into a queue and enforces jitter bounds.
     * Shared by HandleAudioData (Server) and HandleMicData (Client) to keep buffering/FEC/PLC logic unified.
     * @param queue Destination ClientAudioQueue.
     * @param streamFmt Active PCM output format of the target WASAPI renderer.
     * @param packetData Decrypted wire payload starting with Network::AudioPacketHeader.
     * @param packetSize Byte size of packetData.
     * @param targetJitterMs Current target jitter buffer depth in milliseconds.
     * @param recordRxStats True on Server mix queues to update 30s receive-gap telemetry and adaptive gap inputs.
     * @param outSwitchedCodec Receives the new codec ID if the incoming codec changed, or 255 if unchanged.
     * @param outDroppedOverrun Set true if buffer exceeded maxBufferMs and oldest samples were trimmed.
     * @param outLoggedTargetMs Receives the overrun ceiling in milliseconds when outDroppedOverrun is true.
     * @return True if decoded audio was queued cleanly; false if invalid/stale/ignored.
     */
    static bool PushDecodedAudioPacket(ClientAudioQueue& queue,
                                       const AudioFormat& streamFmt,
                                       const void* packetData,
                                       size_t packetSize,
                                       float targetJitterMs,
                                       bool recordRxStats,
                                       uint8_t& outSwitchedCodec,
                                       bool& outDroppedOverrun,
                                       float& outLoggedTargetMs) {
        outSwitchedCodec = 255;
        outDroppedOverrun = false;
        outLoggedTargetMs = 0.0f;

        if (!packetData || packetSize < sizeof(Network::AudioPacketHeader)) {
            return false;
        }
        const size_t frameSize = static_cast<size_t>(streamFmt.channels) * (streamFmt.bitDepth / 8);
        if (streamFmt.sampleRate == 0 || frameSize == 0) {
            return false;
        }

        Network::AudioPacketHeader hdr = {};
        std::memcpy(&hdr, packetData, sizeof(hdr));
        const uint8_t* payload = static_cast<const uint8_t*>(packetData) + sizeof(hdr);
        const size_t payloadBytes = packetSize - sizeof(hdr);

        std::lock_guard<std::mutex> queueLock(queue.mtx);

        if (recordRxStats) {
            const auto rxNow = std::chrono::steady_clock::now();
            if (queue.hasRxTime) {
                const int64_t gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(rxNow - queue.lastRxTime).count();
                if (gapMs > 0) {
                    const uint32_t gapU32 = static_cast<uint32_t>((std::min)(gapMs, static_cast<int64_t>(UINT32_MAX)));
                    AtomicUpdateMax(queue.rxMaxGapMs, gapU32);
                    AtomicUpdateMax(queue.pendingAdaptRxGapMs, gapU32);
                    if (gapU32 > kAudioRxGapSpikeMs) {
                        queue.rxGapSpikes.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            queue.lastRxTime = rxNow;
            queue.hasRxTime = true;
            queue.rxPackets.fetch_add(1, std::memory_order_relaxed);
            AtomicUpdateMax(queue.rxMaxPktBytes,
                            static_cast<uint32_t>((std::min)(packetSize, static_cast<size_t>(UINT32_MAX))));
        }

        const uint8_t* decodedPtr = nullptr;
        size_t decodedBytes = 0;

        if (hdr.codec == AudioCodec::kAudioCodecOpus) {
            if (queue.lastRxCodec != AudioCodec::kAudioCodecOpus) {
                // Reset expected sequence state when switching into Opus from PCM or initial startup
                // so prior PCM audioSeq counts never trigger bogus PLC/FEC synthesis.
                queue.opusDecoder.Reset(true);
            }
            if (!queue.opusDecoder.MatchesFormat(streamFmt.sampleRate, streamFmt.bitDepth, streamFmt.channels)) {
                if (!queue.opusDecoder.Initialize(streamFmt.sampleRate, streamFmt.bitDepth, streamFmt.channels)) {
                    return false;
                }
            }
            queue.decodePcmScratch.clear();
            AudioCodec::OpusDecodeStats dStats = {};
            const bool ok = queue.opusDecoder.DecodePacket(
                hdr.flags, hdr.frameCount, hdr.audioSeq, payload, payloadBytes, queue.decodePcmScratch, dStats);
            queue.opusFecCount.fetch_add(dStats.fecFrames, std::memory_order_relaxed);
            queue.opusPlcCount.fetch_add(dStats.plcFrames, std::memory_order_relaxed);
            queue.opusStaleCount.fetch_add(dStats.staleDropped, std::memory_order_relaxed);
            if (!ok || queue.decodePcmScratch.empty()) {
                return false;
            }
            decodedPtr = queue.decodePcmScratch.data();
            decodedBytes = queue.decodePcmScratch.size();
        } else if (hdr.codec == AudioCodec::kAudioCodecPcm) {
            if ((hdr.flags & Network::AUDIO_FLAG_SILENCE) != 0 && payloadBytes == 0) {
                const size_t silenceFrames = (hdr.frameCount > 0) ? hdr.frameCount : (streamFmt.sampleRate / 100);
                decodedBytes = silenceFrames * frameSize;
                queue.decodePcmScratch.assign(decodedBytes, 0);
                decodedPtr = queue.decodePcmScratch.data();
            } else {
                if (payloadBytes == 0 || (payloadBytes % frameSize) != 0) {
                    return false;
                }
                decodedPtr = payload;
                decodedBytes = payloadBytes;
            }
        } else {
            return false;
        }

        if (queue.lastRxCodec != hdr.codec) {
            queue.lastRxCodec = hdr.codec;
            outSwitchedCodec = hdr.codec;
        }

        queue.buffer.Push(decodedPtr, decodedBytes);

        const size_t bytesPerSec = static_cast<size_t>(streamFmt.sampleRate) * frameSize;
        const BufferBounds bounds = ComputeBufferBounds(targetJitterMs);

        size_t minBufferThreshold = static_cast<size_t>(bytesPerSec * (bounds.releaseMs / 1000.0f));
        minBufferThreshold -= (minBufferThreshold % frameSize);
        if (queue.isBuffering && queue.buffer.Size() >= minBufferThreshold) {
            queue.isBuffering = false;
            queue.wasBuffering = true;
        }

        const size_t maxBufferSize = static_cast<size_t>(bytesPerSec * (bounds.maxBufferMs / 1000.0f));
        if (queue.wasBuffering && queue.buffer.Size() > minBufferThreshold) {
            size_t staleExcess = queue.buffer.Size() - minBufferThreshold;
            staleExcess -= (staleExcess % frameSize);
            if (staleExcess > 0) {
                queue.buffer.Drop(staleExcess);
            }
        } else if (queue.buffer.Size() > maxBufferSize) {
            const size_t trimTargetBytes = static_cast<size_t>(bytesPerSec * (bounds.highDeadbandMs / 1000.0f));
            size_t excess = (queue.buffer.Size() > trimTargetBytes)
                ? (queue.buffer.Size() - trimTargetBytes)
                : 0;
            excess -= (excess % frameSize);

            if (excess > 0) {
                outDroppedOverrun = true;
                outLoggedTargetMs = bounds.maxBufferMs;
                queue.overrunCount.fetch_add(1, std::memory_order_relaxed);
                queue.buffer.Drop(excess);
                queue.needsFadeIn = true;
                queue.hasHermiteHistory = false;
                queue.resamplePhase = 0.0;
            }
        }
        return true;
    }

    /**
     * @brief Evaluates a C1-continuous 4-point Hermite cubic interpolator.
     * Used exclusively when the jitter buffer drifts outside its safe bit-perfect deadband.
     * @param xm1 Sample at index -1.
     * @param x0 Sample at index 0.
     * @param x1 Sample at index +1.
     * @param x2 Sample at index +2.
     * @param t Fractional offset in [0, 1).
     * @return Interpolated float sample.
     */
    static inline float Hermite4(float xm1, float x0, float x1, float x2, float t) {
        float c0 = x0;
        float c1 = 0.5f * (x1 - xm1);
        float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    /**
     * @brief Fills missing tail frames during transient Wi-Fi jitter with smooth exponential decay.
     * Replaces abrupt zero-padding notches that produce audible clicks in continuous audio.
     * @param outSamples Interleaved 32-bit float sample buffer.
     * @param startFrame First missing frame index to fill.
     * @param totalFrames Total frames in the audio callback.
     * @param channels Number of interleaved channels.
     * @param lastFrameFloat Per-channel last valid sample array (updated in place across decayed frames).
     * @return void
     */
    static void FillDecayedTailFloat(float* outSamples, size_t startFrame, size_t totalFrames, size_t channels, float* lastFrameFloat) {
        size_t clampedChannels = (std::min)(channels, static_cast<size_t>(8));
        for (size_t f = startFrame; f < totalFrames; ++f) {
            for (size_t c = 0; c < channels; ++c) {
                if (c < clampedChannels) {
                    lastFrameFloat[c] *= 0.985f;
                    outSamples[f * channels + c] = lastFrameFloat[c];
                } else {
                    outSamples[f * channels + c] = 0.0f;
                }
            }
        }
    }

    /**
     * @brief Applies a smooth linear crossfade from the prior callback tail into the start of a resumed buffer.
     * @param samples Interleaved 32-bit float sample buffer.
     * @param totalFrames Total frames available in the buffer.
     * @param channels Number of interleaved channels.
     * @param prevTailFrame Prior callback's final per-channel sample values.
     * @param maxFadeFrames Maximum crossfade length in frames.
     * @return void
     */
    static void CrossfadeFloatHead(float* samples, size_t totalFrames, size_t channels, const float* prevTailFrame, size_t maxFadeFrames = 32) {
        size_t fadeFrames = (std::min)(maxFadeFrames, totalFrames);
        size_t clampedChannels = (std::min)(channels, static_cast<size_t>(8));
        if (fadeFrames == 0) return;
        for (size_t f = 0; f < fadeFrames; ++f) {
            float alpha = static_cast<float>(f) / static_cast<float>(fadeFrames);
            for (size_t c = 0; c < clampedChannels; ++c) {
                size_t idx = f * channels + c;
                samples[idx] = prevTailFrame[c] * (1.0f - alpha) + samples[idx] * alpha;
            }
        }
    }

    /**
     * @brief Updates the lock-free cached audio mixing state (enabled and volume) for a specific client.
     * Decouples the real-time WASAPI render and UDP receive threads from coarse network mutexes.
     * @param clientIdentifier The socket handle representing the client.
     * @param isEnabled Whether audio mixing is enabled for this client.
     * @param volume Volume multiplier (0.0f to 1.0f).
     * @return void
     */
    void SetClientAudioState(uintptr_t clientIdentifier, bool isEnabled, float volume) {
        std::shared_ptr<ClientAudioQueue> pQueue;
        {
            std::shared_lock<std::shared_mutex> readLock(g_queuesMutex);
            auto it = g_clientAudioQueues.find(clientIdentifier);
            if (it != g_clientAudioQueues.end()) {
                pQueue = it->second;
            }
        }
        if (!pQueue) {
            std::unique_lock<std::shared_mutex> writeLock(g_queuesMutex);
            auto& entry = g_clientAudioQueues[clientIdentifier];
            if (!entry) {
                entry = std::make_shared<ClientAudioQueue>();
            }
            pQueue = entry;
        }
        pQueue->isEnabled.store(isEnabled, std::memory_order_relaxed);
        pQueue->volume.store(volume, std::memory_order_relaxed);
    }

    // --- CLIENT MIC RECEIVER THREAD ---
    std::thread g_clientMicThread;
    std::atomic<bool> g_isClientMicRunning(false);
    HANDLE g_hClientMicEvent = NULL;
    ClientAudioQueue g_clientMicQueue;

    // --- SERVER MIC CAPTURE THREAD ---
    /// The background thread that captures the server's microphone for broadcast.
    std::thread g_serverMicThread;
    std::atomic<bool> g_isServerMicRunning(false);
    HANDLE g_hServerMicEvent = NULL;

    /**
     * @brief Initializes COM and creates the WASAPI device enumerator.
     * @return true if successful, false otherwise.
     */
    bool Initialize() {
        if (pEnumerator) return true;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            g_comInitializedByAudio = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] CoInitializeEx failed (0x%08X).", static_cast<uint32_t>(hr));
            return false;
        }

        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), 
            nullptr, 
            CLSCTX_ALL, 
            __uuidof(IMMDeviceEnumerator), 
            (void**)&pEnumerator
        );

        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] CoCreateInstance for MMDeviceEnumerator failed (0x%08X).", static_cast<uint32_t>(hr));
            if (g_comInitializedByAudio) {
                CoUninitialize();
                g_comInitializedByAudio = false;
            }
            return false;
        }

        if (State::globalDebugMode) UI::LogDebug("[Audio] WASAPI Initialized.");
        return true;
    }

    /**
     * @brief Releases the device enumerator and uninitializes COM.
     * @return void
     */
    void Shutdown() {
        StopLoopbackCapture();
        StopMicBroadcast();
        StopAudioRenderer();
        StopMicReceiver();
        if (pEnumerator) {
            pEnumerator->Release();
            pEnumerator = nullptr;
        }
        if (g_comInitializedByAudio) {
            CoUninitialize();
            g_comInitializedByAudio = false;
        }
        if (State::globalDebugMode) UI::LogDebug("[Audio] WASAPI Shutdown.");
    }

    /**
     * @brief Gets the mix format of the default playback device.
     * @param outFormat The AudioFormat struct to populate.
     * @return true if the format was successfully retrieved.
     */
    bool GetDefaultDeviceFormat(AudioFormat& outFormat) {
        if (!pEnumerator) return false;

        IMMDevice* pDevice = nullptr;
        HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr)) {
            static DebugLog::RateLimitState fmtEpRate;
            LogHResultRateLimited(fmtEpRate, "[Audio] GetDefaultDeviceFormat: GetDefaultAudioEndpoint failed", hr);
            return false;
        }

        IAudioClient* pAudioClient = nullptr;
        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
        pDevice->Release();
        if (FAILED(hr)) {
            static DebugLog::RateLimitState fmtActRate;
            LogHResultRateLimited(fmtActRate, "[Audio] GetDefaultDeviceFormat: IAudioClient Activate failed", hr);
            return false;
        }

        WAVEFORMATEX* pWaveFormat = nullptr;
        hr = pAudioClient->GetMixFormat(&pWaveFormat);
        pAudioClient->Release();
        if (FAILED(hr)) {
            static DebugLog::RateLimitState fmtMixRate;
            LogHResultRateLimited(fmtMixRate, "[Audio] GetDefaultDeviceFormat: GetMixFormat failed", hr);
            return false;
        }

        outFormat.sampleRate = pWaveFormat->nSamplesPerSec;
        outFormat.channels = pWaveFormat->nChannels;
        
        if (pWaveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pWaveFormat->cbSize >= 22) {
            WAVEFORMATEXTENSIBLE* pEx = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pWaveFormat);
            if (IsEqualGUID(pEx->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                outFormat.bitDepth = 32; // Standardize on 32-bit float for mixing
            } else {
                outFormat.bitDepth = pWaveFormat->wBitsPerSample;
            }
        } else {
            outFormat.bitDepth = pWaveFormat->wBitsPerSample;
        }
        outFormat.codec = AudioCodec::ResolveEffectiveAudioCodec(
            State::enableAudioOpus.load(std::memory_order_relaxed),
            outFormat.sampleRate, outFormat.channels, outFormat.bitDepth);

        CoTaskMemFree(pWaveFormat);
        return true;
    }

    /**
     * @brief Queries Windows Core Audio for the friendly name of the default recording device.
     * @return The device name as a string, or "Unknown Device" if it fails.
     */
    std::string GetDefaultMicName() {
        if (!pEnumerator) return "Unknown Device";
        IMMDevice* pDevice = nullptr;
        if (FAILED(pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice))) return "Unknown Device";
        
        IPropertyStore* pProps = nullptr;
        std::string result = "Unknown Device";
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, NULL, 0, NULL, NULL);
                std::string strTo(size_needed, 0);
                WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, &strTo[0], size_needed, NULL, NULL);
                if (size_needed > 0) strTo.pop_back(); // Remove null terminator
                result = strTo;
            }
            PropVariantClear(&varName);
            pProps->Release();
        }
        pDevice->Release();
        return result;
    }

    /**
     * @brief The main loop for the client's audio capture thread.
     * Continuously captures system audio and streams it to the server. Incorporates
     * a self-healing device recovery loop to survive system sleep/standby, headphone unplug,
     * or Windows audio driver restarts without requiring an application restart.
     * @param targetFormat The audio format requested by the server.
     * @return void
     */
    void CaptureThreadLoop(AudioFormat targetFormat) {
        CoInitRAII coInit;
        // Register with MMCSS to guarantee CPU scheduling even when a game is consuming 100% CPU
        HANDLE hMmcss = RegisterMMCSS(L"Pro Audio", "CaptureThread");

        AudioCodec::OpusStreamEncoder capEncoder;
        std::vector<AudioCodec::EncodedOpusPacket> capEncodedScratch;
        std::vector<uint8_t> capTxScratch;
        bool capUsingOpus = false;

        while (g_isCaptureRunning) {
            if (!pEnumerator) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // 1. Get the default audio playback device (e.g., speakers).
            IMMDevice* pDevice = nullptr;
            HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
            if (FAILED(hr) || !pDevice) {
                if (State::globalDebugMode) {
                    static uint64_t lastCapDeviceLog = 0;
                    uint64_t now = GetTickCount64();
                    if (now - lastCapDeviceLog > 3000) {
                        UI::LogDebug("[Audio] CaptureThread: Waiting for default audio playback endpoint...");
                        lastCapDeviceLog = now;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // 2. Activate an IAudioClient for the device.
            IAudioClient* pAudioClient = nullptr;
            hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);    
            if (FAILED(hr) || !pAudioClient) {
                static DebugLog::RateLimitState capActRate;
                LogHResultRateLimited(capActRate, "[Audio] CaptureThread: IAudioClient Activate failed", hr);
                pDevice->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // Mute the client's system audio to prevent echo. We do this now while we still have the device object.
            hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&g_pClientEndpointVolume);
            if (SUCCEEDED(hr)) {
                HRESULT hrGetMute = g_pClientEndpointVolume->GetMute(&g_bOriginalMuteState);
                HRESULT hrSetMute = g_pClientEndpointVolume->SetMute(TRUE, nullptr);
                if (FAILED(hrGetMute) || FAILED(hrSetMute)) {
                    static DebugLog::RateLimitState capMuteRate;
                    LogHResultRateLimited(capMuteRate, "[Audio] WARNING: CaptureThread failed to mute local speakers (possible echo)",
                                          FAILED(hrSetMute) ? hrSetMute : hrGetMute);
                }
            } else {
                static DebugLog::RateLimitState capVolRate;
                LogHResultRateLimited(capVolRate, "[Audio] WARNING: CaptureThread failed to activate volume control (possible echo)", hr);
            }
            pDevice->Release(); // We are now finished with the IMMDevice, so we can safely release it.

            // 3. Set up the target WAVEFORMATEX based on the server's request.
            WAVEFORMATEX targetWaveFormat = {};
            targetWaveFormat.wFormatTag = (targetFormat.bitDepth == 32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
            targetWaveFormat.nChannels = targetFormat.channels;
            targetWaveFormat.nSamplesPerSec = targetFormat.sampleRate;
            targetWaveFormat.wBitsPerSample = targetFormat.bitDepth;
            targetWaveFormat.nBlockAlign = (targetWaveFormat.nChannels * targetWaveFormat.wBitsPerSample) / 8;
            targetWaveFormat.nAvgBytesPerSec = targetWaveFormat.nSamplesPerSec * targetWaveFormat.nBlockAlign;
            targetWaveFormat.cbSize = 0;

            // 4. Initialize the audio client for loopback capture.
            // We ask WASAPI to automatically convert the audio to our target format,
            // and allocate a 100ms buffer to safely absorb Windows OS thread scheduling jitter.
            REFERENCE_TIME hnsRequestedDuration = 1000000; // 100ms
            hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, hnsRequestedDuration, 0, &targetWaveFormat, nullptr);
            if (FAILED(hr)) {
                if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: IAudioClient Initialize failed (0x%08X). Retrying...", hr);
                if (g_pClientEndpointVolume) {
                    g_pClientEndpointVolume->SetMute(g_bOriginalMuteState, nullptr);
                    g_pClientEndpointVolume->Release();
                    g_pClientEndpointVolume = nullptr;
                }
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // 5. Get the capture service.
            IAudioCaptureClient* pCaptureClient = nullptr;
            hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
            if (FAILED(hr) || !pCaptureClient) {
                static DebugLog::RateLimitState capSvcRate;
                LogHResultRateLimited(capSvcRate, "[Audio] CaptureThread: GetService(IAudioCaptureClient) failed", hr);
                if (g_pClientEndpointVolume) {
                    g_pClientEndpointVolume->SetMute(g_bOriginalMuteState, nullptr);
                    g_pClientEndpointVolume->Release();
                    g_pClientEndpointVolume = nullptr;
                }
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // 6. Start the audio stream.
            hr = pAudioClient->Start();
            if (FAILED(hr)) {
                static DebugLog::RateLimitState capStartRate;
                LogHResultRateLimited(capStartRate, "[Audio] CaptureThread: IAudioClient Start failed", hr);
                pCaptureClient->Release();
                if (g_pClientEndpointVolume) {
                    g_pClientEndpointVolume->SetMute(g_bOriginalMuteState, nullptr);
                    g_pClientEndpointVolume->Release();
                    g_pClientEndpointVolume = nullptr;
                }
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // Fast loopback polling period: Polling every 3ms with our enforced 1ms Windows timer resolution
            // prevents beating against WASAPI's 10ms audio engine periods and eliminates bursty packet delivery.
            DWORD sleepTimeMs = 3;

            UINT32 capBufferFrames = 0;
            pAudioClient->GetBufferSize(&capBufferFrames);
            REFERENCE_TIME defaultPeriod = 0;
            REFERENCE_TIME minPeriod = 0;
            pAudioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);

            if (State::globalDebugMode) {
                const double sr = (targetFormat.sampleRate > 0) ? static_cast<double>(targetFormat.sampleRate) : 48000.0;
                const double enginePeriodMs = static_cast<double>(defaultPeriod) / 10000.0;
                const double capBufMs = 1000.0 * static_cast<double>(capBufferFrames) / sr;
                const bool initialOpus = AudioCodec::ResolveEffectiveAudioCodec(
                    Network::GetServerAudioCodec() == AudioCodec::kAudioCodecOpus,
                    targetFormat.sampleRate, targetFormat.channels, targetFormat.bitDepth) == AudioCodec::kAudioCodecOpus;
                UI::LogDebug("[Audio] Loopback capture started (%uHz %uch %u-bit, Codec=%s, enginePeriod=%.1fms, capBuf=%uf/%.1fms, 3ms polling).",
                             targetFormat.sampleRate, targetFormat.channels, targetFormat.bitDepth,
                             initialOpus ? "Opus" : "PCM",
                             enginePeriodMs, capBufferFrames, capBufMs);
            }

            // 7. Main capture streaming loop for this device
            bool deviceInvalidated = false;
            auto lastRealPacketTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(200);
            auto nextSilenceDeadline = std::chrono::steady_clock::now();
            int64_t lastPacketDurationMs = 10;
            bool silenceActive = false;
            auto capPerfStart = std::chrono::steady_clock::now();
            CaptureTimingStats capStats;
            std::chrono::steady_clock::time_point lastAudioSendTime{};
            bool hasLastAudioSend = false;
            static DebugLog::RateLimitState capSendGapWarnRate;
            bool lastWantOpus = (Network::GetServerAudioCodec() == AudioCodec::kAudioCodecOpus);

            auto sendTrackedAudio = [&](const void* data, UINT32 dataSize, bool isSyntheticSilence, const char* srcLabel) {
                const auto sendStart = std::chrono::steady_clock::now();
                const uint32_t sendGapMs = hasLastAudioSend
                    ? static_cast<uint32_t>((std::max)(static_cast<int64_t>(0),
                          std::chrono::duration_cast<std::chrono::milliseconds>(sendStart - lastAudioSendTime).count()))
                    : 0u;
                Network::SendToServer(Network::MessageType::EVENT_AUDIO_DATA, data, dataSize);
                const auto sendEnd = std::chrono::steady_clock::now();
                const uint32_t sendCallMs = static_cast<uint32_t>((std::max)(static_cast<int64_t>(0),
                    std::chrono::duration_cast<std::chrono::milliseconds>(sendEnd - sendStart).count()));

                const bool hadPrevSend = hasLastAudioSend;
                lastAudioSendTime = sendStart;
                hasLastAudioSend = true;

                if (capStats.RecordSend(sendGapMs, hadPrevSend, sendCallMs, dataSize, isSyntheticSilence) &&
                    State::globalDebugMode) {
                    uint32_t suppressed = 0;
                    if (capSendGapWarnRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
                        char supBuf[32] = "";
                        if (suppressed > 0) {
                            snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
                        }
                        UI::LogDebug("[Audio] WARNING: CaptureThread send gap %ums (sendCall=%ums, pkt=%uB, src=%s%s).",
                                     sendGapMs, sendCallMs, dataSize, srcLabel, supBuf);
                    }
                }
            };

            while (g_isCaptureRunning && !deviceInvalidated) {
                const bool wantOpus = (Network::GetServerAudioCodec() == AudioCodec::kAudioCodecOpus);
                if (wantOpus != lastWantOpus) {
                    lastWantOpus = wantOpus;
                    if (State::globalDebugMode) {
                        const uint8_t eff = AudioCodec::ResolveEffectiveAudioCodec(
                            wantOpus, targetFormat.sampleRate, targetFormat.channels, targetFormat.bitDepth);
                        UI::LogDebug("[Audio] CaptureThread: Live codec switched to %s.",
                                     eff == AudioCodec::kAudioCodecOpus ? "Opus" : "PCM");
                    }
                }

                UINT32 packetLength = 0;
                hr = pCaptureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) {
                    // Device invalidated by sleep/wake, driver reset, or hardware unplug
                    if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: Device invalidated (0x%08X). Recovering...", hr);
                    deviceInvalidated = true;
                    break;
                }
                
                if (packetLength == 0) {
                    // Wait >=30ms (or packetDuration + 15ms) before synthesizing keep-alive silence so normal
                    // browser/engine packet gaps during active playback never inject extra frames.
                    auto now = std::chrono::steady_clock::now();
                    auto idleDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRealPacketTime).count();
                    const int64_t idleThresholdMs = ComputeIdleSilenceThresholdMs(lastPacketDurationMs);

                    if (idleDurationMs >= idleThresholdMs) {
                        if (!silenceActive) {
                            silenceActive = true;
                            // Backdate by 10ms so the initial transition sends two 10ms catch-up silence packets,
                            // preventing the ~30ms idleThreshold wait from draining the Server's jitter buffer.
                            nextSilenceDeadline = now - std::chrono::milliseconds(10);
                        }
                        const uint16_t framesPer10ms = static_cast<uint16_t>(targetWaveFormat.nSamplesPerSec / 100);
                        int sentCount = 0;
                        while (now >= nextSilenceDeadline && sentCount < 2) {
                            EmitAudioStreamFrames(
                                targetFormat, wantOpus, false, true, framesPer10ms, nullptr,
                                capEncoder, capUsingOpus, capEncodedScratch, capTxScratch,
                                [&](const void* pkt, UINT32 pktBytes) {
                                    sendTrackedAudio(pkt, pktBytes, true, "IdleSilence");
                                });
                            nextSilenceDeadline += std::chrono::milliseconds(10);
                            ++sentCount;
                        }
                        if (now >= nextSilenceDeadline) {
                            ++capStats.silenceClockSlips;
                            nextSilenceDeadline = now + std::chrono::milliseconds(10);
                        }
                    }
                } else {
                    // DRAIN LOOP: Pull *all* available packets from the WASAPI buffer.
                    // If we only pull one per sleep cycle, audio will silently drift and starve the server!
                    while (packetLength > 0) {
                        BYTE* pData = nullptr;
                        UINT32 numFramesAvailable = 0;
                        DWORD flags = 0;

                        // Get the available data in the shared buffer.
                        hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
                        if (SUCCEEDED(hr)) {
                            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                                ++capStats.discontinuities;
                            }
                            int64_t packetDurMs = (targetWaveFormat.nSamplesPerSec > 0)
                                ? (static_cast<int64_t>(numFramesAvailable) * 1000LL / static_cast<int64_t>(targetWaveFormat.nSamplesPerSec))
                                : 10LL;
                            lastPacketDurationMs = (std::max)(packetDurMs, static_cast<int64_t>(10));
                            lastRealPacketTime = std::chrono::steady_clock::now();
                            silenceActive = false;

                            const bool isSilentBuffer = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                            EmitAudioStreamFrames(
                                targetFormat, wantOpus, false, isSilentBuffer,
                                static_cast<uint16_t>(numFramesAvailable), isSilentBuffer ? nullptr : pData,
                                capEncoder, capUsingOpus, capEncodedScratch, capTxScratch,
                                [&](const void* pkt, UINT32 pktBytes) {
                                    sendTrackedAudio(pkt, pktBytes, false, isSilentBuffer ? "WASAPI-Silent" : "WASAPI");
                                });
                            pCaptureClient->ReleaseBuffer(numFramesAvailable);
                        } else {
                            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
                                if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: GetBuffer invalidated (0x%08X).", static_cast<uint32_t>(hr));
                                deviceInvalidated = true;
                            } else {
                                static DebugLog::RateLimitState capGetBufRate;
                                LogHResultRateLimited(capGetBufRate, "[Audio] CaptureThread: GetBuffer failed", hr);
                            }
                            break; // If GetBuffer fails, stop trying to drain for this cycle
                        }

                        HRESULT hrNext = pCaptureClient->GetNextPacketSize(&packetLength);
                        if (FAILED(hrNext)) {
                            if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: Drain GetNextPacketSize failed (0x%08X). Recovering...", static_cast<uint32_t>(hrNext));
                            deviceInvalidated = true;
                            break;
                        }
                    }
                }
                MaybeLogCapturePerf30s("CaptureThread", capPerfStart, capStats);
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepTimeMs));
            }

            // 8. Device teardown
            pAudioClient->Stop();
            pCaptureClient->Release();
            pAudioClient->Release();

            // Restore the client's original mute state when device is lost or thread exits
            if (g_pClientEndpointVolume) {
                g_pClientEndpointVolume->SetMute(g_bOriginalMuteState, nullptr);
                g_pClientEndpointVolume->Release();
                g_pClientEndpointVolume = nullptr;
            }

            if (deviceInvalidated && g_isCaptureRunning) {
                if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: Audio device lost (sleep/standby/unplug). Re-acquiring in 500ms...");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }

        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (State::globalDebugMode) UI::LogDebug("[Audio] Loopback capture thread stopped.");
    }

    /**
     * @brief Starts the background WASAPI loopback capture thread on the Client.
     * @return void
     */
    void StartLoopbackCapture() {
        if (g_isCaptureRunning) return;
        AudioFormat serverFormat;
        if (!Network::GetServerAudioFormat(serverFormat)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] Cannot start capture: Server audio format not yet received.");
            return;
        }
        if (g_captureThread.joinable()) {
            g_captureThread.join();
        }
        g_isCaptureRunning = true;
        g_captureThread = std::thread(CaptureThreadLoop, serverFormat);
    }

    /**
     * @brief Stops and joins the Client loopback capture thread cleanly.
     * @return void
     */
    void StopLoopbackCapture() {
        if (!g_isCaptureRunning) return;
        g_isCaptureRunning = false;
        if (g_captureThread.joinable()) {
            g_captureThread.join();
        }
    }

    /**
     * @brief The main loop for the server's audio rendering thread.
     * Continuously mixes and plays incoming client audio streams.
     * Incorporates an outer self-healing recovery loop that automatically re-acquires
     * the default audio playback endpoint if the device is invalidated (sleep/standby,
     * headphone unplug, or Windows audio driver restarts).
     * @return void
     */
    void RenderThreadLoop() {
        CoInitRAII coInit;
        // Register with MMCSS to guarantee CPU scheduling even when a game is consuming 100% CPU
        HANDLE hMmcss = RegisterMMCSS(L"Pro Audio", "RenderThread");

        std::vector<uint8_t> linearBuffer; // Reusable extraction buffer for the RingBuffer
        std::vector<float> mixBuffer;      // Reusable master mix buffer

        while (g_isRenderRunning) {
            if (!pEnumerator) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            IMMDevice* pDevice = nullptr;
            HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
            if (FAILED(hr) || !pDevice) {
                if (State::globalDebugMode) {
                    static uint64_t lastFormatRetryLog = 0;
                    uint64_t now = GetTickCount64();
                    if (now - lastFormatRetryLog > 3000) {
                        UI::LogDebug("[Audio] RenderThread: Waiting for default playback device...");
                        lastFormatRetryLog = now;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            IAudioClient* pAudioClient = nullptr;
            hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
            pDevice->Release();
            pDevice = nullptr;
            if (FAILED(hr) || !pAudioClient) {
                static DebugLog::RateLimitState renActRate;
                LogHResultRateLimited(renActRate, "[Audio] RenderThread: IAudioClient Activate failed", hr);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // Mix in 32-bit float format for highest quality
            WAVEFORMATEX* pMixFormat = nullptr;
            hr = pAudioClient->GetMixFormat(&pMixFormat);
            if (FAILED(hr) || !pMixFormat) {
                static DebugLog::RateLimitState renMixRate;
                LogHResultRateLimited(renMixRate, "[Audio] RenderThread: GetMixFormat failed", hr);
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            AudioFormat format;
            format.sampleRate = pMixFormat->nSamplesPerSec;
            format.channels = pMixFormat->nChannels;
            format.bitDepth = 32;
            const bool wantOpus = State::enableAudioOpus.load(std::memory_order_relaxed);
            format.codec = AudioCodec::ResolveEffectiveAudioCodec(
                wantOpus, format.sampleRate, format.channels, format.bitDepth);
            if (wantOpus && format.codec != AudioCodec::kAudioCodecOpus && State::globalDebugMode) {
                UI::LogDebug("[Audio] Opus unavailable for %uHz %uch %u-bit mix format; using MTU-safe PCM fallback.",
                             format.sampleRate, format.channels, format.bitDepth);
            }

            bool hadRenderFormat = false;
            bool formatChanged = false;
            bool codecChanged = false;
            {
                std::lock_guard<std::mutex> fmtLock(g_formatBroadcastMutex);
                hadRenderFormat = g_hasRenderFormat.load(std::memory_order_relaxed);
                formatChanged = hadRenderFormat &&
                                ((g_serverRenderFormat.sampleRate != format.sampleRate) ||
                                 (g_serverRenderFormat.bitDepth != format.bitDepth) ||
                                 (g_serverRenderFormat.channels != format.channels));
                codecChanged = hadRenderFormat && (g_serverRenderFormat.codec != format.codec);
                g_serverRenderFormat = format;
                g_serverRenderRate.store(format.sampleRate, std::memory_order_relaxed);
                g_serverRenderBits.store(format.bitDepth, std::memory_order_relaxed);
                g_serverRenderChannels.store(format.channels, std::memory_order_relaxed);
                g_hasRenderFormat.store(true, std::memory_order_release);
            }

            // Broadcast on initial render format acquisition or whenever mix format/codec changes
            // so connected clients immediately synchronize capture and mic formats.
            if ((!hadRenderFormat || formatChanged || codecChanged) && State::enableServerAudioMix) {
                const Network::AudioFormatPayload wireFmt = MakeAudioFormatPayload(format);
                Network::BroadcastMessage(Network::MessageType::EVENT_AUDIO_FORMAT, &wireFmt, sizeof(wireFmt));
                // Restart server microphone capture if active so it matches the new hardware sample rate/channels
                if (formatChanged && g_isServerMicRunning) {
                    StopMicBroadcast();
                    StartMicBroadcast();
                }
            }

            WAVEFORMATEXTENSIBLE extFormat;
            ZeroMemory(&extFormat, sizeof(WAVEFORMATEXTENSIBLE));
            // Provide a sensible default channel mask if the device didn't natively provide one
            if (pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pMixFormat->cbSize >= 22) {
                memcpy(&extFormat, pMixFormat, sizeof(WAVEFORMATEXTENSIBLE));
            } else {
                memcpy(&extFormat.Format, pMixFormat, sizeof(WAVEFORMATEX));
                if (extFormat.Format.nChannels == 1) extFormat.dwChannelMask = KSAUDIO_SPEAKER_MONO;
                else if (extFormat.Format.nChannels == 2) extFormat.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
                else if (extFormat.Format.nChannels == 4) extFormat.dwChannelMask = KSAUDIO_SPEAKER_QUAD;
                else if (extFormat.Format.nChannels == 6) extFormat.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
                else if (extFormat.Format.nChannels == 8) extFormat.dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
            }

            extFormat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            extFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            extFormat.Format.wBitsPerSample = 32;
            extFormat.Format.cbSize = 22;
            extFormat.Format.nBlockAlign = (extFormat.Format.nChannels * extFormat.Format.wBitsPerSample) / 8;
            extFormat.Format.nAvgBytesPerSec = extFormat.Format.nSamplesPerSec * extFormat.Format.nBlockAlign;
            extFormat.Samples.wValidBitsPerSample = 32;

            if (!g_hRenderEvent) {
                g_hRenderEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (!g_hRenderEvent) {
                    static DebugLog::RateLimitState renEvtRate;
                    LogHResultRateLimited(renEvtRate, "[Audio] RenderThread: CreateEvent failed", HRESULT_FROM_WIN32(GetLastError()));
                    pAudioClient->Release();
                    CoTaskMemFree(pMixFormat);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            }
            ResetEvent(g_hRenderEvent);

            hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 0, 0, &extFormat.Format, nullptr);
            if (FAILED(hr)) {
                static DebugLog::RateLimitState renInitRate;
                LogHResultRateLimited(renInitRate, "[Audio] RenderThread: IAudioClient Initialize failed", hr);
                pAudioClient->Release();
                CoTaskMemFree(pMixFormat);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            hr = pAudioClient->SetEventHandle(g_hRenderEvent);
            if (FAILED(hr)) {
                static DebugLog::RateLimitState renSetEvtRate;
                LogHResultRateLimited(renSetEvtRate, "[Audio] RenderThread: SetEventHandle failed", hr);
                pAudioClient->Release();
                CoTaskMemFree(pMixFormat);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            UINT32 bufferFrameCount = 0;
            pAudioClient->GetBufferSize(&bufferFrameCount);

            IAudioRenderClient* pRenderClient = nullptr;
            hr = pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient);
            if (FAILED(hr) || !pRenderClient) {
                static DebugLog::RateLimitState renSvcRate;
                LogHResultRateLimited(renSvcRate, "[Audio] RenderThread: GetService(IAudioRenderClient) failed", hr);
                pAudioClient->Release();
                CoTaskMemFree(pMixFormat);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            hr = pAudioClient->Start();
            if (FAILED(hr)) {
                static DebugLog::RateLimitState renStartRate;
                LogHResultRateLimited(renStartRate, "[Audio] RenderThread: IAudioClient Start failed", hr);
                pRenderClient->Release();
                pAudioClient->Release();
                CoTaskMemFree(pMixFormat);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            if (State::globalDebugMode) {
                double hwMs = (format.sampleRate > 0)
                    ? (1000.0 * static_cast<double>(bufferFrameCount) / static_cast<double>(format.sampleRate))
                    : 0.0;
                UI::LogDebug("[Audio] Render started: %uHz %uch %u-bit, Codec=%s (HW buffer: %u frames / %.1fms)%s",
                             format.sampleRate, format.channels, format.bitDepth,
                             format.codec == AudioCodec::kAudioCodecOpus ? "Opus" : "PCM",
                             bufferFrameCount, hwMs,
                             formatChanged ? " [Format Changed]" : "");
            }

            bool deviceInvalidated = false;
            auto perfIntervalStart = std::chrono::steady_clock::now();
            auto lastWakeTime = perfIntervalStart;
            auto lastAdaptiveTime = perfIntervalStart;
            bool hasWakeTime = false;
            AdaptiveJitterController adaptiveJitter;
            adaptiveJitter.Reset(State::audioJitterBufferMs.load(std::memory_order_relaxed));
            State::activeJitterBufferMs.store(adaptiveJitter.effectiveTargetMs, std::memory_order_relaxed);
            float bufMinMs = 1e9f;
            float bufMaxMs = 0.0f;
            double bufSumMs = 0.0;
            uint32_t bufSamples = 0;
            uint32_t concealCallbacks = 0;
            uint32_t starvationResets = 0;
            uint32_t hermiteLowCallbacks = 0;
            uint32_t hermiteHighCallbacks = 0;
            uint32_t clipSamples = 0;
            uint32_t wakeJitterSpikes = 0;

            while (g_isRenderRunning && !deviceInvalidated) {
                // Wait for the hardware to demand data. 500ms timeout prevents infinite hangs on sleep/standby
                DWORD waitResult = WaitForSingleObject(g_hRenderEvent, 500);
                if (waitResult != WAIT_OBJECT_0) {
                    hasWakeTime = false;
                    lastAdaptiveTime = std::chrono::steady_clock::now();
                    if (!g_isRenderRunning) break;
                    // On timeout, probe device health using GetCurrentPadding.
                    // When Windows enters sleep or invalidates the endpoint, the event stops firing
                    // and GetCurrentPadding returns AUDCLNT_E_DEVICE_INVALIDATED.
                    UINT32 testPadding = 0;
                    HRESULT hrTest = pAudioClient->GetCurrentPadding(&testPadding);
                    if (FAILED(hrTest)) {
                        if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: Device invalidated on timeout (0x%08X). Recovering...", hrTest);
                        deviceInvalidated = true;
                        break;
                    }
                    continue;
                }

                auto wakeNow = std::chrono::steady_clock::now();
                if (hasWakeTime) {
                    auto wakeDeltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(wakeNow - lastWakeTime).count();
                    if (wakeDeltaMs > 25) {
                        ++wakeJitterSpikes;
                    }
                }
                lastWakeTime = wakeNow;
                hasWakeTime = true;

                UINT32 numPaddingFrames = 0;
                HRESULT hrPad = pAudioClient->GetCurrentPadding(&numPaddingFrames);
                if (FAILED(hrPad)) {
                    if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: Device invalidated (0x%08X). Recovering...", hrPad);
                    deviceInvalidated = true;
                    break;
                }

                UINT32 numFramesAvailable = bufferFrameCount - numPaddingFrames;
                if (numFramesAvailable == 0) {
                    continue;
                }

                BYTE* pData = nullptr;
                hr = pRenderClient->GetBuffer(numFramesAvailable, &pData);
                if (FAILED(hr)) {
                    if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: GetBuffer failed (0x%08X). Recovering...", hr);
                    deviceInvalidated = true;
                    break;
                }
                std::vector<std::shared_ptr<ClientAudioQueue>> activeQueues;
                bool anyConcealOrStarve = false;
                {
                    // Resize if needed, then initialize to silence (0.0f).
                    size_t mixBufferSize = numFramesAvailable * pMixFormat->nChannels;
                    if (mixBuffer.size() < mixBufferSize) mixBuffer.resize(mixBufferSize, 0.0f);
                    std::fill(mixBuffer.begin(), mixBuffer.begin() + mixBufferSize, 0.0f);
                    
                    size_t bytesPerFrame = (format.bitDepth / 8) * format.channels;
                    size_t bytesToProcess = numFramesAvailable * bytesPerFrame;

                    std::vector<float> activeVolumes;

                    {
                        std::shared_lock<std::shared_mutex> mapLock(g_queuesMutex);
                        for (auto& pair : g_clientAudioQueues) {
                            // Read lock-free atomics directly on the queue so RenderThreadLoop never contends on clientsMutex.
                            if (pair.second->isEnabled.load(std::memory_order_relaxed)) {
                                activeQueues.push_back(pair.second);
                                activeVolumes.push_back(pair.second->volume.load(std::memory_order_relaxed));
                            }
                        }
                    }

                    for (size_t i = 0; i < activeQueues.size(); ++i) {
                        auto& queue = activeQueues[i];
                        float vol = activeVolumes[i];

                        size_t availableBytes = 0;
                        size_t bytesRead = 0;
                        bool isBuffering = false;
                        bool logDepleted = false;
                        bool didConceal = false;
                        float sampledBufMs = -1.0f;
                        int64_t silentRxMs = -1;
                        int hermiteDir = 0;
                        bool applyCrossfade = false;
                        float crossfadeFrom[8] = {0.0f};
                        size_t channels = (std::min)(static_cast<size_t>(format.channels), static_cast<size_t>(8));

                        {
                            // Scope the lock tightly to memory extraction/concealment to prevent network thread convoying
                            std::lock_guard<std::mutex> queueLock(queue->mtx);
                            
                            availableBytes = queue->buffer.Size();
                            applyCrossfade = (queue->wasBuffering || queue->needsFadeIn);
                            for (size_t c = 0; c < channels; ++c) {
                                crossfadeFrom[c] = queue->lastFrameFloat[c];
                            }

                            if (queue->isBuffering) {
                                // Still in initial buffering mode - play silence until jitter buffer fills
                                isBuffering = true;
                            } else {
                                if (linearBuffer.size() < bytesToProcess) {
                                    linearBuffer.resize(bytesToProcess);
                                }

                                size_t availableFrames = availableBytes / bytesPerFrame;
                                size_t bytesPerSec = format.sampleRate * bytesPerFrame;
                                float currentBufferMs = (bytesPerSec > 0)
                                    ? (1000.0f * static_cast<float>(availableBytes) / static_cast<float>(bytesPerSec))
                                    : 0.0f;
                                float targetMs = static_cast<float>(State::activeJitterBufferMs.load(std::memory_order_relaxed));
                                sampledBufMs = currentBufferMs;

                                if (queue->appliedTargetMs < 0.0f) {
                                    queue->appliedTargetMs = targetMs;
                                } else if (std::fabs(targetMs - queue->appliedTargetMs) >= 1.0f) {
                                    queue->appliedTargetMs = targetMs;
                                    queue->seekingTarget = true;
                                }

                                if (availableFrames >= numFramesAvailable) {
                                    queue->starvationCount = 0;
                                    queue->wasBuffering = false;
                                    queue->needsFadeIn = false;

                                    const double rate = ResolvePlaybackRate(currentBufferMs, targetMs, queue->seekingTarget);
                                    const bool insideDeadband = (rate == 1.0);
                                    size_t maxNeededFrames = static_cast<size_t>(
                                        std::ceil(queue->resamplePhase + static_cast<double>(numFramesAvailable) * rate)) + 3;

                                    if (!insideDeadband && format.bitDepth == 32 && queue->hasHermiteHistory && availableFrames >= maxNeededFrames) {
                                        hermiteDir = (rate < 1.0) ? -1 : 1;
                                        // Edge Drift-Correction Path: 4-point Hermite cubic micro-resampling (+/- 0.20%)
                                        // gently guides the buffer back into the bit-perfect deadband without audible pitch shift.
                                        static thread_local std::vector<uint8_t> srcPeekBytes;
                                        size_t peekBytes = maxNeededFrames * bytesPerFrame;
                                        if (srcPeekBytes.size() < peekBytes) srcPeekBytes.resize(peekBytes);
                                        queue->buffer.Peek(srcPeekBytes.data(), peekBytes);

                                        const float* srcFloat = reinterpret_cast<const float*>(srcPeekBytes.data());
                                        float* outFloat = reinterpret_cast<float*>(linearBuffer.data());

                                        auto getSrcSample = [&](long long idx, size_t ch) -> float {
                                            if (idx == -3) return queue->prevFramesFloat[0][ch];
                                            if (idx == -2) return queue->prevFramesFloat[1][ch];
                                            if (idx == -1) return queue->prevFramesFloat[2][ch];
                                            if (idx < 0) return queue->prevFramesFloat[0][ch];
                                            size_t clamped = (static_cast<size_t>(idx) < maxNeededFrames) ? static_cast<size_t>(idx) : (maxNeededFrames - 1);
                                            return srcFloat[clamped * format.channels + ch];
                                        };

                                        double pos = queue->resamplePhase;
                                        for (size_t f = 0; f < numFramesAvailable; ++f) {
                                            long long idx = static_cast<long long>(std::floor(pos));
                                            float t = static_cast<float>(pos - static_cast<double>(idx));
                                            for (size_t c = 0; c < format.channels; ++c) {
                                                if (c < 8) {
                                                    outFloat[f * format.channels + c] = Hermite4(
                                                        getSrcSample(idx - 1, c),
                                                        getSrcSample(idx, c),
                                                        getSrcSample(idx + 1, c),
                                                        getSrcSample(idx + 2, c),
                                                        t
                                                    );
                                                } else {
                                                    outFloat[f * format.channels + c] = srcFloat[static_cast<size_t>(idx) * format.channels + c];
                                                }
                                            }
                                            pos += rate;
                                        }

                                        size_t framesConsumed = static_cast<size_t>(std::floor(pos));
                                        queue->resamplePhase = pos - static_cast<double>(framesConsumed);

                                        float newHist[3][8] = {{0.0f}};
                                        for (int h = 0; h < 3; ++h) {
                                            long long hIdx = static_cast<long long>(framesConsumed) - 3 + h;
                                            for (size_t c = 0; c < channels; ++c) {
                                                newHist[h][c] = getSrcSample(hIdx, c);
                                            }
                                        }
                                        for (int h = 0; h < 3; ++h) {
                                            for (size_t c = 0; c < channels; ++c) {
                                                queue->prevFramesFloat[h][c] = newHist[h][c];
                                            }
                                        }
                                        for (size_t c = 0; c < channels; ++c) {
                                            queue->lastFrameFloat[c] = outFloat[(numFramesAvailable - 1) * format.channels + c];
                                        }
                                        queue->hasHermiteHistory = true;
                                        queue->buffer.Drop(framesConsumed * bytesPerFrame);
                                        bytesRead = bytesToProcess;
                                    } else {
                                        // Bit-Perfect 1:1 Path (1.000x exact PCM copy inside the safe deadband)
                                        queue->resamplePhase = 0.0;
                                        queue->buffer.Pop(linearBuffer.data(), bytesToProcess);
                                        bytesRead = bytesToProcess;

                                        if (format.bitDepth == 32 && numFramesAvailable > 0) {
                                            const float* outFloat = reinterpret_cast<const float*>(linearBuffer.data());
                                            for (size_t c = 0; c < channels; ++c) {
                                                queue->lastFrameFloat[c] = outFloat[(numFramesAvailable - 1) * format.channels + c];
                                            }
                                            if (numFramesAvailable >= 3) {
                                                for (int h = 0; h < 3; ++h) {
                                                    size_t srcIdx = numFramesAvailable - 3 + static_cast<size_t>(h);
                                                    for (size_t c = 0; c < channels; ++c) {
                                                        queue->prevFramesFloat[h][c] = outFloat[srcIdx * format.channels + c];
                                                    }
                                                }
                                                queue->hasHermiteHistory = true;
                                            }
                                        }
                                    }
                                } else {
                                    // Partial fill or transient empty poll due to Wi-Fi jitter:
                                    // Maintain waveform continuity via smooth exponential decay instead of 0.0 notches.
                                    if (queue->hasRxTime) {
                                        silentRxMs = (std::max)(static_cast<int64_t>(0),
                                            std::chrono::duration_cast<std::chrono::milliseconds>(wakeNow - queue->lastRxTime).count());
                                    }
                                    if (availableFrames > 0) {
                                        queue->starvationCount = 0;
                                        size_t validBytes = availableFrames * bytesPerFrame;
                                        queue->buffer.Pop(linearBuffer.data(), validBytes);
                                        if (format.bitDepth == 32) {
                                            const float* outFloat = reinterpret_cast<const float*>(linearBuffer.data());
                                            for (size_t c = 0; c < channels; ++c) {
                                                queue->lastFrameFloat[c] = outFloat[(availableFrames - 1) * format.channels + c];
                                            }
                                        }
                                    } else {
                                        queue->starvationCount++;
                                    }

                                    if (queue->starvationCount >= 6) {
                                        // ~60ms of sustained starvation: cleanly transition back to re-buffering
                                        queue->ResetStreamState();
                                        isBuffering = true;
                                        logDepleted = true;
                                    } else {
                                        didConceal = true;
                                        queue->wasBuffering = false;
                                        queue->needsFadeIn = true;
                                        queue->hasHermiteHistory = false;
                                        queue->resamplePhase = 0.0;
                                        if (format.bitDepth == 32) {
                                            float* outFloat = reinterpret_cast<float*>(linearBuffer.data());
                                            FillDecayedTailFloat(outFloat, availableFrames, numFramesAvailable, format.channels, queue->lastFrameFloat);
                                        } else {
                                            size_t validBytes = availableFrames * bytesPerFrame;
                                            if (validBytes < bytesToProcess) {
                                                std::memset(linearBuffer.data() + validBytes, 0, bytesToProcess - validBytes);
                                            }
                                        }
                                        bytesRead = bytesToProcess;
                                    }
                                }
                            }
                        } // Mutex instantly released! Network thread is free to push new data.

                        if (sampledBufMs >= 0.0f) {
                            bufMinMs = (std::min)(bufMinMs, sampledBufMs);
                            bufMaxMs = (std::max)(bufMaxMs, sampledBufMs);
                            bufSumMs += sampledBufMs;
                            ++bufSamples;
                        }
                        if (hermiteDir < 0) ++hermiteLowCallbacks;
                        else if (hermiteDir > 0) ++hermiteHighCallbacks;
                        if (didConceal) ++concealCallbacks;
                        if (logDepleted) ++starvationResets;
                        if (didConceal || logDepleted) anyConcealOrStarve = true;

                        if (logDepleted && State::globalDebugMode) {
                            static DebugLog::RateLimitState depletedRate;
                            uint32_t suppressed = 0;
                            if (depletedRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
                                char supBuf[32] = "";
                                if (suppressed > 0) {
                                    snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
                                }
                                if (silentRxMs >= 0) {
                                    UI::LogDebug("[Audio] Buffer depleted: Re-buffering to %dms target (silentRx=%lldms%s).",
                                                 State::activeJitterBufferMs.load(std::memory_order_relaxed),
                                                 static_cast<long long>(silentRxMs), supBuf);
                                } else {
                                    UI::LogDebug("[Audio] Buffer depleted: Re-buffering to %dms target%s.",
                                                 State::activeJitterBufferMs.load(std::memory_order_relaxed), supBuf);
                                }
                            }
                        }

                        if (didConceal && State::globalDebugMode) {
                            static DebugLog::RateLimitState concealRate;
                            uint32_t suppressed = 0;
                            if (concealRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
                                char supBuf[32] = "";
                                if (suppressed > 0) {
                                    snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
                                }
                                UI::LogDebug("[Audio] WARNING: Transient jitter underrun concealed (buf=%.0fms, target=%dms, silentRx=%lldms%s) — raise Jitter Buffer if persistent.",
                                             sampledBufMs >= 0.0f ? sampledBufMs : 0.0f,
                                             State::activeJitterBufferMs.load(std::memory_order_relaxed),
                                             static_cast<long long>(silentRxMs),
                                             supBuf);
                            }
                        }

                        if (isBuffering) {
                            continue; 
                        }

                        if (bytesRead == 0) continue;

                        // Smooth 32-frame crossfade from the previous callback tail when resuming from
                        // buffering or transient jitter concealment, eliminating waveform discontinuities.
                        if (applyCrossfade && bytesRead > 0) {
                            if (format.bitDepth == 32) {
                                CrossfadeFloatHead(reinterpret_cast<float*>(linearBuffer.data()), numFramesAvailable, format.channels, crossfadeFrom, 32);
                            } else if (format.bitDepth == 16) {
                                size_t fadeInFrames = (std::min)(static_cast<size_t>(32), static_cast<size_t>(numFramesAvailable));
                                int16_t* p = reinterpret_cast<int16_t*>(linearBuffer.data());
                                for (size_t f = 0; f < fadeInFrames; ++f) {
                                    float factor = static_cast<float>(f) / static_cast<float>(fadeInFrames);
                                    for (size_t c = 0; c < format.channels; ++c) {
                                        p[f * format.channels + c] = static_cast<int16_t>(p[f * format.channels + c] * factor);
                                    }
                                }
                            }
                        }

                        // Mix the extracted/concealed frames into the master buffer
                        size_t framesToMix = bytesToProcess / bytesPerFrame;
                        size_t samplesToMix = framesToMix * pMixFormat->nChannels;
                        
                        if (format.bitDepth == 16) {
                            const int16_t* pcmData = reinterpret_cast<const int16_t*>(linearBuffer.data());
                            for (size_t s = 0; s < samplesToMix; ++s) {
                                mixBuffer[s] += (static_cast<float>(pcmData[s]) / 32768.0f) * vol;
                            }
                        } else if (format.bitDepth == 32) {
                            const float* pcmData = reinterpret_cast<const float*>(linearBuffer.data());
                            for (size_t s = 0; s < samplesToMix; ++s) {
                                mixBuffer[s] += pcmData[s] * vol;
                            }
                        }
                    }

                    // Clamp the master mix buffer to prevent clipping and copy to the hardware buffer.
                    float* pFloatData = reinterpret_cast<float*>(pData);
                    for (size_t i = 0; i < mixBufferSize; ++i) {
                        float s = mixBuffer[i];
                        if (s < -1.0f || s > 1.0f) {
                            ++clipSamples;
                            s = (s < -1.0f) ? -1.0f : 1.0f;
                        }
                        pFloatData[i] = s;
                    }

                    hr = pRenderClient->ReleaseBuffer(numFramesAvailable, 0);
                    if (FAILED(hr)) {
                        if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: ReleaseBuffer failed (0x%08X). Recovering...", hr);
                        deviceInvalidated = true;
                        break;
                    }
                }

                {
                    uint32_t tickMaxRxGapMs = 0;
                    for (const auto& q : activeQueues) {
                        const uint32_t rxGap = q->pendingAdaptRxGapMs.exchange(0, std::memory_order_relaxed);
                        tickMaxRxGapMs = (std::max)(tickMaxRxGapMs, rxGap);
                    }
                    double adaptDtSec = 0.0;
                    if (!activeQueues.empty()) {
                        adaptDtSec = (std::min)(0.25, (std::max)(0.0,
                            std::chrono::duration<double>(wakeNow - lastAdaptiveTime).count()));
                    }
                    lastAdaptiveTime = wakeNow;

                    const bool adaptiveEnabled = State::enableAdaptiveJitterBuffer.load(std::memory_order_relaxed);
                    const int requestedFloorMs = State::audioJitterBufferMs.load(std::memory_order_relaxed);
                    const int prevEffectiveMs = adaptiveJitter.effectiveTargetMs;
                    const int prevFloorMs = adaptiveJitter.floorMs;
                    if (adaptiveJitter.Update(adaptiveEnabled, requestedFloorMs, adaptDtSec, anyConcealOrStarve, tickMaxRxGapMs)) {
                        const int newEffectiveMs = adaptiveJitter.effectiveTargetMs;
                        State::activeJitterBufferMs.store(newEffectiveMs, std::memory_order_relaxed);
                        const bool floorMoved = (requestedFloorMs != prevFloorMs);
                        if (adaptiveEnabled && !floorMoved && State::globalDebugMode) {
                            if (newEffectiveMs > prevEffectiveMs) {
                                UI::LogDebug("[Audio] Adaptive Jitter Buffer expanded: %d -> %d ms (min=%dms, conceal=%d, rxGap=%ums).",
                                             prevEffectiveMs, newEffectiveMs, adaptiveJitter.floorMs,
                                             anyConcealOrStarve ? 1 : 0, tickMaxRxGapMs);
                            } else if (newEffectiveMs < prevEffectiveMs) {
                                UI::LogDebug("[Audio] Adaptive Jitter Buffer relaxed: %d -> %d ms (min=%dms).",
                                             prevEffectiveMs, newEffectiveMs, adaptiveJitter.floorMs);
                            }
                        }
                    }
                }

                if ((wakeNow - perfIntervalStart) >= std::chrono::seconds(30)) {
                    uint32_t totalOverruns = 0;
                    uint32_t totalRxGapSpikes = 0;
                    uint32_t maxRxGapMs = 0;
                    uint32_t maxRxPktBytes = 0;
                    uint32_t totalOpusFec = 0;
                    uint32_t totalOpusPlc = 0;
                    for (auto& q : activeQueues) {
                        totalOverruns += q->overrunCount.exchange(0, std::memory_order_relaxed);
                        totalRxGapSpikes += q->rxGapSpikes.exchange(0, std::memory_order_relaxed);
                        maxRxGapMs = (std::max)(maxRxGapMs, q->rxMaxGapMs.exchange(0, std::memory_order_relaxed));
                        maxRxPktBytes = (std::max)(maxRxPktBytes, q->rxMaxPktBytes.exchange(0, std::memory_order_relaxed));
                        q->rxPackets.exchange(0, std::memory_order_relaxed);
                        totalOpusFec += q->opusFecCount.exchange(0, std::memory_order_relaxed);
                        totalOpusPlc += q->opusPlcCount.exchange(0, std::memory_order_relaxed);
                        q->opusStaleCount.exchange(0, std::memory_order_relaxed);
                    }
                    const uint64_t anomalySum = static_cast<uint64_t>(concealCallbacks) + starvationResets +
                                                totalOverruns + hermiteLowCallbacks + hermiteHighCallbacks +
                                                wakeJitterSpikes + clipSamples + totalRxGapSpikes +
                                                totalOpusFec + totalOpusPlc;
                    if (State::globalDebugMode && Network::ShouldEmitPerfSummary(anomalySum)) {
                        float minOut = (bufSamples > 0) ? bufMinMs : 0.0f;
                        double avgOut = (bufSamples > 0) ? (bufSumMs / static_cast<double>(bufSamples)) : 0.0;
                        UI::LogDebug("[Audio Perf] 30s Buf: min=%.0f avg=%.0f max=%.0fms (target=%dms min=%dms auto=%d) | Conceal=%u Starv=%u Overrun=%u | Hermite=-%u/+%u | RxGapMax=%ums (>%ums=%u) MaxPkt=%uB | OpusFEC=%u PLC=%u | Jitter>25ms=%u Clip=%u",
                                     minOut, avgOut, bufMaxMs,
                                     State::activeJitterBufferMs.load(std::memory_order_relaxed),
                                     State::audioJitterBufferMs.load(std::memory_order_relaxed),
                                     State::enableAdaptiveJitterBuffer.load(std::memory_order_relaxed) ? 1 : 0,
                                     concealCallbacks, starvationResets, totalOverruns,
                                     hermiteLowCallbacks, hermiteHighCallbacks,
                                     maxRxGapMs, kAudioRxGapSpikeMs, totalRxGapSpikes, maxRxPktBytes,
                                     totalOpusFec, totalOpusPlc,
                                     wakeJitterSpikes, clipSamples);
                    }
                    perfIntervalStart = wakeNow;
                    bufMinMs = 1e9f;
                    bufMaxMs = 0.0f;
                    bufSumMs = 0.0;
                    bufSamples = 0;
                    concealCallbacks = 0;
                    starvationResets = 0;
                    hermiteLowCallbacks = 0;
                    hermiteHighCallbacks = 0;
                    clipSamples = 0;
                    wakeJitterSpikes = 0;
                }
            }

            // Teardown the active device instance
            pAudioClient->Stop();
            pRenderClient->Release();
            pAudioClient->Release();
            CoTaskMemFree(pMixFormat);

            if (deviceInvalidated && g_isRenderRunning) {
                if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: Audio endpoint lost (sleep/standby/unplug). Flushing queues and re-acquiring in 500ms...");
                // Flush stale audio queues to prevent an ancient audio burst upon wake
                {
                    std::shared_lock<std::shared_mutex> mapLock(g_queuesMutex);
                    for (auto& pair : g_clientAudioQueues) {
                        std::lock_guard<std::mutex> qLock(pair.second->mtx);
                        pair.second->ResetStreamState();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }

        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (State::globalDebugMode) UI::LogDebug("[Audio] Audio renderer thread stopped.");
    }

    /**
     * @brief Starts the Server's event-driven WASAPI audio mixing and rendering thread.
     * @return void
     */
    void StartAudioRenderer() {
        if (g_isRenderRunning) return;
        if (g_renderThread.joinable()) {
            g_renderThread.join();
        }
        if (!g_hRenderEvent) {
            g_hRenderEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }
        g_isRenderRunning = true;
        g_renderThread = std::thread(RenderThreadLoop);
    }

    /**
     * @brief Signals and joins the Server's audio renderer thread and clears all client audio queues.
     * @return void
     */
    void StopAudioRenderer() {
        if (!g_isRenderRunning) return;
        g_isRenderRunning = false;
        
        // Instantly wake the render loop so it doesn't hang on teardown
        if (g_hRenderEvent) SetEvent(g_hRenderEvent);

        if (g_renderThread.joinable()) {
            g_renderThread.join();
        }
        if (g_hRenderEvent) {
            CloseHandle(g_hRenderEvent);
            g_hRenderEvent = NULL;
        }
        g_hasRenderFormat = false;
        // Clear out any stale audio data.
        std::unique_lock<std::shared_mutex> lock(g_queuesMutex);
        g_clientAudioQueues.clear();
    }

    /**
     * @brief The main loop for the server's microphone capture thread.
     * Continuously captures the host's microphone and broadcasts it to connected clients
     * using OS-driven hardware buffer readiness events (AUDCLNT_STREAMFLAGS_EVENTCALLBACK).
     * Incorporates a self-healing device recovery loop to survive system sleep/standby,
     * USB microphone disconnect/reconnect, or Windows audio driver restarts.
     * @param targetFormat The format to capture the microphone in (aligned with the server's mix format).
     * @return void
     */
    void ServerMicCaptureThreadLoop(AudioFormat targetFormat) {
        CoInitRAII coInit;
        // Register with MMCSS to guarantee CPU scheduling even when a game is consuming 100% CPU
        HANDLE hMmcss = RegisterMMCSS(L"Pro Audio", "ServerMicCapture");

        while (g_isServerMicRunning) {
            if (!pEnumerator) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            IMMDevice* pDevice = nullptr;
            // eCapture for microphones, eConsole to respect the standard Windows Default Recording Device
            HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
            if (FAILED(hr) || !pDevice) {
                if (State::globalDebugMode) {
                    static uint64_t lastMicRetryLog = 0;
                    uint64_t now = GetTickCount64();
                    if (now - lastMicRetryLog > 3000) {
                        UI::LogDebug("[Audio] MicCaptureThread: Waiting for default recording endpoint...");
                        lastMicRetryLog = now;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            IAudioClient* pAudioClient = nullptr;
            hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
            pDevice->Release();
            pDevice = nullptr;
            if (FAILED(hr) || !pAudioClient) {
                static DebugLog::RateLimitState micActRate;
                LogHResultRateLimited(micActRate, "[Audio] MicCaptureThread: IAudioClient Activate failed", hr);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            if (!g_hServerMicEvent) {
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            ResetEvent(g_hServerMicEvent);

            WAVEFORMATEX targetWaveFormat = {};
            targetWaveFormat.wFormatTag = (targetFormat.bitDepth == 32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
            targetWaveFormat.nChannels = targetFormat.channels;
            targetWaveFormat.nSamplesPerSec = targetFormat.sampleRate;
            targetWaveFormat.wBitsPerSample = targetFormat.bitDepth;
            targetWaveFormat.nBlockAlign = (targetWaveFormat.nChannels * targetWaveFormat.wBitsPerSample) / 8;
            targetWaveFormat.nAvgBytesPerSec = targetWaveFormat.nSamplesPerSec * targetWaveFormat.nBlockAlign;
            targetWaveFormat.cbSize = 0;

            // Physical capture device using event-driven WASAPI callback and automatic PCM resampling
            hr = pAudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                0, 0, &targetWaveFormat, nullptr);
            if (FAILED(hr)) {
                if (State::globalDebugMode) UI::LogDebug("[Audio] MicCaptureThread: IAudioClient Initialize failed (0x%08X). Retrying...", hr);
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            hr = pAudioClient->SetEventHandle(g_hServerMicEvent);
            if (FAILED(hr)) {
                static DebugLog::RateLimitState micSetEvtRate;
                LogHResultRateLimited(micSetEvtRate, "[Audio] MicCaptureThread: SetEventHandle failed", hr);
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            IAudioCaptureClient* pCaptureClient = nullptr;
            hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
            if (FAILED(hr) || !pCaptureClient) {
                static DebugLog::RateLimitState micSvcRate;
                LogHResultRateLimited(micSvcRate, "[Audio] MicCaptureThread: GetService(IAudioCaptureClient) failed", hr);
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            hr = pAudioClient->Start();
            if (FAILED(hr)) {
                static DebugLog::RateLimitState micStartRate;
                LogHResultRateLimited(micStartRate, "[Audio] MicCaptureThread: IAudioClient Start failed", hr);
                pCaptureClient->Release();
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            if (State::globalDebugMode) {
                std::string micName = GetDefaultMicName();
                const bool initialOpus = AudioCodec::ResolveEffectiveAudioCodec(
                    State::enableAudioOpus.load(std::memory_order_relaxed),
                    targetFormat.sampleRate, targetFormat.channels, targetFormat.bitDepth) == AudioCodec::kAudioCodecOpus;
                UI::LogDebug("[Audio] Server Microphone broadcast started ('%s', %uHz %uch %u-bit, Codec=%s, Event-Driven WASAPI).",
                             micName.c_str(), targetFormat.sampleRate, targetFormat.channels, targetFormat.bitDepth,
                             initialOpus ? "Opus" : "PCM");
            }

            bool deviceInvalidated = false;
            auto micCapPerfStart = std::chrono::steady_clock::now();
            CaptureTimingStats micStats;
            std::chrono::steady_clock::time_point lastMicSendTime{};
            bool hasLastMicSend = false;
            static DebugLog::RateLimitState micSendGapWarnRate;
            AudioCodec::OpusStreamEncoder micEncoder;
            std::vector<AudioCodec::EncodedOpusPacket> micEncodedScratch;
            std::vector<uint8_t> micTxScratch;
            bool micUsingOpus = false;

            auto sendTrackedMic = [&](const void* data, UINT32 dataSize, const char* srcLabel) {
                const auto sendStart = std::chrono::steady_clock::now();
                const uint32_t sendGapMs = hasLastMicSend
                    ? static_cast<uint32_t>((std::max)(static_cast<int64_t>(0),
                          std::chrono::duration_cast<std::chrono::milliseconds>(sendStart - lastMicSendTime).count()))
                    : 0u;
                Network::BroadcastMessage(Network::MessageType::EVENT_MIC_DATA, data, dataSize);
                const auto sendEnd = std::chrono::steady_clock::now();
                const uint32_t sendCallMs = static_cast<uint32_t>((std::max)(static_cast<int64_t>(0),
                    std::chrono::duration_cast<std::chrono::milliseconds>(sendEnd - sendStart).count()));

                const bool hadPrevSend = hasLastMicSend;
                lastMicSendTime = sendStart;
                hasLastMicSend = true;

                if (micStats.RecordSend(sendGapMs, hadPrevSend, sendCallMs, dataSize, false) &&
                    State::globalDebugMode) {
                    uint32_t suppressed = 0;
                    if (micSendGapWarnRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
                        char supBuf[32] = "";
                        if (suppressed > 0) {
                            snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
                        }
                        UI::LogDebug("[Audio] WARNING: ServerMicCapture send gap %ums (sendCall=%ums, pkt=%uB, src=%s%s).",
                                     sendGapMs, sendCallMs, dataSize, srcLabel, supBuf);
                    }
                }
            };

            while (g_isServerMicRunning && !deviceInvalidated) {
                DWORD waitResult = WaitForSingleObject(g_hServerMicEvent, 500);
                if (!g_isServerMicRunning) break;
                if (waitResult != WAIT_OBJECT_0) {
                    UINT32 testPacket = 0;
                    HRESULT hrTest = pCaptureClient->GetNextPacketSize(&testPacket);
                    if (FAILED(hrTest)) {
                        if (State::globalDebugMode) UI::LogDebug("[Audio] MicCaptureThread: Device invalidated on timeout (0x%08X). Recovering...", static_cast<uint32_t>(hrTest));
                        deviceInvalidated = true;
                        break;
                    }
                    continue;
                }

                UINT32 packetLength = 0;
                hr = pCaptureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) {
                    if (State::globalDebugMode) UI::LogDebug("[Audio] MicCaptureThread: Device invalidated (0x%08X). Recovering...", hr);
                    deviceInvalidated = true;
                    break;
                }

                const bool wantMicOpus = State::enableAudioOpus.load(std::memory_order_relaxed);

                // Drain the hardware capture buffer completely upon event notification
                while (packetLength > 0) {
                    BYTE* pData = nullptr;
                    UINT32 numFramesAvailable = 0;
                    DWORD flags = 0;

                    hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
                    if (SUCCEEDED(hr)) {
                        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                            ++micStats.discontinuities;
                        }
                        const bool isSilentBuffer = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                        EmitAudioStreamFrames(
                            targetFormat, wantMicOpus, true, isSilentBuffer,
                            static_cast<uint16_t>(numFramesAvailable), isSilentBuffer ? nullptr : pData,
                            micEncoder, micUsingOpus, micEncodedScratch, micTxScratch,
                            [&](const void* pkt, UINT32 pktBytes) {
                                sendTrackedMic(pkt, pktBytes, isSilentBuffer ? "WASAPI-Silent" : "WASAPI");
                            });
                        pCaptureClient->ReleaseBuffer(numFramesAvailable);
                    } else {
                        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
                            if (State::globalDebugMode) UI::LogDebug("[Audio] MicCaptureThread: GetBuffer invalidated (0x%08X).", static_cast<uint32_t>(hr));
                            deviceInvalidated = true;
                        } else {
                            static DebugLog::RateLimitState micGetBufRate;
                            LogHResultRateLimited(micGetBufRate, "[Audio] MicCaptureThread: GetBuffer failed", hr);
                        }
                        break;
                    }

                    HRESULT hrNext = pCaptureClient->GetNextPacketSize(&packetLength);
                    if (FAILED(hrNext)) {
                        if (State::globalDebugMode) UI::LogDebug("[Audio] MicCaptureThread: Drain GetNextPacketSize failed (0x%08X). Recovering...", static_cast<uint32_t>(hrNext));
                        deviceInvalidated = true;
                        break;
                    }
                }
                MaybeLogCapturePerf30s("ServerMicCapture", micCapPerfStart, micStats);
            }

            pAudioClient->Stop();
            pCaptureClient->Release();
            pAudioClient->Release();

            if (deviceInvalidated && g_isServerMicRunning) {
                if (State::globalDebugMode) UI::LogDebug("[Audio] MicCaptureThread: Microphone device lost. Re-acquiring in 500ms...");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }

        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (State::globalDebugMode) UI::LogDebug("[Audio] Server Microphone broadcast stopped.");
    }

    /**
     * @brief Starts the background thread to broadcast the Server's microphone.
     * @return void
     */
    void StartMicBroadcast() {
        if (g_isServerMicRunning) return;
        
        AudioFormat format;
        // Capture using the exact same format as our render mix to keep network uniform
        if (!GetDefaultDeviceFormat(format)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] Cannot start mic broadcast: Could not get target format.");
            return;
        }
        if (g_serverMicThread.joinable()) {
            g_serverMicThread.join();
        }
        if (!g_hServerMicEvent) {
            g_hServerMicEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }
        g_isServerMicRunning = true;
        g_serverMicThread = std::thread(ServerMicCaptureThreadLoop, format);
    }

    /**
     * @brief Safely stops the Server's microphone broadcast thread.
     * @return void
     */
    void StopMicBroadcast() {
        if (!g_isServerMicRunning) return;
        g_isServerMicRunning = false;
        if (g_hServerMicEvent) {
            SetEvent(g_hServerMicEvent);
        }
        if (g_serverMicThread.joinable()) {
            g_serverMicThread.join();
        }
        if (g_hServerMicEvent) {
            CloseHandle(g_hServerMicEvent);
            g_hServerMicEvent = NULL;
        }
    }

    /**
     * @brief Locates the active VB-Audio Virtual Cable render endpoint ("CABLE Input").
     * @param ppDevice Receives an AddRef'd IMMDevice pointer on success.
     * @return S_OK if the CABLE Input device was found, or an HRESULT error code otherwise.
     */
    HRESULT GetAudioDevice(IMMDevice** ppDevice) {
        IMMDeviceCollection* pCollection = nullptr;
        HRESULT hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
        if (FAILED(hr)) return hr;

        UINT count = 0;
        pCollection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDevice = nullptr;
            if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
                IPropertyStore* pProps = nullptr;
                if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                        if (varName.vt == VT_LPWSTR && wcsstr(varName.pwszVal, L"CABLE Input") != nullptr) {
                            *ppDevice = pDevice;
                            PropVariantClear(&varName);
                            pProps->Release();
                            pCollection->Release();
                            return S_OK;
                        }
                        PropVariantClear(&varName);
                    }
                    pProps->Release();
                }
                pDevice->Release();
            }
        }
        pCollection->Release();
        return E_FAIL;
    }

    /**
     * @brief Checks if the VB-Audio Virtual Cable is installed and active on the system.
     * @return true if the CABLE Input device is found.
     */
    bool IsVirtualAudioCableInstalled() {
        IMMDevice* pDevice = nullptr;
        if (SUCCEEDED(GetAudioDevice(&pDevice)) && pDevice != nullptr) {
            pDevice->Release();
            return true;
        }
        return false;
    }

    /**
     * @brief Plays the incoming microphone stream into the target virtual cable device
     * using OS-driven hardware buffer readiness events (AUDCLNT_STREAMFLAGS_EVENTCALLBACK).
     * Incorporates a self-healing device recovery loop to survive system sleep/standby
     * or Virtual Audio Cable driver restarts.
     * @param sourceFormat The format of the incoming microphone stream.
     * @return void
     */
    void ClientMicRenderThreadLoop(AudioFormat sourceFormat) {
        CoInitRAII coInit;
        // Register with MMCSS to guarantee CPU scheduling even when a game is consuming 100% CPU
        HANDLE hMmcss = RegisterMMCSS(L"Pro Audio", "ClientMicRender");

        while (g_isClientMicRunning) {
            IMMDevice* pDevice = nullptr;
            HRESULT hr = GetAudioDevice(&pDevice);
            if (FAILED(hr) || !pDevice) {
                if (State::globalDebugMode) {
                    static uint64_t lastCableLog = 0;
                    uint64_t now = GetTickCount64();
                    if (now - lastCableLog > 3000) {
                        UI::LogDebug("[Audio] ClientMicRender: Waiting for target virtual cable device...");
                        lastCableLog = now;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            IAudioClient* pAudioClient = nullptr;
            hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
            pDevice->Release();
            pDevice = nullptr;
            if (FAILED(hr) || !pAudioClient) {
                static DebugLog::RateLimitState cmActRate;
                LogHResultRateLimited(cmActRate, "[Audio] ClientMicRender: IAudioClient Activate failed", hr);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            if (!g_hClientMicEvent) {
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            ResetEvent(g_hClientMicEvent);

            WAVEFORMATEX sourceWaveFormat = {};
            sourceWaveFormat.wFormatTag = (sourceFormat.bitDepth == 32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
            sourceWaveFormat.nChannels = sourceFormat.channels;
            sourceWaveFormat.nSamplesPerSec = sourceFormat.sampleRate;
            sourceWaveFormat.wBitsPerSample = sourceFormat.bitDepth;
            sourceWaveFormat.nBlockAlign = (sourceFormat.channels * sourceFormat.bitDepth) / 8;
            sourceWaveFormat.nAvgBytesPerSec = sourceWaveFormat.nSamplesPerSec * sourceWaveFormat.nBlockAlign;
            sourceWaveFormat.cbSize = 0;

            hr = pAudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                0, 0, &sourceWaveFormat, nullptr);
            if (FAILED(hr)) {
                if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: Initialize failed (0x%08X). Retrying...", hr);
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            hr = pAudioClient->SetEventHandle(g_hClientMicEvent);
            if (FAILED(hr)) {
                static DebugLog::RateLimitState cmSetEvtRate;
                LogHResultRateLimited(cmSetEvtRate, "[Audio] ClientMicRender: SetEventHandle failed", hr);
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            UINT32 bufferFrameCount = 0;
            pAudioClient->GetBufferSize(&bufferFrameCount);

            IAudioRenderClient* pRenderClient = nullptr;
            hr = pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient);
            if (FAILED(hr) || !pRenderClient) {
                static DebugLog::RateLimitState cmSvcRate;
                LogHResultRateLimited(cmSvcRate, "[Audio] ClientMicRender: GetService(IAudioRenderClient) failed", hr);
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            hr = pAudioClient->Start();
            if (FAILED(hr)) {
                static DebugLog::RateLimitState cmStartRate;
                LogHResultRateLimited(cmStartRate, "[Audio] ClientMicRender: IAudioClient Start failed", hr);
                pRenderClient->Release();
                pAudioClient->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            if (State::globalDebugMode) {
                UI::LogDebug("[Audio] Client Microphone Receiver started (%uHz %uch %u-bit, HW buffer: %u frames, Event-Driven WASAPI).",
                             sourceFormat.sampleRate, sourceFormat.channels, sourceFormat.bitDepth, bufferFrameCount);
            }

            bool deviceInvalidated = false;
            auto cmPerfStart = std::chrono::steady_clock::now();
            auto lastCmWake = cmPerfStart;
            bool hasCmWake = false;
            uint32_t micConcealCallbacks = 0;
            uint32_t micStarvationResets = 0;
            uint32_t micWakeJitterSpikes = 0;

            while (g_isClientMicRunning && !deviceInvalidated) {
                DWORD waitResult = WaitForSingleObject(g_hClientMicEvent, 500);
                if (!g_isClientMicRunning) break;
                if (waitResult != WAIT_OBJECT_0) {
                    hasCmWake = false;
                    UINT32 testPadding = 0;
                    HRESULT hrTest = pAudioClient->GetCurrentPadding(&testPadding);
                    if (FAILED(hrTest)) {
                        if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: Device invalidated on timeout (0x%08X). Recovering...", static_cast<uint32_t>(hrTest));
                        deviceInvalidated = true;
                        break;
                    }
                    continue;
                }

                auto cmWakeNow = std::chrono::steady_clock::now();
                if (hasCmWake) {
                    auto deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(cmWakeNow - lastCmWake).count();
                    if (deltaMs > 25) {
                        ++micWakeJitterSpikes;
                    }
                }
                lastCmWake = cmWakeNow;
                hasCmWake = true;

                UINT32 numPaddingFrames = 0;
                HRESULT hrPad = pAudioClient->GetCurrentPadding(&numPaddingFrames);
                if (FAILED(hrPad)) {
                    // Device invalidated (sleep/wake, driver update, device unplugged).
                    if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: Device invalidated (0x%08X). Recovering...", hrPad);
                    deviceInvalidated = true;
                    break;
                }
                UINT32 numFramesAvailable = bufferFrameCount - numPaddingFrames;

                if (numFramesAvailable > 0) {
                    BYTE* pData = nullptr;
                    hr = pRenderClient->GetBuffer(numFramesAvailable, &pData);
                    if (FAILED(hr)) {
                        if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: GetBuffer failed (0x%08X). Recovering...", hr);
                        deviceInvalidated = true;
                        break;
                    }
                    {
                        size_t bytesToProcess = numFramesAvailable * sourceWaveFormat.nBlockAlign;
                        bool writeSilence = false;
                        bool justStartedBuffering = false;
                        bool didMicConceal = false;
                        bool applyCrossfade = false;
                        float crossfadeFrom[8] = {0.0f};
                        size_t frameSize = sourceWaveFormat.nBlockAlign;
                        size_t micChannels = (std::min)(static_cast<size_t>(sourceWaveFormat.nChannels), static_cast<size_t>(8));

                        {
                            std::lock_guard<std::mutex> lock(g_clientMicQueue.mtx);
                            size_t availableBytes = g_clientMicQueue.buffer.Size();
                            size_t availableFrames = availableBytes / frameSize;
                            applyCrossfade = (g_clientMicQueue.wasBuffering || g_clientMicQueue.needsFadeIn);
                            for (size_t c = 0; c < micChannels; ++c) {
                                crossfadeFrom[c] = g_clientMicQueue.lastFrameFloat[c];
                            }

                            if (g_clientMicQueue.isBuffering) {
                                writeSilence = true;
                            } else if (availableFrames >= numFramesAvailable) {
                                g_clientMicQueue.starvationCount = 0;
                                g_clientMicQueue.wasBuffering = false;
                                g_clientMicQueue.needsFadeIn = false;
                                g_clientMicQueue.buffer.Pop(pData, bytesToProcess);
                                if (sourceWaveFormat.wBitsPerSample == 32 && numFramesAvailable > 0) {
                                    const float* pFloat = reinterpret_cast<const float*>(pData);
                                    for (size_t c = 0; c < micChannels; ++c) {
                                        g_clientMicQueue.lastFrameFloat[c] = pFloat[(numFramesAvailable - 1) * sourceWaveFormat.nChannels + c];
                                    }
                                }
                            } else {
                                if (availableFrames > 0) {
                                    g_clientMicQueue.starvationCount = 0;
                                    size_t validBytes = availableFrames * frameSize;
                                    g_clientMicQueue.buffer.Pop(pData, validBytes);
                                    if (sourceWaveFormat.wBitsPerSample == 32) {
                                        const float* pFloat = reinterpret_cast<const float*>(pData);
                                        for (size_t c = 0; c < micChannels; ++c) {
                                            g_clientMicQueue.lastFrameFloat[c] = pFloat[(availableFrames - 1) * sourceWaveFormat.nChannels + c];
                                        }
                                    }
                                } else {
                                    g_clientMicQueue.starvationCount++;
                                }

                                // Fast recovery: 6 consecutive empty callbacks (~60ms) triggers re-buffering
                                if (g_clientMicQueue.starvationCount >= 6) {
                                    g_clientMicQueue.ResetStreamState();
                                    writeSilence = true;
                                    justStartedBuffering = true;
                                } else {
                                    didMicConceal = true;
                                    g_clientMicQueue.wasBuffering = false;
                                    g_clientMicQueue.needsFadeIn = true;
                                    if (sourceWaveFormat.wBitsPerSample == 32) {
                                        FillDecayedTailFloat(reinterpret_cast<float*>(pData), availableFrames, numFramesAvailable, sourceWaveFormat.nChannels, g_clientMicQueue.lastFrameFloat);
                                    } else {
                                        size_t validBytes = availableFrames * frameSize;
                                        if (validBytes < bytesToProcess) {
                                            std::memset(pData + validBytes, 0, bytesToProcess - validBytes);
                                        }
                                    }
                                }
                            }
                        }

                        if (didMicConceal) ++micConcealCallbacks;
                        if (justStartedBuffering) ++micStarvationResets;

                        if (justStartedBuffering && State::globalDebugMode) {
                            static uint64_t lastMicStarvationLog = 0;
                            uint64_t now = GetTickCount64();
                            if (now - lastMicStarvationLog > 5000) {
                                UI::LogDebug("[Audio] Mic starvation recovery: Re-buffering stream.");
                                lastMicStarvationLog = now;
                            }
                        }

                        if (didMicConceal && State::globalDebugMode) {
                            static DebugLog::RateLimitState micConcealRate;
                            uint32_t suppressed = 0;
                            if (micConcealRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
                                char supBuf[32] = "";
                                if (suppressed > 0) {
                                    snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
                                }
                                UI::LogDebug("[Audio] WARNING: Transient mic jitter underrun concealed (target=%dms%s) — raise Jitter Buffer if persistent.",
                                             State::audioJitterBufferMs.load(std::memory_order_relaxed),
                                             supBuf);
                            }
                        }

                        // Apply smooth 32-frame crossfade when exiting buffering mode or resuming after transient dropout
                        if (applyCrossfade && !writeSilence) {
                            if (sourceWaveFormat.wBitsPerSample == 32) {
                                CrossfadeFloatHead(reinterpret_cast<float*>(pData), numFramesAvailable, sourceWaveFormat.nChannels, crossfadeFrom, 32);
                            } else if (sourceWaveFormat.wBitsPerSample == 16) {
                                size_t fadeInFrames = (std::min)(static_cast<size_t>(32), static_cast<size_t>(numFramesAvailable));
                                int16_t* p = reinterpret_cast<int16_t*>(pData);
                                for (size_t f = 0; f < fadeInFrames; ++f) {
                                    float factor = static_cast<float>(f) / static_cast<float>(fadeInFrames);
                                    for (size_t c = 0; c < sourceWaveFormat.nChannels; ++c) {
                                        p[f * sourceWaveFormat.nChannels + c] = static_cast<int16_t>(p[f * sourceWaveFormat.nChannels + c] * factor);
                                    }
                                }
                            }
                        }

                        if (writeSilence) {
                            memset(pData, 0, bytesToProcess);
                        }

                        hr = pRenderClient->ReleaseBuffer(numFramesAvailable, writeSilence ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
                        if (FAILED(hr)) {
                            if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: ReleaseBuffer failed (0x%08X). Recovering...", static_cast<uint32_t>(hr));
                            deviceInvalidated = true;
                            break;
                        }
                    }
                }

                if ((cmWakeNow - cmPerfStart) >= std::chrono::seconds(30)) {
                    uint32_t micOverruns = g_clientMicQueue.overrunCount.exchange(0, std::memory_order_relaxed);
                    uint32_t micOpusFec = g_clientMicQueue.opusFecCount.exchange(0, std::memory_order_relaxed);
                    uint32_t micOpusPlc = g_clientMicQueue.opusPlcCount.exchange(0, std::memory_order_relaxed);
                    g_clientMicQueue.opusStaleCount.exchange(0, std::memory_order_relaxed);
                    const uint64_t anomalySum = static_cast<uint64_t>(micConcealCallbacks) + micStarvationResets +
                                                micOverruns + micWakeJitterSpikes + micOpusFec + micOpusPlc;
                    if (State::globalDebugMode && Network::ShouldEmitPerfSummary(anomalySum)) {
                        UI::LogDebug("[Audio Perf] ClientMic 30s: Conceal=%u Starv=%u Overrun=%u OpusFEC=%u PLC=%u Jitter>25ms=%u",
                                     micConcealCallbacks, micStarvationResets, micOverruns,
                                     micOpusFec, micOpusPlc, micWakeJitterSpikes);
                    }
                    cmPerfStart = cmWakeNow;
                    micConcealCallbacks = 0;
                    micStarvationResets = 0;
                    micWakeJitterSpikes = 0;
                }
            }

            pAudioClient->Stop();
            pRenderClient->Release();
            pAudioClient->Release();

            if (deviceInvalidated && g_isClientMicRunning) {
                if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: Target device lost. Re-acquiring in 500ms...");
                {
                    std::lock_guard<std::mutex> lock(g_clientMicQueue.mtx);
                    g_clientMicQueue.ResetStreamState();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }

        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (State::globalDebugMode) UI::LogDebug("[Audio] Client Microphone Receiver stopped.");
    }

    /**
     * @brief Starts the client's microphone receiver thread to accept server mic data.
     * @return void
     */
    void StartMicReceiver() {
        if (g_isClientMicRunning) return;
        AudioFormat serverFormat;
        if (!Network::GetServerAudioFormat(serverFormat)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] Cannot start mic receiver: Server audio format not yet received.");
            return;
        }
        if (g_clientMicThread.joinable()) {
            g_clientMicThread.join();
        }
        if (!g_hClientMicEvent) {
            g_hClientMicEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }
        g_isClientMicRunning = true;
        
        {
            std::lock_guard<std::mutex> lock(g_clientMicQueue.mtx);
            g_clientMicQueue.ResetStreamState();
        }

        g_clientMicThread = std::thread(ClientMicRenderThreadLoop, serverFormat);
    }

    /**
     * @brief Stops the client's microphone receiver thread.
     * @return void
     */
    void StopMicReceiver() {
        if (!g_isClientMicRunning) return;
        g_isClientMicRunning = false;
        if (g_hClientMicEvent) {
            SetEvent(g_hClientMicEvent);
        }
        if (g_clientMicThread.joinable()) {
            g_clientMicThread.join();
        }
        if (g_hClientMicEvent) {
            CloseHandle(g_hClientMicEvent);
            g_hClientMicEvent = NULL;
        }
        std::lock_guard<std::mutex> lock(g_clientMicQueue.mtx);
        g_clientMicQueue.ResetStreamState();
    }

    /**
     * @brief Pushes incoming server microphone PCM data into the playback queue.
     * @param pcmData Pointer to the raw audio buffer.
     * @param dataSize Size of the audio buffer in bytes.
     * @return void
     */
    void HandleMicData(const void* pcmData, size_t dataSize) {
        if (!g_isClientMicRunning) return;

        AudioFormat format;
        if (!Network::GetServerAudioFormat(format)) return;

        uint8_t switchedCodec = 255;
        bool droppedOverrun = false;
        float loggedTargetMs = 0.0f;
        const float jitterTargetMs = static_cast<float>(State::audioJitterBufferMs.load(std::memory_order_relaxed));

        PushDecodedAudioPacket(
            g_clientMicQueue, format, pcmData, dataSize, jitterTargetMs,
            false, switchedCodec, droppedOverrun, loggedTargetMs);

        if (switchedCodec != 255 && State::globalDebugMode) {
            UI::LogDebug("[Audio] ClientMic receiver switched to codec: %s (%uHz %uch %u-bit).",
                         switchedCodec == AudioCodec::kAudioCodecOpus ? "Opus" : "PCM",
                         format.sampleRate, format.channels, format.bitDepth);
        }

        if (droppedOverrun && State::globalDebugMode) {
            static DebugLog::RateLimitState micOverrunRate;
            uint32_t suppressed = 0;
            if (micOverrunRate.TryAcquire(GetTickCount64(), 2000, suppressed)) {
                char supBuf[32] = "";
                if (suppressed > 0) {
                    snprintf(supBuf, sizeof(supBuf), " (+%u suppressed)", suppressed);
                }
                UI::LogDebug("[Audio Debug] Mic OVERRUN! Dropping oldest bytes to maintain %.0fms latency target%s.",
                             loggedTargetMs, supBuf);
            }
        }
    }

    /**
     * @brief Decodes and queues incoming client audio packets into that client's mix queue on the server.
     * @param clientIdentifier The socket handle representing the client.
     * @param pcmData Pointer to wire audio packet starting with AudioPacketHeader.
     * @param dataSize Size of the incoming packet in bytes.
     * @return void
     */
    void HandleAudioData(uintptr_t clientIdentifier, const void* pcmData, size_t dataSize) {
        if (!g_isRenderRunning) return;
        if (!g_hasRenderFormat.load(std::memory_order_acquire)) return;

        // Find or create a queue for this client (avoids locking coarse clientsMutex on the UDP hot path).
        std::shared_ptr<ClientAudioQueue> pQueue;
        {
            std::shared_lock<std::shared_mutex> readLock(g_queuesMutex);
            auto it = g_clientAudioQueues.find(clientIdentifier);
            if (it != g_clientAudioQueues.end()) {
                pQueue = it->second;
            }
        }
        
        if (!pQueue) {
            std::unique_lock<std::shared_mutex> writeLock(g_queuesMutex);
            auto& entry = g_clientAudioQueues[clientIdentifier];
            if (!entry) {
                entry = std::make_shared<ClientAudioQueue>();
            }
            pQueue = entry;
        }

        if (!pQueue->isEnabled.load(std::memory_order_relaxed)) return;

        AudioFormat renderFmt;
        renderFmt.sampleRate = g_serverRenderRate.load(std::memory_order_relaxed);
        renderFmt.bitDepth = g_serverRenderBits.load(std::memory_order_relaxed);
        renderFmt.channels = g_serverRenderChannels.load(std::memory_order_relaxed);
        renderFmt.codec = 0;
        uint8_t switchedCodec = 255;
        bool droppedOverrun = false;
        float loggedTargetMs = 0.0f;
        const float jitterTargetMs = static_cast<float>(State::activeJitterBufferMs.load(std::memory_order_relaxed));

        PushDecodedAudioPacket(
            *pQueue, renderFmt, pcmData, dataSize, jitterTargetMs,
            true, switchedCodec, droppedOverrun, loggedTargetMs);

        if (switchedCodec != 255 && State::globalDebugMode) {
            UI::LogDebug("[Audio] Mix stream (%llu) switched to codec: %s (%uHz %uch %u-bit).",
                         static_cast<unsigned long long>(clientIdentifier),
                         switchedCodec == AudioCodec::kAudioCodecOpus ? "Opus" : "PCM",
                         renderFmt.sampleRate, renderFmt.channels, renderFmt.bitDepth);
        }

        if (droppedOverrun && State::globalDebugMode) {
            static DebugLog::RateLimitState overrunRate;
            uint32_t suppressed = 0;
            if (overrunRate.TryAcquire(GetTickCount64(), 2000, suppressed)) {
                char supBuf[32] = "";
                if (suppressed > 0) {
                    snprintf(supBuf, sizeof(supBuf), " (+%u suppressed)", suppressed);
                }
                UI::LogDebug("[Audio Debug] OVERRUN! Dropping oldest bytes to maintain %.0fms latency target%s.",
                             loggedTargetMs, supBuf);
            }
        }
    }

    /**
     * @brief Clears the audio queue for a specific client (e.g., on disconnect).
     * @param clientIdentifier The socket handle representing the client.
     * @return void
     */
    void ClearClientAudioQueue(uintptr_t clientIdentifier) {
        std::unique_lock<std::shared_mutex> mapLock(g_queuesMutex);
        if (g_clientAudioQueues.count(clientIdentifier)) {
            g_clientAudioQueues.erase(clientIdentifier);
        }
    }

    /**
     * @brief Resets a client's audio jitter buffer and resampling state without erasing its queue entry.
     * Preserves lock-free enable and volume atomics across TCP-to-UDP transport switchover so stale
     * TCP audio backlog never stacks onto newly arriving UDP packets.
     * @param clientIdentifier The socket handle representing the client.
     * @return void
     */
    void ResetClientAudioQueue(uintptr_t clientIdentifier) {
        std::shared_ptr<ClientAudioQueue> pQueue;
        {
            std::shared_lock<std::shared_mutex> mapLock(g_queuesMutex);
            auto it = g_clientAudioQueues.find(clientIdentifier);
            if (it != g_clientAudioQueues.end()) {
                pQueue = it->second;
            }
        }
        if (pQueue) {
            std::lock_guard<std::mutex> qLock(pQueue->mtx);
            pQueue->ResetStreamState();
        }
    }

    /**
     * @brief Notifies the audio subsystem that the system resumed from sleep or hibernate.
     * Unblocks the server render loop, purges stale audio/mic buffers, and triggers device re-acquisition.
     * @return void
     */
    void NotifySystemResumed() {
        if (State::globalDebugMode) UI::LogDebug("[Audio] System resumed from sleep/standby: Signaling audio recovery.");

        // Instantly unblock the server render and mic loops so they do not wait on timeouts
        if (g_hRenderEvent) {
            SetEvent(g_hRenderEvent);
        }
        if (g_hServerMicEvent) {
            SetEvent(g_hServerMicEvent);
        }
        if (g_hClientMicEvent) {
            SetEvent(g_hClientMicEvent);
        }

        // Purge stale audio queues from before sleep to prevent a delayed audio burst on wake
        {
            std::shared_lock<std::shared_mutex> mapLock(g_queuesMutex);
            for (auto& pair : g_clientAudioQueues) {
                std::lock_guard<std::mutex> qLock(pair.second->mtx);
                pair.second->ResetStreamState();
            }
        }

        // Purge client mic queue
        {
            std::lock_guard<std::mutex> lock(g_clientMicQueue.mtx);
            g_clientMicQueue.ResetStreamState();
        }
    }

    /**
     * @brief Broadcasts updated audio codec configuration from Server to all connected clients.
     * Invoked when the Server user toggles Opus compression in real time.
     * @return void
     */
    void NotifyCodecSettingChanged() {
        if (State::currentRole != State::AppRole::SERVER) {
            return;
        }
        AudioFormat fmt;
        bool hasFmt = false;
        {
            std::lock_guard<std::mutex> fmtLock(g_formatBroadcastMutex);
            if (g_hasRenderFormat.load(std::memory_order_relaxed)) {
                fmt = g_serverRenderFormat;
                hasFmt = true;
            }
        }
        if (!hasFmt && !GetDefaultDeviceFormat(fmt)) {
            return;
        }
        const bool wantOpus = State::enableAudioOpus.load(std::memory_order_relaxed);
        fmt.codec = AudioCodec::ResolveEffectiveAudioCodec(wantOpus, fmt.sampleRate, fmt.channels, fmt.bitDepth);
        {
            std::lock_guard<std::mutex> fmtLock(g_formatBroadcastMutex);
            if (g_hasRenderFormat.load(std::memory_order_relaxed)) {
                g_serverRenderFormat.codec = fmt.codec;
            }
        }
        if (State::globalDebugMode) {
            if (wantOpus && fmt.codec != AudioCodec::kAudioCodecOpus) {
                UI::LogDebug("[Audio] Opus unavailable for %uHz %uch %u-bit format; keeping PCM.",
                             fmt.sampleRate, fmt.channels, fmt.bitDepth);
            } else {
                UI::LogDebug("[Audio] Server Codec toggled to %s (%uHz %uch %u-bit). Broadcasting to clients.",
                             fmt.codec == AudioCodec::kAudioCodecOpus ? "Opus" : "PCM",
                             fmt.sampleRate, fmt.channels, fmt.bitDepth);
            }
        }
        const Network::AudioFormatPayload wireFmt = MakeAudioFormatPayload(fmt);
        Network::BroadcastMessage(Network::MessageType::EVENT_AUDIO_FORMAT, &wireFmt, sizeof(wireFmt));
    }
}