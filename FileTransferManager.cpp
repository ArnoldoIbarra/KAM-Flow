// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// File Transfer Manager Implementation
// =============================================================================

/**
 * @file FileTransferManager.cpp
 * @brief Implementation of the OOB File Transfer system.
 * Utilizes memory-safe chunking and AES-GCM cryptography for zero-latency KVM coexistence.
 */

#include "FileTransferManager.h"
#include "StateManager.h"
#include "NetworkServer.h"
#include "NetworkClient.h"
#include "SecurityManager.h"
#include "ConfigManager.h"
#include "CredentialManager.h"
#include "UIManager.h"
#include <iostream>
#include <fstream>
#include <mutex>
#include <map>
#include <algorithm>
#include <thread>
#include <ws2tcpip.h>

namespace FileTransfer {

    std::mutex g_mutex;
    std::vector<PendingTransfer> g_pendingOffers;
    uint32_t g_nextTransferId = 1000; // Monotonic counter for active sessions
    
    /**
     * @brief Context tracking the state and disk I/O target for an active transfer.
     */
    struct TransferContext {
        bool isSender;
        std::string filePath;
        uint64_t fileSize;
        std::string fileName;
        uint64_t transferredSize;
    };
    std::map<uint32_t, TransferContext> g_transfers;

    std::vector<std::shared_ptr<std::thread>> g_transferThreads;
    std::vector<SOCKET> g_activeSockets;
    std::atomic<bool> g_isTransferManagerRunning(true);

    void ExecuteTransferIO(SOCKET sock, uint32_t transferId);
    void OOB_ServerTask(uint32_t transferId, uint16_t port);
    void OOB_ClientTask(uint32_t transferId, uint16_t port);

#pragma pack(push, 1)
    /**
     * @brief Internal header sent before every file chunk on the OOB socket.
     * Ensures the Out-Of-Band stream remains authenticated and prevents chunk-reordering attacks.
     */
    struct FileChunkHeader {
        uint32_t magic;       ///< Must equal Network::PACKET_MAGIC
        uint32_t transferId;  ///< Matches the negotiated transfer ID
        uint32_t chunkSize;   ///< Size of the encrypted chunk payload
        bool isEOF;           ///< True if this is the final chunk to close the stream
    };
#pragma pack(pop)

    /**
     * @brief Thread-safe routine that reads/writes disk chunks over the authenticated temporary socket.
     * @param sock The connected OOB TCP socket.
     * @param transferId The unique identifier for this active transfer.
     * @return void
     */
    void ExecuteTransferIO(SOCKET sock, uint32_t transferId) {
        TransferContext ctx;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_transfers.find(transferId) == g_transfers.end()) return;
            ctx = g_transfers[transferId];
        }

        if (ctx.isSender) {
            std::ifstream file(ctx.filePath, std::ios::binary);
            if (!file.is_open()) {
                if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Failed to open source file.");
                return;
            }

            std::vector<uint8_t> buffer(65536); // Stream 64KB memory-safe chunks
            while (file) {
                file.read((char*)buffer.data(), buffer.size());
                size_t bytesRead = file.gcount();
                bool isEOF = file.eof() || file.peek() == EOF;

                if (bytesRead > 0 || isEOF) {
                    FileChunkHeader header = { Network::PACKET_MAGIC, transferId, 0, isEOF };
                    
                    std::vector<uint8_t> packet;
                    if (bytesRead > 0) {
                        header.chunkSize = (uint32_t)(bytesRead + 28); // Account for 12b IV + 16b Tag
                        std::vector<uint8_t> ciphertext;
                        if (!Security::EncryptPayload(buffer.data(), bytesRead, &header, sizeof(header), ciphertext)) break;
                        
                        packet.resize(sizeof(header) + ciphertext.size());
                        memcpy(packet.data(), &header, sizeof(header));
                        memcpy(packet.data() + sizeof(header), ciphertext.data(), ciphertext.size());
                    } else {
                        packet.resize(sizeof(header));
                        memcpy(packet.data(), &header, sizeof(header));
                    }

                    if (send(sock, (const char*)packet.data(), (int)packet.size(), 0) == SOCKET_ERROR) {
                        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Connection dropped during send.");
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        if (g_transfers.find(transferId) != g_transfers.end()) {
                            g_transfers[transferId].transferredSize += bytesRead;
                        } else {
                            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u aborted by user.", transferId);
                            break;
                        }
                    }
                }
                if (isEOF) break;
            }
            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] File sent completely.");
        } else {
            std::ofstream file(ctx.filePath, std::ios::binary);
            if (!file.is_open()) {
                if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Failed to open destination file.");
                return;
            }

            while (true) {
                FileChunkHeader header;
                int b = recv(sock, (char*)&header, sizeof(header), MSG_WAITALL);
                if (b <= 0 || header.magic != Network::PACKET_MAGIC || header.transferId != transferId) {
                    if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Error or connection closed during receive.");
                    break;
                }

                if (header.chunkSize > 0) {
                    // Enforce payload bounds to prevent OOM std::bad_alloc attacks (CWE-400)
                    if (header.chunkSize > Network::MAX_PAYLOAD_SIZE) {
                        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] CRITICAL: Chunk size exceeds safety bounds! Aborting.");
                        break;
                    }
                    
                    std::vector<uint8_t> ciphertext(header.chunkSize);
                    int cb = recv(sock, (char*)ciphertext.data(), (int)ciphertext.size(), MSG_WAITALL);
                    if (cb != (int)ciphertext.size()) {
                        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Partial chunk receive.");
                        break;
                    }

                    std::vector<uint8_t> plaintext;
                    if (Security::DecryptPayload(ciphertext.data(), ciphertext.size(), &header, sizeof(header), plaintext)) {
                        file.write((const char*)plaintext.data(), plaintext.size());
                        {
                            std::lock_guard<std::mutex> lock(g_mutex);
                            if (g_transfers.find(transferId) != g_transfers.end()) {
                                g_transfers[transferId].transferredSize += plaintext.size();
                            } else {
                                if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u aborted by user.", transferId);
                                break;
                            }
                        }
                    } else {
                        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Chunk decryption failed!");
                        break;
                    }
                }

                if (header.isEOF) {
                    if (State::globalDebugMode) UI::LogDebug("[FileTransfer] File received and saved completely.");
                    break;
                }
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_transfers.erase(transferId);
        }
    }

    /**
     * @brief Background thread task to host the OOB socket and wait for the client connection.
     * @param transferId The unique identifier for this transfer.
     * @param port The TCP port to host the OOB stream on.
     * @return void
     */
    void OOB_ServerTask(uint32_t transferId, uint16_t port) {
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) return;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_activeSockets.push_back(listenSock);
        }

        int opt = 1; setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in addr = {0};
        addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || listen(listenSock, 1) == SOCKET_ERROR) {
            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] OOB Bind/Listen failed on port %u", port);
            closesocket(listenSock); 
            return;
        }

        sockaddr_in clientAddr; int clientSize = sizeof(clientAddr);
        SOCKET oobSock = accept(listenSock, (sockaddr*)&clientAddr, &clientSize);
        closesocket(listenSock); // Listener no longer needed

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_activeSockets.erase(std::remove(g_activeSockets.begin(), g_activeSockets.end(), listenSock), g_activeSockets.end());
            if (oobSock != INVALID_SOCKET) {
                g_activeSockets.push_back(oobSock);
            }
        }

        if (oobSock != INVALID_SOCKET && g_isTransferManagerRunning) {
            int flag = 1; setsockopt(oobSock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
            DWORD timeout = 10000; // 10s idle teardown
            setsockopt(oobSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            setsockopt(oobSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
            ExecuteTransferIO(oobSock, transferId);
            closesocket(oobSock);

            std::lock_guard<std::mutex> lock(g_mutex);
            g_activeSockets.erase(std::remove(g_activeSockets.begin(), g_activeSockets.end(), oobSock), g_activeSockets.end());
        }
    }

    /**
     * @brief Background thread task to connect to the Server's OOB socket for transfer.
     * @param transferId The unique identifier for this transfer.
     * @param port The TCP port the Server is hosting the OOB stream on.
     * @return void
     */
    void OOB_ClientTask(uint32_t transferId, uint16_t port) {
        // Micro-delay ensures the Server thread has executed bind() before we connect
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        SOCKET oobSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (oobSock == INVALID_SOCKET) return;

        std::string serverIp = "127.0.0.1";
        auto targets = CredentialManager::GetSavedTargets("KAMFlow_Server_");
        if (!targets.empty()) {
            serverIp = targets[0].substr(15);
        }
        
        sockaddr_in addr = {0};
        addr.sin_family = AF_INET; addr.sin_port = htons(port); inet_pton(AF_INET, serverIp.c_str(), &addr.sin_addr);

        bool connected = false;
        for (int i = 0; i < 5; ++i) { // 2.5s cumulative retry
            if (connect(oobSock, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR) { connected = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (connected && g_isTransferManagerRunning) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_activeSockets.push_back(oobSock);
            }
            int flag = 1; setsockopt(oobSock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
            DWORD timeout = 10000;
            setsockopt(oobSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            setsockopt(oobSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
            ExecuteTransferIO(oobSock, transferId);
            closesocket(oobSock);

            std::lock_guard<std::mutex> lock(g_mutex);
            g_activeSockets.erase(std::remove(g_activeSockets.begin(), g_activeSockets.end(), oobSock), g_activeSockets.end());
        } else {
            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] OOB Client failed to connect to %s:%u", serverIp.c_str(), port);
            closesocket(oobSock);
        }
    }

    /**
     * @brief Initializes the File Transfer Manager.
     * @return true if initialized successfully.
     */
    bool Initialize() {
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Manager Initialized.");
        return true;
    }

    /**
     * @brief Shuts down the manager and clears pending transfers.
     * @return void
     */
    void Shutdown() {
        g_isTransferManagerRunning = false;
        std::vector<std::shared_ptr<std::thread>> threadsToJoin;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pendingOffers.clear();
            for (SOCKET sock : g_activeSockets) {
                if (sock != INVALID_SOCKET) {
                    shutdown(sock, SD_BOTH);
                    closesocket(sock);
                }
            }
            g_activeSockets.clear();
            threadsToJoin = g_transferThreads;
            g_transferThreads.clear();
        }
        
        for (auto& t : threadsToJoin) {
            if (t && t->joinable()) t->join();
        }

        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Manager Shutdown.");
    }

    /**
     * @brief Sender API: Parses a local file and broadcasts an EVENT_FILE_OFFER to target machines.
     * @param absoluteFilePath The absolute local file path to stream.
     * @param targetSockets A vector of specific client socket connections to offer the file to.
     * @return void
     */
    void InitiateTransfer(const std::string& absoluteFilePath, const std::vector<SOCKET>& targetSockets) {
        if (!State::enableFileTransfer) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        
        std::ifstream file(absoluteFilePath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Error: Could not open file for reading: %s", absoluteFilePath.c_str());
            return;
        }
        uint64_t fileSize = file.tellg();
        file.close();

        std::string fileName = absoluteFilePath.substr(absoluteFilePath.find_last_of("/\\") + 1);

        for (SOCKET target : targetSockets) {
            Network::FileOfferPayload offer;
            offer.transferId = ++g_nextTransferId;
            offer.fileSize = fileSize;
            strncpy_s(offer.fileName, sizeof(offer.fileName), fileName.c_str(), _TRUNCATE);
            
            TransferContext ctx;
            ctx.isSender = true;
            ctx.filePath = absoluteFilePath;
            ctx.fileSize = fileSize;
            ctx.fileName = fileName;
            ctx.transferredSize = 0;
            g_transfers[offer.transferId] = ctx;

            // Dispatch to correct role to utilize the main KVM socket for the handshake
            if (State::currentRole == State::AppRole::SERVER) {
                Network::SendToClient(target, Network::MessageType::EVENT_FILE_OFFER, &offer, sizeof(offer));
            } else {
                Network::SendToServer(Network::MessageType::EVENT_FILE_OFFER, &offer, sizeof(offer));
            }
            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Sent File Offer for '%s' (ID: %u)", fileName.c_str(), offer.transferId);
        }
    }

    /**
     * @brief Receiver API: Retrieves all currently pending file offers.
     * @return A vector of pending transfer structs.
     */
    std::vector<PendingTransfer> GetPendingTransfers() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_pendingOffers;
    }

    /**
     * @brief API: Retrieves all actively streaming transfers for progress visualization.
     * @return A vector of active transfer structs.
     */
    std::vector<ActiveTransfer> GetActiveTransfers() {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<ActiveTransfer> active;
        for (const auto& kv : g_transfers) {
            active.push_back({kv.first, kv.second.fileName, kv.second.fileSize, kv.second.transferredSize});
        }
        return active;
    }

    /**
     * @brief Receiver API: Accepts an offer and begins listening/connecting for the OOB data stream.
     * @param transferId The unique ID of the transfer to accept.
     * @param saveDirectory The local path to save the incoming file data.
     * @return void
     */
    void AcceptTransfer(uint32_t transferId, const std::string& saveDirectory) {
        SOCKET targetSocket = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::string fileName = "Unknown";
            uint64_t expectedSize = 0;
            for (const auto& p : g_pendingOffers) { 
                if (p.transferId == transferId) { 
                    targetSocket = p.remoteSocket; fileName = p.fileName; expectedSize = p.fileSize; break; 
                } 
            }
            
            TransferContext ctx;
            ctx.isSender = false;
            ctx.filePath = saveDirectory;
            ctx.fileSize = expectedSize;
            ctx.fileName = fileName;
            ctx.transferredSize = 0;
            g_transfers[transferId] = ctx;
            
            g_pendingOffers.erase(std::remove_if(g_pendingOffers.begin(), g_pendingOffers.end(),
                [transferId](const PendingTransfer& p) { return p.transferId == transferId; }), g_pendingOffers.end());
        }

        Network::FileAcceptPayload acceptPayload;
        acceptPayload.transferId = transferId;
        acceptPayload.tcpPort = 8082; // Default out-of-band streaming port
        
        if (State::currentRole == State::AppRole::SERVER) {
            auto t = std::make_shared<std::thread>(OOB_ServerTask, transferId, acceptPayload.tcpPort);
            { std::lock_guard<std::mutex> lock(g_mutex); g_transferThreads.push_back(t); }
            if (targetSocket != INVALID_SOCKET) {
                Network::SendToClient(targetSocket, Network::MessageType::EVENT_FILE_ACCEPT, &acceptPayload, sizeof(acceptPayload));
            }
        } else {
            auto t = std::make_shared<std::thread>(OOB_ClientTask, transferId, acceptPayload.tcpPort);
            { std::lock_guard<std::mutex> lock(g_mutex); g_transferThreads.push_back(t); }
            Network::SendToServer(Network::MessageType::EVENT_FILE_ACCEPT, &acceptPayload, sizeof(acceptPayload));
        }
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u Accepted. Will save to: %s", transferId, saveDirectory.c_str());
    }

    /**
     * @brief Receiver API: Declines a pending offer and notifies the sender.
     * @param transferId The unique ID of the declined transfer.
     * @return void
     */
    void DeclineTransfer(uint32_t transferId) {
        std::lock_guard<std::mutex> lock(g_mutex);
        SOCKET targetSocket = INVALID_SOCKET;
        for (const auto& p : g_pendingOffers) { if (p.transferId == transferId) { targetSocket = p.remoteSocket; break; } }
        
        g_pendingOffers.erase(std::remove_if(g_pendingOffers.begin(), g_pendingOffers.end(),
            [transferId](const PendingTransfer& p) { return p.transferId == transferId; }), g_pendingOffers.end());

        Network::FileDeclinePayload decline;
        decline.transferId = transferId;
        
        if (State::currentRole == State::AppRole::SERVER && targetSocket != INVALID_SOCKET) {
            Network::SendToClient(targetSocket, Network::MessageType::EVENT_FILE_DECLINE, &decline, sizeof(decline));
        } else {
            Network::SendToServer(Network::MessageType::EVENT_FILE_DECLINE, &decline, sizeof(decline));
        }
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u Declined.", transferId);
    }

    /**
     * @brief API: Cancels an actively streaming file transfer and closes the connection.
     * @param transferId The unique ID of the active transfer.
     * @return void
     */
    void CancelTransfer(uint32_t transferId) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_transfers.erase(transferId);
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Cancelling active transfer %u...", transferId);
    }

    /**
     * @brief Network Handler: Triggered when a file offer packet arrives.
     * @param senderSocket The socket of the machine offering the file.
     * @param payload The struct payload detailing the file.
     * @return void
     */
    void HandleFileOffer(SOCKET senderSocket, const Network::FileOfferPayload& payload) {
        if (!State::enableFileTransfer) {
            Network::FileDeclinePayload decline;
            decline.transferId = payload.transferId;
            if (State::currentRole == State::AppRole::SERVER) {
                Network::SendToClient(senderSocket, Network::MessageType::EVENT_FILE_DECLINE, &decline, sizeof(decline));
            } else {
                Network::SendToServer(Network::MessageType::EVENT_FILE_DECLINE, &decline, sizeof(decline));
            }
            return; // Auto-decline if transfers are disabled locally
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pendingOffers.push_back({ payload.transferId, payload.fileSize, std::string(payload.fileName, strnlen(payload.fileName, sizeof(payload.fileName))), senderSocket });
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Received File Offer: %s", payload.fileName);
    }

    /**
     * @brief Network Handler: Triggered when a remote machine accepts a file offer.
     * @param senderSocket The socket of the accepting machine.
     * @param payload The acceptance payload containing the dynamic OOB port.
     * @return void
     */
    void HandleFileAccept(SOCKET senderSocket, const Network::FileAcceptPayload& payload) {
        if (State::currentRole == State::AppRole::SERVER) {
            auto t = std::make_shared<std::thread>(OOB_ServerTask, payload.transferId, payload.tcpPort);
            { std::lock_guard<std::mutex> lock(g_mutex); g_transferThreads.push_back(t); }
        } else {
            auto t = std::make_shared<std::thread>(OOB_ClientTask, payload.transferId, payload.tcpPort);
            { std::lock_guard<std::mutex> lock(g_mutex); g_transferThreads.push_back(t); }
        }
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u Accepted by remote host. Establishing OOB stream.", payload.transferId);
    }

    /**
     * @brief Network Handler: Triggered when a remote machine declines a file offer.
     * @param senderSocket The socket of the declining machine.
     * @param payload The decline payload containing the transfer ID.
     * @return void
     */
    void HandleFileDecline(SOCKET senderSocket, const Network::FileDeclinePayload& payload) {
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u was declined by receiver.", payload.transferId);
    }
}