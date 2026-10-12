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
#include "StateManager.h"
#include "NetworkServer.h"   
#include "NetworkMessages.h" 
#include "UIManager.h"
#include <iostream>
#include <mutex>
#include <atomic>

namespace State {
    
    // --- GLOBAL STATE VARIABLES ---
    std::atomic<AppRole> currentRole{AppRole::NONE};
    AppRole defaultRole = AppRole::NONE;
    std::atomic<ControlMode> currentMode{ControlMode::LOCAL};
    std::atomic<bool> globalDebugMode{false};
    std::atomic<bool> minimizeToTray{true};
    std::atomic<bool> enableClipboardSync{true};
    std::atomic<bool> enableKeyboardSync{true}; 
    std::atomic<bool> enableFileTransfer{true};
    std::atomic<bool> enableServerAudioMix{true};
    std::atomic<bool> enableServerMicBroadcast{false};
    std::atomic<bool> enableClientAudioStream{true};
    std::atomic<bool> enableClientMicReceive{false};
    std::atomic<int> audioJitterBufferMs{80};
    std::atomic<int> edgeDeadzonePercent{5};
    std::atomic<float> mouseSensitivity{1.0f};
    std::atomic<char> emergencyHotkey{'M'};
    std::atomic<bool> enableGameMode{false};
    std::atomic<char> gameModeHotkey{'G'};
    std::atomic<bool> enableClientAutoReconnect{true};

    std::string clientConnectionStatus = "Idle";
    std::mutex clientConnectionStatusMutex;

    /**
     * @brief Transitions the application between LOCAL and REMOTE control modes and notifies the controlling client.
     * @param newMode The target ControlMode to switch to.
     * @return void
     */
    void SetMode(ControlMode newMode) {
        ControlMode oldMode = currentMode.exchange(newMode);
        if (oldMode == newMode) return;
        
        Network::StatePayload payload = { static_cast<uint8_t>(newMode == ControlMode::REMOTE ? 1 : 0) };
        
        // Notify the controlling client of the mode change instead of broadcasting to all clients.
        SOCKET ctrl = Network::g_controllingClientSocket.load(std::memory_order_relaxed);
        if (ctrl != INVALID_SOCKET) {
            Network::SendToClient(ctrl, Network::MessageType::EVENT_STATE, &payload, sizeof(payload));
        }

        if (newMode == ControlMode::REMOTE) {
            if (globalDebugMode) UI::LogDebug("[KAM-Flow State] Entered REMOTE mode.");
        } else {
            if (globalDebugMode) UI::LogDebug("[KAM-Flow State] Returned to LOCAL mode.");
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

}