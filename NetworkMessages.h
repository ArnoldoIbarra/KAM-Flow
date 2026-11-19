// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/// @file NetworkMessages.h
/// Defines the data structures and protocol signatures for KAM-Flow.
/// Includes structural definitions for KVM deltas and UDP Auto-Discovery.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

namespace Network {

    /// Unique signature to ensure Server and Client are in sync.
    const uint32_t PACKET_MAGIC = 0x4B414D46; // "KAMF" in ASCII

    /// Hard limit to prevent std::bad_alloc attacks (1 Megabyte, expanded for Clipboard sharing)
    const uint32_t MAX_PAYLOAD_SIZE = 1048576; 

    /// Default TCP and UDP port for encrypted KVM control, input, clipboard, and media streams.
    constexpr uint16_t DEFAULT_SERVER_PORT = 39240;
    /// Default UDP port for local-network Server auto-discovery beacons.
    constexpr uint16_t DEFAULT_DISCOVERY_PORT = 39241;
    /// Default temporary TCP port for out-of-band encrypted file transfers.
    constexpr uint16_t DEFAULT_FILE_TRANSFER_PORT = 39242;

    /**
     * @brief Identifies the type of data contained in the network packet.
     */
    enum class MessageType : uint8_t {
        EVENT_MOUSE,         ///< Delta mouse movement or clicks.
        EVENT_KEYBOARD,      ///< Keystroke data.
        EVENT_STATE,         ///< Control mode transition (Local vs Remote).
        EVENT_AUTH,          ///< Authentication payload containing the PIN.
        EVENT_UDP_HANDSHAKE, ///< UDP Hole-punching payload to associate a connectionless stream.
        EVENT_UDP_HANDSHAKE_ACK, ///< Server confirms UDP connection is fully open.
        EVENT_SYNC_CURSOR,   ///< Instructions to snap the cursor to a specific edge on the Client.
        EVENT_RETURN_CONTROL,///< Sent by Client to request control return.
        EVENT_UDP_BEACON,    ///< Sent via UDP broadcast for local network auto-discovery.
        EVENT_HEARTBEAT,     ///< Keep-alive heartbeat to prevent TCP socket timeouts.
        EVENT_AUDIO_FORMAT,  ///< Server broadcasting its native audio mix format.
        EVENT_AUDIO_DATA,    ///< Raw uncompressed PCM audio stream.
        EVENT_MIC_DATA,      ///< Raw uncompressed PCM microphone stream from the Server.
        EVENT_CLIPBOARD,     ///< Raw UTF-16 text data from the clipboard.
        EVENT_CLIENT_LOCKED, ///< Sent by Client when UAC/Secure Desktop blocks input.
        EVENT_FILE_OFFER,    ///< Sender proposes an out-of-band file transfer.
        EVENT_FILE_ACCEPT,   ///< Receiver accepts the transfer and specifies the temporary port.
        EVENT_FILE_DECLINE,  ///< Receiver declines the file transfer.
        EVENT_AUTH_REJECTED  ///< Server rejects EVENT_AUTH due to Master PIN / AES-GCM tag mismatch.
    };

    /// Returns a human-readable protocol message name for diagnostic logging.
    inline const char* MessageTypeToString(MessageType type) {
        switch (type) {
            case MessageType::EVENT_MOUSE:             return "MOUSE";
            case MessageType::EVENT_KEYBOARD:          return "KEYBOARD";
            case MessageType::EVENT_STATE:             return "STATE";
            case MessageType::EVENT_AUTH:              return "AUTH";
            case MessageType::EVENT_UDP_HANDSHAKE:     return "UDP_HANDSHAKE";
            case MessageType::EVENT_UDP_HANDSHAKE_ACK: return "UDP_HANDSHAKE_ACK";
            case MessageType::EVENT_SYNC_CURSOR:       return "SYNC_CURSOR";
            case MessageType::EVENT_RETURN_CONTROL:    return "RETURN_CONTROL";
            case MessageType::EVENT_UDP_BEACON:        return "UDP_BEACON";
            case MessageType::EVENT_HEARTBEAT:         return "HEARTBEAT";
            case MessageType::EVENT_AUDIO_FORMAT:      return "AUDIO_FORMAT";
            case MessageType::EVENT_AUDIO_DATA:        return "AUDIO_DATA";
            case MessageType::EVENT_MIC_DATA:          return "MIC_DATA";
            case MessageType::EVENT_CLIPBOARD:         return "CLIPBOARD";
            case MessageType::EVENT_CLIENT_LOCKED:     return "CLIENT_LOCKED";
            case MessageType::EVENT_FILE_OFFER:        return "FILE_OFFER";
            case MessageType::EVENT_FILE_ACCEPT:       return "FILE_ACCEPT";
            case MessageType::EVENT_FILE_DECLINE:      return "FILE_DECLINE";
            case MessageType::EVENT_AUTH_REJECTED:     return "AUTH_REJECTED";
            default:                                   return "UNKNOWN";
        }
    }

    /// Returns true for high-frequency streaming/heartbeat messages excluded from routine packet logs.
    inline bool IsHighFrequencyMessage(MessageType type) {
        return type == MessageType::EVENT_MOUSE ||
               type == MessageType::EVENT_KEYBOARD ||
               type == MessageType::EVENT_AUDIO_DATA ||
               type == MessageType::EVENT_MIC_DATA ||
               type == MessageType::EVENT_HEARTBEAT;
    }

#pragma pack(push, 1)

    /**
     * @brief Header with Magic Number to prevent packet desynchronization.
     */
    struct PacketHeader {
        /// Must always match PACKET_MAGIC.
        uint32_t magic;      
        /// The type of message to follow.
        MessageType type;    
        /// Size of the following payload.
        uint32_t payloadSize; 
        /// Monotonically increasing counter to defeat replay attacks.
        uint32_t sequenceNumber;
    };

    /**
     * @brief Payload encrypted under AES-GCM for zero-plaintext-PIN authentication.
     */
    struct AuthPayload {
        /// Must equal PACKET_MAGIC after AES-GCM decryption to prove knowledge of the Master PIN.
        uint32_t authVerifier; 
        /// Cryptographic 64-bit session token binding the client's UDP stream to this TCP session.
        uint64_t sessionToken;
        /// Hostname of the Client machine for UI display.
        char clientName[32];
    };

    /**
     * @brief Payload sent over UDP to verify routing and associate the stream with a TCP session.
     */
    struct UDPHandshakePayload {
        /// Cryptographic 64-bit session token matching the authenticated TCP session.
        uint64_t sessionToken;
        /// Hostname of the Client machine to match against active TCP sessions.
        char clientName[32];
    };

    /**
     * @brief Payload indicating the active control mode.
     */
    struct StatePayload {
        /// 0 for Local, 1 for Remote.
        uint8_t mode; 
    };

    /**
     * @brief Payload mapping the Server's (or another Client's) exit coordinate to the target Client's entry edge.
     */
    struct CursorSyncPayload {
        /// 0 for Left, 1 for Right, 2 for Top, 3 for Bottom.
        uint8_t entryEdge;
        /// Horizontal entry point percentage (0.0 to 1.0).
        float normalizedX;
        /// Vertical entry point percentage (0.0 to 1.0).
        float normalizedY;
    };

    /**
     * @brief Payload sent by Client when its local cursor hits a screen edge.
     */
    struct ReturnControlPayload {
        /// 0 for Left, 1 for Right, 2 for Top, 3 for Bottom.
        uint8_t exitEdge; 
        /// Normalized horizontal position (0.0 to 1.0) where the cursor exited.
        float normalizedX;
        /// Normalized vertical position (0.0 to 1.0) where the cursor exited.
        float normalizedY;
    };

    /**
     * @brief Payload for raw mouse deltas and click flags.
     */
    struct MousePayload {
        /// Horizontal movement delta.
        int32_t deltaX;   
        /// Vertical movement delta.
        int32_t deltaY;   
        /// Scroll wheel data or X-button identifiers.
        uint32_t mouseData; 
        /// Injected Win32 Mouse Event flags.
        uint32_t flags;     
    };

    /**
     * @brief Payload for low-level keyboard hook data.
     */
    struct KeyboardPayload {
        /// Windows Virtual Key Code.
        uint16_t vkCode;  
        /// Hardware Scan Code.
        uint16_t scanCode; 
        /// Keystroke flags (e.g., KeyUp, ExtendedKey).
        uint32_t flags;    
    };

    /**
     * @brief Payload dictating the WASAPI stream format for automatic PCM conversion.
     */
    struct AudioFormatPayload {
        /// e.g., 44100, 48000
        uint32_t sampleRate; 
        /// e.g., 16, 24, 32
        uint16_t bitDepth;   
        /// e.g., 1 (Mono), 2 (Stereo)
        uint16_t channels;   
    };

    /**
     * @brief Payload broadcasted over UDP to announce Server presence.
     */
    struct UDPBeaconPayload {
        /// Hostname of the Server machine.
        char serverName[32]; 
        /// The TCP Port the Server is actively listening on.
        uint16_t tcpPort;    
    };

    /**
     * @brief Payload offering a file transfer to a remote machine.
     */
    struct FileOfferPayload {
        /// Unique identifier for this transfer request.
        uint32_t transferId;       
        /// Total size of the file in bytes.
        uint64_t fileSize;         
        /// Name of the file being sent (excluding directory path).
        char fileName[256];        
    };

    /**
     * @brief Payload accepting a file transfer and providing the out-of-band connection port.
     */
    struct FileAcceptPayload {
        /// Identifier of the accepted transfer.
        uint32_t transferId;       
        /// The temporary TCP port hosted by the Server for the file stream.
        uint16_t tcpPort;          
    };

    /**
     * @brief Payload declining a file transfer.
     */
    struct FileDeclinePayload {
        /// Identifier of the declined transfer.
        uint32_t transferId;       
    };

#pragma pack(pop)

    /// Counters tracking UDP delivery, out-of-order arrivals, sequence gaps, stale drops, and duplicates.
    struct UdpReplayStats {
        uint64_t packetsAccepted = 0;
        uint64_t packetsOutOfOrder = 0;
        uint64_t sequenceGaps = 0;
        uint64_t staleDrops = 0;
        uint64_t duplicateDrops = 0;
    };

    /// Returns true when any performance anomaly counter warrants emitting a periodic summary.
    inline bool ShouldEmitPerfSummary(uint64_t anomalySum) {
        return anomalySum > 0;
    }

    /**
     * @brief Verifies and updates a 64-packet sliding anti-replay bitmask window (RFC 6479 style)
     * while optionally recording packet loss, reordering, stale drops, and duplicate drops.
     * @param seq Incoming packet sequence number.
     * @param maxSeq Reference to the highest sequence number seen so far.
     * @param windowMask Reference to the 64-bit sliding bitmask tracking received packets.
     * @param stats Optional pointer receiving cumulative delivery and anomaly counters.
     * @return true if the packet is fresh and valid, false if stale or duplicate.
     */
    inline bool VerifyUdpReplayWindow(uint32_t seq, uint32_t& maxSeq, uint64_t& windowMask, UdpReplayStats* stats) {
        if (seq == 0) {
            if (stats) stats->duplicateDrops++;
            return false;
        }
        if (seq > maxSeq) {
            uint32_t shift = seq - maxSeq;
            if (maxSeq > 0 && shift > 1 && stats) {
                stats->sequenceGaps += (shift - 1);
            }
            if (shift >= 64) {
                windowMask = 1ULL;
            } else {
                windowMask = (windowMask << shift) | 1ULL;
            }
            maxSeq = seq;
            if (stats) stats->packetsAccepted++;
            return true;
        }
        uint32_t diff = maxSeq - seq;
        if (diff >= 64) {
            if (stats) stats->staleDrops++;
            return false; // Too old (falls behind the 64-packet reorder window)
        }
        uint64_t bit = 1ULL << diff;
        if (windowMask & bit) {
            if (stats) stats->duplicateDrops++;
            return false; // Duplicate / Replay attack within window!
        }
        windowMask |= bit;
        if (stats) {
            stats->packetsAccepted++;
            stats->packetsOutOfOrder++;
            if (stats->sequenceGaps > 0) {
                stats->sequenceGaps--;
            }
        }
        return true;
    }

    /**
     * @brief Verifies and updates a 64-packet sliding anti-replay bitmask window (RFC 6479 style).
     * Accepts out-of-order UDP packets within 64 sequence numbers while strictly rejecting
     * duplicate or replayed sequence numbers.
     * @param seq Incoming packet sequence number.
     * @param maxSeq Reference to the highest sequence number seen so far.
     * @param windowMask Reference to the 64-bit sliding bitmask tracking received packets.
     * @return true if the packet is fresh and valid, false if stale or duplicate.
     */
    inline bool VerifyUdpReplayWindow(uint32_t seq, uint32_t& maxSeq, uint64_t& windowMask) {
        return VerifyUdpReplayWindow(seq, maxSeq, windowMask, nullptr);
    }
}