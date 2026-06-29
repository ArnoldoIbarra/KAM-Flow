/*
 * Copyright (c) 2026 Arnoldo Ibarra. All Rights Reserved.
 *
 * This software is the confidential and proprietary information of Arnoldo Ibarra.
 * ("Confidential Information"). You shall not disclose such Confidential 
 * Information and shall use it only in accordance with the terms of the 
 * license agreement you entered into.
 */

/**
 * @file AudioManager_Win.cpp
 * @brief Implementation of the AudioManager using Windows Core Audio (WASAPI).
 * Handles device enumeration, format negotiation, and stream initialization.
 */

#include "AudioManager.h"
#include "StateManager.h"
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
#include <thread>
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
        bool isBuffering = true;
        bool wasBuffering = false;  // Tracks previous buffering state for silence→audio fade-in
        int starvationCount = 0;
    };
    std::map<uintptr_t, std::shared_ptr<ClientAudioQueue>> g_clientAudioQueues;
    std::shared_mutex g_queuesMutex; // Protects the map itself during additions/removals.

    // --- CLIENT MIC RECEIVER THREAD ---
    std::thread g_clientMicThread;
    std::atomic<bool> g_isClientMicRunning(false);
    ClientAudioQueue g_clientMicQueue;

    // --- SERVER MIC CAPTURE THREAD ---
    /// The background thread that captures the server's microphone for broadcast.
    std::thread g_serverMicThread;
    std::atomic<bool> g_isServerMicRunning(false);

    /**
     * @brief Initializes COM and creates the WASAPI device enumerator.
     * @return true if successful, false otherwise.
     */
    bool Initialize() {
        if (pEnumerator) return true;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] CoInitializeEx failed.");
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
            if (State::globalDebugMode) UI::LogDebug("[Audio] CoCreateInstance for MMDeviceEnumerator failed.");
            CoUninitialize();
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
        CoUninitialize();
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
        if (FAILED(hr)) return false;

        IAudioClient* pAudioClient = nullptr;
        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
        pDevice->Release();
        if (FAILED(hr)) return false;

        WAVEFORMATEX* pWaveFormat = nullptr;
        hr = pAudioClient->GetMixFormat(&pWaveFormat);
        pAudioClient->Release();
        if (FAILED(hr)) return false;

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
     * @param targetFormat The audio format requested by the server.
     * @return void
     */
    void CaptureThreadLoop(AudioFormat targetFormat) {
        CoInitRAII coInit;
        // Register with MMCSS to guarantee CPU scheduling even when a game is consuming 100% CPU
        HANDLE hMmcss = RegisterMMCSS(L"Pro Audio", "CaptureThread");

        // 1. Get the default audio playback device (e.g., speakers).
        IMMDevice* pDevice = nullptr;
        HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: GetDefaultAudioEndpoint failed.");
            return;
        }

        // 2. Activate an IAudioClient for the device.
        IAudioClient* pAudioClient = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);    
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: Activate IAudioClient failed.");
        pDevice->Release();
            return;
        }

    // Mute the client's system audio to prevent echo. We do this now while we still have the device object.
    hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&g_pClientEndpointVolume);
    if (SUCCEEDED(hr)) {
        g_pClientEndpointVolume->GetMute(&g_bOriginalMuteState);
        g_pClientEndpointVolume->SetMute(TRUE, nullptr);
    }
    pDevice->Release(); // We are now finished with the IMMDevice, so we can safely release it.

        // 3. Set up the target WAVEFORMATEX based on the server's request.
        WAVEFORMATEX* pTargetWaveFormat = new WAVEFORMATEX();
        pTargetWaveFormat->wFormatTag = (targetFormat.bitDepth == 32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        pTargetWaveFormat->nChannels = targetFormat.channels;
        pTargetWaveFormat->nSamplesPerSec = targetFormat.sampleRate;
        pTargetWaveFormat->wBitsPerSample = targetFormat.bitDepth;
        pTargetWaveFormat->nBlockAlign = (pTargetWaveFormat->nChannels * pTargetWaveFormat->wBitsPerSample) / 8;
        pTargetWaveFormat->nAvgBytesPerSec = pTargetWaveFormat->nSamplesPerSec * pTargetWaveFormat->nBlockAlign;
        pTargetWaveFormat->cbSize = 0;

        // 4. Initialize the audio client for loopback capture.
        // We ask WASAPI to automatically convert the audio to our target format,
        // and allocate a 100ms buffer to safely absorb Windows OS thread scheduling jitter.
        REFERENCE_TIME hnsRequestedDuration = 1000000; // 100ms
        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, hnsRequestedDuration, 0, pTargetWaveFormat, nullptr);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: IAudioClient Initialize failed. Format mismatch?");
            if (g_pClientEndpointVolume) g_pClientEndpointVolume->Release();
            pAudioClient->Release();
            return;
        }

        // 5. Get the capture service.
        IAudioCaptureClient* pCaptureClient = nullptr;
        hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: GetService for IAudioCaptureClient failed.");
            if (g_pClientEndpointVolume) g_pClientEndpointVolume->Release();
            pAudioClient->Release();
            return;
        }

        // 6. Start the audio stream.
        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] CaptureThread: IAudioClient Start failed.");
            pCaptureClient->Release();
            if (g_pClientEndpointVolume) g_pClientEndpointVolume->Release();
            pAudioClient->Release();
            return;
        }

        // With a 100ms WASAPI buffer, polling every 10ms is completely immune 
        // to Windows' 15.6ms timer resolution worst-case limits.
        DWORD sleepTimeMs = 10;

        if (State::globalDebugMode) UI::LogDebug("[Audio] Loopback capture started. Jitter-immune 10ms Polling active.");

        // 7. Main capture loop.
        while (g_isCaptureRunning) {
            UINT32 packetLength = 0;
            
            // DRAIN LOOP: Pull *all* available packets from the WASAPI buffer.
            // If we only pull one per sleep cycle, audio will silently drift and starve the server!
            while (SUCCEEDED(pCaptureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
                BYTE* pData;
                UINT32 numFramesAvailable;
                DWORD flags;

                // Get the available data in the shared buffer.
                hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    UINT32 dataSize = numFramesAvailable * pTargetWaveFormat->nBlockAlign;
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        // CRITICAL: We MUST send silence to keep the server's playback clock perfectly synced.
                        // Ignoring silent packets causes the server to consume audio faster than real-time, leading to starvation.
                        static thread_local std::vector<BYTE> silenceBuffer;
                        if (silenceBuffer.size() < dataSize) silenceBuffer.resize(dataSize, 0);
                        else std::memset(silenceBuffer.data(), 0, dataSize);
                        
                        Network::SendToServer(Network::MessageType::EVENT_AUDIO_DATA, silenceBuffer.data(), dataSize);
                    } else {
                        Network::SendToServer(Network::MessageType::EVENT_AUDIO_DATA, pData, dataSize);
                    }
                    pCaptureClient->ReleaseBuffer(numFramesAvailable);
                } else {
                    break; // If GetBuffer fails, stop trying to drain for this cycle
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepTimeMs));
        }

        // 8. Cleanup.
        pAudioClient->Stop();
        pCaptureClient->Release();
        pAudioClient->Release();

        // Restore the client's original mute state.
        if (g_pClientEndpointVolume) {
            g_pClientEndpointVolume->SetMute(g_bOriginalMuteState, nullptr);
            g_pClientEndpointVolume->Release();
            g_pClientEndpointVolume = nullptr;
        }

        delete pTargetWaveFormat;
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (State::globalDebugMode) UI::LogDebug("[Audio] Loopback capture thread stopped.");
    }

    void StartLoopbackCapture() {
        if (g_isCaptureRunning) return;
        AudioFormat serverFormat;
        if (!Network::GetServerAudioFormat(serverFormat)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] Cannot start capture: Server audio format not yet received.");
            return;
        }
        g_isCaptureRunning = true;
        g_captureThread = std::thread(CaptureThreadLoop, serverFormat);
    }

    void StopLoopbackCapture() {
        if (!g_isCaptureRunning) return;
        g_isCaptureRunning = false;
        if (g_captureThread.joinable()) {
            g_captureThread.join();
        }
    }

    /**
     * @brief The main loop for the server's audio rendering thread.
     * @return void
     */
    void RenderThreadLoop() {
        CoInitRAII coInit;
        // Register with MMCSS to guarantee CPU scheduling even when a game is consuming 100% CPU
        HANDLE hMmcss = RegisterMMCSS(L"Pro Audio", "RenderThread");

        AudioFormat format;
        if (!GetDefaultDeviceFormat(format)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: Could not get default device format.");
            return;
        }
        g_serverRenderFormat = format;
        g_hasRenderFormat = true;

        IMMDevice* pDevice = nullptr;
        HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr)) return;

        IAudioClient* pAudioClient = nullptr;
        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
        pDevice->Release();
        if (FAILED(hr)) return;

        // We will request to mix in 32-bit float format for highest quality.
        WAVEFORMATEX* pMixFormat = nullptr;
        hr = pAudioClient->GetMixFormat(&pMixFormat);
        
        WAVEFORMATEXTENSIBLE extFormat;
        ZeroMemory(&extFormat, sizeof(WAVEFORMATEXTENSIBLE));
        if (pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pMixFormat->cbSize >= 22) {
            memcpy(&extFormat, pMixFormat, sizeof(WAVEFORMATEXTENSIBLE));
        } else {
            memcpy(&extFormat.Format, pMixFormat, sizeof(WAVEFORMATEX));
            // Provide a sensible default channel mask if the device didn't natively provide one
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

        g_hRenderEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_hRenderEvent) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: Failed to create WASAPI Event.");
            pAudioClient->Release();
            CoTaskMemFree(pMixFormat);
            return;
        }

        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 0, 0, &extFormat.Format, nullptr);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: IAudioClient Initialize failed.");
            CloseHandle(g_hRenderEvent);
            g_hRenderEvent = NULL;
            pAudioClient->Release();
            CoTaskMemFree(pMixFormat);
            return;
        }

        hr = pAudioClient->SetEventHandle(g_hRenderEvent);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: SetEventHandle failed.");
            CloseHandle(g_hRenderEvent);
            g_hRenderEvent = NULL;
            pAudioClient->Release();
            CoTaskMemFree(pMixFormat);
            return;
        }

        UINT32 bufferFrameCount;
        pAudioClient->GetBufferSize(&bufferFrameCount);

        IAudioRenderClient* pRenderClient = nullptr;
        hr = pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient);
        if (FAILED(hr)) {
            CloseHandle(g_hRenderEvent);
            g_hRenderEvent = NULL;
            pAudioClient->Release();
            CoTaskMemFree(pMixFormat);
            return;
        }

        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            CloseHandle(g_hRenderEvent);
            g_hRenderEvent = NULL;
            pRenderClient->Release();
            pAudioClient->Release();
            CoTaskMemFree(pMixFormat);
            return;
        }

        if (State::globalDebugMode) UI::LogDebug("[Audio] Audio renderer thread started successfully.");

        std::vector<uint8_t> linearBuffer; // Reusable extraction buffer for the RingBuffer
        std::vector<float> mixBuffer;      // Reusable master mix buffer

        while (g_isRenderRunning) {
            // Wait precisely for the hardware to demand data, eliminating sleep drift!
            DWORD waitResult = WaitForSingleObject(g_hRenderEvent, 2000);
            if (waitResult != WAIT_OBJECT_0) {
                if (!g_isRenderRunning) break;
                continue; // Timeout or error, try again
            }

            UINT32 numPaddingFrames;
            HRESULT hrPad = pAudioClient->GetCurrentPadding(&numPaddingFrames);
            if (FAILED(hrPad)) {
                // Device invalidated (sleep/wake, driver update, device unplugged).
                // Exit the loop cleanly; the thread will release all resources on exit.
                if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: Device invalidated (0x%08X). Exiting.", hrPad);
                break;
            }

            UINT32 numFramesAvailable = bufferFrameCount - numPaddingFrames;
            if (numFramesAvailable == 0) {
                continue;
            }

            BYTE* pData;
            hr = pRenderClient->GetBuffer(numFramesAvailable, &pData);
            if (FAILED(hr)) {
                // GetBuffer failed — device may be in an invalid state after sleep/wake.
                if (State::globalDebugMode) UI::LogDebug("[Audio] RenderThread: GetBuffer failed (0x%08X). Exiting.", hr);
                break;
            }
            {
                // Resize if needed, then initialize to silence (0.0f).
                size_t mixBufferSize = numFramesAvailable * pMixFormat->nChannels;
                if (mixBuffer.size() < mixBufferSize) mixBuffer.resize(mixBufferSize, 0.0f);
                std::fill(mixBuffer.begin(), mixBuffer.begin() + mixBufferSize, 0.0f);
                
                size_t bytesPerFrame = (format.bitDepth / 8) * format.channels;
                size_t bytesToProcess = numFramesAvailable * bytesPerFrame;

                std::vector<std::shared_ptr<ClientAudioQueue>> activeQueues;
                std::vector<float> activeVolumes;

                {
                    std::shared_lock<std::shared_mutex> mapLock(g_queuesMutex);
                    for (auto& pair : g_clientAudioQueues) {
                        bool isEnabled = false;
                        float vol = 1.0f;
                        if (Network::GetClientAudioState(pair.first, isEnabled, vol) && isEnabled) {
                            activeQueues.push_back(pair.second);
                            activeVolumes.push_back(vol);
                        }
                    }
                }

                for (size_t i = 0; i < activeQueues.size(); ++i) {
                    auto& queue = activeQueues[i];
                    float vol = activeVolumes[i];

                    size_t availableBytes = 0;
                    size_t bytesRead = 0;
                    bool isBuffering = false;

                    {
                        // Scope the lock tightly to just the memory extraction to prevent network thread convoying
                        std::lock_guard<std::mutex> queueLock(queue->mtx);
                        
                        availableBytes = queue->buffer.Size();
                        if (queue->isBuffering) {
                            // Still in initial buffering mode - play silence until jitter buffer fills
                            isBuffering = true;
                        } else if (availableBytes == 0) {
                            // Instead of instantly pausing for 50ms due to microsecond clock drift,
                            // we pad with 5ms of silence. We only rebuild the jitter buffer if it's a true network drop.
                            queue->starvationCount++;
                            // 50 consecutive empty polls (~500ms at 10ms/callback) before triggering re-buffer.
                            // The old threshold of 10 was too aggressive — under CPU stress, momentary 100-200ms
                            // packet delays would trigger a catastrophic re-buffer that causes audible pops.
                            if (queue->starvationCount > 50) {
                                queue->isBuffering = true;
                                queue->starvationCount = 0;
                                isBuffering = true;
                                if (State::globalDebugMode) {
                                    // Rate-limit to once per 5 seconds to prevent flooding
                                    // the debug log during sustained network issues.
                                    static uint64_t lastStarvationLog = 0;
                                    uint64_t now = GetTickCount64();
                                    if (now - lastStarvationLog > 5000) {
                                        UI::LogDebug("[Audio] Starvation! (Requested %zu, Have 0). Re-buffering.", bytesToProcess);
                                        lastStarvationLog = now;
                                    }
                                }
                            }
                        } else {
                            queue->starvationCount = 0;
                            // PARTIAL FILL: Play whatever audio IS available, even if less than requested.
                            // This prevents one slow packet from triggering a catastrophic 50ms re-buffering pause
                            // that causes the vicious starvation loop from hardware clock drift.
                            bytesRead = (std::min)(availableBytes, bytesToProcess);
                            
                            // Align to frame boundaries to prevent mid-sample splits (causes static/clicks)
                            bytesRead = bytesRead - (bytesRead % bytesPerFrame);

                            if (bytesRead > 0) {
                                if (linearBuffer.size() < bytesToProcess) {
                                    linearBuffer.resize(bytesToProcess);
                                }
                                // Zero the entire extraction buffer first so the unfilled tail is silence
                                memset(linearBuffer.data(), 0, bytesToProcess);
                                queue->buffer.Pop(linearBuffer.data(), bytesRead);
                            }
                        }
                    } // Mutex instantly released! Network thread is free to push new data.

                    if (isBuffering) {
                        continue; 
                    }

                    if (bytesRead == 0) continue;

                    // Apply a 64-sample linear fade-in ramp to eliminate the silence→audio click.
                    // This smooths the transition when the buffer resumes after starvation.
                    if (queue->wasBuffering && bytesRead > 0) {
                        size_t rampSamples = (std::min)((size_t)64, bytesRead / (format.bitDepth / 8));
                        if (format.bitDepth == 32) {
                            float* p = reinterpret_cast<float*>(linearBuffer.data());
                            for (size_t s = 0; s < rampSamples; ++s) {
                                p[s] *= static_cast<float>(s) / static_cast<float>(rampSamples);
                            }
                        } else if (format.bitDepth == 16) {
                            int16_t* p = reinterpret_cast<int16_t*>(linearBuffer.data());
                            for (size_t s = 0; s < rampSamples; ++s) {
                                p[s] = static_cast<int16_t>(p[s] * (static_cast<float>(s) / static_cast<float>(rampSamples)));
                            }
                        }
                        queue->wasBuffering = false;
                    }

                    // Log partial fills (only when significant data is missing, rate-limited)
                    if (bytesRead < bytesToProcess && State::globalDebugMode) {
                        static uint64_t lastPartialLog = 0;
                        uint64_t now = GetTickCount64();
                        if (now - lastPartialLog > 2000) {
                            UI::LogDebug("[Audio Debug] Partial fill: Got %zu/%zu bytes (%.0f%%). Padding remainder with silence.", 
                                         bytesRead, bytesToProcess, (100.0 * bytesRead / bytesToProcess));
                            lastPartialLog = now;
                        }
                    }

                    // Mix the data we have (including the zero-padded tail) into the master buffer
                    size_t framesToMix = bytesToProcess / bytesPerFrame;
                    size_t samplesToMix = framesToMix * pMixFormat->nChannels;
                    
                    if (format.bitDepth == 16) {
                        const int16_t* pcmData = reinterpret_cast<const int16_t*>(linearBuffer.data());
                        for (size_t i = 0; i < samplesToMix; ++i) {
                            mixBuffer[i] += (static_cast<float>(pcmData[i]) / 32768.0f) * vol;
                        }
                    } else if (format.bitDepth == 32) {
                        const float* pcmData = reinterpret_cast<const float*>(linearBuffer.data());
                        for (size_t i = 0; i < samplesToMix; ++i) {
                            mixBuffer[i] += pcmData[i] * vol;
                        }
                    }
                }

                // Clamp the master mix buffer to prevent clipping and copy to the hardware buffer.
                float* pFloatData = reinterpret_cast<float*>(pData);
                for (size_t i = 0; i < mixBuffer.size(); ++i) {
                    pFloatData[i] = std::clamp(mixBuffer[i], -1.0f, 1.0f);
                }

                pRenderClient->ReleaseBuffer(numFramesAvailable, 0);
            }
        }

        pAudioClient->Stop();
        pRenderClient->Release();
        pAudioClient->Release();
        CoTaskMemFree(pMixFormat);
        CloseHandle(g_hRenderEvent);
        g_hRenderEvent = NULL;
        g_hasRenderFormat = false;
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (State::globalDebugMode) UI::LogDebug("[Audio] Audio renderer thread stopped.");
    }

    void StartAudioRenderer() {
        if (g_isRenderRunning) return;
        g_isRenderRunning = true;
        g_renderThread = std::thread(RenderThreadLoop);
    }

    void StopAudioRenderer() {
        if (!g_isRenderRunning) return;
        g_isRenderRunning = false;
        
        // Instantly wake the render loop so it doesn't hang on teardown
        if (g_hRenderEvent) SetEvent(g_hRenderEvent);

        if (g_renderThread.joinable()) {
            g_renderThread.join();
        }
        // Clear out any stale audio data.
        std::unique_lock<std::shared_mutex> lock(g_queuesMutex);
        g_clientAudioQueues.clear();
    }

    /**
     * @brief The main loop for the server's microphone capture thread.
     * @param targetFormat The format to capture the microphone in (aligned with the server's mix format).
     * @return void
     */
    void ServerMicCaptureThreadLoop(AudioFormat targetFormat) {
        CoInitRAII coInit;
        // Register with MMCSS to guarantee CPU scheduling even when a game is consuming 100% CPU
        HANDLE hMmcss = RegisterMMCSS(L"Pro Audio", "ServerMicCapture");

        IMMDevice* pDevice = nullptr;
        // eCapture for microphones, eConsole to respect the standard Windows Default Recording Device
        HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] MicCaptureThread: GetDefaultAudioEndpoint failed.");
            return;
        }

        IAudioClient* pAudioClient = nullptr;
        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
        pDevice->Release();
        if (FAILED(hr)) return;

        WAVEFORMATEX* pTargetWaveFormat = new WAVEFORMATEX();
        pTargetWaveFormat->wFormatTag = (targetFormat.bitDepth == 32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        pTargetWaveFormat->nChannels = targetFormat.channels;
        pTargetWaveFormat->nSamplesPerSec = targetFormat.sampleRate;
        pTargetWaveFormat->wBitsPerSample = targetFormat.bitDepth;
        pTargetWaveFormat->nBlockAlign = (pTargetWaveFormat->nChannels * pTargetWaveFormat->wBitsPerSample) / 8;
        pTargetWaveFormat->nAvgBytesPerSec = pTargetWaveFormat->nSamplesPerSec * pTargetWaveFormat->nBlockAlign;
        pTargetWaveFormat->cbSize = 0;

        // Physical capture device (no loopback flag). Ask WASAPI to resample to match the network format.
        REFERENCE_TIME hnsRequestedDuration = 1000000; // 100ms buffer
        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, hnsRequestedDuration, 0, pTargetWaveFormat, nullptr);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] MicCaptureThread: IAudioClient Initialize failed.");
            pAudioClient->Release();
            delete pTargetWaveFormat;
            return;
        }

        IAudioCaptureClient* pCaptureClient = nullptr;
        hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
        if (FAILED(hr)) {
            pAudioClient->Release();
            delete pTargetWaveFormat;
            return;
        }

        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            pCaptureClient->Release();
            pAudioClient->Release();
            delete pTargetWaveFormat;
            return;
        }

        DWORD sleepTimeMs = 10;

        if (State::globalDebugMode) UI::LogDebug("[Audio] Server Microphone broadcast started (100ms buffer, 10ms poll).");

        while (g_isServerMicRunning) {
            UINT32 packetLength = 0;
            // Drain the buffer entirely to prevent drift
            while (SUCCEEDED(pCaptureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
                BYTE* pData;
                UINT32 numFramesAvailable;
                DWORD flags;

                hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    UINT32 dataSize = numFramesAvailable * pTargetWaveFormat->nBlockAlign;
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        static thread_local std::vector<BYTE> silenceBuffer;
                        if (silenceBuffer.size() < dataSize) silenceBuffer.resize(dataSize, 0);
                        else std::memset(silenceBuffer.data(), 0, dataSize);
                        
                        Network::BroadcastMessage(Network::MessageType::EVENT_MIC_DATA, silenceBuffer.data(), dataSize);
                    } else {
                        Network::BroadcastMessage(Network::MessageType::EVENT_MIC_DATA, pData, dataSize);
                    }
                    pCaptureClient->ReleaseBuffer(numFramesAvailable);
                } else {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepTimeMs));
        }

        pAudioClient->Stop();
        pCaptureClient->Release();
        pAudioClient->Release();
        delete pTargetWaveFormat;
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
        if (g_serverMicThread.joinable()) {
            g_serverMicThread.join();
        }
    }

    /**
     * @brief Helper to find the correct audio endpoint.
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
     * @brief Plays the incoming microphone stream into the target device.
     */
    void ClientMicRenderThreadLoop(AudioFormat sourceFormat) {
        CoInitRAII coInit;
        // Register with MMCSS to guarantee CPU scheduling even when a game is consuming 100% CPU
        HANDLE hMmcss = RegisterMMCSS(L"Pro Audio", "ClientMicRender");

        IMMDevice* pDevice = nullptr;
        HRESULT hr = GetAudioDevice(&pDevice);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: Failed to find target audio device.");
            return;
        }

        IAudioClient* pAudioClient = nullptr;
        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
        pDevice->Release();
        if (FAILED(hr)) return;

        WAVEFORMATEX* pSourceFormat = new WAVEFORMATEX();
        pSourceFormat->wFormatTag = (sourceFormat.bitDepth == 32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        pSourceFormat->nChannels = sourceFormat.channels;
        pSourceFormat->nSamplesPerSec = sourceFormat.sampleRate;
        pSourceFormat->wBitsPerSample = sourceFormat.bitDepth;
        pSourceFormat->nBlockAlign = (sourceFormat.channels * sourceFormat.bitDepth) / 8;
        pSourceFormat->nAvgBytesPerSec = pSourceFormat->nSamplesPerSec * pSourceFormat->nBlockAlign;
        pSourceFormat->cbSize = 0;

        REFERENCE_TIME hnsRequestedDuration = 1000000; // 100ms buffer
        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, hnsRequestedDuration, 0, pSourceFormat, nullptr);
        if (FAILED(hr)) {
            if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: Initialize failed. AUTOCONVERTPCM unsupported?");
            pAudioClient->Release();
            delete pSourceFormat;
            return;
        }

        UINT32 bufferFrameCount;
        pAudioClient->GetBufferSize(&bufferFrameCount);

        IAudioRenderClient* pRenderClient = nullptr;
        hr = pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient);
        if (FAILED(hr)) {
            pAudioClient->Release();
            delete pSourceFormat;
            return;
        }

        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            pRenderClient->Release();
            pAudioClient->Release();
            delete pSourceFormat;
            return;
        }

        DWORD sleepTimeMs = 10;

        if (State::globalDebugMode) UI::LogDebug("[Audio] Client Microphone Receiver started (100ms buffer, 10ms poll).");

        while (g_isClientMicRunning) {
            UINT32 numPaddingFrames;
            HRESULT hrPad = pAudioClient->GetCurrentPadding(&numPaddingFrames);
            if (FAILED(hrPad)) {
                // Device invalidated (sleep/wake, driver update, device unplugged).
                if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: Device invalidated (0x%08X). Exiting.", hrPad);
                break;
            }
            UINT32 numFramesAvailable = bufferFrameCount - numPaddingFrames;

            if (numFramesAvailable > 0) {
                BYTE* pData;
                hr = pRenderClient->GetBuffer(numFramesAvailable, &pData);
                if (FAILED(hr)) {
                    if (State::globalDebugMode) UI::LogDebug("[Audio] ClientMicRender: GetBuffer failed (0x%08X). Exiting.", hr);
                    break;
                }
                {
                    size_t bytesToProcess = numFramesAvailable * pSourceFormat->nBlockAlign;
                    bool writeSilence = false;
                    bool justStartedBuffering = false;

                    {
                        std::lock_guard<std::mutex> lock(g_clientMicQueue.mtx);
                        size_t availableBytes = g_clientMicQueue.buffer.Size();
                        
                        if (g_clientMicQueue.isBuffering) {
                            writeSilence = true;
                        } else if (availableBytes == 0) {
                            g_clientMicQueue.starvationCount++;
                            // 50 consecutive empty polls (~500ms) before re-buffering (same as server render)
                            if (g_clientMicQueue.starvationCount > 50) {
                                g_clientMicQueue.isBuffering = true;
                                g_clientMicQueue.starvationCount = 0;
                                writeSilence = true;
                                justStartedBuffering = true;
                            } else {
                                writeSilence = true; // Pad with silence temporarily
                            }
                        } else {
                            g_clientMicQueue.starvationCount = 0;
                            // Partial fill logic to prevent catastrophic 50ms rebuffering pauses
                            size_t bytesRead = (std::min)(availableBytes, bytesToProcess);
                            // Align to frame boundaries
                            size_t frameSize = pSourceFormat->nBlockAlign;
                            bytesRead = bytesRead - (bytesRead % frameSize);
                            
                            if (bytesRead > 0) {
                                g_clientMicQueue.buffer.Pop(pData, bytesRead);
                                if (bytesRead < bytesToProcess) {
                                    // Zero out the remaining buffer
                                    std::memset(pData + bytesRead, 0, bytesToProcess - bytesRead);
                                }
                            } else {
                                writeSilence = true;
                            }
                        }
                    }

                    if (justStartedBuffering && State::globalDebugMode) {
                        static uint64_t lastMicStarvationLog = 0;
                        uint64_t now = GetTickCount64();
                        if (now - lastMicStarvationLog > 5000) {
                            UI::LogDebug("[Audio] Mic starvation! (50 consecutive drops). Re-buffering.");
                            lastMicStarvationLog = now;
                        }
                    }

                    // Apply fade-in ramp when exiting buffering mode to eliminate clicks
                    if (g_clientMicQueue.wasBuffering && !writeSilence) {
                        size_t frameSize = pSourceFormat->nBlockAlign;
                        size_t totalSamples = bytesToProcess / (pSourceFormat->wBitsPerSample / 8);
                        size_t rampSamples = (std::min)((size_t)64, totalSamples);
                        if (pSourceFormat->wBitsPerSample == 32) {
                            float* p = reinterpret_cast<float*>(pData);
                            for (size_t s = 0; s < rampSamples; ++s) {
                                p[s] *= static_cast<float>(s) / static_cast<float>(rampSamples);
                            }
                        } else if (pSourceFormat->wBitsPerSample == 16) {
                            int16_t* p = reinterpret_cast<int16_t*>(pData);
                            for (size_t s = 0; s < rampSamples; ++s) {
                                p[s] = static_cast<int16_t>(p[s] * (static_cast<float>(s) / static_cast<float>(rampSamples)));
                            }
                        }
                        g_clientMicQueue.wasBuffering = false;
                    }

                    if (writeSilence) {
                        memset(pData, 0, bytesToProcess);
                    }

                    pRenderClient->ReleaseBuffer(numFramesAvailable, writeSilence ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepTimeMs));
        }

        pAudioClient->Stop();
        pRenderClient->Release();
        pAudioClient->Release();
        delete pSourceFormat;
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
        g_isClientMicRunning = true;
        
        {
            std::lock_guard<std::mutex> lock(g_clientMicQueue.mtx);
            g_clientMicQueue.buffer.Clear();
            g_clientMicQueue.isBuffering = true;
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
        if (g_clientMicThread.joinable()) {
            g_clientMicThread.join();
        }
        std::lock_guard<std::mutex> lock(g_clientMicQueue.mtx);
        g_clientMicQueue.buffer.Clear();
    }

    /**
     * @brief Pushes incoming server microphone PCM data into the playback queue.
     * @param pcmData Pointer to the raw audio buffer.
     * @param dataSize Size of the audio buffer in bytes.
     * @return void
     */
    void HandleMicData(const void* pcmData, size_t dataSize) {
        if (!g_isClientMicRunning) return;
        
        std::lock_guard<std::mutex> lock(g_clientMicQueue.mtx);
        const uint8_t* pBytes = static_cast<const uint8_t*>(pcmData);
        g_clientMicQueue.buffer.Push(pBytes, dataSize);

        AudioFormat format;
        if (Network::GetServerAudioFormat(format)) {
            size_t bytesPerSec = format.sampleRate * format.channels * (format.bitDepth / 8);
            
            float jitterTargetMs = static_cast<float>(State::audioJitterBufferMs);
            size_t minBufferThreshold = static_cast<size_t>(bytesPerSec * (jitterTargetMs / 1000.0f));
            if (g_clientMicQueue.isBuffering && g_clientMicQueue.buffer.Size() >= minBufferThreshold) {
                g_clientMicQueue.isBuffering = false;
                g_clientMicQueue.wasBuffering = true;  // Signal fade-in ramp on the next render callback
            }

            // Target latency maximum: Provide a massive burst ceiling to gracefully absorb network spikes
            // or CPU contention without forcefully dropping audio packets.
            const float LATENCY_TARGET_MS = (static_cast<float>(jitterTargetMs) * 3.0f) + 150.0f;
            size_t maxBufferSize = static_cast<size_t>(bytesPerSec * (LATENCY_TARGET_MS / 1000.0f));

            if (g_clientMicQueue.buffer.Size() > maxBufferSize) {
                size_t excess = g_clientMicQueue.buffer.Size() - maxBufferSize;
                size_t frameSize = format.channels * (format.bitDepth / 8);
                excess = excess - (excess % frameSize);
                
                if (excess > 0) {
                    g_clientMicQueue.buffer.Drop(excess);
                }
            }
        }
    }

    /**
     * @brief Pushes incoming client PCM data into their specific mix queue on the server.
     * @param clientIdentifier The socket handle representing the client.
     * @param pcmData Pointer to the raw audio buffer.
     * @param dataSize Size of the audio buffer in bytes.
     * @return void
     */
    void HandleAudioData(uintptr_t clientIdentifier, const void* pcmData, size_t dataSize) {
        if (!g_isRenderRunning) return;

        // Extremely lightweight check to prevent allocating std::vector/std::string in this high-frequency loop
        bool isEnabled = false;
        float vol = 0.0f;
        if (!Network::GetClientAudioState(clientIdentifier, isEnabled, vol) || !isEnabled) return;

        // Find or create a queue for this client.
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
            if (g_clientAudioQueues.find(clientIdentifier) == g_clientAudioQueues.end()) {
                g_clientAudioQueues[clientIdentifier] = std::make_shared<ClientAudioQueue>();
            }
            pQueue = g_clientAudioQueues[clientIdentifier];
        }
        
        // Lock the individual queue and push the new data.
        std::lock_guard<std::mutex> queueLock(pQueue->mtx);
        const uint8_t* pBytes = static_cast<const uint8_t*>(pcmData);
        pQueue->buffer.Push(pBytes, dataSize);

        // Bounded buffer logic.
        if (g_hasRenderFormat) {
            size_t bytesPerSec = g_serverRenderFormat.sampleRate * g_serverRenderFormat.channels * (g_serverRenderFormat.bitDepth / 8);
            
            float jitterTargetMs = static_cast<float>(State::audioJitterBufferMs);
            
            // Turn off buffering mode once we have a healthy margin
            size_t minBufferThreshold = static_cast<size_t>(bytesPerSec * (jitterTargetMs / 1000.0f));
            if (pQueue->isBuffering && pQueue->buffer.Size() >= minBufferThreshold) {
                pQueue->isBuffering = false;
                pQueue->wasBuffering = true;  // Signal fade-in ramp on the next render callback
            }

            // Target latency maximum: Provide a massive burst ceiling to gracefully absorb network spikes
            // or CPU contention without forcefully dropping audio packets.
            const float LATENCY_TARGET_MS = (static_cast<float>(jitterTargetMs) * 3.0f) + 150.0f; 
            
            size_t maxBufferSize = static_cast<size_t>(bytesPerSec * (LATENCY_TARGET_MS / 1000.0f));

            if (pQueue->buffer.Size() > maxBufferSize) {
                size_t excess = pQueue->buffer.Size() - maxBufferSize;
                
                // Align to frame boundaries to prevent static/clipping from split samples
                size_t frameSize = g_serverRenderFormat.channels * (g_serverRenderFormat.bitDepth / 8);
                excess = excess - (excess % frameSize);
                
                if (excess > 0) {
                    if (State::globalDebugMode) { 
                        static uint64_t lastOverrunLog = 0;
                        uint64_t now = GetTickCount64();
                        if (now - lastOverrunLog > 500) {
                            UI::LogDebug("[Audio Debug] OVERRUN! Dropping oldest bytes to maintain %.0fms latency target. (Rate-limited log)", LATENCY_TARGET_MS);
                            lastOverrunLog = now;
                        }
                    }
                    pQueue->buffer.Drop(excess);
                }
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
}