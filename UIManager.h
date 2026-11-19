// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/// @file UIManager.h
/// Manages the DirectX 11 Window and Dear ImGui rendering context for KAM-Flow.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <algorithm>
#include <cstdint>

/// Namespace managing the user interface, DirectX 11 swap chain, and ImGui context.
namespace UI {

    /// Registered window message name used to ask a running instance to restore its UI.
    constexpr const char* kRestoreWindowMessageName = "KAMFlow_RestoreWindow";

    /// Axis-aligned Win32 window position and dimensions in virtual desktop coordinates.
    struct WindowBounds {
        int x;
        int y;
        int width;
        int height;
    };

    /// Decision returned by tray icon recovery checks after wake or Explorer restart.
    enum class TrayRecoveryAction : uint8_t {
        None = 0,
        IconValid,
        RestoreWindow
    };

    /// Returns true if the window's title bar has enough visible area inside the given monitor work area.
    inline bool IsWindowTitleBarVisibleInWorkArea(WindowBounds win, int workLeft, int workTop, int workRight, int workBottom) {
        if (win.width < 100 || win.height < 100 || workRight <= workLeft || workBottom <= workTop) {
            return false;
        }
        const int overlapLeft = (std::max)(win.x, workLeft);
        const int overlapRight = (std::min)(win.x + win.width, workRight);
        const int minHorizVisible = (std::min)(100, win.width);
        if ((overlapRight - overlapLeft) < minHorizVisible) {
            return false;
        }
        return win.y >= workTop && win.y <= workBottom - 32;
    }

    /// Clamps desired window bounds so the entire window fits inside the given monitor work area.
    inline WindowBounds ClampWindowBoundsToWorkArea(WindowBounds desired, int workLeft, int workTop, int workRight, int workBottom,
                                                    int minWidth = 300, int minHeight = 300) {
        if (desired.width < minWidth) desired.width = 450;
        if (desired.height < minHeight) desired.height = 480;

        const int workW = workRight - workLeft;
        const int workH = workBottom - workTop;
        if (workW <= 0 || workH <= 0) {
            return desired;
        }

        desired.width = (std::min)(desired.width, workW);
        desired.height = (std::min)(desired.height, workH);
        desired.x = (std::clamp)(desired.x, workLeft, workRight - desired.width);
        desired.y = (std::clamp)(desired.y, workTop, workBottom - desired.height);
        return desired;
    }

    /// Resolves whether a hidden-in-tray window kept its tray icon or must be restored to visible desktop mode.
    inline TrayRecoveryAction ResolveTrayRecoveryAction(bool isMinimizedToTray, bool modifySucceeded, bool addSucceeded) {
        if (!isMinimizedToTray) {
            return TrayRecoveryAction::None;
        }
        return (modifySucceeded || addSucceeded) ? TrayRecoveryAction::IconValid : TrayRecoveryAction::RestoreWindow;
    }
    
    /// Blazing fast, thread-safe, atomic logger for time-critical threads.
    void LogDebug(const char* fmt, ...);

    /// Initializes the Win32 window, DirectX 11 device, and Dear ImGui context.
    bool Initialize();

    /// Executes the rendering pipeline for a single frame.
    void RenderFrame();

    /// Cleans up D3D11 resources, destroys the ImGui context, and closes the window.
    void Shutdown();

    /// Checks if the UI window is still active and hasn't received a close request.
    bool IsRunning();
}