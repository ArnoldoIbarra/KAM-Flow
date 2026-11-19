// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
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
#include <chrono>
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
        std::string peerIp;
    };
    std::map<uint32_t, TransferContext> g_transfers;

    /**
     * @brief Tracks an asynchronous OOB transfer thread and its completion state for reaping.
     */
    struct TransferThreadInfo {
        std::thread thread;
        std::atomic<bool> isFinished{false};
    };
    std::vector<std::shared_ptr<TransferThreadInfo>> g_transferThreads;
    std::vector<SOCKET> g_activeSockets;
    std::atomic<bool> g_isTransferManagerRunning(true);

    /// Guards the hardcoded OOB port (8082) against concurrent bind collisions.
    /// Only one file transfer stream may be active at a time. Checked before
    /// spawning OOB tasks and cleared when the task completes.
    std::atomic<bool> g_isOobPortInUse(false);

    void ExecuteTransferIO(SOCKET sock, uint32_t transferId);
    void OOB_ServerTask(uint32_t transferId, uint16_t port);
    void OOB_ClientTask(uint32_t transferId, uint16_t port);

    /**
     * @brief Resolves the remote IPv4 address string of a connected TCP socket.
     * @param s The connected socket handle.
     * @return The dotted-quad IPv4 string, or empty string on failure.
     */
    static std::string GetSocketPeerIp(SOCKET s) {
        if (s == INVALID_SOCKET) return "";
        sockaddr_in peerAddr = {0};
        int peerLen = sizeof(peerAddr);
        if (getpeername(s, (sockaddr*)&peerAddr, &peerLen) == 0) {
            char ipStr[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &(peerAddr.sin_addr), ipStr, INET_ADDRSTRLEN)) {
                return std::string(ipStr);
            }
        }
        return "";
    }

    /**
     * @brief Joins and removes completed OOB transfer threads. Must be called with g_mutex held.
     * @return void
     */
    static void ReapFinishedTransferThreadsLocked() {
        for (auto it = g_transferThreads.begin(); it != g_transferThreads.end();) {
            if ((*it)->isFinished.load(std::memory_order_acquire)) {
                if ((*it)->thread.joinable()) {
                    (*it)->thread.join();
                }
                it = g_transferThreads.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief Loops send() until all requested bytes are transmitted over the TCP socket.
     * @param s The connected TCP socket.
     * @param buf Source byte buffer.
     * @param len Total number of bytes to send.
     * @return true if all bytes were sent, false on socket error.
     */
    static bool SendExact(SOCKET s, const char* buf, int len) {
        int total = 0;
        while (total < len) {
            int n = send(s, buf + total, len - total, 0);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    /**
     * @brief Loops recv() until all requested bytes are received from the TCP socket.
     * @param s The connected TCP socket.
     * @param buf Destination byte buffer.
     * @param len Total number of bytes to read.
     * @return true if all bytes were read, false on disconnect or error.
     */
    static bool RecvExact(SOCKET s, char* buf, int len) {
        int total = 0;
        while (total < len) {
            int n = recv(s, buf + total, len - total, 0);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

#pragma pack(push, 1)
    /**
     * @brief Internal header sent before every file chunk on the OOB socket.
     * Bound into AES-GCM AAD to prevent chunk-reordering, truncation, and injection attacks.
     */
    struct FileChunkHeader {
        uint32_t magic;       ///< Must equal Network::PACKET_MAGIC
        uint32_t transferId;  ///< Matches the negotiated transfer ID
        uint32_t chunkIndex;  ///< Strictly monotonic chunk index (0, 1, 2...)
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
                if (State::globalDebugMode) {
                    UI::LogDebug("[FileTransfer] ERROR: Failed to open source file '%s' (err=%lu).",
                                 ctx.filePath.c_str(), GetLastError());
                }
                return;
            }

            // Pre-allocate reusable buffers once outside the chunk loop to eliminate per-chunk heap churn
            std::vector<uint8_t> buffer(65536); // Stream 64KB memory-safe chunks
            std::vector<uint8_t> ciphertext;
            ciphertext.reserve(65536 + 28);
            std::vector<uint8_t> packet;
            packet.reserve(sizeof(FileChunkHeader) + 65536 + 28);
            uint32_t chunkIndex = 0;
            bool sendCompletedCleanly = false;
            auto transferStart = std::chrono::steady_clock::now();

            while (file) {
                file.read((char*)buffer.data(), buffer.size());
                size_t bytesRead = (size_t)file.gcount();
                bool isEOF = file.eof() || file.peek() == EOF;

                if (bytesRead > 0 || isEOF) {
                    uint32_t currentChunkIdx = chunkIndex++;
                    FileChunkHeader header = { Network::PACKET_MAGIC, transferId, currentChunkIdx, 0, isEOF };
                    
                    if (bytesRead > 0) {
                        header.chunkSize = (uint32_t)(bytesRead + 28); // Account for 12b IV + 16b Tag
                        if (!Security::EncryptPayload(buffer.data(), bytesRead, &header, sizeof(header), ciphertext)) {
                            if (State::globalDebugMode) {
                                UI::LogDebug("[FileTransfer] ERROR: Chunk %u encryption failed (ID: %u).",
                                             currentChunkIdx, transferId);
                            }
                            break;
                        }
                        
                        packet.resize(sizeof(header) + ciphertext.size());
                        memcpy(packet.data(), &header, sizeof(header));
                        memcpy(packet.data() + sizeof(header), ciphertext.data(), ciphertext.size());
                    } else {
                        packet.resize(sizeof(header));
                        memcpy(packet.data(), &header, sizeof(header));
                    }

                    if (!SendExact(sock, (const char*)packet.data(), (int)packet.size())) {
                        if (State::globalDebugMode) {
                            UI::LogDebug("[FileTransfer] Connection dropped during send (ID: %u, chunk: %u, WSA Error: %d).",
                                         transferId, currentChunkIdx, WSAGetLastError());
                        }
                        break;
                    }
                    bool aborted = false;
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        if (g_transfers.find(transferId) != g_transfers.end()) {
                            g_transfers[transferId].transferredSize += bytesRead;
                        } else {
                            aborted = true;
                        }
                    }
                    if (aborted) {
                        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u aborted by user.", transferId);
                        break;
                    }
                }
                if (isEOF) {
                    sendCompletedCleanly = true;
                    break;
                }
            }
            if (sendCompletedCleanly && State::globalDebugMode) {
                double elapsedSec = (std::max)(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - transferStart).count());
                double mb = static_cast<double>(ctx.fileSize) / (1024.0 * 1024.0);
                UI::LogDebug("[FileTransfer] Sent '%s' (ID: %u, %.2f MB in %.2fs, %.1f MB/s).",
                             ctx.fileName.c_str(), transferId, mb, elapsedSec, mb / elapsedSec);
            }
        } else {
            std::ofstream file(ctx.filePath, std::ios::binary);
            if (!file.is_open()) {
                if (State::globalDebugMode) {
                    UI::LogDebug("[FileTransfer] ERROR: Failed to open destination file '%s' (err=%lu).",
                                 ctx.filePath.c_str(), GetLastError());
                }
                return;
            }

            // Pre-allocate reusable receive buffers once outside the loop
            std::vector<uint8_t> ciphertext;
            ciphertext.reserve(65536 + 28);
            std::vector<uint8_t> plaintext;
            plaintext.reserve(65536);
            uint32_t expectedChunkIndex = 0;
            bool transferCompletedCleanly = false;
            auto transferStart = std::chrono::steady_clock::now();

            while (true) {
                FileChunkHeader header;
                if (!RecvExact(sock, (char*)&header, sizeof(header))) {
                    if (State::globalDebugMode) {
                        UI::LogDebug("[FileTransfer] Connection closed or recv failed on header (ID: %u, chunk: %u, WSA Error: %d).",
                                     transferId, expectedChunkIndex, WSAGetLastError());
                    }
                    break;
                }
                if (header.magic != Network::PACKET_MAGIC) {
                    if (State::globalDebugMode) {
                        UI::LogDebug("[FileTransfer] ERROR: Bad chunk magic 0x%08X (ID: %u).",
                                     header.magic, transferId);
                    }
                    break;
                }
                if (header.transferId != transferId) {
                    if (State::globalDebugMode) {
                        UI::LogDebug("[FileTransfer] ERROR: Chunk transferId mismatch (got %u, expected %u).",
                                     header.transferId, transferId);
                    }
                    break;
                }
                if (header.chunkIndex != expectedChunkIndex) {
                    if (State::globalDebugMode) {
                        UI::LogDebug("[FileTransfer] ERROR: Chunk index desync (got %u, expected %u, ID: %u).",
                                     header.chunkIndex, expectedChunkIndex, transferId);
                    }
                    break;
                }
                ++expectedChunkIndex;

                if (header.chunkSize > 0) {
                    // Enforce payload bounds to prevent OOM std::bad_alloc attacks (CWE-400)
                    if (header.chunkSize > Network::MAX_PAYLOAD_SIZE) {
                        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] CRITICAL: Chunk size exceeds safety bounds (%u bytes, ID: %u)! Aborting.", header.chunkSize, transferId);
                        break;
                    }
                    
                    ciphertext.resize(header.chunkSize);
                    if (!RecvExact(sock, (char*)ciphertext.data(), (int)ciphertext.size())) {
                        if (State::globalDebugMode) {
                            UI::LogDebug("[FileTransfer] Partial chunk receive (ID: %u, chunk: %u, WSA Error: %d).",
                                         transferId, header.chunkIndex, WSAGetLastError());
                        }
                        break;
                    }

                    if (Security::DecryptPayload(ciphertext.data(), ciphertext.size(), &header, sizeof(header), plaintext)) {
                        if (!file.write((const char*)plaintext.data(), plaintext.size())) {
                            DWORD writeErr = GetLastError();
                            if (State::globalDebugMode) {
                                UI::LogDebug("[FileTransfer] ERROR: Disk write failed for transfer %u at chunk %u (err=%lu).",
                                             transferId, header.chunkIndex, writeErr);
                            }
                            break;
                        }
                        bool aborted = false;
                        {
                            std::lock_guard<std::mutex> lock(g_mutex);
                            if (g_transfers.find(transferId) != g_transfers.end()) {
                                g_transfers[transferId].transferredSize += plaintext.size();
                            } else {
                                aborted = true;
                            }
                        }
                        if (aborted) {
                            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u aborted by user.", transferId);
                            break;
                        }
                    } else {
                        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Chunk %u decryption failed (ID: %u)!", header.chunkIndex, transferId);
                        break;
                    }
                }

                if (header.isEOF) {
                    uint64_t finalTransferred = 0;
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        if (g_transfers.find(transferId) != g_transfers.end()) {
                            finalTransferred = g_transfers[transferId].transferredSize;
                        }
                    }
                    if (finalTransferred != ctx.fileSize) {
                        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] WARNING: EOF reached but received size (%llu) != expected (%llu).",
                            (unsigned long long)finalTransferred, (unsigned long long)ctx.fileSize);
                    } else {
                        transferCompletedCleanly = true;
                        if (State::globalDebugMode) {
                            double elapsedSec = (std::max)(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - transferStart).count());
                            double mb = static_cast<double>(ctx.fileSize) / (1024.0 * 1024.0);
                            UI::LogDebug("[FileTransfer] Received '%s' (ID: %u, %.2f MB in %.2fs, %.1f MB/s).",
                                         ctx.fileName.c_str(), transferId, mb, elapsedSec, mb / elapsedSec);
                        }
                    }
                    break;
                }
            }
            file.close();
            if (!transferCompletedCleanly) {
                DeleteFileA(ctx.filePath.c_str());
                if (State::globalDebugMode) {
                    UI::LogDebug("[FileTransfer] Removed incomplete file for failed/aborted transfer %u: %s",
                                 transferId, ctx.filePath.c_str());
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
        // Ensure the OOB port guard is released on every exit path
        struct OobGuard { ~OobGuard() { g_isOobPortInUse.store(false, std::memory_order_relaxed); } } guard;

        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            if (State::globalDebugMode) {
                UI::LogDebug("[FileTransfer] ERROR: OOB listener socket() failed (WSA Error: %d).", WSAGetLastError());
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_activeSockets.push_back(listenSock);
        }

        int opt = 1; setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in addr = {0};
        addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || listen(listenSock, 1) == SOCKET_ERROR) {
            if (State::globalDebugMode) {
                UI::LogDebug("[FileTransfer] ERROR: OOB Bind/Listen failed on port %u (WSA Error: %d).",
                             port, WSAGetLastError());
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            g_activeSockets.erase(std::remove(g_activeSockets.begin(), g_activeSockets.end(), listenSock), g_activeSockets.end());
            closesocket(listenSock); 
            return;
        }

        // Bounded wait for the peer to connect. Without this, accept() blocks forever
        // when the peer never connects (e.g. it rejected the transfer because its own
        // OOB port was busy). That kept g_isOobPortInUse stuck at true and silently
        // disabled all future file transfers until the app was restarted.
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        timeval acceptTimeout = { 15, 0 };
        if (select(0, &readSet, nullptr, nullptr, &acceptTimeout) <= 0) {
            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] OOB accept timed out on port %u (transfer %u).", port, transferId);
            // Only close the listener if Shutdown() has not already closed and
            // removed it, to avoid a double closesocket() on a recycled handle.
            bool ownsListenSock = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = std::find(g_activeSockets.begin(), g_activeSockets.end(), listenSock);
                if (it != g_activeSockets.end()) { g_activeSockets.erase(it); ownsListenSock = true; }
                g_transfers.erase(transferId);
            }
            if (ownsListenSock) closesocket(listenSock);
            return;
        }

        sockaddr_in clientAddr; int clientSize = sizeof(clientAddr);
        SOCKET oobSock = accept(listenSock, (sockaddr*)&clientAddr, &clientSize);
        closesocket(listenSock); // Listener no longer needed

        std::string expectedPeerIp;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_activeSockets.erase(std::remove(g_activeSockets.begin(), g_activeSockets.end(), listenSock), g_activeSockets.end());
            auto it = g_transfers.find(transferId);
            if (it != g_transfers.end()) {
                expectedPeerIp = it->second.peerIp;
            }
        }

        if (oobSock != INVALID_SOCKET) {
            char connectedIp[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &(clientAddr.sin_addr), connectedIp, INET_ADDRSTRLEN);

            // Verify the connecting peer IP matches the authenticated session peer that negotiated the transfer
            if (!expectedPeerIp.empty() && expectedPeerIp != connectedIp) {
                if (State::globalDebugMode) {
                    UI::LogDebug("[FileTransfer] SECURITY: Rejected unauthorized OOB connection from %s (expected %s).",
                                 connectedIp, expectedPeerIp.c_str());
                }
                closesocket(oobSock);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_transfers.erase(transferId);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_activeSockets.push_back(oobSock);
            }

            if (g_isTransferManagerRunning) {
                int flag = 1; setsockopt(oobSock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
                DWORD timeout = 10000; // 10s idle teardown
                setsockopt(oobSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
                setsockopt(oobSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
                ExecuteTransferIO(oobSock, transferId);
            }
            closesocket(oobSock);

            std::lock_guard<std::mutex> lock(g_mutex);
            g_activeSockets.erase(std::remove(g_activeSockets.begin(), g_activeSockets.end(), oobSock), g_activeSockets.end());
        } else if (g_isTransferManagerRunning && State::globalDebugMode) {
            UI::LogDebug("[FileTransfer] ERROR: OOB accept() failed on port %u (transfer %u, WSA Error: %d).",
                         port, transferId, WSAGetLastError());
        }
    }

    /**
     * @brief Background thread task to connect to the Server's OOB socket for transfer.
     * @param transferId The unique identifier for this transfer.
     * @param port The TCP port the Server is hosting the OOB stream on.
     * @return void
     */
    void OOB_ClientTask(uint32_t transferId, uint16_t port) {
        // Ensure the OOB port guard is released on every exit path
        struct OobGuard { ~OobGuard() { g_isOobPortInUse.store(false, std::memory_order_relaxed); } } guard;

        // Micro-delay ensures the Server thread has executed bind() before we connect
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::string serverIp;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_transfers.find(transferId);
            if (it != g_transfers.end() && !it->second.peerIp.empty()) {
                serverIp = it->second.peerIp;
            }
        }
        if (serverIp.empty()) {
            serverIp = "127.0.0.1";
            auto targets = CredentialManager::GetSavedTargets("KAMFlow_Server_");
            if (!targets.empty()) {
                serverIp = targets[0].substr(15);
            }
        }
        
        sockaddr_in addr = {0};
        addr.sin_family = AF_INET; addr.sin_port = htons(port); inet_pton(AF_INET, serverIp.c_str(), &addr.sin_addr);

        SOCKET oobSock = INVALID_SOCKET;
        bool connected = false;
        int lastErr = 0;
        for (int i = 0; i < 5 && g_isTransferManagerRunning; ++i) { // 2.5s cumulative retry
            oobSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (oobSock == INVALID_SOCKET) {
                lastErr = WSAGetLastError();
                break;
            }
            if (connect(oobSock, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR) {
                connected = true;
                break;
            }
            lastErr = WSAGetLastError();
            closesocket(oobSock);
            oobSock = INVALID_SOCKET;
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
            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] OOB Client failed to connect to %s:%u (WSA Error: %d).", serverIp.c_str(), port, lastErr);
            if (oobSock != INVALID_SOCKET) closesocket(oobSock);
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
        std::vector<std::shared_ptr<TransferThreadInfo>> threadsToJoin;
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
            if (t && t->thread.joinable()) t->thread.join();
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
        std::streampos endPos = file.tellg();
        if (endPos < 0) {
            file.close();
            if (State::globalDebugMode) {
                UI::LogDebug("[FileTransfer] ERROR: Failed to determine file size for '%s'.", absoluteFilePath.c_str());
            }
            return;
        }
        uint64_t fileSize = static_cast<uint64_t>(endPos);
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
            ctx.peerIp = GetSocketPeerIp(target);
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
        ReapFinishedTransferThreadsLocked();
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
        // Enforce one-at-a-time: if the OOB port is already bound, auto-decline.
        bool expected = false;
        if (!g_isOobPortInUse.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u rejected: another transfer is already in progress.", transferId);
            DeclineTransfer(transferId);
            return;
        }

        SOCKET targetSocket = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::string fileName = "Unknown";
            uint64_t expectedSize = 0;
            bool found = false;
            for (const auto& p : g_pendingOffers) { 
                if (p.transferId == transferId) { 
                    targetSocket = p.remoteSocket; fileName = p.fileName; expectedSize = p.fileSize; found = true; break; 
                } 
            }

            // Stale or unknown ID (e.g. offer already declined/expired): release the
            // OOB guard claimed above instead of spawning an orphan OOB thread.
            if (!found) {
                g_isOobPortInUse.store(false, std::memory_order_relaxed);
                if (State::globalDebugMode) UI::LogDebug("[FileTransfer] AcceptTransfer ignored: unknown transfer %u.", transferId);
                return;
            }
            
            TransferContext ctx;
            ctx.isSender = false;
            ctx.filePath = saveDirectory;
            ctx.fileSize = expectedSize;
            ctx.fileName = fileName;
            ctx.transferredSize = 0;
            ctx.peerIp = GetSocketPeerIp(targetSocket);
            g_transfers[transferId] = ctx;
            
            g_pendingOffers.erase(std::remove_if(g_pendingOffers.begin(), g_pendingOffers.end(),
                [transferId](const PendingTransfer& p) { return p.transferId == transferId; }), g_pendingOffers.end());
        }

        Network::FileAcceptPayload acceptPayload;
        acceptPayload.transferId = transferId;
        acceptPayload.tcpPort = 8082; // Default out-of-band streaming port
        
        if (State::currentRole == State::AppRole::SERVER) {
            auto info = std::make_shared<TransferThreadInfo>();
            info->thread = std::thread([info, transferId, port = acceptPayload.tcpPort]() {
                OOB_ServerTask(transferId, port);
                info->isFinished.store(true, std::memory_order_release);
            });
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                ReapFinishedTransferThreadsLocked();
                g_transferThreads.push_back(info);
            }
            if (targetSocket != INVALID_SOCKET) {
                Network::SendToClient(targetSocket, Network::MessageType::EVENT_FILE_ACCEPT, &acceptPayload, sizeof(acceptPayload));
            }
        } else {
            auto info = std::make_shared<TransferThreadInfo>();
            info->thread = std::thread([info, transferId, port = acceptPayload.tcpPort]() {
                OOB_ClientTask(transferId, port);
                info->isFinished.store(true, std::memory_order_release);
            });
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                ReapFinishedTransferThreadsLocked();
                g_transferThreads.push_back(info);
            }
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
        // Unblock any active OOB socket I/O immediately instead of waiting for SO_RCVTIMEO.
        for (SOCKET sock : g_activeSockets) {
            if (sock != INVALID_SOCKET) {
                shutdown(sock, SD_BOTH);
            }
        }
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Cancelling active transfer %u...", transferId);
    }

    /**
     * @brief Strips directory paths and replaces illegal Windows filename characters from an untrusted peer filename.
     * @param rawBuf Raw filename character buffer received from the network.
     * @param maxLen Maximum byte length of the raw buffer.
     * @return A sanitized base filename safe for Win32 file save dialogs.
     */
    static std::string SanitizeFileName(const char* rawBuf, size_t maxLen) {
        std::string name(rawBuf, strnlen(rawBuf, maxLen));
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
        for (char& c : name) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 32 || strchr("<>:\"/\\|?*", c) != nullptr) {
                c = '_';
            }
        }
        if (name.empty() || name == "." || name == "..") {
            return "TransferredFile.bin";
        }
        return name;
    }

    /**
     * @brief Network Handler: Triggered when a file offer packet arrives.
     * @param senderSocket The socket of the machine offering the file.
     * @param payload The struct payload detailing the file.
     * @return void
     */
    void HandleFileOffer(SOCKET senderSocket, const Network::FileOfferPayload& payload) {
        if (!State::enableFileTransfer) {
            if (State::globalDebugMode) {
                UI::LogDebug("[FileTransfer] Auto-declined incoming offer %u ('%s'): file transfers are disabled.",
                             payload.transferId, SanitizeFileName(payload.fileName, sizeof(payload.fileName)).c_str());
            }
            Network::FileDeclinePayload decline;
            decline.transferId = payload.transferId;
            if (State::currentRole == State::AppRole::SERVER) {
                Network::SendToClient(senderSocket, Network::MessageType::EVENT_FILE_DECLINE, &decline, sizeof(decline));
            } else {
                Network::SendToServer(Network::MessageType::EVENT_FILE_DECLINE, &decline, sizeof(decline));
            }
            return; // Auto-decline if transfers are disabled locally
        }
        std::string safeName = SanitizeFileName(payload.fileName, sizeof(payload.fileName));
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pendingOffers.push_back({ payload.transferId, payload.fileSize, safeName, senderSocket });
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Received File Offer: %s", safeName.c_str());
    }

    /**
     * @brief Network Handler: Triggered when a remote machine accepts a file offer.
     * @param senderSocket The socket of the accepting machine.
     * @param payload The acceptance payload containing the dynamic OOB port.
     * @return void
     */
    void HandleFileAccept(SOCKET senderSocket, const Network::FileAcceptPayload& payload) {
        // Enforce one-at-a-time: prevent a second OOB stream from colliding on port 8082.
        bool expected = false;
        if (!g_isOobPortInUse.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (State::globalDebugMode) UI::LogDebug("[FileTransfer] HandleFileAccept rejected for transfer %u: OOB port already in use.", payload.transferId);
            // Drop our sender-side context so the UI does not show a phantom "active"
            // transfer, then tell the peer so it can drop its own context. The mutex is
            // released BEFORE sending to avoid the double-lock deadlock class fixed in v2.7.
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_transfers.erase(payload.transferId);
            }
            Network::FileDeclinePayload decline;
            decline.transferId = payload.transferId;
            if (State::currentRole == State::AppRole::SERVER) {
                Network::SendToClient(senderSocket, Network::MessageType::EVENT_FILE_DECLINE, &decline, sizeof(decline));
            } else {
                Network::SendToServer(Network::MessageType::EVENT_FILE_DECLINE, &decline, sizeof(decline));
            }
            return;
        }

        std::string senderIp = GetSocketPeerIp(senderSocket);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_transfers.find(payload.transferId);
            if (it == g_transfers.end()) {
                g_isOobPortInUse.store(false, std::memory_order_relaxed);
                if (State::globalDebugMode) {
                    UI::LogDebug("[FileTransfer] HandleFileAccept ignored: unknown or cancelled transfer %u.",
                                 payload.transferId);
                }
                return;
            }
            if (it->second.peerIp.empty() && !senderIp.empty()) {
                it->second.peerIp = senderIp;
            }
        }

        if (State::currentRole == State::AppRole::SERVER) {
            auto info = std::make_shared<TransferThreadInfo>();
            info->thread = std::thread([info, transferId = payload.transferId, port = payload.tcpPort]() {
                OOB_ServerTask(transferId, port);
                info->isFinished.store(true, std::memory_order_release);
            });
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                ReapFinishedTransferThreadsLocked();
                g_transferThreads.push_back(info);
            }
        } else {
            auto info = std::make_shared<TransferThreadInfo>();
            info->thread = std::thread([info, transferId = payload.transferId, port = payload.tcpPort]() {
                OOB_ClientTask(transferId, port);
                info->isFinished.store(true, std::memory_order_release);
            });
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                ReapFinishedTransferThreadsLocked();
                g_transferThreads.push_back(info);
            }
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
        // Remove the local context so a declined/rejected transfer does not linger
        // forever in GetActiveTransfers() as a stuck "active" entry in the UI.
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_transfers.erase(payload.transferId);
        }
        if (State::globalDebugMode) UI::LogDebug("[FileTransfer] Transfer %u was declined by receiver.", payload.transferId);
    }
}