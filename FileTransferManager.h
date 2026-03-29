// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// File Transfer Manager Interface
// =============================================================================

/// @file FileTransferManager.h
/// Manages out-of-band file transfers, asynchronous chunking, and memory-safe disk I/O.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <string>
#include <vector>
#include <cstdint>
#include <winsock2.h>
#include "NetworkMessages.h"

namespace FileTransfer {

    /// Represents an incoming file transfer request waiting for user approval.
    struct PendingTransfer {
        uint32_t transferId;   ///< Unique transfer identifier.
        uint64_t fileSize;     ///< Total file size in bytes.
        std::string fileName;  ///< The original name of the file.
        SOCKET remoteSocket;   ///< The socket of the machine that offered the file.
    };

    /// Represents an actively streaming file transfer for UI tracking.
    struct ActiveTransfer {
        uint32_t transferId;
        std::string fileName;
        uint64_t fileSize;
        uint64_t transferredSize;
    };

    /// Initializes the File Transfer Manager.
    bool Initialize();

    /// Shuts down the manager and aborts any active background streams.
    void Shutdown();

    /// Sender API: Parses a local file and broadcasts an EVENT_FILE_OFFER to target machines.
    void InitiateTransfer(const std::string& absoluteFilePath, const std::vector<SOCKET>& targetSockets);

    /// Receiver API: Retrieves all currently pending file offers to display in the UI.
    std::vector<PendingTransfer> GetPendingTransfers();

    /// API: Retrieves all actively streaming transfers for progress visualization.
    std::vector<ActiveTransfer> GetActiveTransfers();

    /// Receiver API: Accepts an offer and begins listening/connecting for the OOB data stream.
    void AcceptTransfer(uint32_t transferId, const std::string& saveDirectory);

    /// Receiver API: Declines a pending offer and notifies the sender.
    void DeclineTransfer(uint32_t transferId);

    /// API: Cancels an actively streaming file transfer and closes the connection.
    void CancelTransfer(uint32_t transferId);

    /// Network Handlers: Injected from NetworkServer/NetworkClient when packets arrive.
    void HandleFileOffer(SOCKET senderSocket, const Network::FileOfferPayload& payload);
    void HandleFileAccept(SOCKET senderSocket, const Network::FileAcceptPayload& payload);
    void HandleFileDecline(SOCKET senderSocket, const Network::FileDeclinePayload& payload);
}