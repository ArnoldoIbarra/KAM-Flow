// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/// @file AudioManager.h
/// Defines the abstract interface for audio capture and playback using WASAPI.

#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace Audio {

    /// Structure to hold the audio stream format and active wire codec.
    struct AudioFormat {
        uint32_t sampleRate = 0;
        uint16_t bitDepth = 0;
        uint16_t channels = 0;
        uint8_t codec = 0; ///< Active wire codec (0 = PCM, 1 = Opus).
    };

    /// Jitter-buffer deadband, hysteresis release threshold, and burst trim ceiling in milliseconds.
    struct BufferBounds {
        float lowDeadbandMs;
        float highDeadbandMs;
        float releaseMs;
        float maxBufferMs;
    };

    /// Computes bit-perfect deadband, pre-fill release threshold, and active overrun ceiling for a target jitter buffer.
    inline BufferBounds ComputeBufferBounds(float targetMs) {
        if (targetMs < 10.0f) {
            targetMs = 10.0f;
        }
        return {
            (std::max)(18.0f, targetMs * 0.65f),
            targetMs * 1.60f + 15.0f,
            targetMs + 12.0f,
            (std::max)(180.0f, targetMs * 2.5f + 100.0f)
        };
    }

    /// Computes the playback resampling rate: exact 1.0 inside the deadband, +/-0.20% at edges, and up to +0.75% when >40ms bloated.
    inline double ComputePlaybackRate(float currentBufferMs, float targetMs) {
        const BufferBounds bounds = ComputeBufferBounds(targetMs);
        if (currentBufferMs >= bounds.lowDeadbandMs && currentBufferMs <= bounds.highDeadbandMs) {
            return 1.0;
        }
        if (currentBufferMs < bounds.lowDeadbandMs) {
            return 0.9980;
        }
        const float fastDrainMs = bounds.highDeadbandMs + 40.0f;
        if (currentBufferMs <= fastDrainMs) {
            return 1.0020;
        }
        const double rampFrac = (std::min)(1.0, static_cast<double>(currentBufferMs - fastDrainMs) / 80.0);
        return 1.0020 + rampFrac * 0.0055;
    }

    /// Computes playback rate, smoothly seeking into [targetMs, targetMs + 15ms] at +/-0.60% after live slider changes.
    inline double ResolvePlaybackRate(float currentBufferMs, float targetMs, bool& inOutSeekingTarget) {
        const float safeTarget = (targetMs < 10.0f) ? 10.0f : targetMs;
        if (inOutSeekingTarget) {
            const float settleHighMs = safeTarget + 15.0f;
            if (currentBufferMs >= safeTarget && currentBufferMs <= settleHighMs) {
                inOutSeekingTarget = false;
                return 1.0;
            }
            if (currentBufferMs < safeTarget) {
                return 0.9940;
            }
            return (std::max)(1.0060, ComputePlaybackRate(currentBufferMs, safeTarget));
        }
        return ComputePlaybackRate(currentBufferMs, safeTarget);
    }

    /// Computes the minimum idle gap (in ms) before client loopback capture injects synthetic keep-alive silence.
    inline int64_t ComputeIdleSilenceThresholdMs(int64_t lastPacketDurationMs) {
        const int64_t safeDuration = (lastPacketDurationMs > 0) ? lastPacketDurationMs : 10;
        return (std::max)(static_cast<int64_t>(30), safeDuration + 15);
    }

    /// Outgoing capture send-gap threshold (ms) counted as a spike in 30s summaries (above normal 30-38ms silence transitions).
    constexpr uint32_t kCaptureSendGapSpikeMs = 40;
    /// Outgoing capture send-gap threshold (ms) triggering an immediate rate-limited warning.
    constexpr uint32_t kCaptureSendGapWarnMs = 50;
    /// Outgoing network send() duration threshold (ms) flagging blocking audio transmits.
    constexpr uint32_t kCaptureSlowSendCallMs = 10;
    /// Server incoming audio inter-arrival gap threshold (ms) counted as a receive spike.
    constexpr uint32_t kAudioRxGapSpikeMs = 45;

    /// Accumulates per-30-second capture send timing, packet size, and WASAPI discontinuity telemetry.
    struct CaptureTimingStats {
        uint32_t realPackets = 0;
        uint32_t silencePackets = 0;
        uint32_t sendGapSpikes = 0;
        uint32_t slowSendCalls = 0;
        uint32_t maxSendGapMs = 0;
        uint32_t maxSendCallMs = 0;
        uint32_t maxPacketBytes = 0;
        uint32_t discontinuities = 0;
        uint32_t silenceClockSlips = 0;

        /// Records an outgoing audio packet send and returns true if an immediate warning should be logged.
        inline bool RecordSend(uint32_t sendGapMs, bool hasPrevSend, uint32_t sendCallMs,
                               uint32_t packetBytes, bool isSyntheticSilence) {
            if (isSyntheticSilence) {
                ++silencePackets;
            } else {
                ++realPackets;
            }
            maxPacketBytes = (std::max)(maxPacketBytes, packetBytes);
            maxSendCallMs = (std::max)(maxSendCallMs, sendCallMs);
            if (sendCallMs > kCaptureSlowSendCallMs) {
                ++slowSendCalls;
            }
            if (hasPrevSend) {
                maxSendGapMs = (std::max)(maxSendGapMs, sendGapMs);
                if (sendGapMs > kCaptureSendGapSpikeMs) {
                    ++sendGapSpikes;
                }
            }
            return (hasPrevSend && sendGapMs > kCaptureSendGapWarnMs) || (sendCallMs > kCaptureSlowSendCallMs);
        }

        /// Returns true if any timing anomaly, discontinuity, or blocking send occurred in this window.
        inline bool HasAnomaly() const {
            return discontinuities > 0 || silenceClockSlips > 0 || sendGapSpikes > 0 || slowSendCalls > 0;
        }

        /// Resets all counters and maxima for the next 30-second reporting interval.
        inline void Reset() {
            realPackets = 0;
            silencePackets = 0;
            sendGapSpikes = 0;
            slowSendCalls = 0;
            maxSendGapMs = 0;
            maxSendCallMs = 0;
            maxPacketBytes = 0;
            discontinuities = 0;
            silenceClockSlips = 0;
        }
    };

    /// Minimum allowed jitter buffer target in milliseconds.
    constexpr int kMinJitterBufferMs = 20;
    /// Maximum allowed jitter buffer target in milliseconds.
    constexpr int kMaxJitterBufferMs = 200;
    /// Step size (ms) when adaptive jitter expands on concealment/starvation/severe network gaps.
    constexpr int kAdaptiveJitterStepUpMs = 20;
    /// Step size (ms) when adaptive jitter relaxes after sustained clean playback.
    constexpr int kAdaptiveJitterStepDownMs = 10;
    /// Cooldown (seconds) between consecutive upward adaptive jitter expansions.
    constexpr double kAdaptiveExpandCooldownSec = 4.0;
    /// Clean playback duration (seconds) required before adaptive jitter steps down.
    constexpr double kAdaptiveRelaxCleanSec = 60.0;
    /// Minimum cooldown (seconds) between consecutive downward relaxation steps.
    constexpr double kAdaptiveRelaxCooldownSec = 30.0;

    /// Pure-logic controller for fast-attack, slow-decay adaptive jitter buffer sizing.
    struct AdaptiveJitterController {
        int floorMs = 80;
        int effectiveTargetMs = 80;
        double cleanSeconds = 0.0;
        double secondsSinceLastExpand = kAdaptiveExpandCooldownSec;
        double secondsSinceLastRelax = kAdaptiveRelaxCooldownSec;

        /// Resets controller state to the given floor target in milliseconds.
        inline void Reset(int floor) {
            const int clamped = (std::max)(kMinJitterBufferMs, (std::min)(kMaxJitterBufferMs, floor));
            floorMs = clamped;
            effectiveTargetMs = clamped;
            cleanSeconds = 0.0;
            secondsSinceLastExpand = kAdaptiveExpandCooldownSec;
            secondsSinceLastRelax = kAdaptiveRelaxCooldownSec;
        }

        /// Returns true if adaptive logic has raised the active target above the configured floor.
        inline bool IsExpanded() const {
            return effectiveTargetMs > floorMs;
        }

        /// Advances timers and updates effectiveTargetMs; returns true if effectiveTargetMs changed.
        inline bool Update(bool adaptiveEnabled,
                           int requestedFloorMs,
                           double dtSec,
                           bool hadConcealOrStarve,
                           uint32_t maxRxGapMs) {
            const int clampedFloor = (std::max)(kMinJitterBufferMs, (std::min)(kMaxJitterBufferMs, requestedFloorMs));
            const int prevTarget = effectiveTargetMs;

            if (!adaptiveEnabled) {
                floorMs = clampedFloor;
                effectiveTargetMs = clampedFloor;
                cleanSeconds = 0.0;
                secondsSinceLastExpand = kAdaptiveExpandCooldownSec;
                secondsSinceLastRelax = kAdaptiveRelaxCooldownSec;
                return effectiveTargetMs != prevTarget;
            }

            if (clampedFloor != floorMs) {
                const int oldFloor = floorMs;
                floorMs = clampedFloor;
                if (effectiveTargetMs < floorMs || effectiveTargetMs <= oldFloor) {
                    effectiveTargetMs = floorMs;
                }
            }

            if (dtSec > 0.0) {
                secondsSinceLastExpand += dtSec;
                secondsSinceLastRelax += dtSec;
            }

            const double severeRxThresholdMs = static_cast<double>(effectiveTargetMs) * 0.80;
            const bool severeRxGap = static_cast<double>(maxRxGapMs) > severeRxThresholdMs;
            if (hadConcealOrStarve || severeRxGap) {
                cleanSeconds = 0.0;
                if (secondsSinceLastExpand >= kAdaptiveExpandCooldownSec &&
                    effectiveTargetMs < kMaxJitterBufferMs) {
                    effectiveTargetMs = (std::min)(kMaxJitterBufferMs, effectiveTargetMs + kAdaptiveJitterStepUpMs);
                    secondsSinceLastExpand = 0.0;
                }
                return effectiveTargetMs != prevTarget;
            }

            if (dtSec > 0.0 && effectiveTargetMs > floorMs) {
                cleanSeconds += dtSec;
                if (cleanSeconds >= kAdaptiveRelaxCleanSec &&
                    secondsSinceLastRelax >= kAdaptiveRelaxCooldownSec) {
                    effectiveTargetMs = (std::max)(floorMs, effectiveTargetMs - kAdaptiveJitterStepDownMs);
                    cleanSeconds = 0.0;
                    secondsSinceLastRelax = 0.0;
                }
            }
            return effectiveTargetMs != prevTarget;
        }
    };

    /// Initializes the Audio Manager and COM for WASAPI.
    bool Initialize();

    /// Shuts down the Audio Manager and releases all WASAPI/COM resources.
    void Shutdown();

    /// Retrieves the native audio format of the default system playback device.
    bool GetDefaultDeviceFormat(AudioFormat& outFormat);

    /// Queries Windows for the friendly name of the active default microphone.
    std::string GetDefaultMicName();

    /// Checks if the VB-Audio Virtual Cable is installed and active on the system.
    bool IsVirtualAudioCableInstalled();

    /// Starts the client-side audio loopback capture thread.
    void StartLoopbackCapture();

    /// Stops the client-side audio loopback capture thread.
    void StopLoopbackCapture();

    /// Starts the server-side audio renderer thread to mix and play client audio.
    void StartAudioRenderer();

    /// Stops the server-side audio renderer thread.
    void StopAudioRenderer();

    /// Starts capturing the server's default communications microphone and broadcasting it to clients.
    void StartMicBroadcast();

    /// Stops the server's microphone broadcast thread.
    void StopMicBroadcast();

    /// Starts the client-side receiver for the server's microphone.
    void StartMicReceiver();

    /// Stops the client-side microphone receiver.
    void StopMicReceiver();

    /// Queues incoming microphone data from the server.
    void HandleMicData(const void* pcmData, size_t dataSize);

    /// Called by the network layer to queue incoming audio data from a specific client.
    void HandleAudioData(uintptr_t clientIdentifier, const void* pcmData, size_t dataSize);

    /// Updates the lock-free cached audio mixing state (enabled and volume) for a specific client.
    void SetClientAudioState(uintptr_t clientIdentifier, bool isEnabled, float volume);

    /// Called by the network layer to purge a disconnected or disabled client's audio buffer.
    void ClearClientAudioQueue(uintptr_t clientIdentifier);

    /// Resets a client's audio jitter buffer state on transport switchover while preserving enable/volume atomics.
    void ResetClientAudioQueue(uintptr_t clientIdentifier);

    /// Notifies the audio subsystem that the system resumed from sleep or hibernate.
    void NotifySystemResumed();

    /// Broadcasts updated audio codec configuration from Server to all connected clients.
    void NotifyCodecSettingChanged();
}