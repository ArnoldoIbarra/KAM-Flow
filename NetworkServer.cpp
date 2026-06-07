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
#include "UIManager.h"
#include <iostream>
#include <ws2tcpip.h>
#include <map>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <chrono>
#include <bcrypt.h>
#include <avrt.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "avrt.lib")

namespace Network {

    /// Active TCP listener socket.
    SOCKET serverSocket = INVALID_SOCKET;
    /// Active UDP listener socket for latency-sensitive streaming.
    SOCKET serverUdpSocket = INVALID_SOCKET;
    /// Vector containing all active and authenticated client sockets.
    std::vector<ConnectedClientInfo> activeClients;
    /// Mutex protecting the active clients list.
    std::mutex clientsMutex;
    /// Per-client send mutexes to prevent interleaved TCP writes without global serialization.
    /// Protected by clientsMutex for additions/removals. shared_ptr ensures the mutex stays alive
    /// even if a client disconnects while another thread is mid-send (the pointer keeps it valid).
    std::map<SOCKET, std::shared_ptr<std::mutex>> clientSendMutexes;
    /// Background thread for accepting incoming TCP connections.
    std::thread serverThread;   
    /// Control flag for the TCP server lifecycle.
    std::atomic<bool> isRunning(false);

    /// Zero-cost atomic flag indicating if any clients are currently connected.
    std::atomic<bool> g_hasClients(false);

    /// The active 8-digit security PIN loaded from the Credential Manager.
    std::string activeServerPin;

    /// Global transmit sequence counter.
    std::atomic<uint32_t> serverTxSequence(0);
    /// Independent UDP transmit sequence counter to prevent cross-talk replay drops.
    std::atomic<uint32_t> serverUdpTxSequence{0};

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
    
    /// Background thread for receiving connectionless UDP packets.
    std::thread udpListenerThread;

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
        uint32_t rng;
        BCryptGenRandom(NULL, (PUCHAR)&rng, sizeof(rng), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        activeServerPin = std::to_string(10000000 + (rng % 90000000));
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
        return g_hasClients.load(std::memory_order_relaxed);
    }

    /**
     * @brief Thread-safe getter for the list of currently connected clients.
     * @return A vector containing active client details.
     */
    std::vector<ConnectedClientInfo> GetConnectedClients() {
        static std::vector<ConnectedClientInfo> cachedClients;
        static uint64_t lastCacheTime = 0;
        uint64_t now = GetTickCount64();

        if (now - lastCacheTime > 500) {
            std::lock_guard<std::mutex> lock(clientsMutex);
            cachedClients = activeClients;
            lastCacheTime = now;
        }
        return cachedClients;
    }

    /**
     * @brief Forcefully disconnects a specific client by safely aborting the connection.
     * @param clientSocket The socket of the client to disconnect.
     * @return void
     */
    void DisconnectClient(SOCKET clientSocket) {
        shutdown(clientSocket, SD_BOTH); // Abort the connection safely; the listener thread will clean up the socket.
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (const auto& c : activeClients) {
            if (c.socket == clientSocket) {
                // Client disconnected, UDP routing will naturally drop.
                break;
            }
        }
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
     * @brief Retrieves the audio state for a specific client without deep-copying structures.
     * @param clientSocket The socket of the client.
     */
    bool GetClientAudioState(SOCKET clientSocket, bool& isEnabled, float& volume) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (const auto& c : activeClients) {
            if (c.socket == clientSocket) {
                isEnabled = c.isAudioEnabled;
                volume = c.audioVolume;
                return true;
            }
        }
        return false;
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
        int rxHbCount = 0;
        
        thread_local std::vector<uint8_t> tls_rawPayload;
        thread_local std::vector<uint8_t> tls_decrypted;
        tls_rawPayload.reserve(MAX_PAYLOAD_SIZE);
        tls_decrypted.reserve(MAX_PAYLOAD_SIZE);

        while (isRunning) {
            PacketHeader header;
            int bytes = recv(clientSocket, (char*)&header, sizeof(header), MSG_WAITALL);
            if (bytes <= 0) {
                int wsaErr = WSAGetLastError();
                if (wsaErr == WSAETIMEDOUT) {
                    if (State::globalDebugMode) UI::LogDebug("[Network Server] Client timed out (No Heartbeat received). WSA Error: %d", wsaErr);
                } else {
                    if (State::globalDebugMode) UI::LogDebug("[Network Server] Client socket severed. WSA Error: %d", wsaErr);
                }
                break; 
            }

            if (header.magic == PACKET_MAGIC) {
                if (header.payloadSize > MAX_PAYLOAD_SIZE) {
                    if (State::globalDebugMode) UI::LogDebug("[Network Server] CRITICAL: Payload size exceeds safety bounds! Dropping connection.");
                    break;
                }

                tls_rawPayload.resize(header.payloadSize);
                if (header.payloadSize > 0) {
                    int pBytes = recv(clientSocket, (char*)tls_rawPayload.data(), header.payloadSize, MSG_WAITALL);
                    if (pBytes != static_cast<int>(header.payloadSize)) {
                        if (State::globalDebugMode) UI::LogDebug("[Network Server] WARNING: Partial payload received. (Got %d of %u bytes, Error: %d). Dropping connection.", pBytes, header.payloadSize, WSAGetLastError());
                        break; 
                    }
                }

                if (header.sequenceNumber <= rxSequence && header.sequenceNumber != 0) {
                    if (State::globalDebugMode) UI::LogDebug("[Security] Replay attack detected. Dropping packet.");
                    continue;
                }
                rxSequence = header.sequenceNumber;

                const uint8_t* finalPayload = tls_rawPayload.data();
                size_t finalSize = tls_rawPayload.size();

                bool isEncrypted = (header.type != MessageType::EVENT_AUTH);

                if (isEncrypted) {
                    if (!Security::DecryptPayload(tls_rawPayload.data(), tls_rawPayload.size(), &header, sizeof(header), tls_decrypted)) {
                        if (State::globalDebugMode) UI::LogDebug("[Security] Failed to decrypt client packet.");
                        continue;
                    }
                    finalPayload = tls_decrypted.data();
                    finalSize = tls_decrypted.size();
                }

                if (header.type == MessageType::EVENT_HEARTBEAT) {
                    if (++rxHbCount % 10 == 0 && State::globalDebugMode) {
                        UI::LogDebug("[Network Server] Received Keep-Alive Heartbeat from Client.");
                    }
                    continue; // Keep-alive heartbeat acknowledged.
                }

                if (header.type == MessageType::EVENT_AUDIO_DATA) {
                    // Pass the raw PCM data to the audio manager for mixing and playback.
                    Audio::HandleAudioData(clientSocket, finalPayload, finalSize);
                } else if (header.type == MessageType::EVENT_CLIPBOARD) {
                    if (finalSize > 0) {
                        // Convert byte stream back to a wide string and set it locally.
                        std::wstring text;
                if (finalSize % sizeof(wchar_t) == 0 && finalSize > 0) {
                    text.assign((wchar_t*)finalPayload, finalSize / sizeof(wchar_t));
                    // Ensure the string is properly terminated before passing to clipboard
                    if (!text.empty() && text.back() == L'\0') {
                        text.pop_back();
                    }
                }
                
                if (!text.empty()) {
                    ClipboardManager::SetRemoteClipboard(text);
                }
                    }
                } else if (header.type == MessageType::EVENT_RETURN_CONTROL) {
                    if (finalSize == sizeof(ReturnControlPayload)) {
                        ReturnControlPayload p;
                        memcpy(&p, finalPayload, sizeof(p));
                        
                        int sourceX = 0, sourceY = 0;
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            for (const auto& c : activeClients) {
                                if (c.socket == clientSocket) {
                                    sourceX = c.gridX;
                                    sourceY = c.gridY;
                                    break;
                                }
                            }
                        }
                        RouteCursorTransition(sourceX, sourceY, p.exitEdge, p.normalizedX, p.normalizedY);
                    }
                } else if (header.type == MessageType::EVENT_CLIENT_LOCKED) {
                    if (State::IsRemote()) {
                        if (State::globalDebugMode) UI::LogDebug("[Network Server] Client locked by UAC/Secure Desktop. Auto-reverting control.");
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

        if (State::globalDebugMode) UI::LogDebug("[Network Server] Client disconnected.");
        
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto it = activeClients.begin(); it != activeClients.end(); ++it) {
            if (it->socket == clientSocket) {
                activeClients.erase(it);
                g_hasClients.store(!activeClients.empty(), std::memory_order_relaxed);
                clientSendMutexes.erase(clientSocket);
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
                            if (auth.clientName[sizeof(auth.clientName) - 1] != '\0') {
                    auth.clientName[sizeof(auth.clientName) - 1] = '\0';
                }
                if (auth.pin[sizeof(auth.pin) - 1] != '\0') {
                    auth.pin[sizeof(auth.pin) - 1] = '\0';
                }

                volatile uint8_t diff = 0;
                for (int i = 0; i < 8; ++i) {
                    diff |= auth.pin[i] ^ activeServerPin.c_str()[i];
                }

                if (diff == 0) {


                                if (State::globalDebugMode) UI::LogDebug("[Network Server] Client Authenticated.");

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
                                DWORD sndTimeout = 5000; 
                                setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

                                int flag = 1;
                                setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
                                
                                int tos = 0xB8; // DSCP 46 (Expedited Forwarding) for Voice/Real-Time priority
                                setsockopt(clientSocket, IPPROTO_IP, IP_TOS, (char*)&tos, sizeof(tos));

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
                                    sockaddr_in emptyUdpAddr;
                                    memset(&emptyUdpAddr, 0, sizeof(emptyUdpAddr));
                                    activeClients.push_back({clientSocket, emptyUdpAddr, false, ipStr, safeName, 1.0f, true, true, targetX, targetY, 0});
                                    g_hasClients.store(true, std::memory_order_relaxed);
                                    clientSendMutexes[clientSocket] = std::make_shared<std::mutex>();
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
     * @brief Listener thread for connectionless UDP streaming (Mouse and Audio).
     */
    void UDPListenerLoop() {
        std::vector<uint8_t> buffer(MAX_PAYLOAD_SIZE);
        std::vector<uint8_t> decrypted(MAX_PAYLOAD_SIZE);
        sockaddr_in senderAddr;
        int senderAddrSize = sizeof(senderAddr);

        while (isRunning) {
            int bytes = recvfrom(serverUdpSocket, (char*)buffer.data(), (int)buffer.size(), 0, (sockaddr*)&senderAddr, &senderAddrSize);
            if (bytes <= 0) continue;

            if (bytes < sizeof(PacketHeader)) continue;

            PacketHeader header;
            memcpy(&header, buffer.data(), sizeof(PacketHeader));

            if (header.magic != PACKET_MAGIC) continue;

            size_t payloadSize = header.payloadSize;
            if (payloadSize > MAX_PAYLOAD_SIZE || bytes < sizeof(PacketHeader) + payloadSize) continue;

            const uint8_t* rawPayload = buffer.data() + sizeof(PacketHeader);
            
            if (!Security::DecryptPayload(rawPayload, payloadSize, &header, sizeof(header), decrypted)) {
                continue; // Decryption failed, drop packet
            }

            if (header.type == MessageType::EVENT_UDP_HANDSHAKE) {
                if (decrypted.size() == sizeof(UDPHandshakePayload)) {
                    UDPHandshakePayload p;
                    memcpy(&p, decrypted.data(), sizeof(p));
                    // Safely ensure null termination
                    p.clientName[sizeof(p.clientName) - 1] = '\0';
                    
                    SOCKET ackSocket = INVALID_SOCKET;
                    {
                        std::lock_guard<std::mutex> lock(clientsMutex);
                        for (auto& c : activeClients) {
                            if (strncmp(c.name.c_str(), p.clientName, 32) == 0) {
                                c.udpAddr = senderAddr;
                                c.hasUdp = true;
                                ackSocket = c.socket;
                                if (State::globalDebugMode) UI::LogDebug("[Network Server] UDP Stream Handshake matched for %s. Connection verified.", p.clientName);
                                break;
                            }
                        }
                    } // Release mutex before calling SendToClient
                    
                    if (ackSocket != INVALID_SOCKET) {
                        SendToClient(ackSocket, MessageType::EVENT_UDP_HANDSHAKE_ACK, nullptr, 0);
                    }
                }
                continue;
            }

            SOCKET matchingSocket = INVALID_SOCKET;
            int sourceX = 0, sourceY = 0;
            
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                for (auto& c : activeClients) {
                    if (c.hasUdp && c.udpAddr.sin_addr.s_addr == senderAddr.sin_addr.s_addr && c.udpAddr.sin_port == senderAddr.sin_port) {
                        // Drop out-of-order packets immediately to prevent backward time travel in audio/mouse
                        if (header.sequenceNumber <= c.udpRxSequence && header.sequenceNumber != 0) {
                            matchingSocket = (SOCKET)-1; // Flag as replay drop
                            break;
                        }
                        c.udpRxSequence = header.sequenceNumber;
                        matchingSocket = c.socket;
                        sourceX = c.gridX;
                        sourceY = c.gridY;
                        break;
                    }
                }
            }

            if (matchingSocket == INVALID_SOCKET || matchingSocket == (SOCKET)-1) continue;

            if (header.type == MessageType::EVENT_AUDIO_DATA || header.type == MessageType::EVENT_MIC_DATA) {
                Audio::HandleAudioData(matchingSocket, decrypted.data(), decrypted.size());
            } else if (header.type == MessageType::EVENT_RETURN_CONTROL) {
                if (decrypted.size() == sizeof(ReturnControlPayload)) {
                    ReturnControlPayload p;
                    memcpy(&p, decrypted.data(), sizeof(p));
                    // Route the cursor dynamically based on the client's grid coordinates
                    RouteCursorTransition(sourceX, sourceY, p.exitEdge, p.normalizedX, p.normalizedY);
                }
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
        serverUdpTxSequence = 0;

        if (!CredentialManager::LoadSecret("KAMFlow_LocalServer", activeServerPin)) {
            // First boot: Generate a secure 8-digit random PIN
            uint32_t rng;
            BCryptGenRandom(NULL, (PUCHAR)&rng, sizeof(rng), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            activeServerPin = std::to_string(10000000 + (rng % 90000000));
            CredentialManager::SaveSecret("KAMFlow_LocalServer", activeServerPin);
            if (State::globalDebugMode) UI::LogDebug("[Security] Generated new Master PIN: %s", activeServerPin.c_str());
        }

        if (!Security::Initialize(activeServerPin.c_str())) {
            if (State::globalDebugMode) UI::LogDebug("[Security] FATAL: Cryptography Engine failed to initialize.");
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
        
        serverUdpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (bind(serverUdpSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;

        isRunning = true;
        serverThread = std::thread([]() {
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            AcceptClientsLoop();
        });
        
        udpListenerThread = std::thread([]() {
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            UDPListenerLoop();
        });

        isBeaconRunning = true;
        beaconThread = std::thread(UDPBeaconLoop, port);

        serverHeartbeatThread = std::thread([]() {
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            int txHbCount = 0;
            while (isRunning) {
                for (int i = 0; i < 20 && isRunning; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (isRunning) {
                    BroadcastMessage(MessageType::EVENT_HEARTBEAT, nullptr, 0);
                    
                    if (++txHbCount % 10 == 0 && State::globalDebugMode) {
                        UI::LogDebug("[Network Server] Sent Keep-Alive Heartbeat to Clients.");
                    }
                }
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

        if (serverUdpSocket != INVALID_SOCKET) {
            closesocket(serverUdpSocket);
            serverUdpSocket = INVALID_SOCKET;
        }

        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (const auto& client : activeClients) {
                shutdown(client.socket, SD_BOTH);
            }
            clientSendMutexes.clear();
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
        // Snapshot targets WITH their per-client send mutexes so we don't hold clientsMutex during send().
        struct SendTarget { SOCKET sock; std::shared_ptr<std::mutex> mtx; bool useUdp; sockaddr_in udpAddr; };
        std::vector<SendTarget> targets;
        static bool hasLoggedTcpFallback = false;
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            if (activeClients.empty()) return false;
            for (const auto& c : activeClients) {
                SOCKET targetSock = c.socket;
                bool isMediaPacket = (type == MessageType::EVENT_MOUSE || type == MessageType::EVENT_AUDIO_DATA || type == MessageType::EVENT_MIC_DATA);
                bool useUdp = isMediaPacket && c.hasUdp;
                
                if (isMediaPacket && !c.hasUdp && State::globalDebugMode) {
                    if (!hasLoggedTcpFallback) {
                        UI::LogDebug("[Network Server] UDP not available. Falling back to TCP for media streams.");
                        hasLoggedTcpFallback = true;
                    }
                } else if (c.hasUdp) {
                    hasLoggedTcpFallback = false; // Reset if UDP becomes available again
                }

                auto it = clientSendMutexes.find(targetSock);
                if (it != clientSendMutexes.end()) {
                    targets.push_back({targetSock, it->second, useUdp, c.udpAddr});
                }
            }
        }

        // Per-client send: a congested Client A no longer blocks sends to Client B from other threads.
        // CRITICAL: Sequence generation AND encryption MUST happen inside the sendLock.
        // Otherwise, concurrent threads (Mouse, Heartbeat) will interleave sequence numbers 
        // into the TCP stream, causing the receiver to drop packets as false-positive Replay Attacks.
        for (auto& t : targets) {
            std::lock_guard<std::mutex> sendLock(*t.mtx);
            
            bool isUdp = t.useUdp;
            uint32_t seq = isUdp ? ++serverUdpTxSequence : ++serverTxSequence;
            PacketHeader h = { PACKET_MAGIC, type, static_cast<uint32_t>(payloadSize), seq };
            
            thread_local std::vector<uint8_t> tls_sendBuffer;
            thread_local std::vector<uint8_t> tls_ciphertext;

            if (type != MessageType::EVENT_AUTH && type != MessageType::EVENT_UDP_BEACON) {
                h.payloadSize = static_cast<uint32_t>(payloadSize + 28); // 12-byte IV + 16-byte Tag
                if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h), tls_ciphertext)) {
                    tls_sendBuffer.resize(sizeof(h) + tls_ciphertext.size());
                    memcpy(tls_sendBuffer.data(), &h, sizeof(h));
                    memcpy(tls_sendBuffer.data() + sizeof(h), tls_ciphertext.data(), tls_ciphertext.size());
                } else {
                    continue; 
                }
            } else {
                tls_sendBuffer.resize(sizeof(h) + payloadSize);
                memcpy(tls_sendBuffer.data(), &h, sizeof(h));
                memcpy(tls_sendBuffer.data() + sizeof(h), payload, payloadSize);
            }

            uint64_t tStart = GetTickCount64();
            if (isUdp) {
                sendto(serverUdpSocket, (const char*)tls_sendBuffer.data(), (int)tls_sendBuffer.size(), 0, (sockaddr*)&t.udpAddr, sizeof(t.udpAddr));
            } else {
                if (send(t.sock, (const char*)tls_sendBuffer.data(), (int)tls_sendBuffer.size(), 0) == SOCKET_ERROR) {
                    shutdown(t.sock, SD_BOTH); // Force the listener loop to sever the dead socket.
                }
            }
            uint64_t tEnd = GetTickCount64();
            if (State::globalDebugMode && (tEnd - tStart) > 50 && !isUdp) {
                UI::LogDebug("[Network Server] WARNING: send() to client blocked for %llu ms! TCP congested.", (unsigned long long)(tEnd - tStart));
            }
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
        // Snapshot clipboard-enabled targets with their per-client send mutexes.
        struct SendTarget { SOCKET sock; std::shared_ptr<std::mutex> mtx; };
        std::vector<SendTarget> targets;
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            if (activeClients.empty()) return false;
            for (const auto& c : activeClients) {
                if (c.isClipboardEnabled) {
                    auto it = clientSendMutexes.find(c.socket);
                    if (it != clientSendMutexes.end()) {
                        targets.push_back({c.socket, it->second});
                    }
                }
            }
        }
        if (targets.empty()) return false;

        // Per-client send: clipboard delivery to each client is independently locked.
        for (auto& t : targets) {
            std::lock_guard<std::mutex> sendLock(*t.mtx);
            
            PacketHeader h = { PACKET_MAGIC, MessageType::EVENT_CLIPBOARD, static_cast<uint32_t>(payloadSize), ++serverTxSequence };
            h.payloadSize = static_cast<uint32_t>(payloadSize + 28); // 12-byte IV + 16-byte Tag

            thread_local std::vector<uint8_t> tls_sendBuffer;
            thread_local std::vector<uint8_t> tls_ciphertext;

            if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h), tls_ciphertext)) {
                tls_sendBuffer.resize(sizeof(h) + tls_ciphertext.size());
                memcpy(tls_sendBuffer.data(), &h, sizeof(h));
                memcpy(tls_sendBuffer.data() + sizeof(h), tls_ciphertext.data(), tls_ciphertext.size());
                
                if (send(t.sock, (const char*)tls_sendBuffer.data(), (int)tls_sendBuffer.size(), 0) == SOCKET_ERROR) {
                    shutdown(t.sock, SD_BOTH);
                }
            }
        }
        return true;
    }

    bool SendToClient(SOCKET clientSocket, MessageType type, const void* payload, size_t payloadSize) {
        bool useUdp = false;
        sockaddr_in udpAddr;
        
        thread_local std::vector<uint8_t> tls_sendBuffer;
        thread_local std::vector<uint8_t> tls_ciphertext;
            
            // Lock only this specific client's send mutex, not a global lock.
            std::shared_ptr<std::mutex> sendMtx;
            SOCKET targetSock = clientSocket;
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                for (const auto& c : activeClients) {
                    if (c.socket == clientSocket) {
                        useUdp = (type == MessageType::EVENT_MOUSE || type == MessageType::EVENT_AUDIO_DATA || type == MessageType::EVENT_MIC_DATA) && c.hasUdp;
                        udpAddr = c.udpAddr;
                        break;
                    }
                }
                auto it = clientSendMutexes.find(targetSock);
                if (it != clientSendMutexes.end()) sendMtx = it->second;
            }
            if (!sendMtx) return false; // Client disconnected before we could send
            
            std::lock_guard<std::mutex> sendLock(*sendMtx);
            
            // Generate sequence and encrypt under the per-client lock
            uint32_t seq = useUdp ? ++serverUdpTxSequence : ++serverTxSequence;
            PacketHeader h = { PACKET_MAGIC, type, static_cast<uint32_t>(payloadSize), seq };

            h.payloadSize = static_cast<uint32_t>(payloadSize + 28);
            if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h), tls_ciphertext)) {
                tls_sendBuffer.resize(sizeof(h) + tls_ciphertext.size());
                memcpy(tls_sendBuffer.data(), &h, sizeof(h));
                memcpy(tls_sendBuffer.data() + sizeof(h), tls_ciphertext.data(), tls_ciphertext.size());

                uint64_t tStart = GetTickCount64();
                int res = 0;
                if (useUdp) {
                    res = sendto(serverUdpSocket, (const char*)tls_sendBuffer.data(), (int)tls_sendBuffer.size(), 0, (sockaddr*)&udpAddr, sizeof(udpAddr));
                } else {
                    res = send(targetSock, (const char*)tls_sendBuffer.data(), (int)tls_sendBuffer.size(), 0);
                }
                uint64_t tEnd = GetTickCount64();
                if (State::globalDebugMode && (tEnd - tStart) > 50 && !useUdp) {
                    UI::LogDebug("[Network Server] WARNING: SendToClient blocked for %llu ms! TCP congested.", (unsigned long long)(tEnd - tStart));
                }
                return res != SOCKET_ERROR;
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