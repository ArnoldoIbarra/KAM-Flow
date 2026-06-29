// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Network Client Implementation
// =============================================================================

/**
 * @file NetworkClient.cpp
 * @brief Implementation of the TCP Client and UDP Auto-Discovery system.
 */

#include "NetworkClient.h"
#include "AudioManager.h"
#include "ClipboardManager.h"
#include "ConfigManager.h"
#include "CredentialManager.h"
#include "FileTransferManager.h"
#include "NetworkMessages.h"
#include "SecurityManager.h"
#include "StateManager.h"
#include "UIManager.h"
#include <algorithm>
#include <atomic>
#include <avrt.h>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <ws2tcpip.h>

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

/// Mutex to serialize UDP sends without blocking TCP heartbeats.
std::mutex clientUdpSendMutex;

/// Independent UDP transmit sequence counter to prevent cross-talk replay
/// drops.
std::atomic<uint32_t> clientUdpTxSequence{0};
/// Independent UDP receive sequence counter.
uint32_t udpRxSequence = 0;

/// Active socket connected to the Server specifically for latency-sensitive UDP
/// data.
SOCKET clientUdpSocket = INVALID_SOCKET;
/// Address of the Server's UDP endpoint.
sockaddr_in serverUdpAddr;
/// Background thread processing incoming UDP packets.
std::thread clientUdpListenerThread;

/// Tracks whether the UDP connection has been successfully established and
/// verified by the Server.
std::atomic<bool> isUdpActive{false};

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

/// Exponential backoff delay for auto-reconnect (seconds). Doubles on each
/// short-lived session, capped at 30s. Resets after a stable connection (>30s).
int reconnectBackoffSeconds = 5;

/**
 * @brief Injects KEYUP events for modifier keys to prevent them from getting
 * stuck when the Server suddenly revokes control or disconnects.
 */
void ReleaseStuckModifiers() {
  const WORD modifiers[] = {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
                            VK_LSHIFT,   VK_RSHIFT,   VK_LWIN,  VK_RWIN};
  for (WORD vk : modifiers) {
    INPUT i = {0};
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = vk;
    i.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &i, sizeof(INPUT));
  }
}

/**
 * @brief Packages a payload with a header, optionally encrypts it, and sends it
 * to the Server.
 * @param type The MessageType identifier.
 * @param payload Pointer to the raw payload structure.
 * @param payloadSize Size of the payload structure.
 * @return true if sent successfully.
 */
bool SendToServer(MessageType type, const void *payload, size_t payloadSize) {
  if (!isClientRunning)
    return false;

  bool isUdp = false;
  SOCKET targetSock = clientSocket;
  std::mutex *targetMutex = &clientSendMutex;

  if (type == MessageType::EVENT_UDP_HANDSHAKE) {
    isUdp = true;
    targetSock = clientUdpSocket;
    targetMutex = &clientUdpSendMutex;
  } else if (type == MessageType::EVENT_MOUSE ||
             type == MessageType::EVENT_AUDIO_DATA ||
             type == MessageType::EVENT_MIC_DATA) {
    if (isUdpActive.load(std::memory_order_relaxed)) {
      isUdp = true;
      targetSock = clientUdpSocket;
      targetMutex = &clientUdpSendMutex;
      static bool hasLoggedUdpRecovery = false;
      if (hasLoggedUdpRecovery) {
        hasLoggedUdpRecovery = false;
      }
    } else {
      static bool hasLoggedTcpFallback = false;
      if (!hasLoggedTcpFallback && State::globalDebugMode) {
        UI::LogDebug("[Network Client] UDP not active. Falling back to TCP for "
                     "media streams.");
        hasLoggedTcpFallback = true;
      }
      // Fallback to TCP is automatic because targetSock is already clientSocket
    }
  }

  if (targetSock == INVALID_SOCKET)
    return false;

  std::lock_guard<std::mutex> lock(*targetMutex);

  uint32_t seq = isUdp ? ++clientUdpTxSequence : ++clientTxSequence;
  PacketHeader h = {PACKET_MAGIC, type, static_cast<uint32_t>(payloadSize),
                    seq};

  thread_local std::vector<uint8_t> tls_sendBuffer;
  thread_local std::vector<uint8_t> tls_ciphertext;

  if (type != MessageType::EVENT_AUTH) {
    h.payloadSize = static_cast<uint32_t>(
        payloadSize + 28); // 12-byte IV + 16-byte Tag overhead
    if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h),
                                 tls_ciphertext)) {
      tls_sendBuffer.resize(sizeof(h) + tls_ciphertext.size());
      memcpy(tls_sendBuffer.data(), &h, sizeof(h));
      memcpy(tls_sendBuffer.data() + sizeof(h), tls_ciphertext.data(),
             tls_ciphertext.size());
    } else {
      return false;
    }
  } else {
    tls_sendBuffer.resize(sizeof(h) + payloadSize);
    memcpy(tls_sendBuffer.data(), &h, sizeof(h));
    memcpy(tls_sendBuffer.data() + sizeof(h), payload, payloadSize);
  }

  if (!tls_sendBuffer.empty()) {
    int res = 0;
    if (isUdp) {
      res = sendto(clientUdpSocket, (const char *)tls_sendBuffer.data(),
                   (int)tls_sendBuffer.size(), 0, (sockaddr *)&serverUdpAddr,
                   sizeof(serverUdpAddr));
    } else {
      res = send(targetSock, (const char *)tls_sendBuffer.data(),
                 (int)tls_sendBuffer.size(), 0);
    }
    return res != SOCKET_ERROR;
  }
  return false;
}

/**
 * @brief Specially sends a Keep-Alive heartbeat on the specified socket to
 * prevent SO_RCVTIMEO disconnects.
 */
void SendHeartbeatToSocket(SOCKET sock, std::mutex &sockMutex) {
  if (sock == INVALID_SOCKET)
    return;

  std::unique_lock<std::mutex> lock(sockMutex, std::try_to_lock);
  if (!lock.owns_lock())
    return; // Socket is actively streaming data, heartbeat is unnecessary.

  PacketHeader h = {PACKET_MAGIC, MessageType::EVENT_HEARTBEAT, 0,
                    ++clientTxSequence};

  thread_local std::vector<uint8_t> tls_sendBuffer;
  thread_local std::vector<uint8_t> tls_ciphertext;

  h.payloadSize = 28; // 12-byte IV + 16-byte Tag overhead for a 0-byte payload
  if (Security::EncryptPayload(nullptr, 0, &h, sizeof(h), tls_ciphertext)) {
    tls_sendBuffer.resize(sizeof(h) + tls_ciphertext.size());
    memcpy(tls_sendBuffer.data(), &h, sizeof(h));
    memcpy(tls_sendBuffer.data() + sizeof(h), tls_ciphertext.data(),
           tls_ciphertext.size());

    send(sock, (const char *)tls_sendBuffer.data(), (int)tls_sendBuffer.size(),
         0);
  }
}

/**
 * @brief Uses Win32 SendInput to synthesize mouse actions natively.
 * @param p The MousePayload containing deltas and flags.
 * @return void
 */
void InjectMouseInput(const MousePayload &p) {
  INPUT i = {0};
  i.type = INPUT_MOUSE;
  i.mi.dx = p.deltaX;
  i.mi.dy = p.deltaY;
  i.mi.mouseData = p.mouseData;
  i.mi.dwFlags = p.flags;
  SendInput(1, &i, sizeof(INPUT));
}

/**
 * @brief Uses Win32 SendInput to synthesize keyboard actions natively.
 * @param p The KeyboardPayload containing Virtual Key Codes and state flags.
 * @return void
 */
void InjectKeyboardInput(const KeyboardPayload &p) {
  INPUT i = {0};
  i.type = INPUT_KEYBOARD;
  i.ki.wVk = p.vkCode;
  i.ki.wScan = p.scanCode;
  i.ki.dwFlags = p.flags;
  SendInput(1, &i, sizeof(INPUT));
}

/**
 * @brief Checks if the active TCP Client is currently connected and processing
 * data.
 * @return true if fully connected, false otherwise.
 */
bool IsClientConnected() { return isClientRunning; }

/**
 * @brief Returns a thread-safe copy of all currently active Servers on the
 * local network.
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
bool GetServerAudioFormat(Audio::AudioFormat &outFormat) {
  if (!g_hasServerAudioFormat)
    return false;
  outFormat = g_serverAudioFormat;
  return true;
}

/**
 * @brief Background loop that listens for Server Beacons and manages the
 * network map.
 * @return void
 */
void DiscoveryLoop() {
  udpListenerSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udpListenerSocket == INVALID_SOCKET) {
    if (State::globalDebugMode)
      UI::LogDebug("[Network Client] UDP Socket creation failed.");
    return;
  }

  int reuseAddr = 1;
  setsockopt(udpListenerSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&reuseAddr,
             sizeof(reuseAddr));

  sockaddr_in localAddr;
  localAddr.sin_family = AF_INET;
  localAddr.sin_port = htons(8081);
  localAddr.sin_addr.s_addr = INADDR_ANY;

  if (bind(udpListenerSocket, (sockaddr *)&localAddr, sizeof(localAddr)) ==
      SOCKET_ERROR) {
    if (State::globalDebugMode)
      UI::LogDebug(
          "[Network Client] UDP Bind failed. Port 8081 may be blocked.");
  }

  DWORD timeout = 500;
  setsockopt(udpListenerSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));

  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Listening for UDP Beacons on port 8081...");

  while (isDiscoveryRunning) {
    char buffer[1024];
    sockaddr_in senderAddr;
    int senderAddrSize = sizeof(senderAddr);

    int bytes = recvfrom(udpListenerSocket, buffer, sizeof(buffer), 0,
                         (sockaddr *)&senderAddr, &senderAddrSize);

    if (bytes > 0) {
      if (bytes == (sizeof(PacketHeader) + sizeof(UDPBeaconPayload))) {
        PacketHeader *header = (PacketHeader *)buffer;
        if (header->magic == PACKET_MAGIC &&
            header->type == MessageType::EVENT_UDP_BEACON) {
          UDPBeaconPayload *payload =
              (UDPBeaconPayload *)(buffer + sizeof(PacketHeader));

          char ipStr[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &(senderAddr.sin_addr), ipStr, INET_ADDRSTRLEN);

          char safeServerName[33] = {0};
          memcpy(safeServerName, payload->serverName, 32);

          std::lock_guard<std::mutex> lock(discoveryMutex);
          bool found = false;
          uint64_t currentTick = GetTickCount64();

          for (auto &srv : discoveredServers) {
            if (srv.ip == ipStr) {
              srv.lastUpdateTick = currentTick;
              srv.tcpPort = payload->tcpPort;
              srv.hostname = safeServerName;
              found = true;
              break;
            }
          }
          if (!found) {
            discoveredServers.push_back(
                {ipStr, safeServerName, payload->tcpPort, currentTick});
            if (State::globalDebugMode)
              UI::LogDebug("[Network Client] Discovered Server: %s at %s",
                           safeServerName, ipStr);
          }
        }
      } else {
        if (State::globalDebugMode)
          UI::LogDebug("[Network Client] UDP RX Size Mismatch. Got %d bytes.",
                       bytes);
      }
    }

    {
      std::lock_guard<std::mutex> lock(discoveryMutex);
      uint64_t currentTick = GetTickCount64();
      discoveredServers.erase(
          std::remove_if(discoveredServers.begin(), discoveredServers.end(),
                         [currentTick](const DiscoveredServer &srv) {
                           return (currentTick - srv.lastUpdateTick) > 5000;
                         }),
          discoveredServers.end());
    }
  }
  closesocket(udpListenerSocket);
}

/**
 * @brief Initializes the WinSock2 UDP listener thread to map local Servers.
 * @return void
 */
void StartDiscoveryListener() {
  if (isDiscoveryRunning)
    return;
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  isDiscoveryRunning = true;
  discoveryThread = std::thread([]() {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    DiscoveryLoop();
  });
}

/**
 * @brief Stops the UDP listener thread cleanly.
 * @return void
 */
void StopDiscoveryListener() {
  isDiscoveryRunning = false;
  if (discoveryThread.joinable())
    discoveryThread.join();
  std::lock_guard<std::mutex> lock(discoveryMutex);
  discoveredServers.clear();
  WSACleanup();
}

/**
 * @brief Dedicated thread for handling incoming connectionless UDP packets
 * (Audio/Mouse).
 */
void ClientUDPListenerLoop() {
  int pullBackAccumulator = 0;
  const int PULL_BACK_THRESHOLD = 150;

  std::vector<uint8_t> buffer(MAX_PAYLOAD_SIZE);
  std::vector<uint8_t> decrypted(MAX_PAYLOAD_SIZE);
  sockaddr_in senderAddr;
  int senderAddrSize = sizeof(senderAddr);

  while (isClientRunning && clientUdpSocket != INVALID_SOCKET) {
    int bytes =
        recvfrom(clientUdpSocket, (char *)buffer.data(), (int)buffer.size(), 0,
                 (sockaddr *)&senderAddr, &senderAddrSize);
    if (bytes <= 0)
      continue;

    if (bytes < sizeof(PacketHeader))
      continue;

    // Only accept UDP traffic from the Server we explicitly connected to
    if (senderAddr.sin_addr.s_addr != serverUdpAddr.sin_addr.s_addr)
      continue;

    PacketHeader header;
    memcpy(&header, buffer.data(), sizeof(PacketHeader));

    if (header.magic != PACKET_MAGIC)
      continue;

    // Tolerate minor UDP reordering instead of strictly dropping all seq <= last.
    // Mouse (1000Hz) and audio (100Hz) share the same UDP sequence counter, so
    // audio packets frequently arrive "behind" the latest mouse packet. A window
    // of 32 accepts slightly late packets while still dropping true duplicates
    // or severely stale data. Without this, ~1-5% of audio packets are falsely
    // discarded, causing audible skips.
    const uint32_t UDP_REORDER_WINDOW = 32;
    if (header.sequenceNumber != 0 && udpRxSequence > UDP_REORDER_WINDOW &&
        header.sequenceNumber < udpRxSequence - UDP_REORDER_WINDOW) {
      continue; // Too far behind the high watermark — genuinely stale
    }
    // Advance the high watermark if this is a newer packet
    if (header.sequenceNumber > udpRxSequence) {
      udpRxSequence = header.sequenceNumber;
    }

    size_t payloadSize = header.payloadSize;
    if (payloadSize > MAX_PAYLOAD_SIZE ||
        bytes < sizeof(PacketHeader) + payloadSize)
      continue;

    const uint8_t *rawPayload = buffer.data() + sizeof(PacketHeader);

    if (!Security::DecryptPayload(rawPayload, payloadSize, &header,
                                  sizeof(header), decrypted)) {
      continue; // Decryption failed, drop packet
    }

    if (header.type == MessageType::EVENT_MIC_DATA ||
        header.type == MessageType::EVENT_AUDIO_DATA) {
      if (decrypted.size() > 0) {
        Audio::HandleMicData(decrypted.data(), decrypted.size());
      }
    } else if (header.type == MessageType::EVENT_MOUSE) {
      if (decrypted.size() == sizeof(MousePayload)) {
        MousePayload p;
        memcpy(&p, decrypted.data(), sizeof(p));
        InjectMouseInput(p);

        POINT pt;
        GetCursorPos(&pt);
        int sw = GetSystemMetrics(SM_CXSCREEN),
            sh = GetSystemMetrics(SM_CYSCREEN);

        int deadzoneX = (sw * State::edgeDeadzonePercent) / 100;
        int deadzoneY = (sh * State::edgeDeadzonePercent) / 100;

        bool safeY = pt.y > deadzoneY && pt.y < (sh - deadzoneY);
        bool safeX = pt.x > deadzoneX && pt.x < (sw - deadzoneX);

        uint8_t exitEdge = 255;
        float normX = 0.0f, normY = 0.0f;

        // Calculate the normalized exit coordinates based on which screen edge
        // was breached.
        if (pt.x <= 0 && p.deltaX < 0 && safeY) {
          exitEdge = 0;
          normX = 0.0f;
          normY = (float)pt.y / sh;
        } else if (pt.x >= sw - 1 && p.deltaX > 0 && safeY) {
          exitEdge = 1;
          normX = 1.0f;
          normY = (float)pt.y / sh;
        } else if (pt.y <= 0 && p.deltaY < 0 && safeX) {
          exitEdge = 2;
          normX = (float)pt.x / sw;
          normY = 0.0f;
        } else if (pt.y >= sh - 1 && p.deltaY > 0 && safeX) {
          exitEdge = 3;
          normX = (float)pt.x / sw;
          normY = 1.0f;
        }

        if (exitEdge != 255) {
          pullBackAccumulator +=
              (exitEdge <= 1) ? abs(p.deltaX) : abs(p.deltaY);
          if (pullBackAccumulator > PULL_BACK_THRESHOLD) {
            ReturnControlPayload retPayload = {exitEdge, normX, normY};
            SendToServer(MessageType::EVENT_RETURN_CONTROL, &retPayload,
                         sizeof(retPayload));
            pullBackAccumulator = 0;
          }
        } else {
          pullBackAccumulator = 0;
        }
      }
    }
  }
}

/**
 * @brief Background loop for the Client that continually blocks on recv()
 * parsing incoming network packets.
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

  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] TCP Listener Thread active.");

  while (isClientRunning) {
    PacketHeader h;
    int bytes = recv(clientSocket, (char *)&h, sizeof(h), MSG_WAITALL);

    if (bytes <= 0) {
      int wsaErr = WSAGetLastError();
      if (wsaErr == WSAETIMEDOUT) {
        State::SetClientStatus("Disconnected (Timeout).");
        if (State::globalDebugMode)
          UI::LogDebug("[Network Client] Connection timed out (No Heartbeat "
                       "received). WSA Error: %d",
                       wsaErr);
      } else {
        State::SetClientStatus("Disconnected (Connection Severed).");
        if (State::globalDebugMode)
          UI::LogDebug("[Network Client] Connection severed. WSA Error: %d",
                       wsaErr);
      }
      break;
    }

    if (h.magic != PACKET_MAGIC) {
      State::SetClientStatus("Disconnected (Packet Desync).");
      if (State::globalDebugMode)
        UI::LogDebug("[Network Client] FATAL: Packet desync detected!");
      break;
    }

    // Enforce payload size limits to prevent memory allocation attacks
    // (CWE-400).
    if (h.payloadSize > MAX_PAYLOAD_SIZE) {
      State::SetClientStatus("Disconnected (Payload Size Violation).");
      if (State::globalDebugMode)
        UI::LogDebug("[Network Client] CRITICAL: Payload size exceeds safety "
                     "bounds! Dropping connection.");
      break;
    }

    tls_rawPayload.resize(h.payloadSize);
    if (h.payloadSize > 0) {
      // Ensure full payload is received to avoid processing corrupted state
      // data.
      int pBytes = recv(clientSocket, (char *)tls_rawPayload.data(),
                        h.payloadSize, MSG_WAITALL);
      if (pBytes != static_cast<int>(h.payloadSize)) {
        if (State::globalDebugMode)
          UI::LogDebug("[Network Client] WARNING: Partial payload received. "
                       "(Got %d of %u bytes, Error: %d). Dropping connection.",
                       pBytes, h.payloadSize, WSAGetLastError());
        break;
      }
    }

    if (h.sequenceNumber <= rxSequence && h.sequenceNumber != 0) {
      if (State::globalDebugMode)
        UI::LogDebug("[Security] Replay attack detected. Dropping packet.");
      continue; // Safe to continue since the raw payload was already cleared
                // from the socket buffer.
    }
    rxSequence = h.sequenceNumber;

    const uint8_t *finalPayload = tls_rawPayload.data();
    size_t finalSize = tls_rawPayload.size();

    bool isEncrypted = (h.type != MessageType::EVENT_AUTH) &&
                       (h.type != MessageType::EVENT_UDP_BEACON);

    if (isEncrypted) {
      if (!Security::DecryptPayload(tls_rawPayload.data(),
                                    tls_rawPayload.size(), &h, sizeof(h),
                                    tls_decrypted)) {
        if (State::globalDebugMode)
          UI::LogDebug("[Security] Failed to decrypt server packet. Dropping.");
        continue;
      }
      finalPayload = tls_decrypted.data();
      finalSize = tls_decrypted.size();
    }

    // Filter high-frequency and routine events BEFORE the generic log
    // to prevent flooding the debug window with heartbeat/mouse noise.
    if (h.type == MessageType::EVENT_HEARTBEAT) {
      static int rxHbCount = 0;
      if (++rxHbCount % 10 == 0 && State::globalDebugMode) {
        UI::LogDebug(
            "[Network Client] Received Keep-Alive Heartbeat from Server.");
      }
      continue;
    } else if (h.type == MessageType::EVENT_UDP_HANDSHAKE_ACK) {
      isUdpActive.store(true, std::memory_order_relaxed);
      if (State::globalDebugMode)
        UI::LogDebug(
            "[Network Client] UDP Connection fully established with Server.");
      continue;
    }

    // Log non-routine TCP packets with human-readable type names
    if (State::globalDebugMode && h.type != MessageType::EVENT_MOUSE) {
      const char* typeName = "UNKNOWN";
      switch (h.type) {
        case MessageType::EVENT_STATE:          typeName = "STATE"; break;
        case MessageType::EVENT_SYNC_CURSOR:    typeName = "SYNC_CURSOR"; break;
        case MessageType::EVENT_AUDIO_FORMAT:   typeName = "AUDIO_FORMAT"; break;
        case MessageType::EVENT_AUDIO_DATA:     typeName = "AUDIO_DATA"; break;
        case MessageType::EVENT_MIC_DATA:       typeName = "MIC_DATA"; break;
        case MessageType::EVENT_CLIPBOARD:      typeName = "CLIPBOARD"; break;
        case MessageType::EVENT_FILE_OFFER:     typeName = "FILE_OFFER"; break;
        case MessageType::EVENT_FILE_ACCEPT:    typeName = "FILE_ACCEPT"; break;
        case MessageType::EVENT_FILE_DECLINE:   typeName = "FILE_DECLINE"; break;
        case MessageType::EVENT_CLIENT_LOCKED:  typeName = "CLIENT_LOCKED"; break;
        default: break;
      }
      UI::LogDebug("[Network Client] RX -> %s | %zu bytes", typeName, finalSize);
    }

    if (h.type == MessageType::EVENT_MOUSE) {
      if (finalSize == sizeof(MousePayload)) {
        MousePayload p;
        memcpy(&p, finalPayload, sizeof(p));
        InjectMouseInput(p);

        POINT pt;
        GetCursorPos(&pt);
        int sw = GetSystemMetrics(SM_CXSCREEN),
            sh = GetSystemMetrics(SM_CYSCREEN);

        int deadzoneX = (sw * State::edgeDeadzonePercent) / 100;
        int deadzoneY = (sh * State::edgeDeadzonePercent) / 100;

        bool safeY = pt.y > deadzoneY && pt.y < (sh - deadzoneY);
        bool safeX = pt.x > deadzoneX && pt.x < (sw - deadzoneX);

        uint8_t exitEdge = 255;
        float normX = 0.0f, normY = 0.0f;

        // Calculate the normalized exit coordinates based on which screen edge
        // was breached.
        if (pt.x <= 0 && p.deltaX < 0 && safeY) {
          exitEdge = 0;
          normX = 0.0f;
          normY = (float)pt.y / sh;
        } else if (pt.x >= sw - 1 && p.deltaX > 0 && safeY) {
          exitEdge = 1;
          normX = 1.0f;
          normY = (float)pt.y / sh;
        } else if (pt.y <= 0 && p.deltaY < 0 && safeX) {
          exitEdge = 2;
          normX = (float)pt.x / sw;
          normY = 0.0f;
        } else if (pt.y >= sh - 1 && p.deltaY > 0 && safeX) {
          exitEdge = 3;
          normX = (float)pt.x / sw;
          normY = 1.0f;
        }

        if (exitEdge != 255) {
          pullBackAccumulator +=
              (exitEdge <= 1) ? abs(p.deltaX) : abs(p.deltaY);
          if (pullBackAccumulator > PULL_BACK_THRESHOLD) {
            ReturnControlPayload retPayload = {exitEdge, normX, normY};
            SendToServer(MessageType::EVENT_RETURN_CONTROL, &retPayload,
                         sizeof(retPayload));
            pullBackAccumulator = 0;
          }
        } else {
          pullBackAccumulator = 0;
        }
      }
    } else if (h.type == MessageType::EVENT_KEYBOARD) {
      if (finalSize == sizeof(KeyboardPayload)) {
        KeyboardPayload p;
        memcpy(&p, finalPayload, sizeof(p));
        if (State::enableKeyboardSync) {
          InjectKeyboardInput(p);
        }
      }
    } else if (h.type == MessageType::EVENT_SYNC_CURSOR) {
      if (finalSize == sizeof(CursorSyncPayload)) {
        CursorSyncPayload p;
        memcpy(&p, finalPayload, sizeof(p));
        int sw = GetSystemMetrics(SM_CXSCREEN),
            sh = GetSystemMetrics(SM_CYSCREEN);

        int ty = std::clamp((int)(sh * p.normalizedY), 10, sh - 10);
        int tx = std::clamp((int)(sw * p.normalizedX), 15, sw - 15);
        if (p.entryEdge == 0)
          tx = 15; // Left
        else if (p.entryEdge == 1)
          tx = sw - 15; // Right
        else if (p.entryEdge == 2)
          ty = 15; // Top
        else if (p.entryEdge == 3)
          ty = sh - 15; // Bottom

        SetCursorPos(tx, ty);
        pullBackAccumulator = 0;
      }
    } else if (h.type == MessageType::EVENT_STATE) {
      if (finalSize == sizeof(StatePayload)) {
        StatePayload p;
        memcpy(&p, finalPayload, sizeof(p));
        if (p.mode == 0) { // 0 represents ControlMode::LOCAL
          ReleaseStuckModifiers();
        }
      }
    } else if (h.type == MessageType::EVENT_AUDIO_FORMAT) {
      if (finalSize == sizeof(AudioFormatPayload)) {
        memcpy(&g_serverAudioFormat, finalPayload, sizeof(AudioFormatPayload));
        g_hasServerAudioFormat = true;
        if (State::globalDebugMode)
          UI::LogDebug(
              "[Audio] Received Server Audio Format: %uHz, %u-bit, %uch",
              g_serverAudioFormat.sampleRate, g_serverAudioFormat.bitDepth,
              g_serverAudioFormat.channels);

        // Now that we have the format, start capturing if the user has it
        // enabled.
        if (State::enableClientAudioStream) {
          Audio::StartLoopbackCapture();
        }
        if (State::enableClientMicReceive) {
          Audio::StartMicReceiver();
        }
      }
    } else if (h.type == MessageType::EVENT_CLIPBOARD) {
      if (finalSize > 0) {
        // Convert byte stream back to a wide string and set it locally.
        std::wstring text;
        if (finalSize % sizeof(wchar_t) == 0 && finalSize > 0) {
          text.assign((wchar_t *)finalPayload, finalSize / sizeof(wchar_t));
          if (!text.empty() && text.back() == L'\0') {
            text.pop_back();
          }
        }

        if (!text.empty()) {
          ClipboardManager::SetRemoteClipboard(text);
        }
      }
    } else if (h.type == MessageType::EVENT_MIC_DATA) {
      // Handled in ClientAudioLoop, should not arrive here but ignored safely
      // if it does.
    } else if (h.type == MessageType::EVENT_FILE_OFFER) {
      if (finalSize == sizeof(FileOfferPayload)) {
        FileOfferPayload p;
        memcpy(&p, finalPayload, sizeof(p));
        FileTransfer::HandleFileOffer(clientSocket, p);
      }
    } else if (h.type == MessageType::EVENT_FILE_ACCEPT) {
      if (finalSize == sizeof(FileAcceptPayload)) {
        FileAcceptPayload p;
        memcpy(&p, finalPayload, sizeof(p));
        FileTransfer::HandleFileAccept(clientSocket, p);
      }
    } else if (h.type == MessageType::EVENT_FILE_DECLINE) {
      if (finalSize == sizeof(FileDeclinePayload)) {
        FileDeclinePayload p;
        memcpy(&p, finalPayload, sizeof(p));
        FileTransfer::HandleFileDecline(clientSocket, p);
      }
    }
  }
  isClientRunning = false;
  ReleaseStuckModifiers(); // Catch all hard disconnects or timeouts
  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Listener Thread exiting.");

  uint64_t sessionDuration = GetTickCount64() - connectionStartTime;

  if (!isIntentionalDisconnect && State::enableClientAutoReconnect &&
      !isAutoReconnecting) {
    // Reset backoff if the connection was stable for >30 seconds
    if (sessionDuration > 30000) {
      reconnectBackoffSeconds = 5;
    }

    if (isCurrentSessionAutoReconnected && sessionDuration < 10000) {
      // Increase backoff on rapid failures: 5 -> 10 -> 20 -> 30 (cap)
      reconnectBackoffSeconds = (std::min)(reconnectBackoffSeconds * 2, 30);
      State::SetClientStatus("Auto-reconnect failed. Retrying in " +
                             std::to_string(reconnectBackoffSeconds) + "s...");
      if (State::globalDebugMode)
        UI::LogDebug("[Network Client] Short-lived session (%llums). "
                     "Increasing backoff to %ds.",
                     (unsigned long long)sessionDuration,
                     reconnectBackoffSeconds);
    } else {
      reconnectBackoffSeconds =
          5; // First failure after a stable connection starts at 5s
    }

    isAutoReconnecting = true;
    State::SetClientStatus("Connection dropped. Auto-reconnecting in " +
                           std::to_string(reconnectBackoffSeconds) + "s...");
    if (State::globalDebugMode)
      UI::LogDebug("[Network Client] Connection dropped unintentionally. "
                   "Preparing to auto-reconnect...");

    // Stop audio capture/mic receiver before reconnecting to prevent stale
    // threads from accumulating across reconnect cycles.
    Audio::StopLoopbackCapture();
    Audio::StopMicReceiver();

    if (autoReconnectThread.joinable()) {
      autoReconnectThread.join();
    }

    std::string capturedIp = lastConnectedIp;
    uint16_t capturedPort = lastConnectedPort;
    int capturedBackoff = reconnectBackoffSeconds;
    autoReconnectThread = std::thread([capturedIp, capturedPort,
                                       capturedBackoff]() {
      std::this_thread::sleep_for(std::chrono::seconds(capturedBackoff));
      if (!isIntentionalDisconnect && State::enableClientAutoReconnect) {
        if (State::globalDebugMode)
          UI::LogDebug("[Network Client] Attempting auto-reconnect to %s:%u...",
                       capturedIp.c_str(), capturedPort);
        if (!StartClient(capturedIp, capturedPort, true)) {
          State::SetClientStatus("Auto-reconnect failed. Server unreachable.");
        }
      }
      isAutoReconnecting = false;
    });
  }
}

/**
 * @brief Initializes WinSock2 and connects to the Server's IP and port.
 * @param ip The IPv4 address of the Server PC.
 * @param port The TCP port the Server is listening on.
 * @return true if connected successfully, false otherwise.
 */
bool StartClient(const std::string &ip, uint16_t port, bool isAutoReconnect) {
  if (isClientRunning)
    return false;
  g_hasServerAudioFormat =
      false; // Reset audio format state for the new connection.

  isIntentionalDisconnect = false;
  lastConnectedIp = ip;
  lastConnectedPort = port;
  isCurrentSessionAutoReconnected = isAutoReconnect;
  connectionStartTime = GetTickCount64();

  State::SetClientStatus("Connecting...");

  clientTxSequence = 0; // Reset monotonic counter for new connection
  clientUdpTxSequence = 0;
  udpRxSequence = 0;
  isUdpActive.store(false, std::memory_order_relaxed);

  if (clientThread.joinable()) {
    if (State::globalDebugMode)
      UI::LogDebug("[Network Client] Reaping zombie thread from previous "
                   "connection attempt...");
    clientThread.join();
  }
  if (clientHeartbeatThread.joinable()) {
    clientHeartbeatThread.join();
  }
  if (clientUdpSocket != INVALID_SOCKET) {
    closesocket(clientUdpSocket);
    clientUdpSocket = INVALID_SOCKET;
  }

  if (clientUdpListenerThread.joinable()) {
    clientUdpListenerThread.join();
  }

  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Attempting TCP Connection to %s:%u...",
                 ip.c_str(), port);

  std::string targetPin;
  std::string targetName = "KAMFlow_Server_" + ip;
  if (!CredentialManager::LoadSecret(targetName, targetPin)) {
    State::SetClientStatus("Authentication Failed (No PIN).");
    if (State::globalDebugMode)
      UI::LogDebug("[Security] No paired PIN found for %s. Please pair via UI.",
                   ip.c_str());
    return false;
  }

  // Always reset the crypto engine before re-initializing to ensure the key is
  // derived from the current PIN. Without this, Security::Initialize()
  // early-returns if already initialized (e.g., from a prior auto-reconnect or
  // session), leaving a stale key that causes all encryption/decryption to
  // fail.
  Security::Shutdown();
  if (!Security::Initialize(targetPin.c_str())) {
    State::SetClientStatus("Security Initialization Failed.");
    if (State::globalDebugMode)
      UI::LogDebug(
          "[Security] FATAL: Cryptography Engine failed to initialize.");
    return false;
  }

  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (clientSocket == INVALID_SOCKET)
    return false;

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

  if (connect(clientSocket, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
    State::SetClientStatus("Connection Failed. Server is offline or blocked.");
    if (State::globalDebugMode)
      UI::LogDebug("[Network Client] Connection Failed. Server is offline or "
                   "blocked by firewall.");
    closesocket(clientSocket);
    return false;
  }

  int flag = 1;
  setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
             sizeof(flag));

  int tos = 0xB8; // DSCP 46 (Expedited Forwarding) for Voice/Real-Time priority
  setsockopt(clientSocket, IPPROTO_IP, IP_TOS, (char *)&tos, sizeof(tos));

  // 10-second timeout gives 5× the heartbeat interval as margin,
  // preventing premature disconnects during gaming CPU stalls.
  DWORD timeout = 10000;
  setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));
  DWORD sndTimeout = 10000;
  setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&sndTimeout,
             sizeof(sndTimeout));

  isClientRunning = true;
  AuthPayload p;
  memset(&p, 0, sizeof(p));
  strncpy_s(p.pin, sizeof(p.pin), targetPin.c_str(), _TRUNCATE);

  char hostname[32] = "KAM-Flow-Client";
  DWORD size = sizeof(hostname);
  GetComputerNameA(hostname, &size);
  strncpy_s(p.clientName, sizeof(p.clientName), hostname, _TRUNCATE);

  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Handshake Sent. AES-GCM Active.");

  State::SetClientStatus("Authenticating...");
  SendToServer(MessageType::EVENT_AUTH, &p, sizeof(p));

  // Configure the Server's UDP address
  memset(&serverUdpAddr, 0, sizeof(serverUdpAddr));
  serverUdpAddr.sin_family = AF_INET;
  serverUdpAddr.sin_port =
      htons(port); // Server UDP listener is bound to the same port
  inet_pton(AF_INET, ip.c_str(), &serverUdpAddr.sin_addr);

  // Bind the UDP listener socket
  clientUdpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (clientUdpSocket != INVALID_SOCKET) {
    sockaddr_in localAddr;
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0; // Ephemeral port
    bind(clientUdpSocket, (sockaddr *)&localAddr, sizeof(localAddr));

    // Set SO_RCVTIMEO on the UDP socket so recvfrom() in ClientUDPListenerLoop
    // periodically unblocks and checks isClientRunning, preventing zombie
    // threads when the socket is closed during reconnect.
    DWORD udpTimeout = 500;
    setsockopt(clientUdpSocket, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&udpTimeout, sizeof(udpTimeout));

    clientUdpListenerThread = std::thread([]() {
      // Register with MMCSS to guarantee CPU scheduling for latency-sensitive
      // UDP receives
      DWORD taskIndex = 0;
      HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
      if (!hMmcss) {
        ::SetThreadPriority(::GetCurrentThread(),
                            THREAD_PRIORITY_TIME_CRITICAL);
      } else {
        if (State::globalDebugMode)
          UI::LogDebug(
              "[Network Client] UDPListenerLoop: MMCSS registered (index=%u).",
              taskIndex);
      }
      ClientUDPListenerLoop();
      if (hMmcss)
        AvRevertMmThreadCharacteristics(hMmcss);
    });

    // Send Hole-punching payload to Server
    UDPHandshakePayload udpHandshake;
    strncpy_s(udpHandshake.clientName, sizeof(udpHandshake.clientName),
              hostname, _TRUNCATE);
    SendToServer(MessageType::EVENT_UDP_HANDSHAKE, &udpHandshake,
                 sizeof(udpHandshake));
  }

  State::SetClientStatus("Connected");

  clientThread = std::thread([]() {
    // Elevate thread priority to prevent starvation from the OS scheduler
    // during heavy workloads.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ClientLoop();
  });
  clientHeartbeatThread = std::thread([]() {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    bool wasLocked = false;
    int txHbCount = 0;
    while (isClientRunning) {
      for (int i = 0; i < 20 && isClientRunning; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (isClientRunning) {
        SendHeartbeatToSocket(clientSocket, clientSendMutex);
        if (++txHbCount % 10 == 0 && State::globalDebugMode) {
          UI::LogDebug("[Network Client] Sent Keep-Alive Heartbeat to Server.");
        }

        if (!isUdpActive.load(std::memory_order_relaxed) &&
            clientUdpSocket != INVALID_SOCKET) {
          char hName[MAX_COMPUTERNAME_LENGTH + 1];
          DWORD sSize = sizeof(hName);
          if (GetComputerNameA(hName, &sSize)) {
            UDPHandshakePayload udpHandshake;
            strncpy_s(udpHandshake.clientName, sizeof(udpHandshake.clientName),
                      hName, _TRUNCATE);
            SendToServer(MessageType::EVENT_UDP_HANDSHAKE, &udpHandshake,
                         sizeof(udpHandshake));
          }
        }

        // Secure desktop and UAC prompt failsafe.
        bool isLocked = false;
        HDESK hDesk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (hDesk) {
          char name[256] = {0};
          DWORD needed = 0;
          if (GetUserObjectInformationA(hDesk, UOI_NAME, name, sizeof(name),
                                        &needed)) {
            if (_stricmp(name, "Default") != 0)
              isLocked =
                  true; // Check if a lock screen or Winlogon prompt is active.
          }
          CloseDesktop(hDesk);
        } else {
          isLocked = true; // Access denied indicates a UAC Secure Desktop
                           // prompt is active.
        }

        if (isLocked && !wasLocked) {
          if (State::globalDebugMode)
            UI::LogDebug("[Network Client] UAC/Secure Desktop detected. "
                         "Triggering Server Auto-Revert.");
          SendToServer(MessageType::EVENT_CLIENT_LOCKED, nullptr, 0);
        }
        wasLocked = isLocked;
      }
    }
  });
  return true;
}

/**
 * @brief Notifies the client that the system resumed from sleep/hibernate.
 * Resets the exponential backoff to 1 second so auto-reconnect fires near-instantly.
 * Sleep-triggered disconnects are expected OS behavior and should not be penalized
 * with escalating delays (5→10→20→30s). The user expects seamless resumption.
 * @return void
 */
void NotifySystemResumed() {
  reconnectBackoffSeconds = 1;
  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] System resumed from sleep. Reconnect backoff "
                 "reset to 1s for instant recovery.");
}

/**
 * @brief Safely disconnects from the Server and shuts down the TCP socket.
 * @return void
 */
void StopClient() {
  isIntentionalDisconnect = true;
  State::SetClientStatus("Idle");
  isClientRunning = false;
  isUdpActive.store(false, std::memory_order_relaxed);
  // Ensure audio capture streams are always stopped on disconnect.
  Audio::StopLoopbackCapture();
  Audio::StopMicReceiver();

  if (clientSocket != INVALID_SOCKET) {
    closesocket(clientSocket);
    clientSocket = INVALID_SOCKET;
  }
  if (clientUdpSocket != INVALID_SOCKET) {
    closesocket(clientUdpSocket);
    clientUdpSocket = INVALID_SOCKET;
  }
  if (clientThread.joinable())
    clientThread.join();
  if (clientUdpListenerThread.joinable())
    clientUdpListenerThread.join();
  if (clientHeartbeatThread.joinable())
    clientHeartbeatThread.join();
  if (autoReconnectThread.joinable())
    autoReconnectThread.join();
  Security::Shutdown();
  WSACleanup();
  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Client successfully stopped.");
}
} // namespace Network