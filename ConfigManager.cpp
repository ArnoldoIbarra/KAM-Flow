// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/**
 * @file ConfigManager.cpp
 * @brief Implementation of Win32-based .ini configuration management.
 */

#include "ConfigManager.h"
#include "StateManager.h"
#include "UIManager.h"
#include <windows.h>
#include <string>
#include <iostream>

namespace Config {
    static std::string resolvedIniPath = "";

    /**
     * @brief Resolves and caches the absolute path to the kamflow.ini file.
     * @return A constant reference to the resolved absolute path string.
     */
    const std::string& GetResolvedPath() {
        if (resolvedIniPath.empty()) {
            char buffer[MAX_PATH];
            GetModuleFileNameA(NULL, buffer, MAX_PATH);
            std::string path(buffer);
            size_t pos = path.find_last_of("\\/");
            if (pos != std::string::npos) {
                resolvedIniPath = path.substr(0, pos + 1) + "kamflow.ini";
            } else {
                resolvedIniPath = ".\\kamflow.ini"; 
            }
        }
        return resolvedIniPath;
    }

    /**
     * @brief Loads the current configuration from the kamflow.ini file into the global State.
     * @return True if the configuration was successfully loaded or a default file was created.
     */
    bool LoadConfig() {
        const std::string& fullPath = GetResolvedPath();

        DWORD attribs = GetFileAttributesA(fullPath.c_str());
        bool fileExists = (attribs != INVALID_FILE_ATTRIBUTES && !(attribs & FILE_ATTRIBUTE_DIRECTORY));

        if (!fileExists) {
            SaveConfig();
            if (State::globalDebugMode) {
                UI::LogDebug("[Config] No config file found; created default %s.", fullPath.c_str());
            }
            return true;
        }

        bool hadClamp = false;
        bool sensParseFailed = false;

        // Load system role configuration.
        int savedRole = GetPrivateProfileIntA("System", "DefaultRole", 0, fullPath.c_str());
        if (savedRole < 0 || savedRole > 2) {
            savedRole = 0;
            hadClamp = true;
        }
        State::defaultRole = static_cast<State::AppRole>(savedRole);

        // Load global debug and security preferences.
        State::globalDebugMode = GetPrivateProfileIntA("System", "GlobalDebugMode", 0, fullPath.c_str()) != 0;
        State::minimizeToTray = GetPrivateProfileIntA("System", "MinimizeToTray", 1, fullPath.c_str()) != 0;

        // Load input synchronization toggles.
        State::enableKeyboardSync = GetPrivateProfileIntA("Control", "EnableKeyboardSync", 1, fullPath.c_str()) != 0;
        State::enableClipboardSync = GetPrivateProfileIntA("Control", "EnableClipboardSync", 1, fullPath.c_str()) != 0;
        State::enableFileTransfer = GetPrivateProfileIntA("Control", "EnableFileTransfer", 1, fullPath.c_str()) != 0;
        State::enableClientAutoReconnect = GetPrivateProfileIntA("Network", "EnableClientAutoReconnect", 1, fullPath.c_str()) != 0;

        int deadzone = GetPrivateProfileIntA("Control", "EdgeDeadzonePercent", 5, fullPath.c_str());
        if (deadzone < 0) { deadzone = 0; hadClamp = true; }
        if (deadzone > 10) { deadzone = 10; hadClamp = true; }
        State::edgeDeadzonePercent = deadzone;

        char floatBuf[32];
        GetPrivateProfileStringA("Control", "MouseSensitivity", "1.0", floatBuf, sizeof(floatBuf), fullPath.c_str());
        float sens = 1.0f;
        try {
            sens = std::stof(floatBuf);
        } catch (...) {
            sens = 1.0f;
            sensParseFailed = true;
        }
        // Clamp to safe values in case the .ini was manually edited outside the UI.
        if (sens < 0.1f) { sens = 0.1f; hadClamp = true; }
        if (sens > 3.0f) { sens = 3.0f; hadClamp = true; }
        State::mouseSensitivity = sens;

        char hotkeyBuf[8];
        GetPrivateProfileStringA("Control", "EmergencyHotkey", "M", hotkeyBuf, sizeof(hotkeyBuf), fullPath.c_str());
        State::emergencyHotkey = (hotkeyBuf[0] >= 'A' && hotkeyBuf[0] <= 'Z') ? hotkeyBuf[0] : 'M';
        
        State::enableGameMode = GetPrivateProfileIntA("Control", "EnableGameMode", 0, fullPath.c_str()) != 0;
        char gmHotkeyBuf[8];
        GetPrivateProfileStringA("Control", "GameModeHotkey", "G", gmHotkeyBuf, sizeof(gmHotkeyBuf), fullPath.c_str());
        State::gameModeHotkey = (gmHotkeyBuf[0] >= 'A' && gmHotkeyBuf[0] <= 'Z') ? gmHotkeyBuf[0] : 'G';

        // Load audio subsystem toggles.
        State::enableServerAudioMix = GetPrivateProfileIntA("Audio", "EnableServerAudioMix", 1, fullPath.c_str()) != 0;
        State::enableServerMicBroadcast = GetPrivateProfileIntA("Audio", "EnableServerMicBroadcast", 0, fullPath.c_str()) != 0;
        State::enableClientAudioStream = GetPrivateProfileIntA("Audio", "EnableClientAudioStream", 1, fullPath.c_str()) != 0;
        State::enableClientMicReceive = GetPrivateProfileIntA("Audio", "EnableClientMicReceive", 0, fullPath.c_str()) != 0;

        int jitterMs = GetPrivateProfileIntA("Audio", "JitterBufferMs", 80, fullPath.c_str());
        if (jitterMs < 20) { jitterMs = 20; hadClamp = true; }
        if (jitterMs > 200) { jitterMs = 200; hadClamp = true; }
        State::audioJitterBufferMs = jitterMs;

        if (State::globalDebugMode) {
            UI::LogDebug("[Config] Loaded %s (DefaultRole=%d, Jitter=%dms, Deadzone=%d%%, Sens=%.2f, GameMode=%d)",
                         fullPath.c_str(), static_cast<int>(State::defaultRole),
                         State::audioJitterBufferMs.load(), State::edgeDeadzonePercent.load(),
                         State::mouseSensitivity.load(), State::enableGameMode.load() ? 1 : 0);
            if (sensParseFailed || hadClamp) {
                UI::LogDebug("[Config] WARNING: Some settings in '%s' were malformed or out of range and were clamped to safe bounds.",
                             fullPath.c_str());
            }
        }

        return true;
    }

    /**
     * @brief Saves the current configuration from the global State to the kamflow.ini file.
     * @return void
     */
    void SaveConfig() {
        const std::string& fullPath = GetResolvedPath();
        DWORD firstWriteErr = 0;
        auto writeIni = [&](const char* section, const char* key, const char* value) {
            if (!WritePrivateProfileStringA(section, key, value, fullPath.c_str()) && firstWriteErr == 0) {
                firstWriteErr = GetLastError();
            }
        };

        writeIni("System", "DefaultRole", std::to_string(static_cast<int>(State::defaultRole)).c_str());
        writeIni("System", "GlobalDebugMode", State::globalDebugMode.load() ? "1" : "0");
        writeIni("System", "MinimizeToTray", State::minimizeToTray.load() ? "1" : "0");

        writeIni("Network", "EnableClientAutoReconnect", State::enableClientAutoReconnect.load() ? "1" : "0");

        writeIni("Control", "EnableKeyboardSync", State::enableKeyboardSync.load() ? "1" : "0");
        writeIni("Control", "EnableClipboardSync", State::enableClipboardSync.load() ? "1" : "0");
        writeIni("Control", "EnableFileTransfer", State::enableFileTransfer.load() ? "1" : "0");
        writeIni("Control", "EdgeDeadzonePercent", std::to_string(State::edgeDeadzonePercent.load()).c_str());

        writeIni("Control", "MouseSensitivity", std::to_string(State::mouseSensitivity.load()).c_str());
        std::string hkStr(1, State::emergencyHotkey.load());
        writeIni("Control", "EmergencyHotkey", hkStr.c_str());

        writeIni("Control", "EnableGameMode", State::enableGameMode.load() ? "1" : "0");
        std::string gmHkStr(1, State::gameModeHotkey.load());
        writeIni("Control", "GameModeHotkey", gmHkStr.c_str());

        writeIni("Audio", "EnableServerAudioMix", State::enableServerAudioMix.load() ? "1" : "0");
        writeIni("Audio", "EnableServerMicBroadcast", State::enableServerMicBroadcast.load() ? "1" : "0");
        writeIni("Audio", "EnableClientAudioStream", State::enableClientAudioStream.load() ? "1" : "0");
        writeIni("Audio", "EnableClientMicReceive", State::enableClientMicReceive.load() ? "1" : "0");
        writeIni("Audio", "JitterBufferMs", std::to_string(State::audioJitterBufferMs.load()).c_str());

        if (firstWriteErr != 0 && State::globalDebugMode) {
            UI::LogDebug("[Config] WARNING: Failed to write config file '%s' (err=%lu — check folder write permissions).",
                         fullPath.c_str(), firstWriteErr);
        }
    }

    /**
     * @brief Saves a client's spatial matrix grid coordinates to the configuration file.
     * @param clientName The network identifier/hostname of the client.
     * @param x The X-coordinate on the spatial grid.
     * @param y The Y-coordinate on the spatial grid.
     * @return void
     */
    void SaveClientLayout(const std::string& clientName, int x, int y) {
        const std::string& fullPath = GetResolvedPath();
        std::string val = std::to_string(x) + "," + std::to_string(y);
        if (!WritePrivateProfileStringA("SpatialLayout", clientName.c_str(), val.c_str(), fullPath.c_str())) {
            if (State::globalDebugMode) {
                UI::LogDebug("[Config] WARNING: Failed to save layout for '%s' to '%s' (err=%lu).",
                             clientName.c_str(), fullPath.c_str(), GetLastError());
            }
        }
    }

    /**
     * @brief Loads a client's spatial matrix grid coordinates from the configuration file.
     * @param clientName The network identifier/hostname of the client.
     * @param outX Reference to store the loaded X-coordinate.
     * @param outY Reference to store the loaded Y-coordinate.
     * @return true if the layout was successfully loaded, false if no layout was found or parsing failed.
     */
    bool LoadClientLayout(const std::string& clientName, int& outX, int& outY) {
        const std::string& fullPath = GetResolvedPath();
        char buf[64];
        GetPrivateProfileStringA("SpatialLayout", clientName.c_str(), "", buf, sizeof(buf), fullPath.c_str());
        if (buf[0] == '\0') return false;
        std::string s(buf);
        size_t comma = s.find(',');
        if (comma != std::string::npos) {
            try {
                outX = std::stoi(s.substr(0, comma));
                outY = std::stoi(s.substr(comma + 1));
                return true;
            } catch (...) {
                if (State::globalDebugMode) {
                    UI::LogDebug("[Config] WARNING: Malformed SpatialLayout entry for '%s': '%s'.",
                                 clientName.c_str(), buf);
                }
                return false;
            }
        }
        return false;
    }
}