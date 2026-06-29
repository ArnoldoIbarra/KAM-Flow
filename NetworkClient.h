/*
 * Copyright (c) 2026 Arnoldo Ibarra. All Rights Reserved.
 *
 * This software is the confidential and proprietary information of Arnoldo Ibarra.
 * ("Confidential Information"). You shall not disclose such Confidential 
 * Information and shall use it only in accordance with the terms of the 
 * license agreement you entered into.
 */

/// @file NetworkClient.h
/// Manages the TCP Client for connecting to the Server PC and UDP Auto-Discovery listening.

#pragma once
#include "NetworkMessages.h"
#include <string>
#include <cstdint>
#include <vector>
#include "AudioManager.h"

/// Namespace managing network client connections and auto-discovery.
namespace Network {
    
    /// Data structure representing a Server discovered via UDP broadcast.
    struct DiscoveredServer {
        std::string ip;              ///< IPv4 address of the discovered Server.
        std::string hostname;        ///< Machine name of the Server.
        uint16_t tcpPort;            ///< The connection port the Server is listening on.
        uint64_t lastUpdateTick;     ///< System tick count of the last received beacon (used for pruning).
    };

    /// Initializes the WinSock2 UDP listener thread to map local Servers.
    void StartDiscoveryListener();

    /// Stops the UDP listener thread cleanly.
    void StopDiscoveryListener();

    /// Returns a thread-safe copy of all currently active Servers on the local network.
    std::vector<DiscoveredServer> GetDiscoveredServers();

    /// Retrieves the audio format sent by the server upon connection.
    bool GetServerAudioFormat(Audio::AudioFormat& outFormat);

    /// Checks if the active TCP Client is currently connected and processing data.
    bool IsClientConnected();

    /// Initializes WinSock2 and connects to the Server's IP and port.
    bool StartClient(const std::string& serverIp, uint16_t port, bool isAutoReconnect = false);

    /// Safely disconnects from the Server and shuts down the TCP socket.
    void StopClient();

    /// Packages a payload with a header, optionally encrypts it, and sends it to the Server.
    bool SendToServer(MessageType type, const void* payload, size_t payloadSize);

    /// Notifies the client that the system resumed from sleep. Resets reconnect backoff for instant recovery.
    void NotifySystemResumed();
}