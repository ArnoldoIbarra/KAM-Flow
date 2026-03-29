// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Application State Machine Implementation
// =============================================================================

/**
 * @file StateManager.cpp
 * @brief Implementation of the application state machine.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <cstdio> 
#include "StateManager.h"
#include "NetworkServer.h"   
#include "NetworkMessages.h" 
#include <iostream>
#include <mutex>

namespace State {
    
    // --- GLOBAL STATE VARIABLES ---
    AppRole currentRole = AppRole::NONE;
    AppRole defaultRole = AppRole::NONE;
    ControlMode currentMode = ControlMode::LOCAL;
    bool globalDebugMode = false;
    bool minimizeToTray = true;
    bool enableClipboardSync = true;
    bool enableKeyboardSync = true; 
    bool enableFileTransfer = true;
    bool enableServerAudioMix = true;
    bool enableServerMicBroadcast = false;
    bool enableClientAudioStream = true;
    bool enableClientMicReceive = false;
    int audioJitterBufferMs = 50;
    int edgeDeadzonePercent = 5;
    float mouseSensitivity = 1.0f;
    char emergencyHotkey = 'M';
    bool enableClientAutoReconnect = true;

    std::string clientConnectionStatus = "Idle";
    std::mutex clientConnectionStatusMutex;

    /**
     * @brief Transitions the application between LOCAL and REMOTE control modes and broadcasts the change.
     * @param newMode The target ControlMode to switch to.
     * @return void
     */
    void SetMode(ControlMode newMode) {
        if (currentMode == newMode) return;

        currentMode = newMode;
        
        Network::StatePayload payload = { static_cast<uint8_t>(currentMode == ControlMode::REMOTE ? 1 : 0) };
        Network::BroadcastMessage(Network::MessageType::EVENT_STATE, &payload, sizeof(payload));

        if (currentMode == ControlMode::REMOTE) {
            if (globalDebugMode) std::cout << "[KAM-Flow State] Entered REMOTE mode.\n";
        } else {
            if (globalDebugMode) std::cout << "[KAM-Flow State] Returned to LOCAL mode.\n";
        }
    }

    /**
     * @brief Retrieves the current input control mode.
     * @return ControlMode The active control mode (LOCAL or REMOTE).
     */
    ControlMode GetMode() { 
        return currentMode; 
    }

    /**
     * @brief Helper function to rapidly check if the system is actively tethered to a remote client.
     * @return true if currentMode is REMOTE, false otherwise.
     */
    bool IsRemote() { 
        return currentMode == ControlMode::REMOTE; 
    }

    /**
     * @brief Thread-safe setter for the client connection status string.
     * @param status The new status string to display in the UI.
     * @return void
     */
    void SetClientStatus(const std::string& status) {
        std::lock_guard<std::mutex> lock(clientConnectionStatusMutex);
        clientConnectionStatus = status;
    }

    /**
     * @brief Thread-safe getter for the client connection status string.
     * @return A copy of the current status string.
     */
    std::string GetClientStatus() {
        std::lock_guard<std::mutex> lock(clientConnectionStatusMutex);
        return clientConnectionStatus;
    }

    /**
     * @brief Dynamically allocates or frees the Win32 Console window to bypass Win11 Terminal minimizing issues.
     * @return void
     */
    void UpdateConsoleVisibility() {
        if (globalDebugMode) {
            AllocConsole();
            FILE* fDummy;
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
            std::cout.clear();
            std::clog.clear();
            std::cerr.clear();

            HWND consoleWnd = ::GetConsoleWindow();
            if (consoleWnd) {
                ::SetForegroundWindow(consoleWnd);
            }
            
            // Disable QuickEdit Mode so clicking the console doesn't pause the application thread
            HANDLE hInput = ::GetStdHandle(STD_INPUT_HANDLE);
            DWORD consoleMode;
            if (::GetConsoleMode(hInput, &consoleMode)) {
                ::SetConsoleMode(hInput, consoleMode & ~ENABLE_QUICK_EDIT_MODE);
            }
        } else {
            HWND consoleWnd = ::GetConsoleWindow();
            FreeConsole();
            if (consoleWnd) {
                ::PostMessage(consoleWnd, WM_CLOSE, 0, 0);
            }
        }
    }
}