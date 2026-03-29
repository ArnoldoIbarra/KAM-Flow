// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Keyboard Capture Hook Implementation
// =============================================================================

/**
 * @file KeyboardCapture.cpp
 * @brief Implementation of global keyboard interception and transmission.
 * Features a gated transmission logic that only fires if clients are authenticated.
 */

#include "KeyboardCapture.h"
#include "StateManager.h"
#include "NetworkMessages.h" 
#include "NetworkServer.h"   
#include <iostream>

namespace Input {
    
    /// Handle to the low-level keyboard hook.
    HHOOK globalKeyboardHook = NULL;
    /// Internal tracking of modifier keys for the override shortcut.
    bool isCtrlDown = false;
    bool isAltDown = false;

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
            if (!Network::HasAuthenticatedClients()) {
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
                if (State::globalDebugMode) std::cout << "[KAM-Flow] Emergency Override Triggered. Returning control to Server.\n";
                State::SetMode(State::ControlMode::LOCAL);
                return 1;
            }

            if (State::IsRemote() && State::enableKeyboardSync) {
                uint32_t injectionFlags = 0;
                
                if (kbd->flags & LLKHF_EXTENDED) injectionFlags |= KEYEVENTF_EXTENDEDKEY;
                if (isKeyUp) injectionFlags |= KEYEVENTF_KEYUP;

                Network::KeyboardPayload p = { (uint16_t)kbd->vkCode, (uint16_t)kbd->scanCode, injectionFlags };
                Network::BroadcastMessage(Network::MessageType::EVENT_KEYBOARD, &p, sizeof(p));
                
                return 1; 
            }
        }
        return CallNextHookEx(globalKeyboardHook, nCode, wParam, lParam);
    }

    /**
     * @brief Installs the low-level keyboard hook.
     * @return bool True if the hook was successfully installed, false otherwise.
     */
    bool StartKeyboardCapture() {
        globalKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookCallback, NULL, 0);
        return (globalKeyboardHook != NULL);
    }

    /**
     * @brief Uninstalls the keyboard hook.
     * @return void
     */
    void StopKeyboardCapture() {
        if (globalKeyboardHook) {
            UnhookWindowsHookEx(globalKeyboardHook);
            globalKeyboardHook = NULL;
        }
    }
}