// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Config Manager Interface
// =============================================================================

/// @file ConfigManager.h
/// Handles persistent storage of application settings via .ini files.

#pragma once
#include <string>
#include <cstdint>

/// Namespace managing configuration persistence.
namespace Config {
    
    /// Resolves and returns the absolute path to the kamflow.ini file.
    const std::string& GetResolvedPath();

    /// Loads settings from the .ini file into the global State.
    bool LoadConfig();

    /// Saves the current configuration to the .ini file.
    void SaveConfig(const std::string& targetIp, uint16_t port);

    /// Saves a client's spatial matrix grid coordinates.
    void SaveClientLayout(const std::string& clientName, int x, int y);

    /// Loads a client's spatial matrix grid coordinates.
    bool LoadClientLayout(const std::string& clientName, int& outX, int& outY);
}