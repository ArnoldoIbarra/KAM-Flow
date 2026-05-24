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
#include <iostream>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <cstring> 
#include <algorithm>
#include <vector>
#include <mutex>

namespace Network {

    /// Active socket connected to the Server.
    SOCKET clientSocket = INVALID_SOCKET;
    /// Background thread processing incoming TCP packets.
    std::thread clientThread;
    /// Control flag for the TCP Client lifecycle.
    std::atomic<bool> isClientRunning(false);

    /// Global transmit sequence counter.
    std::atomic<uint32_t> clientTxSequence(0);

    /// Mutex to prevent interleaved sends from KVM hooks vs heartbeat threads.
    std::mutex clientSendMutex;
    /// Background thread for keeping the connection alive.
    std::thread clientHeartbeatThread;

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
    std::atomic<bool> isIntentionalDisconnect(false);
    std::atomic<bool> isAutoReconnecting(false);
    std::atomic<bool> isCurrentSessionAutoReconnected(false);
    uint64_t connectionStartTime = 0;

    /**
     * @brief Packages a payload with a header, optionally encrypts it, and sends it to the Server.
     * @param type The MessageType identifier.
     * @param payload Pointer to the raw payload structure.
     * @param payloadSize Size of the payload structure.
     * @return true if sent successfully.
     */
    bool SendToServer(MessageType type, const void* payload, size_t payloadSize) {
        std::lock_guard<std::mutex> lock(clientSendMutex);
        
        if (clientSocket == INVALID_SOCKET || !isClientRunning) return false;

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

        return send(clientSocket, (const char*)buffer.data(), (int)buffer.size(), 0) != SOCKET_ERROR;
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
            if (State::globalDebugMode) std::cerr << "[Network Client] UDP Socket creation failed.\n";
            return;
        }
        
        int reuseAddr = 1;
        setsockopt(udpListenerSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseAddr, sizeof(reuseAddr));

        sockaddr_in localAddr;
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = htons(8081);
        localAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(udpListenerSocket, (sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
            if (State::globalDebugMode) std::cerr << "[Network Client] UDP Bind failed. Port 8081 may be blocked.\n";
        }

        DWORD timeout = 500;
        setsockopt(udpListenerSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

        if (State::globalDebugMode) std::cout << "[Network Client] Listening for UDP Beacons on port 8081...\n";

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
                                if (State::globalDebugMode) std::cout << "[Network Client] Discovered Server: " << safeServerName << " at " << ipStr << "\n";
                        }
                    }
                } else {
                    if (State::globalDebugMode) std::cerr << "[Network Client] UDP RX Size Mismatch. Got " << bytes << " bytes.\n";
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
    }

    /**
     * @brief Background loop that handles receiving, decrypting, and dispatching payloads.
     * @return void
     */
    void ClientLoop() {
        int pullBackAccumulator = 0;
        const int PULL_BACK_THRESHOLD = 150; 
        uint32_t rxSequence = 0;

        if (State::globalDebugMode) std::cout << "[Network Client] TCP Listener Thread active.\n";

        while (isClientRunning) {
            PacketHeader h;
            int bytes = recv(clientSocket, (char*)&h, sizeof(h), MSG_WAITALL);
            
            if (bytes <= 0) {
                if (WSAGetLastError() == WSAETIMEDOUT) {
                    State::SetClientStatus("Disconnected (Timeout).");
                    if (State::globalDebugMode) std::cout << "[Network Client] Connection timed out (No Heartbeat).\n";
                } else {
                    State::SetClientStatus("Disconnected (Connection Severed).");
                    if (State::globalDebugMode) std::cout << "[Network Client] Connection severed by remote host.\n";
                }
                break;
            }
            
            if (h.magic != PACKET_MAGIC) { 
                State::SetClientStatus("Disconnected (Packet Desync).");
                if (State::globalDebugMode) std::cerr << "[Network Client] FATAL: Packet desync detected!\n"; 
                break; 
            }

            // Enforce payload size limits to prevent memory allocation attacks (CWE-400).
            if (h.payloadSize > MAX_PAYLOAD_SIZE) {
                State::SetClientStatus("Disconnected (Payload Size Violation).");
                if (State::globalDebugMode) std::cerr << "[Network Client] CRITICAL: Payload size exceeds safety bounds! Dropping connection.\n";
                break;
            }

            std::vector<uint8_t> rawPayload(h.payloadSize);
            if (h.payloadSize > 0) {
                // Ensure full payload is received to avoid processing corrupted state data.
                int pBytes = recv(clientSocket, (char*)rawPayload.data(), h.payloadSize, MSG_WAITALL);
                if (pBytes != static_cast<int>(h.payloadSize)) {
                    if (State::globalDebugMode) std::cerr << "[Network Client] WARNING: Partial payload received. Dropping packet.\n";
                    break; 
                }
            }

            if (h.sequenceNumber <= rxSequence && h.sequenceNumber != 0) {
                if (State::globalDebugMode) std::cerr << "[Security] Replay attack detected. Dropping packet.\n";
                continue; // Safe to continue since the raw payload was already cleared from the socket buffer.
            }
            rxSequence = h.sequenceNumber;

            std::vector<uint8_t> decrypted;
            const uint8_t* finalPayload = rawPayload.data();
            size_t finalSize = rawPayload.size();

            bool isEncrypted = (h.type != MessageType::EVENT_AUTH) && (h.type != MessageType::EVENT_UDP_BEACON);

            if (isEncrypted) {
                if (!Security::DecryptPayload(rawPayload.data(), rawPayload.size(), &h, sizeof(h), decrypted)) {
                    if (State::globalDebugMode) std::cerr << "[Security] Failed to decrypt server packet. Dropping.\n";
                    continue; 
                }
                finalPayload = decrypted.data();
                finalSize = decrypted.size();
                
                if (State::globalDebugMode && h.type != MessageType::EVENT_MOUSE) {
                    std::cout << "[Network Client] RX Decrypted -> Type: " << (int)h.type << " | Size: " << finalSize << "b\n";
                }
            }

            if (h.type == MessageType::EVENT_HEARTBEAT) {
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
                }
            }
            else if (h.type == MessageType::EVENT_AUDIO_FORMAT) {
                if (finalSize == sizeof(AudioFormatPayload)) {
                    memcpy(&g_serverAudioFormat, finalPayload, sizeof(AudioFormatPayload));
                    g_hasServerAudioFormat = true;
                    if (State::globalDebugMode) std::cout << "[Audio] Received Server Audio Format: " << g_serverAudioFormat.sampleRate << "Hz, " << g_serverAudioFormat.bitDepth << "-bit, " << g_serverAudioFormat.channels << "ch\n";

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
                if (finalSize > 0) {
                    Audio::HandleMicData(finalPayload, finalSize);
                }
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
        if (State::globalDebugMode) std::cout << "[Network Client] Listener Thread exiting.\n";

        uint64_t sessionDuration = GetTickCount64() - connectionStartTime;

        if (!isIntentionalDisconnect && State::enableClientAutoReconnect && !isAutoReconnecting) {
            if (isCurrentSessionAutoReconnected && sessionDuration < 10000) {
                State::SetClientStatus("Auto-reconnect failed. Server unreachable or rejected connection.");
            } else {
                isAutoReconnecting = true;
                State::SetClientStatus("Connection dropped. Auto-reconnecting in 5s...");
                if (State::globalDebugMode) std::cout << "[Network Client] Connection dropped unintentionally. Preparing to auto-reconnect...\n";
                
                std::thread([]() {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    if (!isIntentionalDisconnect && State::enableClientAutoReconnect) {
                        if (State::globalDebugMode) std::cout << "[Network Client] Attempting auto-reconnect to " << lastConnectedIp << ":" << lastConnectedPort << "...\n";
                        if (!StartClient(lastConnectedIp, lastConnectedPort, true)) {
                            State::SetClientStatus("Auto-reconnect failed. Server unreachable.");
                        }
                    }
                    isAutoReconnecting = false;
                }).detach();
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
            if (State::globalDebugMode) std::cout << "[Network Client] Reaping zombie thread from previous connection attempt...\n";
            clientThread.join();
        }
        if (clientHeartbeatThread.joinable()) {
            clientHeartbeatThread.join();
        }

        if (clientSocket != INVALID_SOCKET) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
        }

        if (State::globalDebugMode) std::cout << "[Network Client] Attempting TCP Connection to " << ip << ":" << port << "...\n";

        std::string targetPin;
        std::string targetName = "KAMFlow_Server_" + ip;
        if (!CredentialManager::LoadSecret(targetName, targetPin)) {
            State::SetClientStatus("Authentication Failed (No PIN).");
            if (State::globalDebugMode) std::cerr << "[Security] No paired PIN found for " << ip << ". Please pair via UI.\n";
            return false;
        }

        if (!Security::Initialize(targetPin.c_str())) {
            State::SetClientStatus("Security Initialization Failed.");
            if (State::globalDebugMode) std::cerr << "[Security] FATAL: Cryptography Engine failed to initialize.\n";
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
            if (State::globalDebugMode) std::cerr << "[Network Client] Connection Failed. Server is offline or blocked by firewall.\n";
            closesocket(clientSocket);
            return false;
        }

        int flag = 1; setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

        DWORD timeout = 5000;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        DWORD sndTimeout = 1000;
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

        isClientRunning = true; 
        AuthPayload p; 
        memset(&p, 0, sizeof(p)); 
        strncpy_s(p.pin, sizeof(p.pin), targetPin.c_str(), _TRUNCATE);
        
        char hostname[32] = "KAM-Flow-Client";
        DWORD size = sizeof(hostname);
        GetComputerNameA(hostname, &size);
        strncpy_s(p.clientName, sizeof(p.clientName), hostname, _TRUNCATE);

        if (State::globalDebugMode) std::cout << "[Network Client] Handshake Sent. AES-GCM Active.\n";
        
        State::SetClientStatus("Authenticating...");
        SendToServer(MessageType::EVENT_AUTH, &p, sizeof(p));
        
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
                // Sleep in short intervals to allow instant thread teardown on shutdown.
                for (int i = 0; i < 20 && isClientRunning; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (isClientRunning) {
                    SendToServer(MessageType::EVENT_HEARTBEAT, nullptr, 0);
                    
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
                        if (State::globalDebugMode) std::cout << "[Network Client] UAC/Secure Desktop detected. Triggering Server Auto-Revert.\n";
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
        if (clientThread.joinable()) clientThread.join();
        if (clientHeartbeatThread.joinable()) clientHeartbeatThread.join();
        Security::Shutdown();
        if (State::globalDebugMode) std::cout << "[Network Client] Client successfully stopped.\n";
    }
}