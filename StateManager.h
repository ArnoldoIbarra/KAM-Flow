// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Application State Machine Interface
// =============================================================================

/// @file StateManager.h
/// Manages the application's current control state and global application role.

#pragma once

#include <cstdint>
#include <string>

/// Namespace managing global application state, modes, and feature toggles.
namespace State {
    
    /// Defines the macro role of the application instance.
    enum class AppRole : uint8_t {
        NONE = 0,   ///< Awaiting user selection in the Pre-Flight UI.
        SERVER = 1, ///< Running as the Main PC (Intercepting input, mixing audio).
        CLIENT = 2  ///< Running as the Laptop (Injecting input, streaming audio).
    };

    /// Defines the current target of input control.
    enum class ControlMode : uint8_t {
        LOCAL = 0,  ///< Input controls the physical Server PC.
        REMOTE = 1  ///< Input is swallowed and sent to the Client PC.
    };

    /// The current macro role of the application.
    extern AppRole currentRole;
    /// The default role loaded from kamflow.ini to facilitate auto-start.
    extern AppRole defaultRole;

    /// Changes the current control mode and notifies connected clients.
    void SetMode(ControlMode newMode);

    /// Retrieves the current control mode.
    ControlMode GetMode();

    /// Helper to check if the system is currently in Remote mode.
    bool IsRemote();

    /// Dynamically shows or hides the allocated Win32 Console window based on globalDebugMode.
    void UpdateConsoleVisibility();

    /// Master toggle for rendering the console window and enabling verbose std::cout logging.
    extern bool globalDebugMode;

    /// Toggle to minimize the application to the System Tray instead of the Taskbar.
    extern bool minimizeToTray;

    /// Master toggle for clipboard text synchronization.
    extern bool enableClipboardSync;

    /// Toggle to enable or disable keyboard keystroke synchronization.
    extern bool enableKeyboardSync;

    /// Toggle to enable or disable file transfers.
    extern bool enableFileTransfer;

    /// Toggle to enable automatic reconnection on unintentional drop (Client side).
    extern bool enableClientAutoReconnect;

    /// Thread-safe setter for the client connection status.
    void SetClientStatus(const std::string& status);
    
    /// Thread-safe getter for the client connection status.
    std::string GetClientStatus();

    // --- AUDIO ROUTING TOGGLES ---
    /// Server: Master toggle to receive and mix incoming client audio.
    extern bool enableServerAudioMix;
    /// Server: Toggle to broadcast local microphone to clients.
    extern bool enableServerMicBroadcast;
    /// Client: Toggle to capture and send laptop audio to the server.
    extern bool enableClientAudioStream;
    /// Client: Toggle to receive server microphone data into the Virtual Audio Cable.
    extern bool enableClientMicReceive;

    /// Target jitter buffer latency in milliseconds for incoming audio.
    extern int audioJitterBufferMs;

    /// Percentage of the screen corners (0-10) to ignore for edge detection transitions.
    extern int edgeDeadzonePercent;

    /// Mouse sensitivity multiplier for Client injection.
    extern float mouseSensitivity;
    /// Character representing the emergency hotkey (default 'M').
    extern char emergencyHotkey;

    /// Toggle to prevent cursor transitions to clients when gaming.
    extern bool enableGameMode;
    /// Character representing the game mode toggle hotkey (default 'G').
    extern char gameModeHotkey;
}