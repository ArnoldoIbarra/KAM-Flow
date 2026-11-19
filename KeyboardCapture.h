// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/// @file KeyboardCapture.h
/// Manages global keyboard interception and hotkey detection.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>

/// Namespace handling global input interception and processing.
namespace Input {
    
    /// Installs the low-level global keyboard hook (WH_KEYBOARD_LL).
    bool StartKeyboardCapture();

    /// Removes the low-level keyboard hook safely.
    void StopKeyboardCapture();
}