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

    /// Structure to hold the standard PCM audio format for network transmission.
    struct AudioFormat {
        uint32_t sampleRate;
        uint16_t bitDepth;
        uint16_t channels;
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
}