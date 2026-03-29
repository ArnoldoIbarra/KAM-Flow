// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Clipboard Manager Interface
// =============================================================================

/// @file ClipboardManager.h
/// Manages clipboard synchronization between Server and Client.

#pragma once
#include <string>

/// Namespace for handling clipboard change detection and data transfer.
namespace ClipboardManager {
    /// Initializes the ClipboardManager.
    bool Initialize();
    /// Shuts down the ClipboardManager and its background thread.
    void Shutdown();
    /// Starts the clipboard monitoring thread.
    void Start();
    /// Stops the clipboard monitoring thread.
    void Stop();
    /// Sets the local clipboard with text received from a remote machine.
    void SetRemoteClipboard(const std::wstring& text);
}
