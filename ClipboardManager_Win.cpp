// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// ClipboardManager Windows Implementation
// =============================================================================

/**
 * @file ClipboardManager_Win.cpp
 * @brief Implementation of the ClipboardManager using Win32 API sequence polling.
 */

#include "ClipboardManager.h"
#include "StateManager.h"
#include "NetworkServer.h"
#include "NetworkClient.h"
#include "UIManager.h"
#include <windows.h>
#include <thread>
#include <atomic>
#include <iostream>
#include <vector>

namespace ClipboardManager {
    std::thread g_clipboardThread;
    std::atomic<bool> g_isClipboardRunning(false);
    DWORD g_lastSequenceNumber = 0;

    /**
     * @brief Main loop for the clipboard monitoring thread. Polls the sequence number to avoid hook instability.
     * @return void
     */
    void ClipboardThreadLoop() {
        g_lastSequenceNumber = GetClipboardSequenceNumber();

        while (g_isClipboardRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (!State::enableClipboardSync) continue;

            DWORD currentSequence = GetClipboardSequenceNumber();
            if (currentSequence != g_lastSequenceNumber) {
                g_lastSequenceNumber = currentSequence;

                if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) continue;
                if (!OpenClipboard(NULL)) continue;

                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData == NULL) {
                    CloseClipboard();
                    continue;
                }

                SIZE_T dataSize = GlobalSize(hData);
                wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
                if (pszText == NULL) {
                    CloseClipboard();
                    continue;
                }

                // Enforce extraction limits to prevent std::bad_alloc OOM crashes and respect network bounds.
                size_t availableChars = dataSize / sizeof(wchar_t);
                size_t maxSafeChars = 500000; // ~1MB max characters
                size_t textLen = 0;
                while (textLen < availableChars && textLen < maxSafeChars && pszText[textLen] != L'\0') {
                    textLen++;
                }

                std::wstring text(pszText, textLen);
                GlobalUnlock(hData);
                CloseClipboard();

                if (!text.empty()) {
                    size_t payloadSize = (text.length() + 1) * sizeof(wchar_t);
                    if (State::currentRole == State::AppRole::SERVER) {
                        Network::BroadcastClipboardMessage(text.c_str(), payloadSize);
                    } else {
                        Network::SendToServer(Network::MessageType::EVENT_CLIPBOARD, text.c_str(), payloadSize);
                    }
                }
            }
        }
    }

    /**
     * @brief Sets the local clipboard with text received from a remote machine.
     * @param text The wide string to place on the clipboard.
     * @return void
     */
    void SetRemoteClipboard(const std::wstring& text) {
        if (!State::enableClipboardSync) return;

        if (!OpenClipboard(NULL)) return;

        EmptyClipboard();
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (text.length() + 1) * sizeof(wchar_t));
        if (!hg) {
            CloseClipboard();
            return;
        }

        wchar_t* pDst = (wchar_t*)GlobalLock(hg);
        if (!pDst) {
            CloseClipboard();
            return;
        }
        memcpy(pDst, text.c_str(), (text.length() + 1) * sizeof(wchar_t));
        GlobalUnlock(hg);

        SetClipboardData(CF_UNICODETEXT, hg);
        CloseClipboard();

        // Update the sequence number tracker to prevent creating an infinite echo loop with the remote machine.
        g_lastSequenceNumber = GetClipboardSequenceNumber();
    }

    /**
     * @brief Initializes the ClipboardManager.
     * @return true if initialization succeeds.
     */
    bool Initialize() {
        if (State::globalDebugMode) UI::LogDebug("[Clipboard] Manager Initialized.");
        return true;
    }

    /**
     * @brief Shuts down the ClipboardManager and its background thread.
     * @return void
     */
    void Shutdown() {
        Stop();
    }

    /**
     * @brief Starts the clipboard monitoring thread.
     * @return void
     */
    void Start() {
        if (g_isClipboardRunning) return;
        g_isClipboardRunning = true;
        g_clipboardThread = std::thread(ClipboardThreadLoop);
    }

    /**
     * @brief Stops the clipboard monitoring thread.
     * @return void
     */
    void Stop() {
        if (!g_isClipboardRunning) return;
        g_isClipboardRunning = false;
        if (g_clipboardThread.joinable()) {
            g_clipboardThread.join();
        }
    }
}
