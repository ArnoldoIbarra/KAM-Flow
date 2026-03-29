// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Network Server Implementation
// =============================================================================

/**
 * @file NetworkServer.cpp
 * @brief Implementation of the WinSock2 TCP Server and UDP Auto-Discovery Beacon.
 */

#include "NetworkMessages.h"
#include "NetworkServer.h"
#include "StateManager.h"
#include "ConfigManager.h"
#include "MouseCapture.h" 
#include "SecurityManager.h"
#include "AudioManager.h"
#include "ClipboardManager.h"
#include "FileTransferManager.h"
#include "CredentialManager.h"
#include <iostream>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <chrono>

namespace Network {

    /// Active TCP listener socket.
    SOCKET serverSocket = INVALID_SOCKET;
    /// Vector containing all active and authenticated client sockets.
    std::vector<ConnectedClientInfo> activeClients;
    /// Mutex protecting the active clients list.
    std::mutex clientsMutex;
    /// Background thread for accepting incoming TCP connections.
    std::thread serverThread;   
    /// Control flag for the TCP server lifecycle.
    std::atomic<bool> isRunning(false);

    /// The active 8-digit security PIN loaded from the Credential Manager.
    std::string activeServerPin;

    /// Global transmit sequence counter.
    std::atomic<uint32_t> serverTxSequence(0);

    // UDP Beacon state variables.
    std::thread beaconThread;
    std::atomic<bool> isBeaconRunning(false);

    // Heartbeat state variables.
    std::thread serverHeartbeatThread;

    /// Wrapper for safely tracking and reaping individual client threads.
    struct ClientThreadInfo {
        std::thread thread;
        std::atomic<bool> isFinished{false};
    };
    std::vector<std::shared_ptr<ClientThreadInfo>> activeClientThreadsList;
    std::mutex clientThreadsListMutex;

    /**
     * @brief Retrieves the active Master PIN from memory for display in the UI.
     * @return The 8-digit Master PIN string.
     */
    std::string GetMasterPin() {
        return activeServerPin;
    }

    /**
     * @brief Generates a new Master PIN, saves it to the vault, and instantly disconnects all current clients.
     * @return void
     */
    void RegenerateMasterPin() {
        srand((unsigned int)time(NULL));
        activeServerPin = std::to_string(10000000 + (rand() % 90000000));
        CredentialManager::SaveSecret("KAMFlow_LocalServer", activeServerPin);
        
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (const auto& client : activeClients) {
            shutdown(client.socket, SD_BOTH);
        }
    }

    /**
     * @brief Checks if there are any clients currently connected and authenticated.
     * @return true if at least one client is ready to receive input.
     */
    bool HasAuthenticatedClients() {
        std::lock_guard<std::mutex> lock(clientsMutex);
        return !activeClients.empty();
    }

    /**
     * @brief Thread-safe getter for the list of currently connected clients.
     * @return A vector containing active client details.
     */
    std::vector<ConnectedClientInfo> GetConnectedClients() {
        std::lock_guard<std::mutex> lock(clientsMutex);
        return activeClients;
    }

    /**
     * @brief Forcefully disconnects a specific client by safely aborting the connection.
     * @param clientSocket The socket of the client to disconnect.
     * @return void
     */
    void DisconnectClient(SOCKET clientSocket) {
        shutdown(clientSocket, SD_BOTH); // Abort the connection safely; the listener thread will clean up the socket.
    }

    /**
     * @brief Sets the individual audio mix volume for a specific connected client.
     * @param clientSocket The socket of the client.
     * @param volume The volume level (0.0f to 1.0f).
     * @return void
     */
    void SetClientVolume(SOCKET clientSocket, float volume) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto& client : activeClients) {
            if (client.socket == clientSocket) {
                client.audioVolume = volume;
                break;
            }
        }
    }

    /**
     * @brief Toggles whether the server mixes and plays audio from a specific client.
     * @param clientSocket The socket of the client.
     * @param enabled True to enable audio, false to mute and clear the queue.
     * @return void
     */
    void ToggleClientAudio(SOCKET clientSocket, bool enabled) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto& client : activeClients) {
            if (client.socket == clientSocket) {
                client.isAudioEnabled = enabled;
                if (!enabled) {
                    Audio::ClearClientAudioQueue(clientSocket);
                }
                break;
            }
        }
    }

    /**
     * @brief Updates the spatial grid coordinates of a specific client.
     * @param clientSocket The socket of the client.
     * @param x The new X coordinate on the grid.
     * @param y The new Y coordinate on the grid.
     * @return void
     */
    void UpdateClientGridPosition(SOCKET clientSocket, int x, int y) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto& client : activeClients) {
            if (client.socket == clientSocket) {
                client.gridX = x;
                client.gridY = y;
                Config::SaveClientLayout(client.name, x, y);
                break;
            }
        }
    }

    /**
     * @brief Toggles clipboard synchronization for a specific client.
     * @param clientSocket The socket of the client.
     * @param enabled True to enable clipboard sync, false to disable.
     * @return void
     */
    void ToggleClientClipboard(SOCKET clientSocket, bool enabled) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto& client : activeClients) {
            if (client.socket == clientSocket) {
                client.isClipboardEnabled = enabled;
                break;
            }
        }
    }


    /**
     * @brief Background loop that broadcasts the Server's IP and Port to the local subnet.
     * @param tcpPort The port the TCP Server is currently listening on.
     * @return void
     */
    void UDPBeaconLoop(uint16_t tcpPort) {
        SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udpSocket == INVALID_SOCKET) return;

        int broadcastEnable = 1;
        setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, (char*)&broadcastEnable, sizeof(broadcastEnable));

        sockaddr_in broadcastAddr;
        broadcastAddr.sin_family = AF_INET;
        broadcastAddr.sin_port = htons(8081);
        broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

        sockaddr_in localhostAddr;
        localhostAddr.sin_family = AF_INET;
        localhostAddr.sin_port = htons(8081);
        inet_pton(AF_INET, "127.0.0.1", &localhostAddr.sin_addr);

        char hostname[32] = "KAM-Flow-Server";
        DWORD size = sizeof(hostname);
        GetComputerNameA(hostname, &size);

        while (isBeaconRunning) {
            PacketHeader header = { PACKET_MAGIC, MessageType::EVENT_UDP_BEACON, sizeof(UDPBeaconPayload), 0 };
            UDPBeaconPayload payload;
            memset(&payload, 0, sizeof(payload));
            strncpy_s(payload.serverName, hostname, _TRUNCATE);
            payload.tcpPort = tcpPort;

            char buffer[sizeof(PacketHeader) + sizeof(UDPBeaconPayload)];
            memcpy(buffer, &header, sizeof(header));
            memcpy(buffer + sizeof(header), &payload, sizeof(payload));

            // Send to physical network
            sendto(udpSocket, buffer, sizeof(buffer), 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));
            // Send to loopback for local 1-PC testing
            sendto(udpSocket, buffer, sizeof(buffer), 0, (sockaddr*)&localhostAddr, sizeof(localhostAddr));

            for(int i = 0; i < 20 && isBeaconRunning; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        closesocket(udpSocket);
    }

    /**
     * @brief Dedicated thread for an authenticated client to listen for return control commands.
     * @param clientSocket The authenticated socket connection.
     * @return void
     */
    void ClientListenerLoop(SOCKET clientSocket, uint32_t initialRxSequence) {
        uint32_t rxSequence = initialRxSequence;
        while (isRunning) {
            PacketHeader header;
            int bytes = recv(clientSocket, (char*)&header, sizeof(header), MSG_WAITALL);
            if (bytes <= 0) {
                if (WSAGetLastError() == WSAETIMEDOUT) {
                    if (State::globalDebugMode) std::cout << "[Network Server] Client timed out (No Heartbeat).\n";
                }
                break; 
            }

            if (header.magic == PACKET_MAGIC) {
                // CRITICAL GUARD: Prevent memory allocation attacks (CWE-400)
                if (header.payloadSize > MAX_PAYLOAD_SIZE) {
                    if (State::globalDebugMode) std::cerr << "[Network Server] CRITICAL: Payload size exceeds safety bounds! Dropping connection.\n";
                    break;
                }

                std::vector<uint8_t> rawPayload(header.payloadSize);
                if (header.payloadSize > 0) {
                    // Ensure full payload is received to avoid processing corrupted state data.
                    int pBytes = recv(clientSocket, (char*)rawPayload.data(), header.payloadSize, MSG_WAITALL);
                    if (pBytes != static_cast<int>(header.payloadSize)) {
                        if (State::globalDebugMode) std::cerr << "[Network Server] WARNING: Partial payload received. Dropping packet.\n";
                        break; 
                    }
                }

                if (header.sequenceNumber <= rxSequence && header.sequenceNumber != 0) {
                    if (State::globalDebugMode) std::cerr << "[Security] Replay attack detected. Dropping packet.\n";
                    continue;
                }
                rxSequence = header.sequenceNumber;

                std::vector<uint8_t> decrypted;
                const uint8_t* finalPayload = rawPayload.data();
                size_t finalSize = rawPayload.size();

                bool isEncrypted = (header.type != MessageType::EVENT_AUTH);

                if (isEncrypted) {
                    if (!Security::DecryptPayload(rawPayload.data(), rawPayload.size(), &header, sizeof(header), decrypted)) {
                        if (State::globalDebugMode) std::cerr << "[Security] Failed to decrypt client packet.\n";
                        continue;
                    }
                    finalPayload = decrypted.data();
                    finalSize = decrypted.size();
                }

                if (header.type == MessageType::EVENT_HEARTBEAT) {
                    continue; // Keep-alive heartbeat acknowledged.
                }

                if (header.type == MessageType::EVENT_RETURN_CONTROL) {
                    if (finalSize == sizeof(ReturnControlPayload)) {
                        ReturnControlPayload p;
                        memcpy(&p, finalPayload, sizeof(p));
                        
                        int sourceX = 0, sourceY = 0;
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            for (const auto& c : activeClients) {
                                if (c.socket == clientSocket) { sourceX = c.gridX; sourceY = c.gridY; break; }
                            }
                        }
                        // Route the cursor dynamically based on the client's grid coordinates
                        RouteCursorTransition(sourceX, sourceY, p.exitEdge, p.normalizedX, p.normalizedY);
                    }
                } else if (header.type == MessageType::EVENT_AUDIO_DATA) {
                    // Pass the raw PCM data to the audio manager for mixing and playback.
                    Audio::HandleAudioData(clientSocket, finalPayload, finalSize);
                } else if (header.type == MessageType::EVENT_CLIPBOARD) {
                    if (finalSize > 0) {
                        // Convert byte stream back to a wide string and set it locally.
                        std::wstring text((wchar_t*)finalPayload, finalSize / sizeof(wchar_t));
                        ClipboardManager::SetRemoteClipboard(text);
                    }
                } else if (header.type == MessageType::EVENT_CLIENT_LOCKED) {
                    if (State::IsRemote()) {
                        if (State::globalDebugMode) std::cout << "[Network Server] Client locked by UAC/Secure Desktop. Auto-reverting control.\n";
                        State::SetMode(State::ControlMode::LOCAL);
                    }
                } else if (header.type == MessageType::EVENT_FILE_OFFER) {
                    if (finalSize == sizeof(FileOfferPayload)) {
                        FileOfferPayload p; memcpy(&p, finalPayload, sizeof(p));
                        FileTransfer::HandleFileOffer(clientSocket, p);
                    }
                } else if (header.type == MessageType::EVENT_FILE_ACCEPT) {
                    if (finalSize == sizeof(FileAcceptPayload)) {
                        FileAcceptPayload p; memcpy(&p, finalPayload, sizeof(p));
                        FileTransfer::HandleFileAccept(clientSocket, p);
                    }
                } else if (header.type == MessageType::EVENT_FILE_DECLINE) {
                    if (finalSize == sizeof(FileDeclinePayload)) {
                        FileDeclinePayload p; memcpy(&p, finalPayload, sizeof(p));
                        FileTransfer::HandleFileDecline(clientSocket, p);
                    }
                }
            }
        }

        if (State::globalDebugMode) std::cout << "[Network Server] Client disconnected.\n";
        
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto it = activeClients.begin(); it != activeClients.end(); ++it) {
            if (it->socket == clientSocket) {
                activeClients.erase(it);
                break;
            }
        }
        Audio::ClearClientAudioQueue(clientSocket);
        closesocket(clientSocket);
    }

    /**
     * @brief Main listener loop that accepts incoming TCP connections and handles the authentication handshake.
     * @return void
     */
    void AcceptClientsLoop() {
        while (isRunning) {
            // Clean up and reap finished client listener threads.
            {
                std::lock_guard<std::mutex> lock(clientThreadsListMutex);
                for (auto it = activeClientThreadsList.begin(); it != activeClientThreadsList.end(); ) {
                    if ((*it)->isFinished) {
                        if ((*it)->thread.joinable()) {
                            (*it)->thread.join();
                        }
                        it = activeClientThreadsList.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            sockaddr_in clientAddr;
            int clientAddrSize = sizeof(clientAddr);
            SOCKET clientSocket = accept(serverSocket, (sockaddr*)&clientAddr, &clientAddrSize);
            
            if (clientSocket != INVALID_SOCKET) {
                DWORD timeout = 2000;
                setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

                PacketHeader header;
                if (recv(clientSocket, (char*)&header, sizeof(header), MSG_WAITALL) > 0 && 
                    header.magic == PACKET_MAGIC && header.type == MessageType::EVENT_AUTH) {
                    
                    // Validate authentication payload bounds to prevent overflow attacks.
                    if (header.payloadSize >= sizeof(AuthPayload) && header.payloadSize <= MAX_PAYLOAD_SIZE) {
                        std::vector<uint8_t> authBuffer(header.payloadSize);
                        
                        int pBytes = recv(clientSocket, (char*)authBuffer.data(), header.payloadSize, MSG_WAITALL);
                        if (pBytes > 0 && pBytes == static_cast<int>(header.payloadSize)) {
                            
                            AuthPayload auth;
                            memcpy(&auth, authBuffer.data(), sizeof(AuthPayload));
                            if (strncmp(auth.pin, activeServerPin.c_str(), 8) == 0) {


                                if (State::globalDebugMode) std::cout << "[Network Server] Client Authenticated.\n";

                                // Immediately send the server's native audio format directly to the newly authenticated client.
                                // This is a critical handshake step that establishes the format standard for all network audio streams.
                                // We send it directly instead of broadcasting to ensure this specific client receives it.
                                if (State::enableServerAudioMix) {
                                    Audio::AudioFormat serverFormat;
                                    if (Audio::GetDefaultDeviceFormat(serverFormat)) {
                                        MessageType type = MessageType::EVENT_AUDIO_FORMAT;
                                        const void* payload = &serverFormat;
                                        size_t payloadSize = sizeof(serverFormat);

                                        PacketHeader h = { PACKET_MAGIC, type, static_cast<uint32_t>(payloadSize), ++serverTxSequence };
                                        std::vector<uint8_t> buffer;

                                        std::vector<uint8_t> ciphertext;
                                        h.payloadSize = static_cast<uint32_t>(payloadSize + 28); // 12-byte IV + 16-byte Tag
                                        if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h), ciphertext)) {
                                            buffer.resize(sizeof(h) + ciphertext.size());
                                            memcpy(buffer.data(), &h, sizeof(h));
                                            memcpy(buffer.data() + sizeof(h), ciphertext.data(), ciphertext.size());
                                        }

                                        if (!buffer.empty()) {
                                            send(clientSocket, (const char*)buffer.data(), (int)buffer.size(), 0);
                                        }
                                    }
                                }
                                
                                timeout = 5000; 
                                setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
                                DWORD sndTimeout = 1000; 
                                setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

                                int flag = 1;
                                setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

                                char ipStr[INET_ADDRSTRLEN];
                                inet_ntop(AF_INET, &(clientAddr.sin_addr), ipStr, INET_ADDRSTRLEN);
                                
                                char safeName[33] = {0};
                                memcpy(safeName, auth.clientName, 32);

                                int targetX = 0, targetY = 0;
                                bool loaded = Config::LoadClientLayout(safeName, targetX, targetY);

                                {
                                    std::lock_guard<std::mutex> lock(clientsMutex);
                                bool occupied = (targetX == 0 && targetY == 0); // Coordinate (0,0) is explicitly reserved for the Server.
                                    if (loaded && !occupied) {
                                        for (const auto& c : activeClients) {
                                            if (c.gridX == targetX && c.gridY == targetY) { occupied = true; break; }
                                        }
                                    }
                                // Perform a radial search to auto-assign the nearest empty slot to the Server.
                                    if (!loaded || occupied) {
                                        bool found = false;
                                        for (int radius = 1; radius < 10 && !found; ++radius) {
                                            for (int dx = -radius; dx <= radius && !found; ++dx) {
                                                for (int dy = -radius; dy <= radius && !found; ++dy) {
                                                    if (abs(dx) != radius && abs(dy) != radius) continue;
                                                    
                                                    // Only auto-place in slots orthogonally adjacent to the Server or an existing Client
                                                    bool isAdjacent = (abs(dx) + abs(dy) == 1);
                                                    for (const auto& c : activeClients) {
                                                        if (abs(c.gridX - dx) + abs(c.gridY - dy) == 1) { isAdjacent = true; break; }
                                                    }
                                                    if (!isAdjacent) continue;

                                                    bool isOccupied = false;
                                                    for (const auto& c : activeClients) { if (c.gridX == dx && c.gridY == dy) { isOccupied = true; break; } }
                                                    if (!isOccupied) { targetX = dx; targetY = dy; found = true; }
                                                }
                                            }
                                        }
                                    }
                                    activeClients.push_back({clientSocket, ipStr, safeName, 1.0f, true, true, targetX, targetY});
                                    Config::SaveClientLayout(safeName, targetX, targetY);
                                }

                                auto threadInfo = std::make_shared<ClientThreadInfo>();
                                threadInfo->thread = std::thread([clientSocket, seq = header.sequenceNumber, threadInfo]() {
                                    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                                    ClientListenerLoop(clientSocket, seq);
                                    threadInfo->isFinished = true;
                                });
                                
                                {
                                    std::lock_guard<std::mutex> lock(clientThreadsListMutex);
                                    activeClientThreadsList.push_back(threadInfo);
                                }
                                continue; 
                            }
                        }
                    }
                }
                closesocket(clientSocket);
            }
        }
    }

    /**
     * @brief Initializes WinSock2, binds the socket, and starts the background listening thread.
     * @param port The TCP port to listen on.
     * @return true if the server started successfully, false otherwise.
     */
    bool StartServer(uint16_t port) {
        serverTxSequence = 0; // Reset monotonic counter for new sessions

        if (!CredentialManager::LoadSecret("KAMFlow_LocalServer", activeServerPin)) {
            // First boot: Generate a secure 8-digit random PIN
            srand((unsigned int)time(NULL));
            activeServerPin = std::to_string(10000000 + (rand() % 90000000));
            CredentialManager::SaveSecret("KAMFlow_LocalServer", activeServerPin);
            if (State::globalDebugMode) std::cout << "[Security] Generated new Master PIN: " << activeServerPin << "\n";
        }

        if (!Security::Initialize(activeServerPin.c_str())) {
            if (State::globalDebugMode) std::cerr << "[Security] FATAL: Cryptography Engine failed to initialize.\n";
            return false;
        }

        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(serverSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;
        listen(serverSocket, SOMAXCONN);

        isRunning = true;
        serverThread = std::thread(AcceptClientsLoop);

        isBeaconRunning = true;
        beaconThread = std::thread(UDPBeaconLoop, port);

        serverHeartbeatThread = std::thread([]() {
            while (isRunning) {
                for (int i = 0; i < 20 && isRunning; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (isRunning) BroadcastMessage(MessageType::EVENT_HEARTBEAT, nullptr, 0);
            }
        });

        return true;
    }

    /**
     * @brief Shuts down the server, disconnects all clients, and cleans up WinSock resources.
     * @return void
     */
    void StopServer() {
        isRunning = false;
        isBeaconRunning = false;

        if (serverSocket != INVALID_SOCKET) {
            closesocket(serverSocket);
            serverSocket = INVALID_SOCKET;
        }

        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (const auto& client : activeClients) {
                shutdown(client.socket, SD_BOTH);
            }
        }
        
        if (serverThread.joinable()) serverThread.join();
        if (beaconThread.joinable()) beaconThread.join();
        if (serverHeartbeatThread.joinable()) serverHeartbeatThread.join();

        // Securely wait for all client handlers to finish before destroying subsystems
        {
            std::lock_guard<std::mutex> lock(clientThreadsListMutex);
            for (auto& info : activeClientThreadsList) {
                if (info->thread.joinable()) {
                    info->thread.join();
                }
            }
            activeClientThreadsList.clear();
        }

        WSACleanup();
        Security::Shutdown();
    }

    /**
     * @brief Packages a payload with a header, optionally encrypts it, and broadcasts it to all clients.
     * @param type The MessageType identifier.
     * @param payload Pointer to the raw payload structure.
     * @param payloadSize Size of the payload structure.
     * @return true if data was sent to at least one client.
     */
    bool BroadcastMessage(MessageType type, const void* payload, size_t payloadSize) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (activeClients.empty()) return false;

        PacketHeader h = { PACKET_MAGIC, type, static_cast<uint32_t>(payloadSize), ++serverTxSequence };
        std::vector<uint8_t> buffer;

        if (type != MessageType::EVENT_AUTH && type != MessageType::EVENT_UDP_BEACON) {
            std::vector<uint8_t> ciphertext;
            h.payloadSize = static_cast<uint32_t>(payloadSize + 28); // 12-byte IV + 16-byte Tag
            if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h), ciphertext)) {
                buffer.resize(sizeof(h) + ciphertext.size());
                memcpy(buffer.data(), &h, sizeof(h));
                memcpy(buffer.data() + sizeof(h), ciphertext.data(), ciphertext.size());
            } else {
                return false; 
            }
        } else {
            buffer.resize(sizeof(h) + payloadSize);
            memcpy(buffer.data(), &h, sizeof(h));
            memcpy(buffer.data() + sizeof(h), payload, payloadSize);
        }

        for (auto it = activeClients.begin(); it != activeClients.end(); ) {
            if (send(it->socket, (const char*)buffer.data(), (int)buffer.size(), 0) == SOCKET_ERROR) {
                shutdown(it->socket, SD_BOTH); // Force the listener loop to sever the dead socket.
            }
            ++it;
        }
        return true;
    }

    /**
     * @brief Packages clipboard data and broadcasts it to all enabled clients.
     * @param payload Pointer to the raw wide-character string data.
     * @param payloadSize Size of the string data in bytes.
     * @return true if data was sent to at least one client.
     */
    bool BroadcastClipboardMessage(const void* payload, size_t payloadSize) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (activeClients.empty()) return false;

        PacketHeader h = { PACKET_MAGIC, MessageType::EVENT_CLIPBOARD, static_cast<uint32_t>(payloadSize), ++serverTxSequence };
        std::vector<uint8_t> buffer;

        std::vector<uint8_t> ciphertext;
        h.payloadSize = static_cast<uint32_t>(payloadSize + 28); // 12-byte IV + 16-byte Tag
        if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h), ciphertext)) {
            buffer.resize(sizeof(h) + ciphertext.size());
            memcpy(buffer.data(), &h, sizeof(h));
            memcpy(buffer.data() + sizeof(h), ciphertext.data(), ciphertext.size());
        } else {
            return false;
        }

        for (auto it = activeClients.begin(); it != activeClients.end(); ) {
            if (it->isClipboardEnabled) {
                if (send(it->socket, (const char*)buffer.data(), (int)buffer.size(), 0) == SOCKET_ERROR) {
                    shutdown(it->socket, SD_BOTH);
                }
            }
            ++it;
        }
        return true;
    }

    bool SendToClient(SOCKET clientSocket, MessageType type, const void* payload, size_t payloadSize) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        PacketHeader h = { PACKET_MAGIC, type, static_cast<uint32_t>(payloadSize), ++serverTxSequence };
        std::vector<uint8_t> buffer;

        std::vector<uint8_t> ciphertext;
        h.payloadSize = static_cast<uint32_t>(payloadSize + 28);
        if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h), ciphertext)) {
            buffer.resize(sizeof(h) + ciphertext.size());
            memcpy(buffer.data(), &h, sizeof(h));
            memcpy(buffer.data() + sizeof(h), ciphertext.data(), ciphertext.size());
            return send(clientSocket, (const char*)buffer.data(), (int)buffer.size(), 0) != SOCKET_ERROR;
        }
        return false;
    }

    bool RouteCursorTransition(int sourceX, int sourceY, uint8_t exitEdge, float normX, float normY) {
        int dx = 0, dy = 0;
        uint8_t entryEdge = 0;
        
        if (exitEdge == 0)      { dx = -1; entryEdge = 1; normX = 1.0f; } // Left exit -> Right entry
        else if (exitEdge == 1) { dx = 1;  entryEdge = 0; normX = 0.0f; } // Right exit -> Left entry
        else if (exitEdge == 2) { dy = -1; entryEdge = 3; normY = 1.0f; } // Top exit -> Bottom entry
        else if (exitEdge == 3) { dy = 1;  entryEdge = 2; normY = 0.0f; } // Bottom exit -> Top entry

        int targetX = sourceX + dx;
        int targetY = sourceY + dy;
        
        // Cast a directional ray across the spatial grid limits to find the next active node.
        while (targetX >= -10 && targetX <= 10 && targetY >= -10 && targetY <= 10) {
            if (targetX == 0 && targetY == 0) {
                // Ray hit the Server's coordinates; pull control back locally.
                Input::HandleReturnControl(entryEdge, normX, normY);
                State::SetMode(State::ControlMode::LOCAL);
                return true;
            }
            
            SOCKET targetSocket = INVALID_SOCKET;
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                for (const auto& c : activeClients) {
                    if (c.gridX == targetX && c.gridY == targetY) {
                        targetSocket = c.socket;
                        break;
                    }
                }
            }
            
            if (targetSocket != INVALID_SOCKET) {
                // Ray hit an active client; send the synchronization command directly.
                CursorSyncPayload p = { entryEdge, normX, normY };
                if (SendToClient(targetSocket, MessageType::EVENT_SYNC_CURSOR, &p, sizeof(p))) {
                    State::SetMode(State::ControlMode::REMOTE);
                    return true;
                } else {
                    // A client socket error occurred; continue raycasting.
                    targetSocket = INVALID_SOCKET;
                }
            }
            
            // The current node is empty or orphaned.
            // Skip over it by continuing the raycast in the same direction.
            targetX += dx;
            targetY += dy;
        }
        
        // Hit a boundary wall; no device exists in that physical direction.
        return false;
    }
}