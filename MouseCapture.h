// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Mouse Capture Hook Interface
// =============================================================================

/// @file MouseCapture.h
/// Manages global mouse interception and edge detection.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <cstdint>

/// Namespace handling global input interception and processing.
namespace Input {
    
    /// Installs the low-level global mouse hook (WH_MOUSE_LL).
    bool StartMouseCapture();

    /// Removes the low-level mouse hook safely and cleans up resources.
    void StopMouseCapture();

    /// Teleports the Server cursor to the physical edge based on 4-way coordinate tracking.
    void HandleReturnControl(uint8_t entryEdge, float normalizedX, float normalizedY);
}