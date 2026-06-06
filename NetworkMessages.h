/*
 * Copyright (c) 2026 Arnoldo Ibarra. All Rights Reserved.
 *
 * This software is the confidential and proprietary information of Arnoldo Ibarra.
 * ("Confidential Information"). You shall not disclose such Confidential 
 * Information and shall use it only in accordance with the terms of the 
 * license agreement you entered into.
 */

/**
 * @file NetworkMessages.h
 * @brief Defines the data structures and protocol signatures for KAM-Flow.
 * Includes structural definitions for KVM deltas and UDP Auto-Discovery.
 */

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

    /**
     * @brief Identifies the type of data contained in the network packet.
     */
    enum class MessageType : uint8_t {
        EVENT_MOUSE,         ///< Delta mouse movement or clicks.
        EVENT_KEYBOARD,      ///< Keystroke data.
        EVENT_STATE,         ///< Control mode transition (Local vs Remote).
        EVENT_AUTH,          ///< Authentication payload containing the PIN.
        EVENT_AUTH_AUDIO,    ///< Authentication payload for the dedicated audio OOB socket.
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
        EVENT_FILE_DECLINE   ///< Receiver declines the file transfer.
    };

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
     * @brief Payload containing the authentication PIN and Encryption State.
     */
    struct AuthPayload {
        /// Null-terminated 8-digit security PIN.
        char pin[9]; 
        /// Hostname of the Client machine for UI display.
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
}