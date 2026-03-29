// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Audio Manager Interface
// =============================================================================

/// @file AudioManager.h
/// Defines the abstract interface for audio capture and playback using WASAPI.

#pragma once
#include <cstdint>
#include <string>

namespace Audio {

    /// Structure to hold the standard PCM audio format for network transmission.
    struct AudioFormat {
        uint32_t sampleRate;
        uint16_t bitDepth;
        uint16_t channels;
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

    /// Called by the network layer to purge a disconnected or disabled client's audio buffer.
    void ClearClientAudioQueue(uintptr_t clientIdentifier);
}