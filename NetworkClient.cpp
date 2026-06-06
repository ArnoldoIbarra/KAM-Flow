// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Network Client Implementation
// =============================================================================

/**
 * @file NetworkClient.cpp
 * @brief Implementation of the TCP Client and UDP Auto-Discovery system.
 */

#include "NetworkMessages.h"
#include "NetworkClient.h"
#include "ConfigManager.h"
#include "StateManager.h"
#include "SecurityManager.h"
#include "CredentialManager.h"
#include "AudioManager.h"
#include "ClipboardManager.h"
#include "FileTransferManager.h"
#include "UIManager.h"
#include <iostream>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <cstring> 
#include <algorithm>
#include <vector>
#include <mutex>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

namespace Network {

    /// Active socket connected to the Server.
    SOCKET clientSocket = INVALID_SOCKET;
    /// Background thread processing incoming TCP packets.
    std::thread clientThread;
    /// Control flag for the TCP Client lifecycle.
    std::atomic<bool> isClientRunning(false);

    /// Global transmit sequence counter.
    std::atomic<uint32_t> clientTxSequence{0};

    /// Mutex to prevent interleaved sends from KVM hooks vs heartbeat threads.
    std::mutex clientSendMutex;
    /// Background thread for keeping the connection alive.
    std::thread clientHeartbeatThread;

    /// Active socket connected to the Server specifically for Audio OOB data.
    SOCKET clientAudioSocket = INVALID_SOCKET;
    /// Background thread processing incoming Audio OOB TCP packets.
    std::thread clientAudioThread;
    /// Mutex to prevent interleaved sends of audio data.
    std::mutex clientAudioSendMutex;

    // UDP Discovery state variables.
    SOCKET udpListenerSocket = INVALID_SOCKET;
    std::thread discoveryThread;
    std::atomic<bool> isDiscoveryRunning(false);
    std::vector<DiscoveredServer> discoveredServers;
    std::mutex discoveryMutex;

    // Audio state variables.
    Audio::AudioFormat g_serverAudioFormat;
    std::atomic<bool> g_hasServerAudioFormat(false);

    // Auto-Reconnect state variables.
    std::string lastConnectedIp = "";
    uint16_t lastConnectedPort = 0;
    std::thread autoReconnectThread;
    std::atomic<bool> isIntentionalDisconnect(false);
    std::atomic<bool> isAutoReconnecting(false);
    std::atomic<bool> isCurrentSessionAutoReconnected(false);
    uint64_t connectionStartTime = 0;

    /**
     * @brief Injects KEYUP events for modifier keys to prevent them from getting stuck
     *        when the Server suddenly revokes control or disconnects.
     */
    void ReleaseStuckModifiers() {
        const WORD modifiers[] = { VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN };
        for (WORD vk : modifiers) {
            INPUT i = { 0 };
            i.type = INPUT_KEYBOARD;
            i.ki.wVk = vk;
            i.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &i, sizeof(INPUT));
        }
    }

    /**
     * @brief Packages a payload with a header, optionally encrypts it, and sends it to the Server.
     * @param type The MessageType identifier.
     * @param payload Pointer to the raw payload structure.
     * @param payloadSize Size of the payload structure.
     * @return true if sent successfully.
     */
    bool SendToServer(MessageType type, const void* payload, size_t payloadSize) {
        if (!isClientRunning) return false;
        
        SOCKET targetSock = clientSocket;
        std::mutex* targetMutex = &clientSendMutex;
        if ((type == MessageType::EVENT_AUDIO_DATA || type == MessageType::EVENT_MIC_DATA) && clientAudioSocket != INVALID_SOCKET) {
            targetSock = clientAudioSocket;
            targetMutex = &clientAudioSendMutex;
        }

        if (targetSock == INVALID_SOCKET) return false;

        std::lock_guard<std::mutex> lock(*targetMutex);
        
        // CRITICAL: Sequence generation AND encryption MUST happen inside the targetMutex.
        // Otherwise, concurrent threads (Audio, Heartbeat) will interleave sequence numbers 
        // into the TCP stream, causing the receiver to drop packets as false-positive Replay Attacks.
        PacketHeader h = { PACKET_MAGIC, type, static_cast<uint32_t>(payloadSize), ++clientTxSequence };
        std::vector<uint8_t> buffer;

        if (type != MessageType::EVENT_AUTH) {
            std::vector<uint8_t> ciphertext;
            h.payloadSize = static_cast<uint32_t>(payloadSize + 28); // 12-byte IV + 16-byte Tag overhead
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
        
        if (!buffer.empty()) {
            int res = send(targetSock, (const char*)buffer.data(), (int)buffer.size(), 0);
            return res != SOCKET_ERROR;
        }
        return false;
    }

    /**
     * @brief Specially sends a Keep-Alive heartbeat on the specified socket to prevent SO_RCVTIMEO disconnects.
     */
    void SendHeartbeatToSocket(SOCKET sock, std::mutex& sockMutex) {
        if (sock == INVALID_SOCKET) return;
        
        std::unique_lock<std::mutex> lock(sockMutex, std::try_to_lock);
        if (!lock.owns_lock()) return; // Socket is actively streaming data, heartbeat is unnecessary.
        
        PacketHeader h = { PACKET_MAGIC, MessageType::EVENT_HEARTBEAT, 0, ++clientTxSequence };
        std::vector<uint8_t> buffer;
        std::vector<uint8_t> ciphertext;
        
        h.payloadSize = 28; // 12-byte IV + 16-byte Tag overhead for a 0-byte payload
        if (Security::EncryptPayload(nullptr, 0, &h, sizeof(h), ciphertext)) {
            buffer.resize(sizeof(h) + ciphertext.size());
            memcpy(buffer.data(), &h, sizeof(h));
            memcpy(buffer.data() + sizeof(h), ciphertext.data(), ciphertext.size());
            
            send(sock, (const char*)buffer.data(), (int)buffer.size(), 0);
        }
    }

    /**
     * @brief Uses Win32 SendInput to synthesize mouse actions natively.
     * @param p The MousePayload containing deltas and flags.
     * @return void
     */
    void InjectMouseInput(const MousePayload& p) {
        INPUT i = { 0 }; i.type = INPUT_MOUSE;
        i.mi.dx = p.deltaX; i.mi.dy = p.deltaY;
        i.mi.mouseData = p.mouseData; i.mi.dwFlags = p.flags;
        SendInput(1, &i, sizeof(INPUT));
    }

    /**
     * @brief Uses Win32 SendInput to synthesize keyboard actions natively.
     * @param p The KeyboardPayload containing Virtual Key Codes and state flags.
     * @return void
     */
    void InjectKeyboardInput(const KeyboardPayload& p) {
        INPUT i = { 0 }; i.type = INPUT_KEYBOARD;
        i.ki.wVk = p.vkCode; i.ki.wScan = p.scanCode; i.ki.dwFlags = p.flags;
        SendInput(1, &i, sizeof(INPUT));
    }

    /**
     * @brief Checks if the active TCP Client is currently connected and processing data.
     * @return true if fully connected, false otherwise.
     */
    bool IsClientConnected() { return isClientRunning; }

    /**
     * @brief Returns a thread-safe copy of all currently active Servers on the local network.
     * @return A vector of DiscoveredServer structs.
     */
    std::vector<DiscoveredServer> GetDiscoveredServers() {
        std::lock_guard<std::mutex> lock(discoveryMutex);
        return discoveredServers;
    }

    /**
     * @brief Retrieves the audio format sent by the server upon connection.
     * @param outFormat The AudioFormat struct to populate.
     * @return true if the format has been received and is valid.
     */
    bool GetServerAudioFormat(Audio::AudioFormat& outFormat) {
        if (!g_hasServerAudioFormat) return false;
        outFormat = g_serverAudioFormat;
        return true;
    }

    /**
     * @brief Background loop that listens for Server Beacons and manages the network map.
     * @return void
     */
    void DiscoveryLoop() {
        udpListenerSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udpListenerSocket == INVALID_SOCKET) {
            if (State::globalDebugMode) UI::LogDebug("[Network Client] UDP Socket creation failed.");
            return;
        }
        
        int reuseAddr = 1;
        setsockopt(udpListenerSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseAddr, sizeof(reuseAddr));

        sockaddr_in localAddr;
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = htons(8081);
        localAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(udpListenerSocket, (sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
            if (State::globalDebugMode) UI::LogDebug("[Network Client] UDP Bind failed. Port 8081 may be blocked.");
        }

        DWORD timeout = 500;
        setsockopt(udpListenerSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

        if (State::globalDebugMode) UI::LogDebug("[Network Client] Listening for UDP Beacons on port 8081...");

        while (isDiscoveryRunning) {
            char buffer[1024];
            sockaddr_in senderAddr;
            int senderAddrSize = sizeof(senderAddr);
            
            int bytes = recvfrom(udpListenerSocket, buffer, sizeof(buffer), 0, (sockaddr*)&senderAddr, &senderAddrSize);
            
            if (bytes > 0) {
                if (bytes == (sizeof(PacketHeader) + sizeof(UDPBeaconPayload))) {
                    PacketHeader* header = (PacketHeader*)buffer;
                    if (header->magic == PACKET_MAGIC && header->type == MessageType::EVENT_UDP_BEACON) {
                        UDPBeaconPayload* payload = (UDPBeaconPayload*)(buffer + sizeof(PacketHeader));
                    
                    char ipStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &(senderAddr.sin_addr), ipStr, INET_ADDRSTRLEN);

                        char safeServerName[33] = {0};
                        memcpy(safeServerName, payload->serverName, 32);

                    std::lock_guard<std::mutex> lock(discoveryMutex);
                    bool found = false;
                    uint64_t currentTick = GetTickCount64();
                    
                    for (auto& srv : discoveredServers) {
                        if (srv.ip == ipStr) {
                            srv.lastUpdateTick = currentTick; 
                            srv.tcpPort = payload->tcpPort;
                                    srv.hostname = safeServerName;
                            found = true;
                            break;
                        }
                    }
                        if (!found) {
                                discoveredServers.push_back({ipStr, safeServerName, payload->tcpPort, currentTick});
                                if (State::globalDebugMode) UI::LogDebug("[Network Client] Discovered Server: %s at %s", safeServerName, ipStr);
                        }
                    }
                } else {
                    if (State::globalDebugMode) UI::LogDebug("[Network Client] UDP RX Size Mismatch. Got %d bytes.", bytes);
                }
            }

            {
                std::lock_guard<std::mutex> lock(discoveryMutex);
                uint64_t currentTick = GetTickCount64();
                discoveredServers.erase(std::remove_if(discoveredServers.begin(), discoveredServers.end(), 
                    [currentTick](const DiscoveredServer& srv) {
                        return (currentTick - srv.lastUpdateTick) > 5000;
                    }), discoveredServers.end());
            }
        }
        closesocket(udpListenerSocket);
    }

    /**
     * @brief Initializes the WinSock2 UDP listener thread to map local Servers.
     * @return void
     */
    void StartDiscoveryListener() {
        if (isDiscoveryRunning) return;
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
        isDiscoveryRunning = true;
        discoveryThread = std::thread([]() {
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            DiscoveryLoop();
        });
    }

    /**
     * @brief Stops the UDP listener thread cleanly.
     * @return void
     */
    void StopDiscoveryListener() {
        isDiscoveryRunning = false;
        if (discoveryThread.joinable()) discoveryThread.join();
        std::lock_guard<std::mutex> lock(discoveryMutex);
        discoveredServers.clear();
        WSACleanup();
    }

    /**
     * @brief Dedicated thread for handling the audio OOB socket.
     * @return void
     */
    void ClientAudioLoop() {
        uint32_t rxSequence = 0;
        
        thread_local std::vector<uint8_t> tls_rawPayload;
        thread_local std::vector<uint8_t> tls_decrypted;
        tls_rawPayload.reserve(MAX_PAYLOAD_SIZE);
        tls_decrypted.reserve(MAX_PAYLOAD_SIZE);

        while (isClientRunning && clientAudioSocket != INVALID_SOCKET) {
            PacketHeader h;
            int bytes = recv(clientAudioSocket, (char*)&h, sizeof(h), MSG_WAITALL);
            if (bytes <= 0) break;

            if (h.magic == PACKET_MAGIC) {
                if (h.payloadSize > MAX_PAYLOAD_SIZE) break;

                tls_rawPayload.resize(h.payloadSize);
                if (h.payloadSize > 0) {
                    int pBytes = recv(clientAudioSocket, (char*)tls_rawPayload.data(), h.payloadSize, MSG_WAITALL);
                    if (pBytes != static_cast<int>(h.payloadSize)) break;
                }

                if (h.sequenceNumber <= rxSequence && h.sequenceNumber != 0) continue;
                rxSequence = h.sequenceNumber;

                const uint8_t* finalPayload = tls_rawPayload.data();
                size_t finalSize = tls_rawPayload.size();

                bool isEncrypted = (h.type != MessageType::EVENT_AUTH_AUDIO);
                if (isEncrypted) {
                    if (!Security::DecryptPayload(tls_rawPayload.data(), tls_rawPayload.size(), &h, sizeof(h), tls_decrypted)) continue;
                    finalPayload = tls_decrypted.data();
                    finalSize = tls_decrypted.size();
                }

                if (h.type == MessageType::EVENT_HEARTBEAT) continue;

                if (h.type == MessageType::EVENT_MIC_DATA) {
                    if (finalSize > 0) {
                        Audio::HandleMicData(finalPayload, finalSize);
                    }
                }
            }
        }
    }

    /**
     * @brief Background loop for the Client that continually blocks on recv() parsing incoming network packets.
     * @return void
     */
    void ClientLoop() {
        int pullBackAccumulator = 0;
        const int PULL_BACK_THRESHOLD = 150; 
        uint32_t rxSequence = 0;

        thread_local std::vector<uint8_t> tls_rawPayload;
        thread_local std::vector<uint8_t> tls_decrypted;
        tls_rawPayload.reserve(MAX_PAYLOAD_SIZE);
        tls_decrypted.reserve(MAX_PAYLOAD_SIZE);

        if (State::globalDebugMode) UI::LogDebug("[Network Client] TCP Listener Thread active.");

        while (isClientRunning) {
            PacketHeader h;
            int bytes = recv(clientSocket, (char*)&h, sizeof(h), MSG_WAITALL);
            
            if (bytes <= 0) {
                int wsaErr = WSAGetLastError();
                if (WSAGetLastError() == WSAETIMEDOUT) {
                    State::SetClientStatus("Disconnected (Timeout).");
                    if (State::globalDebugMode) UI::LogDebug("[Network Client] Connection timed out (No Heartbeat received). WSA Error: %d", wsaErr);
                } else {
                    State::SetClientStatus("Disconnected (Connection Severed).");
                    if (State::globalDebugMode) UI::LogDebug("[Network Client] Connection severed. WSA Error: %d", wsaErr);
                }
                break;
            }
            
            if (h.magic != PACKET_MAGIC) { 
                State::SetClientStatus("Disconnected (Packet Desync).");
                if (State::globalDebugMode) UI::LogDebug("[Network Client] FATAL: Packet desync detected!"); 
                break; 
            }

            // Enforce payload size limits to prevent memory allocation attacks (CWE-400).
            if (h.payloadSize > MAX_PAYLOAD_SIZE) {
                State::SetClientStatus("Disconnected (Payload Size Violation).");
                if (State::globalDebugMode) UI::LogDebug("[Network Client] CRITICAL: Payload size exceeds safety bounds! Dropping connection.");
                break;
            }

            tls_rawPayload.resize(h.payloadSize);
            if (h.payloadSize > 0) {
                // Ensure full payload is received to avoid processing corrupted state data.
                int pBytes = recv(clientSocket, (char*)tls_rawPayload.data(), h.payloadSize, MSG_WAITALL);
                if (pBytes != static_cast<int>(h.payloadSize)) {
                    if (State::globalDebugMode) UI::LogDebug("[Network Client] WARNING: Partial payload received. (Got %d of %u bytes, Error: %d). Dropping connection.", pBytes, h.payloadSize, WSAGetLastError());
                    break; 
                }
            }

            if (h.sequenceNumber <= rxSequence && h.sequenceNumber != 0) {
                if (State::globalDebugMode) UI::LogDebug("[Security] Replay attack detected. Dropping packet.");
                continue; // Safe to continue since the raw payload was already cleared from the socket buffer.
            }
            rxSequence = h.sequenceNumber;

            const uint8_t* finalPayload = tls_rawPayload.data();
            size_t finalSize = tls_rawPayload.size();

            bool isEncrypted = (h.type != MessageType::EVENT_AUTH) && (h.type != MessageType::EVENT_UDP_BEACON);

            if (isEncrypted) {
                if (!Security::DecryptPayload(tls_rawPayload.data(), tls_rawPayload.size(), &h, sizeof(h), tls_decrypted)) {
                    if (State::globalDebugMode) UI::LogDebug("[Security] Failed to decrypt server packet. Dropping.");
                    continue; 
                }
                finalPayload = tls_decrypted.data();
                finalSize = tls_decrypted.size();
                
                if (State::globalDebugMode && h.type != MessageType::EVENT_MOUSE) {
                    UI::LogDebug("[Network Client] RX Decrypted -> Type: %d | Size: %zu b", (int)h.type, finalSize);
                }
            }

            if (h.type == MessageType::EVENT_HEARTBEAT) {
                static int rxHbCount = 0;
                if (++rxHbCount % 10 == 0 && State::globalDebugMode) {
                    UI::LogDebug("[Network Client] Received Keep-Alive Heartbeat from Server.");
                }
                continue; // Keep-alive heartbeat acknowledged.
            }

            if (h.type == MessageType::EVENT_MOUSE) {
                if (finalSize == sizeof(MousePayload)) {
                    MousePayload p; memcpy(&p, finalPayload, sizeof(p));
                    InjectMouseInput(p);

                    POINT pt; GetCursorPos(&pt);
                    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

                    int deadzoneX = (sw * State::edgeDeadzonePercent) / 100;
                    int deadzoneY = (sh * State::edgeDeadzonePercent) / 100;

                    bool safeY = pt.y > deadzoneY && pt.y < (sh - deadzoneY);
                    bool safeX = pt.x > deadzoneX && pt.x < (sw - deadzoneX);

                    uint8_t exitEdge = 255;
                    float normX = 0.0f, normY = 0.0f;

                    // Calculate the normalized exit coordinates based on which screen edge was breached.
                    if (pt.x <= 0 && p.deltaX < 0 && safeY) { exitEdge = 0; normX = 0.0f; normY = (float)pt.y / sh; }
                    else if (pt.x >= sw - 1 && p.deltaX > 0 && safeY) { exitEdge = 1; normX = 1.0f; normY = (float)pt.y / sh; }
                    else if (pt.y <= 0 && p.deltaY < 0 && safeX) { exitEdge = 2; normX = (float)pt.x / sw; normY = 0.0f; }
                    else if (pt.y >= sh - 1 && p.deltaY > 0 && safeX) { exitEdge = 3; normX = (float)pt.x / sw; normY = 1.0f; }

                    if (exitEdge != 255) {
                        pullBackAccumulator += (exitEdge <= 1) ? abs(p.deltaX) : abs(p.deltaY);
                        if (pullBackAccumulator > PULL_BACK_THRESHOLD) {
                            ReturnControlPayload retPayload = { exitEdge, normX, normY };
                            SendToServer(MessageType::EVENT_RETURN_CONTROL, &retPayload, sizeof(retPayload));
                            pullBackAccumulator = 0;
                        }
                    } else {
                        pullBackAccumulator = 0;
                    }
                }
            } 
            else if (h.type == MessageType::EVENT_KEYBOARD) {
                if (finalSize == sizeof(KeyboardPayload)) {
                    KeyboardPayload p; memcpy(&p, finalPayload, sizeof(p));
                    if (State::enableKeyboardSync) {
                        InjectKeyboardInput(p);
                    }
                }
            } 
            else if (h.type == MessageType::EVENT_SYNC_CURSOR) {
                if (finalSize == sizeof(CursorSyncPayload)) {
                    CursorSyncPayload p; memcpy(&p, finalPayload, sizeof(p));
                    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
                    
                    int ty = std::clamp((int)(sh * p.normalizedY), 10, sh - 10);
                    int tx = std::clamp((int)(sw * p.normalizedX), 15, sw - 15);
                    if (p.entryEdge == 0) tx = 15;             // Left
                    else if (p.entryEdge == 1) tx = sw - 15;   // Right
                    else if (p.entryEdge == 2) ty = 15;        // Top
                    else if (p.entryEdge == 3) ty = sh - 15;   // Bottom
                    
                    SetCursorPos(tx, ty);
                    pullBackAccumulator = 0; 
                }
            } 
            else if (h.type == MessageType::EVENT_STATE) {
                if (finalSize == sizeof(StatePayload)) {
                    StatePayload p; memcpy(&p, finalPayload, sizeof(p));
                    if (p.mode == 0) { // 0 represents ControlMode::LOCAL
                        ReleaseStuckModifiers();
                    }
                }
            }
            else if (h.type == MessageType::EVENT_AUDIO_FORMAT) {
                if (finalSize == sizeof(AudioFormatPayload)) {
                    memcpy(&g_serverAudioFormat, finalPayload, sizeof(AudioFormatPayload));
                    g_hasServerAudioFormat = true;
                    if (State::globalDebugMode) UI::LogDebug("[Audio] Received Server Audio Format: %uHz, %u-bit, %uch", g_serverAudioFormat.sampleRate, g_serverAudioFormat.bitDepth, g_serverAudioFormat.channels);

                    // Now that we have the format, start capturing if the user has it enabled.
                    if (State::enableClientAudioStream) {
                        Audio::StartLoopbackCapture();
                    }
                    if (State::enableClientMicReceive) {
                        Audio::StartMicReceiver();
                    }
                }
            }
            else if (h.type == MessageType::EVENT_CLIPBOARD) {
                if (finalSize > 0) {
                    // Convert byte stream back to a wide string and set it locally.
                    std::wstring text((wchar_t*)finalPayload, finalSize / sizeof(wchar_t));
                    ClipboardManager::SetRemoteClipboard(text);
                }
            }
            else if (h.type == MessageType::EVENT_MIC_DATA) {
                // Handled in ClientAudioLoop, should not arrive here but ignored safely if it does.
            }
            else if (h.type == MessageType::EVENT_FILE_OFFER) {
                if (finalSize == sizeof(FileOfferPayload)) {
                    FileOfferPayload p; memcpy(&p, finalPayload, sizeof(p));
                    FileTransfer::HandleFileOffer(clientSocket, p);
                }
            }
            else if (h.type == MessageType::EVENT_FILE_ACCEPT) {
                if (finalSize == sizeof(FileAcceptPayload)) {
                    FileAcceptPayload p; memcpy(&p, finalPayload, sizeof(p));
                    FileTransfer::HandleFileAccept(clientSocket, p);
                }
            }
            else if (h.type == MessageType::EVENT_FILE_DECLINE) {
                if (finalSize == sizeof(FileDeclinePayload)) {
                    FileDeclinePayload p; memcpy(&p, finalPayload, sizeof(p));
                    FileTransfer::HandleFileDecline(clientSocket, p);
                }
            }
        }
        isClientRunning = false;
        ReleaseStuckModifiers(); // Catch all hard disconnects or timeouts
        if (State::globalDebugMode) UI::LogDebug("[Network Client] Listener Thread exiting.");

        uint64_t sessionDuration = GetTickCount64() - connectionStartTime;

        if (!isIntentionalDisconnect && State::enableClientAutoReconnect && !isAutoReconnecting) {
            if (isCurrentSessionAutoReconnected && sessionDuration < 10000) {
                State::SetClientStatus("Auto-reconnect failed. Server unreachable or rejected connection.");
            } else {
                isAutoReconnecting = true;
                State::SetClientStatus("Connection dropped. Auto-reconnecting in 5s...");
                if (State::globalDebugMode) UI::LogDebug("[Network Client] Connection dropped unintentionally. Preparing to auto-reconnect...");
                
                if (autoReconnectThread.joinable()) {
                    autoReconnectThread.join();
                }
                
                autoReconnectThread = std::thread([]() {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    if (!isIntentionalDisconnect && State::enableClientAutoReconnect) {
                        if (State::globalDebugMode) UI::LogDebug("[Network Client] Attempting auto-reconnect to %s:%u...", lastConnectedIp.c_str(), lastConnectedPort);
                        if (!StartClient(lastConnectedIp, lastConnectedPort, true)) {
                            State::SetClientStatus("Auto-reconnect failed. Server unreachable.");
                        }
                    }
                    isAutoReconnecting = false;
                });
            }
        }
    }

    /**
     * @brief Initializes WinSock2 and connects to the Server's IP and port.
     * @param ip The IPv4 address of the Server PC.
     * @param port The TCP port the Server is listening on.
     * @return true if connected successfully, false otherwise.
     */
    bool StartClient(const std::string& ip, uint16_t port, bool isAutoReconnect) {
        if (isClientRunning) return false;
        g_hasServerAudioFormat = false; // Reset audio format state for the new connection.

        isIntentionalDisconnect = false;
        lastConnectedIp = ip;
        lastConnectedPort = port;
        isCurrentSessionAutoReconnected = isAutoReconnect;
        connectionStartTime = GetTickCount64();
        
        State::SetClientStatus("Connecting...");

        clientTxSequence = 0; // Reset monotonic counter for new connection

        if (clientThread.joinable()) {
            if (State::globalDebugMode) UI::LogDebug("[Network Client] Reaping zombie thread from previous connection attempt...");
            clientThread.join();
        }
        if (clientHeartbeatThread.joinable()) {
            clientHeartbeatThread.join();
        }
        if (clientAudioThread.joinable()) {
            clientAudioThread.join();
        }

        if (clientSocket != INVALID_SOCKET) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
        }
        if (clientAudioSocket != INVALID_SOCKET) {
            closesocket(clientAudioSocket);
            clientAudioSocket = INVALID_SOCKET;
        }

        if (State::globalDebugMode) UI::LogDebug("[Network Client] Attempting TCP Connection to %s:%u...", ip.c_str(), port);

        std::string targetPin;
        std::string targetName = "KAMFlow_Server_" + ip;
        if (!CredentialManager::LoadSecret(targetName, targetPin)) {
            State::SetClientStatus("Authentication Failed (No PIN).");
            if (State::globalDebugMode) UI::LogDebug("[Security] No paired PIN found for %s. Please pair via UI.", ip.c_str());
            return false;
        }

        if (!Security::Initialize(targetPin.c_str())) {
            State::SetClientStatus("Security Initialization Failed.");
            if (State::globalDebugMode) UI::LogDebug("[Security] FATAL: Cryptography Engine failed to initialize.");
            return false;
        }

        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

        clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSocket == INVALID_SOCKET) return false;
        
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        if (connect(clientSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            State::SetClientStatus("Connection Failed. Server is offline or blocked.");
            if (State::globalDebugMode) UI::LogDebug("[Network Client] Connection Failed. Server is offline or blocked by firewall.");
            closesocket(clientSocket);
            return false;
        }

        int flag = 1; setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
        
        int tos = 0xB8; // DSCP 46 (Expedited Forwarding) for Voice/Real-Time priority
        setsockopt(clientSocket, IPPROTO_IP, IP_TOS, (char*)&tos, sizeof(tos));

        DWORD timeout = 5000;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        DWORD sndTimeout = 5000;
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

        isClientRunning = true; 
        AuthPayload p; 
        memset(&p, 0, sizeof(p)); 
        strncpy_s(p.pin, sizeof(p.pin), targetPin.c_str(), _TRUNCATE);
        
        char hostname[32] = "KAM-Flow-Client";
        DWORD size = sizeof(hostname);
        GetComputerNameA(hostname, &size);
        strncpy_s(p.clientName, sizeof(p.clientName), hostname, _TRUNCATE);

        if (State::globalDebugMode) UI::LogDebug("[Network Client] Handshake Sent. AES-GCM Active.");
        
        State::SetClientStatus("Authenticating...");
        SendToServer(MessageType::EVENT_AUTH, &p, sizeof(p));
        
        // Immediately connect the Audio OOB socket
        clientAudioSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientAudioSocket != INVALID_SOCKET) {
            if (connect(clientAudioSocket, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR) {
                setsockopt(clientAudioSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
                setsockopt(clientAudioSocket, IPPROTO_IP, IP_TOS, (char*)&tos, sizeof(tos));
                setsockopt(clientAudioSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
                setsockopt(clientAudioSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));
                
                int audioBufSize = 65536;
                setsockopt(clientAudioSocket, SOL_SOCKET, SO_RCVBUF, (char*)&audioBufSize, sizeof(audioBufSize));
                setsockopt(clientAudioSocket, SOL_SOCKET, SO_SNDBUF, (char*)&audioBufSize, sizeof(audioBufSize));

                // We send manually because SendToServer doesn't know about clientAudioSocket yet
                PacketHeader authH = { PACKET_MAGIC, MessageType::EVENT_AUTH_AUDIO, static_cast<uint32_t>(sizeof(p)), 0 };
                std::vector<uint8_t> authBuffer(sizeof(authH) + sizeof(p));
                memcpy(authBuffer.data(), &authH, sizeof(authH));
                memcpy(authBuffer.data() + sizeof(authH), &p, sizeof(p));
                send(clientAudioSocket, (const char*)authBuffer.data(), (int)authBuffer.size(), 0);
                
                clientAudioThread = std::thread([]() {
                    // Register with MMCSS to guarantee CPU scheduling for audio receive from server
                    DWORD taskIndex = 0;
                    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
                    if (!hMmcss) {
                        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                    } else {
                        if (State::globalDebugMode) UI::LogDebug("[Network Client] AudioLoop: MMCSS registered (index=%u).", taskIndex);
                    }
                    ClientAudioLoop();
                    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                });
            } else {
                closesocket(clientAudioSocket);
                clientAudioSocket = INVALID_SOCKET;
            }
        }

        State::SetClientStatus("Connected");

        clientThread = std::thread([]() {
            // Elevate thread priority to prevent starvation from the OS scheduler during heavy workloads.
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            ClientLoop();
        });
        clientHeartbeatThread = std::thread([]() {
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            bool wasLocked = false;
            while (isClientRunning) {
                for (int i = 0; i < 20 && isClientRunning; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (isClientRunning) {
                    SendHeartbeatToSocket(clientSocket, clientSendMutex);
                    if (clientAudioSocket != INVALID_SOCKET) {
                        SendHeartbeatToSocket(clientAudioSocket, clientAudioSendMutex);
                    }
                    static int txHbCount = 0;
                    if (++txHbCount % 10 == 0 && State::globalDebugMode) {
                        UI::LogDebug("[Network Client] Sent Keep-Alive Heartbeat to Server.");
                    }
                    
                    // Secure desktop and UAC prompt failsafe.
                    bool isLocked = false;
                    HDESK hDesk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
                    if (hDesk) {
                        char name[256] = {0};
                        DWORD needed = 0;
                        if (GetUserObjectInformationA(hDesk, UOI_NAME, name, sizeof(name), &needed)) {
                            if (_stricmp(name, "Default") != 0) isLocked = true; // Check if a lock screen or Winlogon prompt is active.
                        }
                        CloseDesktop(hDesk);
                    } else {
                        isLocked = true; // Access denied indicates a UAC Secure Desktop prompt is active.
                    }

                    if (isLocked && !wasLocked) {
                        if (State::globalDebugMode) UI::LogDebug("[Network Client] UAC/Secure Desktop detected. Triggering Server Auto-Revert.");
                        SendToServer(MessageType::EVENT_CLIENT_LOCKED, nullptr, 0);
                    }
                    wasLocked = isLocked;
                }
            }
        });
        return true;
    }

    /**
     * @brief Safely disconnects from the Server and shuts down the TCP socket.
     * @return void
     */
    void StopClient() {
        isIntentionalDisconnect = true;
        State::SetClientStatus("Idle");
        isClientRunning = false;
        // Ensure audio capture streams are always stopped on disconnect.
        Audio::StopLoopbackCapture();
        Audio::StopMicReceiver();

        if (clientSocket != INVALID_SOCKET) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
        }
        if (clientAudioSocket != INVALID_SOCKET) {
            closesocket(clientAudioSocket);
            clientAudioSocket = INVALID_SOCKET;
        }
        if (clientThread.joinable()) clientThread.join();
        if (clientAudioThread.joinable()) clientAudioThread.join();
        if (clientHeartbeatThread.joinable()) clientHeartbeatThread.join();
        if (autoReconnectThread.joinable()) autoReconnectThread.join();
        Security::Shutdown();
        WSACleanup();
        if (State::globalDebugMode) UI::LogDebug("[Network Client] Client successfully stopped.");
    }
}