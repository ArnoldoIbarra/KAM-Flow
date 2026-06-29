// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Keyboard Capture Hook Implementation
// =============================================================================

/**
 * @file KeyboardCapture.cpp
 * @brief Implementation of global keyboard interception and transmission.
 * Features a gated transmission logic that only fires if clients are authenticated.
 * Uses a dedicated queue + network thread to decouple the OS hook callback
 * from blocking network calls, preventing Windows from silently removing
 * the hook when send() blocks for longer than the LL hook timeout (~200-300ms).
 */

#include "KeyboardCapture.h"
#include "StateManager.h"
#include "NetworkMessages.h" 
#include "NetworkServer.h"   
#include "UIManager.h"
#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>

namespace Input {
    
    /// Handle to the low-level keyboard hook.
    HHOOK globalKeyboardHook = NULL;
    /// Internal tracking of modifier keys for the override shortcut.
    bool isCtrlDown = false;
    bool isAltDown = false;

    /// Background queue to decouple the OS keyboard hook from blocking network
    /// calls. Without this, BroadcastMessage() was called directly from the
    /// WH_KEYBOARD_LL callback. If the per-client send mutex was contended or
    /// send() blocked on a congested socket for >200-300ms, Windows would
    /// silently remove the keyboard hook — permanently killing keyboard
    /// forwarding until the application restarted.
    std::queue<Network::KeyboardPayload> g_keyboardQueue;
    std::mutex g_keyboardQueueMutex;
    std::condition_variable g_keyboardQueueCv;
    std::atomic<bool> g_isKeyboardNetworkThreadRunning{false};
    std::thread g_keyboardNetworkThread;

    /**
     * @brief Background thread loop that consumes keyboard payloads and broadcasts them over the network.
     * This fully decouples the high-frequency OS keyboard hook from blocking network calls,
     * preventing Windows from silently removing the hook due to LL callback timeout violations.
     * @return void
     */
    void KeyboardNetworkThreadLoop() {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (g_isKeyboardNetworkThreadRunning) {
            Network::KeyboardPayload p;
            bool hasPayload = false;
            {
                std::unique_lock<std::mutex> lock(g_keyboardQueueMutex);
                g_keyboardQueueCv.wait(lock, [] {
                    return !g_keyboardQueue.empty() || !g_isKeyboardNetworkThreadRunning;
                });

                if (!g_isKeyboardNetworkThreadRunning && g_keyboardQueue.empty()) {
                    break;
                }

                if (!g_keyboardQueue.empty()) {
                    p = g_keyboardQueue.front();
                    g_keyboardQueue.pop();
                    hasPayload = true;
                }
            }

            // Perform the blocking network call outside of the OS hook chain.
            // Keyboard events are never coalesced — each keypress/release must
            // be delivered individually to preserve correct modifier state.
            if (hasPayload) {
                Network::BroadcastMessage(Network::MessageType::EVENT_KEYBOARD, &p, sizeof(p));
            }
        }
    }

    /**
     * @brief Processes keyboard events intercepted by the hook.
     * Includes the Ctrl+Alt+M emergency override.
     * @param nCode A code the hook procedure uses to determine how to process the message.
     * @param wParam The identifier of the keyboard message.
     * @param lParam A pointer to a KBDLLHOOKSTRUCT structure.
     * @return LRESULT Returns 1 to block the event, or the result of CallNextHookEx.
     */
    LRESULT CALLBACK KeyboardHookCallback(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0) {
            // Absolute Fast-Path: Instantly return to OS scheduler if Game Mode is active
            if (State::enableGameMode) return CallNextHookEx(globalKeyboardHook, nCode, wParam, lParam);

            if (!Network::g_hasClients.load(std::memory_order_relaxed)) {
                return CallNextHookEx(globalKeyboardHook, nCode, wParam, lParam);
            }

            KBDLLHOOKSTRUCT* kbd = (KBDLLHOOKSTRUCT*)lParam;

            if (kbd->flags & LLKHF_INJECTED) {
                return CallNextHookEx(globalKeyboardHook, nCode, wParam, lParam);
            }

            bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

            if (kbd->vkCode == VK_CONTROL || kbd->vkCode == VK_LCONTROL || kbd->vkCode == VK_RCONTROL) {
                if (isKeyDown) isCtrlDown = true; if (isKeyUp) isCtrlDown = false;
            }
            if (kbd->vkCode == VK_MENU || kbd->vkCode == VK_LMENU || kbd->vkCode == VK_RMENU) {
                if (isKeyDown) isAltDown = true; if (isKeyUp) isAltDown = false;
            }

            if (isKeyDown && kbd->vkCode == State::emergencyHotkey && isCtrlDown && isAltDown) {
                if (State::globalDebugMode) UI::LogDebug("[KAM-Flow] Emergency Override Triggered. Returning control to Server.");
                State::SetMode(State::ControlMode::LOCAL);
                return 1;
            }

            if (State::IsRemote() && State::enableKeyboardSync) {
                uint32_t injectionFlags = 0;
                
                if (kbd->flags & LLKHF_EXTENDED) injectionFlags |= KEYEVENTF_EXTENDEDKEY;
                if (isKeyUp) injectionFlags |= KEYEVENTF_KEYUP;

                Network::KeyboardPayload p = { (uint16_t)kbd->vkCode, (uint16_t)kbd->scanCode, injectionFlags };
                
                // Enqueue the payload for the background network thread instead of
                // calling BroadcastMessage() directly. This prevents Windows from
                // silently removing the keyboard hook when network I/O blocks.
                {
                    std::lock_guard<std::mutex> lock(g_keyboardQueueMutex);
                    g_keyboardQueue.push(p);
                    g_keyboardQueueCv.notify_one();
                }
                
                return 1; 
            }
        }
        return CallNextHookEx(globalKeyboardHook, nCode, wParam, lParam);
    }

    /**
     * @brief Installs the low-level keyboard hook and starts the background network thread.
     * @return bool True if the hook was successfully installed, false otherwise.
     */
    bool StartKeyboardCapture() {
        if (globalKeyboardHook != NULL) {
            return true; // Prevent handle leaking from double-hooks
        }
        
        g_isKeyboardNetworkThreadRunning = true;
        g_keyboardNetworkThread = std::thread(KeyboardNetworkThreadLoop);
        
        globalKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookCallback, NULL, 0);
        return (globalKeyboardHook != NULL);
    }

    /**
     * @brief Uninstalls the keyboard hook and joins the network thread.
     * @return void
     */
    void StopKeyboardCapture() {
        if (globalKeyboardHook) {
            UnhookWindowsHookEx(globalKeyboardHook);
            globalKeyboardHook = NULL;
        }

        g_isKeyboardNetworkThreadRunning = false;
        g_keyboardQueueCv.notify_all();
        if (g_keyboardNetworkThread.joinable()) {
            g_keyboardNetworkThread.join();
        }
    }
}