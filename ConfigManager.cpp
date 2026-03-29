// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Config Manager Implementation
// =============================================================================

/**
 * @file ConfigManager.cpp
 * @brief Implementation of Win32-based .ini configuration management.
 */

#include "ConfigManager.h"
#include "StateManager.h"
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
            SaveConfig("127.0.0.1", 8080);
            State::UpdateConsoleVisibility(); // Ensure initial console state applies
            return true;
        }

        // Load system role configuration.
        int savedRole = GetPrivateProfileIntA("System", "DefaultRole", 0, fullPath.c_str());
        State::defaultRole = static_cast<State::AppRole>(savedRole);

        // Load global debug and security preferences.
        State::globalDebugMode = GetPrivateProfileIntA("System", "GlobalDebugMode", 0, fullPath.c_str()) != 0;
        State::minimizeToTray = GetPrivateProfileIntA("System", "MinimizeToTray", 1, fullPath.c_str()) != 0;

        // Apply console visibility immediately upon reading the configuration.
        State::UpdateConsoleVisibility();

        // Load input synchronization toggles.
        State::enableKeyboardSync = GetPrivateProfileIntA("Control", "EnableKeyboardSync", 1, fullPath.c_str()) != 0;
        State::enableClipboardSync = GetPrivateProfileIntA("Control", "EnableClipboardSync", 1, fullPath.c_str()) != 0;
        State::enableFileTransfer = GetPrivateProfileIntA("Control", "EnableFileTransfer", 1, fullPath.c_str()) != 0;
        State::enableClientAutoReconnect = GetPrivateProfileIntA("Network", "EnableClientAutoReconnect", 1, fullPath.c_str()) != 0;

        State::edgeDeadzonePercent = GetPrivateProfileIntA("Control", "EdgeDeadzonePercent", 5, fullPath.c_str());
        if (State::edgeDeadzonePercent < 0) State::edgeDeadzonePercent = 0;
        if (State::edgeDeadzonePercent > 10) State::edgeDeadzonePercent = 10;

        char floatBuf[32];
        GetPrivateProfileStringA("Control", "MouseSensitivity", "1.0", floatBuf, sizeof(floatBuf), fullPath.c_str());
        try {
            State::mouseSensitivity = std::stof(floatBuf);
        } catch (...) {
            State::mouseSensitivity = 1.0f;
        }
        // Clamp to safe values in case the .ini was manually edited outside the UI.
        if (State::mouseSensitivity < 0.1f) State::mouseSensitivity = 0.1f;
        if (State::mouseSensitivity > 3.0f) State::mouseSensitivity = 3.0f;

        char hotkeyBuf[8];
        GetPrivateProfileStringA("Control", "EmergencyHotkey", "M", hotkeyBuf, sizeof(hotkeyBuf), fullPath.c_str());
        State::emergencyHotkey = (hotkeyBuf[0] >= 'A' && hotkeyBuf[0] <= 'Z') ? hotkeyBuf[0] : 'M';

        // Load audio subsystem toggles.
        State::enableServerAudioMix = GetPrivateProfileIntA("Audio", "EnableServerAudioMix", 1, fullPath.c_str()) != 0;
        State::enableServerMicBroadcast = GetPrivateProfileIntA("Audio", "EnableServerMicBroadcast", 0, fullPath.c_str()) != 0;
        State::enableClientAudioStream = GetPrivateProfileIntA("Audio", "EnableClientAudioStream", 1, fullPath.c_str()) != 0;
        State::enableClientMicReceive = GetPrivateProfileIntA("Audio", "EnableClientMicReceive", 0, fullPath.c_str()) != 0;

        State::audioJitterBufferMs = GetPrivateProfileIntA("Audio", "JitterBufferMs", 50, fullPath.c_str());
        if (State::audioJitterBufferMs < 20) State::audioJitterBufferMs = 20;
        if (State::audioJitterBufferMs > 500) State::audioJitterBufferMs = 500;

        return true;
    }

    /**
     * @brief Saves the current configuration from the global State to the kamflow.ini file.
     * @param targetIp The IP address string to save for network connections.
     * @param port The port number to save for network connections.
     */
    void SaveConfig(const std::string& targetIp, uint16_t port) {
        const std::string& fullPath = GetResolvedPath();

        WritePrivateProfileStringA("System", "DefaultRole", std::to_string(static_cast<int>(State::defaultRole)).c_str(), fullPath.c_str());
        WritePrivateProfileStringA("System", "GlobalDebugMode", State::globalDebugMode ? "1" : "0", fullPath.c_str());
        WritePrivateProfileStringA("System", "MinimizeToTray", State::minimizeToTray ? "1" : "0", fullPath.c_str());

        WritePrivateProfileStringA("Network", "TargetIP", targetIp.c_str(), fullPath.c_str());
        WritePrivateProfileStringA("Network", "Port", std::to_string(port).c_str(), fullPath.c_str());
        WritePrivateProfileStringA("Network", "EnableClientAutoReconnect", State::enableClientAutoReconnect ? "1" : "0", fullPath.c_str());

        WritePrivateProfileStringA("Control", "EnableKeyboardSync", State::enableKeyboardSync ? "1" : "0", fullPath.c_str());
        WritePrivateProfileStringA("Control", "EnableClipboardSync", State::enableClipboardSync ? "1" : "0", fullPath.c_str());
        WritePrivateProfileStringA("Control", "EnableFileTransfer", State::enableFileTransfer ? "1" : "0", fullPath.c_str());
        WritePrivateProfileStringA("Control", "EdgeDeadzonePercent", std::to_string(State::edgeDeadzonePercent).c_str(), fullPath.c_str());

        WritePrivateProfileStringA("Control", "MouseSensitivity", std::to_string(State::mouseSensitivity).c_str(), fullPath.c_str());
        std::string hkStr(1, State::emergencyHotkey);
        WritePrivateProfileStringA("Control", "EmergencyHotkey", hkStr.c_str(), fullPath.c_str());

        WritePrivateProfileStringA("Audio", "EnableServerAudioMix", State::enableServerAudioMix ? "1" : "0", fullPath.c_str());
        WritePrivateProfileStringA("Audio", "EnableServerMicBroadcast", State::enableServerMicBroadcast ? "1" : "0", fullPath.c_str());
        WritePrivateProfileStringA("Audio", "EnableClientAudioStream", State::enableClientAudioStream ? "1" : "0", fullPath.c_str());
        WritePrivateProfileStringA("Audio", "EnableClientMicReceive", State::enableClientMicReceive ? "1" : "0", fullPath.c_str());
        WritePrivateProfileStringA("Audio", "JitterBufferMs", std::to_string(State::audioJitterBufferMs).c_str(), fullPath.c_str());
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
        WritePrivateProfileStringA("SpatialLayout", clientName.c_str(), val.c_str(), fullPath.c_str());
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
            } catch (...) { return false; }
        }
        return false;
    }
}