// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

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
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include "NetworkMessages.h"

/// Namespace managing network server operations and incoming client connections.
namespace Network {
    
    /// Lock-free per-client transport flags shared across TCP and UDP threads without locking clientsMutex.
    struct ClientTransportState {
        std::atomic<bool> udpActive{false};   ///< True once UDP handshake succeeds for this session.
        std::atomic<bool> sawTcpAudio{false}; ///< True if TCP audio fallback frames were queued before UDP.
    };

    /// Structure containing information about an actively connected client.
    struct ConnectedClientInfo {
        SOCKET socket;           ///< The TCP socket handle.
        sockaddr_in udpAddr;     ///< The UDP return address for this client.
        bool hasUdp;             ///< True if the client has successfully completed the UDP handshake.
        std::string ip;          ///< IPv4 address of the client.
        std::string name;        ///< Hostname of the client PC.
        float audioVolume;       ///< Local mixing volume for this client (0.0f to 1.0f).
        bool isAudioEnabled;     ///< Individual toggle for mixing this client's audio.
        bool isClipboardEnabled; ///< Individual toggle for syncing this client's clipboard.
        int gridX;               ///< Spatial X coordinate relative to Server at (0,0).
        int gridY;               ///< Spatial Y coordinate relative to Server at (0,0).
        uint32_t udpRxSequence;  ///< Independent UDP receive sequence counter for this client.
        uint64_t udpReplayMask;  ///< 64-packet sliding bitmask to reject duplicate UDP packets.
        uint64_t sessionToken;   ///< Cryptographic token binding UDP handshakes to this TCP session.
        bool hasLoggedTcpFallback = false; ///< Tracks whether TCP media fallback was logged for this client.
        UdpReplayStats udpStats = {};       ///< Cumulative UDP delivery, reorder, loss, stale, and duplicate counters.
        uint32_t tcpStallCount = 0;         ///< Count of TCP writes to this client exceeding 200ms.
        uint32_t consecutiveHbSkips = 0;    ///< Consecutive heartbeat try_lock skips due to TCP send saturation.
        uint64_t connectTickMs = 0;         ///< GetTickCount64() timestamp when this client authenticated.
        std::shared_ptr<ClientTransportState> transportState; ///< Lock-free transport flags shared with ClientListenerLoop.
    };

    /// Initializes WinSock2 and starts the background listening thread.
    bool StartServer(uint16_t port);

    /// Shuts down the server, disconnects all clients, and cleans up WinSock resources.
    void StopServer();

    /// Packages a payload with a header, optionally encrypts it, and broadcasts it to all clients.
    bool BroadcastMessage(MessageType type, const void* payload, size_t payloadSize);

    /// Checks if there are any clients currently connected and authenticated.
    bool HasAuthenticatedClients();

    /// Zero-cost atomic flag indicating if any clients are currently connected.
    extern std::atomic<bool> g_hasClients;

    /// Tracks which client socket currently has active KVM control.
    extern std::atomic<SOCKET> g_controllingClientSocket;

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

    /// Retrieves the audio state for a specific client to avoid heavy vector copies.
    bool GetClientAudioState(SOCKET clientSocket, bool& isEnabled, float& volume);

    /// Packages clipboard data and broadcasts it to all enabled clients (optionally excluding the sender).
    bool BroadcastClipboardMessage(const void* payload, size_t payloadSize, SOCKET excludeSocket = INVALID_SOCKET);

    /// Sends a payload directly to a specific client.
    bool SendToClient(SOCKET clientSocket, MessageType type, const void* payload, size_t payloadSize);

    /// Updates a client's spatial grid coordinates and persists them to the .ini file.
    void UpdateClientGridPosition(SOCKET clientSocket, int x, int y);

    /// Calculates grid trajectory and routes control to the appropriate machine.
    bool RouteCursorTransition(int sourceX, int sourceY, uint8_t exitEdge, float normX, float normY);

    /// Fast non-blocking check returning true if an active node exists in the given exit direction.
    bool HasTargetInDirection(int sourceX, int sourceY, uint8_t exitEdge);

    /// Retrieves the active Master PIN from memory for display in the UI.
    std::string GetMasterPin();

    /// Generates a new Master PIN, saves it to the vault, and instantly disconnects all current clients.
    void RegenerateMasterPin();
}