// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// UI Manager Interface
// =============================================================================

/// @file UIManager.h
/// Manages the DirectX 11 Window and Dear ImGui rendering context for KAM-Flow.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

/// Namespace managing the user interface, DirectX 11 swap chain, and ImGui context.
namespace UI {
    
    /// Initializes the Win32 window, DirectX 11 device, and Dear ImGui context.
    bool Initialize();

    /// Executes the rendering pipeline for a single frame.
    void RenderFrame();

    /// Cleans up D3D11 resources, destroys the ImGui context, and closes the window.
    void Shutdown();

    /// Checks if the UI window is still active and hasn't received a close request.
    bool IsRunning();
}