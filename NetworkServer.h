/*
 * Copyright (c) 2026 Arnoldo Ibarra. All Rights Reserved.
 *
 * This software is the confidential and proprietary information of Arnoldo Ibarra.
 * ("Confidential Information"). You shall not disclose such Confidential 
 * Information and shall use it only in accordance with the terms of the 
 * license agreement you entered into.
 */

/// @file NetworkServer.h
/// Manages the TCP Server for receiving and authenticating client connections.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "NetworkMessages.h"

/// Namespace managing network server operations and incoming client connections.
namespace Network {
    
    /// Structure containing information about an actively connected client.
    struct ConnectedClientInfo {
        SOCKET socket;           ///< The TCP socket handle.
        std::string ip;          ///< IPv4 address of the client.
        std::string name;        ///< Hostname of the client PC.
        float audioVolume;       ///< Local mixing volume for this client (0.0f to 1.0f).
        bool isAudioEnabled;     ///< Individual toggle for mixing this client's audio.
        bool isClipboardEnabled; ///< Individual toggle for syncing this client's clipboard.
        int gridX;               ///< Spatial X coordinate relative to Server at (0,0).
        int gridY;               ///< Spatial Y coordinate relative to Server at (0,0).
    };

    /// Initializes WinSock2 and starts the background listening thread.
    bool StartServer(uint16_t port);

    /// Shuts down the server, disconnects all clients, and cleans up WinSock resources.
    void StopServer();

    /// Packages a payload with a header, optionally encrypts it, and broadcasts it to all clients.
    bool BroadcastMessage(MessageType type, const void* payload, size_t payloadSize);

    /// Checks if there are any clients currently connected and authenticated.
    bool HasAuthenticatedClients();

    /// Retrieves a list of all currently connected and authenticated clients.
    std::vector<ConnectedClientInfo> GetConnectedClients();

    /// Safely terminates the connection to a specific client socket.
    void DisconnectClient(SOCKET clientSocket);

    /// Adjusts the incoming audio volume modifier for a specific client.
    void SetClientVolume(SOCKET clientSocket, float volume);

    /// Toggles audio mixing for a specific client.
    void ToggleClientAudio(SOCKET clientSocket, bool enabled);

    /// Toggles clipboard sync for a specific client.
    void ToggleClientClipboard(SOCKET clientSocket, bool enabled);

    /// Packages clipboard data and broadcasts it to all enabled clients.
    bool BroadcastClipboardMessage(const void* payload, size_t payloadSize);

    /// Sends a payload directly to a specific client.
    bool SendToClient(SOCKET clientSocket, MessageType type, const void* payload, size_t payloadSize);

    /// Updates a client's spatial grid coordinates and persists them to the .ini file.
    void UpdateClientGridPosition(SOCKET clientSocket, int x, int y);

    /// Calculates grid trajectory and routes control to the appropriate machine.
    bool RouteCursorTransition(int sourceX, int sourceY, uint8_t exitEdge, float normX, float normY);

    /// Retrieves the active Master PIN from memory for display in the UI.
    std::string GetMasterPin();

    /// Generates a new Master PIN, saves it to the vault, and instantly disconnects all current clients.
    void RegenerateMasterPin();
}