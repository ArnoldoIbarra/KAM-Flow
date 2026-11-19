// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/**
 * @file ClipboardManager_Win.cpp
 * @brief Implementation of the ClipboardManager using event-driven Win32 WM_CLIPBOARDUPDATE notifications.
 */

#include "ClipboardManager.h"
#include "DebugLog.h"
#include "StateManager.h"
#include "NetworkServer.h"
#include "NetworkClient.h"
#include "UIManager.h"
#include <windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>
#include <vector>

namespace ClipboardManager {
    std::thread g_clipboardThread;
    std::atomic<bool> g_isClipboardRunning(false);
    std::atomic<DWORD> g_clipboardThreadId(0);
    std::mutex g_clipboardMutex;
    DWORD g_lastSequenceNumber = 0;
    std::wstring g_lastRemoteText;

    /**
     * @brief Extracts Unicode text from the local clipboard and dispatches it to the network if changed locally.
     * @return void
     */
    static void HandleLocalClipboardUpdate() {
        if (!State::enableClipboardSync) return;

        std::wstring text;
        {
            std::lock_guard<std::mutex> lock(g_clipboardMutex);
            DWORD currentSequence = GetClipboardSequenceNumber();
            if (currentSequence == g_lastSequenceNumber) return;
            g_lastSequenceNumber = currentSequence;

            if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;

            // Briefly retry OpenClipboard in case the source application still holds the clipboard lock
            bool opened = false;
            for (int attempt = 0; attempt < 3; ++attempt) {
                if (OpenClipboard(NULL)) {
                    opened = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!opened) {
                DWORD err = GetLastError();
                if (State::globalDebugMode) {
                    static DebugLog::RateLimitState openClipRate;
                    uint32_t suppressed = 0;
                    if (openClipRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
                        UI::LogDebug("[Clipboard] WARNING: OpenClipboard failed after 3 attempts on local copy (err=%lu).", err);
                    }
                }
                return;
            }

            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData == NULL) {
                DWORD err = GetLastError();
                CloseClipboard();
                if (State::globalDebugMode) {
                    static DebugLog::RateLimitState getClipRate;
                    uint32_t suppressed = 0;
                    if (getClipRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
                        UI::LogDebug("[Clipboard] WARNING: GetClipboardData(CF_UNICODETEXT) failed (err=%lu).", err);
                    }
                }
                return;
            }

            SIZE_T dataSize = GlobalSize(hData);
            wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
            if (pszText == NULL) {
                DWORD err = GetLastError();
                CloseClipboard();
                if (State::globalDebugMode) {
                    static DebugLog::RateLimitState lockClipRate;
                    uint32_t suppressed = 0;
                    if (lockClipRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
                        UI::LogDebug("[Clipboard] WARNING: GlobalLock failed on local clipboard read (err=%lu).", err);
                    }
                }
                return;
            }

            // Enforce extraction limits to prevent std::bad_alloc OOM crashes and respect network bounds.
            size_t availableChars = dataSize / sizeof(wchar_t);
            size_t maxSafeChars = 500000; // ~1MB max characters
            size_t textLen = 0;
            while (textLen < availableChars && textLen < maxSafeChars && pszText[textLen] != L'\0') {
                textLen++;
            }
            bool wasTruncated = (textLen == maxSafeChars && availableChars > maxSafeChars && pszText[textLen] != L'\0');

            text.assign(pszText, textLen);
            GlobalUnlock(hData);
            CloseClipboard();

            if (wasTruncated && State::globalDebugMode) {
                UI::LogDebug("[Clipboard] WARNING: Local clipboard text exceeded 1 MB limit; truncated to %zu chars.", textLen);
            }

            // Suppress echo if the clipboard contents match what was most recently injected from a remote peer
            if (text.empty() || text == g_lastRemoteText) {
                return;
            }
            g_lastRemoteText.clear();
        }

        size_t payloadSize = (text.length() + 1) * sizeof(wchar_t);
        if (State::currentRole == State::AppRole::SERVER) {
            Network::BroadcastClipboardMessage(text.c_str(), payloadSize);
        } else {
            Network::SendToServer(Network::MessageType::EVENT_CLIPBOARD, text.c_str(), payloadSize);
        }
    }

    /**
     * @brief Window procedure for the hidden message-only clipboard listener window.
     * @param hwnd Window handle.
     * @param uMsg Message identifier.
     * @param wParam First message parameter.
     * @param lParam Second message parameter.
     * @return LRESULT message processing result.
     */
    static LRESULT CALLBACK ClipboardWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (uMsg == WM_CLIPBOARDUPDATE) {
            HandleLocalClipboardUpdate();
            return 0;
        }
        return DefWindowProcA(hwnd, uMsg, wParam, lParam);
    }

    /**
     * @brief Main loop for the clipboard monitoring thread using an OS event-driven message-only window.
     * Eliminates background polling wakeups when the clipboard is idle.
     * @return void
     */
    void ClipboardThreadLoop() {
        g_clipboardThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_clipboardMutex);
            g_lastSequenceNumber = GetClipboardSequenceNumber();
        }

        const char* className = "KAMFlowClipboardListenerClass";
        HINSTANCE hInstance = GetModuleHandleA(NULL);

        WNDCLASSEXA wc = {0};
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.lpfnWndProc = ClipboardWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = className;
        RegisterClassExA(&wc);

        HWND hwnd = CreateWindowExA(0, className, "KAMFlowClipboardListener",
                                    0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
        if (hwnd != NULL) {
            if (!AddClipboardFormatListener(hwnd)) {
                if (State::globalDebugMode) {
                    UI::LogDebug("[Clipboard] ERROR: AddClipboardFormatListener failed (err=%lu). Clipboard sync inactive.",
                                 GetLastError());
                }
            } else {
                if (State::globalDebugMode) {
                    UI::LogDebug("[Clipboard] Event-driven clipboard listener active.");
                }
                MSG msg;
                while (g_isClipboardRunning.load(std::memory_order_acquire) &&
                       GetMessageA(&msg, NULL, 0, 0) > 0) {
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
                RemoveClipboardFormatListener(hwnd);
            }
            DestroyWindow(hwnd);
        } else {
            if (State::globalDebugMode) {
                UI::LogDebug("[Clipboard] ERROR: CreateWindowExA failed for listener window (err=%lu). Clipboard sync inactive.",
                             GetLastError());
            }
        }
        UnregisterClassA(className, hInstance);
        g_clipboardThreadId.store(0, std::memory_order_release);
    }

    /**
     * @brief Sets the local clipboard with text received from a remote machine.
     * @param text The wide string to place on the clipboard.
     * @return void
     */
    void SetRemoteClipboard(const std::wstring& text) {
        if (!State::enableClipboardSync || text.empty()) return;

        std::lock_guard<std::mutex> lock(g_clipboardMutex);
        g_lastRemoteText = text;

        bool opened = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (OpenClipboard(NULL)) {
                opened = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!opened) {
            DWORD err = GetLastError();
            if (State::globalDebugMode) {
                static DebugLog::RateLimitState setOpenRate;
                uint32_t suppressed = 0;
                if (setOpenRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
                    UI::LogDebug("[Clipboard] WARNING: OpenClipboard failed after 3 attempts on remote paste (err=%lu).", err);
                }
            }
            return;
        }

        EmptyClipboard();
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (text.length() + 1) * sizeof(wchar_t));
        if (!hg) {
            DWORD err = GetLastError();
            CloseClipboard();
            if (State::globalDebugMode) {
                UI::LogDebug("[Clipboard] ERROR: GlobalAlloc failed for remote clipboard (%zu chars, err=%lu).",
                             text.length(), err);
            }
            return;
        }

        wchar_t* pDst = (wchar_t*)GlobalLock(hg);
        if (!pDst) {
            DWORD err = GetLastError();
            GlobalFree(hg);
            CloseClipboard();
            if (State::globalDebugMode) {
                UI::LogDebug("[Clipboard] ERROR: GlobalLock failed on remote clipboard buffer (err=%lu).", err);
            }
            return;
        }
        memcpy(pDst, text.c_str(), (text.length() + 1) * sizeof(wchar_t));
        GlobalUnlock(hg);

        if (!SetClipboardData(CF_UNICODETEXT, hg)) {
            DWORD err = GetLastError();
            GlobalFree(hg);
            CloseClipboard();
            if (State::globalDebugMode) {
                UI::LogDebug("[Clipboard] ERROR: SetClipboardData failed (err=%lu).", err);
            }
            return;
        }
        CloseClipboard();

        // Update the sequence number tracker while holding g_clipboardMutex to prevent echo loops
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
     * @brief Stops the clipboard monitoring thread immediately by posting WM_QUIT.
     * @return void
     */
    void Stop() {
        if (!g_isClipboardRunning.exchange(false)) return;
        DWORD tid = g_clipboardThreadId.load(std::memory_order_acquire);
        if (tid != 0) {
            PostThreadMessageA(tid, WM_QUIT, 0, 0);
        }
        if (g_clipboardThread.joinable()) {
            g_clipboardThread.join();
        }
    }
}
