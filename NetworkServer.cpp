// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/**
 * @file NetworkServer.cpp
 * @brief Implementation of the WinSock2 TCP Server and UDP Auto-Discovery
 * Beacon.
 */

#include "NetworkServer.h"
#include "AudioManager.h"
#include "ClipboardManager.h"
#include "ConfigManager.h"
#include "CredentialManager.h"
#include "DebugLog.h"
#include "FileTransferManager.h"
#include "MouseCapture.h"
#include "NetworkMessages.h"
#include "SecurityManager.h"
#include "StateManager.h"
#include "UIManager.h"
#include <atomic>
#include <avrt.h>
#include <bcrypt.h>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <ws2tcpip.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "avrt.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace Network {

/// Active TCP listener socket.
SOCKET serverSocket = INVALID_SOCKET;
/// Active UDP listener socket for latency-sensitive streaming.
SOCKET serverUdpSocket = INVALID_SOCKET;
/// Vector containing all active and authenticated client sockets.
std::vector<ConnectedClientInfo> activeClients;
/// Mutex protecting the active clients list.
std::mutex clientsMutex;
/// Per-client send mutexes to prevent interleaved TCP writes without global
/// serialization. Protected by clientsMutex for additions/removals. shared_ptr
/// ensures the mutex stays alive even if a client disconnects while another
/// thread is mid-send (the pointer keeps it valid).
std::map<SOCKET, std::shared_ptr<std::mutex>> clientSendMutexes;
/// Tracks which client socket currently has control (receives mouse/keyboard
/// input). Set when RouteCursorTransition successfully sends a sync to a target
/// client. Reset to INVALID_SOCKET when control returns to LOCAL.
std::atomic<SOCKET> g_controllingClientSocket{INVALID_SOCKET};
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
/// Independent UDP transmit sequence counter to prevent cross-talk replay
/// drops.
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
/// Mutex serializing UDP packet sequence assignment and sendto() independently of per-client TCP locks.
std::mutex serverUdpSendMutex;
/// Tracks whether StartServer acquired a WinSock reference for this session.
static std::atomic<bool> g_serverWsaInitialized{false};
/// Active server TCP/UDP listening port recorded by StartServer.
static std::atomic<uint16_t> g_serverPort{DEFAULT_SERVER_PORT};
/// Signals GetConnectedClients() to immediately refresh its cached snapshot after any client list mutation.
static std::atomic<bool> g_clientsCacheDirty{true};
/// Monotonic server-wide UDP stats accumulated from disconnected/evicted clients so 30s deltas never underflow.
static UdpReplayStats g_serverRetiredUdpStats = {};
/// Lock-free server UDP decrypt and transmit error counters drained every 30s.
static std::atomic<uint64_t> g_serverUdpDecryptFails{0};
static std::atomic<uint64_t> g_serverUdpSendErrors{0};

/**
 * @brief Adds a departing client's UDP replay counters into g_serverRetiredUdpStats.
 * Caller must hold clientsMutex.
 * @param s The departing client's cumulative UDP replay counters.
 * @return void
 */
static void AccumulateRetiredUdpStats(const UdpReplayStats &s) {
  g_serverRetiredUdpStats.packetsAccepted += s.packetsAccepted;
  g_serverRetiredUdpStats.packetsOutOfOrder += s.packetsOutOfOrder;
  g_serverRetiredUdpStats.sequenceGaps += s.sequenceGaps;
  g_serverRetiredUdpStats.staleDrops += s.staleDrops;
  g_serverRetiredUdpStats.duplicateDrops += s.duplicateDrops;
}

/**
 * @brief Retrieves the active Master PIN from memory for display in the UI.
 * @return The 8-digit Master PIN string.
 */
std::string GetMasterPin() { return activeServerPin; }

/**
 * @brief Generates a new Master PIN, saves it to the vault, and instantly
 * disconnects all current clients.
 * @return void
 */
void RegenerateMasterPin() {
  // Generate 8 independent random digits for maximum visual diversity.
  // First digit is 1-9 (no leading zero), remaining 7 are 0-9.
  uint8_t digits[8];
  BCryptGenRandom(NULL, digits, sizeof(digits),
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  char pin[9];
  pin[0] = '1' + (digits[0] % 9);
  for (int i = 1; i < 8; ++i)
    pin[i] = '0' + (digits[i] % 10);
  pin[8] = '\0';
  activeServerPin = pin;
  CredentialManager::SaveSecret("KAMFlow_LocalServer", activeServerPin);

  // Re-derive the AES-GCM key from the new PIN. Without this, the server would
  // continue encrypting with the old key, causing all new client connections to
  // fail.
  Security::Shutdown();
  Security::Initialize(activeServerPin.c_str());

  size_t disconnectedCount = 0;
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    disconnectedCount = activeClients.size();
    for (const auto &client : activeClients) {
      shutdown(client.socket, SD_BOTH);
    }
  }
  if (State::globalDebugMode) {
    UI::LogDebug("[Security] Master PIN regenerated. Disconnecting %zu active client(s).",
                 disconnectedCount);
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
  static std::mutex
      cacheMutex; // Protect the cache itself from concurrent readers
  uint64_t now = GetTickCount64();

  std::lock_guard<std::mutex> cacheLock(cacheMutex);

  if (g_clientsCacheDirty.exchange(false, std::memory_order_acq_rel) ||
      (now - lastCacheTime > 500)) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    cachedClients = activeClients;
    lastCacheTime = now;
  }
  return cachedClients;
}

/**
 * @brief Forcefully disconnects a specific client by safely aborting the
 * connection.
 * @param clientSocket The socket of the client to disconnect.
 * @return void
 */
void DisconnectClient(SOCKET clientSocket) {
  shutdown(clientSocket, SD_BOTH); // Abort the connection safely; the listener
                                   // thread will clean up the socket.
  std::lock_guard<std::mutex> lock(clientsMutex);
  for (const auto &c : activeClients) {
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
  bool found = false;
  bool isEnabled = true;
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (auto &client : activeClients) {
      if (client.socket == clientSocket) {
        client.audioVolume = volume;
        isEnabled = client.isAudioEnabled;
        found = true;
        g_clientsCacheDirty.store(true, std::memory_order_release);
        break;
      }
    }
  }
  if (found) {
    // Sync lock-free audio state outside clientsMutex
    Audio::SetClientAudioState(clientSocket, isEnabled, volume);
  }
}

/**
 * @brief Toggles whether the server mixes and plays audio from a specific
 * client.
 * @param clientSocket The socket of the client.
 * @param enabled True to enable audio, false to mute and clear the queue.
 * @return void
 */
void ToggleClientAudio(SOCKET clientSocket, bool enabled) {
  bool found = false;
  float vol = 1.0f;
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (auto &client : activeClients) {
      if (client.socket == clientSocket) {
        client.isAudioEnabled = enabled;
        vol = client.audioVolume;
        found = true;
        if (!enabled) {
          Audio::ClearClientAudioQueue(clientSocket);
        }
        g_clientsCacheDirty.store(true, std::memory_order_release);
        break;
      }
    }
  }
  if (found && enabled) {
    // Sync lock-free audio state outside clientsMutex when re-enabled
    Audio::SetClientAudioState(clientSocket, true, vol);
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
  for (auto &client : activeClients) {
    if (client.socket == clientSocket) {
      client.gridX = x;
      client.gridY = y;
      Config::SaveClientLayout(client.name, x, y);
      g_clientsCacheDirty.store(true, std::memory_order_release);
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
  for (auto &client : activeClients) {
    if (client.socket == clientSocket) {
      client.isClipboardEnabled = enabled;
      g_clientsCacheDirty.store(true, std::memory_order_release);
      break;
    }
  }
}

/**
 * @brief Retrieves the audio state for a specific client without deep-copying
 * structures.
 * @param clientSocket The socket of the client.
 * @param isEnabled Output parameter receiving whether audio mixing is enabled for the client.
 * @param volume Output parameter receiving the client's current volume multiplier.
 * @return true if the client was found in activeClients, false otherwise.
 */
bool GetClientAudioState(SOCKET clientSocket, bool &isEnabled, float &volume) {
  std::lock_guard<std::mutex> lock(clientsMutex);
  for (const auto &c : activeClients) {
    if (c.socket == clientSocket) {
      isEnabled = c.isAudioEnabled;
      volume = c.audioVolume;
      return true;
    }
  }
  return false;
}

/**
 * @brief Background loop that broadcasts the Server's IP and Port to the local
 * subnet, iterating all active IPv4 interfaces for multi-NIC / virtual adapter resilience.
 * @param tcpPort The port the TCP Server is currently listening on.
 * @return void
 */
void UDPBeaconLoop(uint16_t tcpPort) {
  SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udpSocket == INVALID_SOCKET) {
    if (State::globalDebugMode) {
      UI::LogDebug("[Network Server] ERROR: UDP Beacon socket() failed (WSA Error: %d).",
                   WSAGetLastError());
    }
    return;
  }

  int broadcastEnable = 1;
  setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, (char *)&broadcastEnable,
             sizeof(broadcastEnable));

  sockaddr_in broadcastAddr;
  broadcastAddr.sin_family = AF_INET;
  broadcastAddr.sin_port = htons(DEFAULT_DISCOVERY_PORT);
  broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

  sockaddr_in localhostAddr;
  localhostAddr.sin_family = AF_INET;
  localhostAddr.sin_port = htons(DEFAULT_DISCOVERY_PORT);
  inet_pton(AF_INET, "127.0.0.1", &localhostAddr.sin_addr);

  char hostname[32] = "KAM-Flow-Server";
  DWORD size = sizeof(hostname);
  GetComputerNameA(hostname, &size);

  bool loggedBeaconStart = false;
  bool loggedIoctlFail = false;

  while (isBeaconRunning) {
    PacketHeader header = {PACKET_MAGIC, MessageType::EVENT_UDP_BEACON,
                           sizeof(UDPBeaconPayload), 0};
    UDPBeaconPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy_s(payload.serverName, hostname, _TRUNCATE);
    payload.tcpPort = tcpPort;

    char buffer[sizeof(PacketHeader) + sizeof(UDPBeaconPayload)];
    memcpy(buffer, &header, sizeof(header));
    memcpy(buffer + sizeof(header), &payload, sizeof(payload));

    // Send to each active IPv4 interface's directed subnet broadcast address ((ip & mask) | ~mask)
    // so multi-NIC machines (WSL, Hyper-V, Docker, VPNs) always reach the physical LAN subnet.
    int ifaceCount = 0;
    INTERFACE_INFO ifList[32];
    DWORD bytesReturned = 0;
    if (WSAIoctl(udpSocket, SIO_GET_INTERFACE_LIST, nullptr, 0, ifList,
                 sizeof(ifList), &bytesReturned, nullptr, nullptr) == 0) {
      int numInterfaces = bytesReturned / sizeof(INTERFACE_INFO);
      for (int i = 0; i < numInterfaces; ++i) {
        u_long flags = ifList[i].iiFlags;
        if ((flags & IFF_UP) && !(flags & IFF_LOOPBACK)) {
          uint32_t ip = ifList[i].iiAddress.AddressIn.sin_addr.s_addr;
          uint32_t mask = ifList[i].iiNetmask.AddressIn.sin_addr.s_addr;
          if (ip != 0 && mask != 0) {
            sockaddr_in subnetBroadcast = {0};
            subnetBroadcast.sin_family = AF_INET;
            subnetBroadcast.sin_port = htons(DEFAULT_DISCOVERY_PORT);
            subnetBroadcast.sin_addr.s_addr = (ip & mask) | (~mask);
            sendto(udpSocket, buffer, sizeof(buffer), 0,
                   (sockaddr *)&subnetBroadcast, sizeof(subnetBroadcast));
            ++ifaceCount;
          }
        }
      }
    } else if (!loggedIoctlFail) {
      loggedIoctlFail = true;
      if (State::globalDebugMode) {
        UI::LogDebug("[Network Server] WARNING: WSAIoctl(SIO_GET_INTERFACE_LIST) failed (WSA Error: %d); using global broadcast only.",
                     WSAGetLastError());
      }
    }

    if (!loggedBeaconStart && State::globalDebugMode) {
      loggedBeaconStart = true;
      UI::LogDebug("[Network Server] UDP Discovery Beacon active on %d IPv4 interface(s) (port %u).",
                   ifaceCount, static_cast<unsigned int>(DEFAULT_DISCOVERY_PORT));
    }

    // Fallback global broadcast and loopback for local 1-PC testing
    sendto(udpSocket, buffer, sizeof(buffer), 0, (sockaddr *)&broadcastAddr,
           sizeof(broadcastAddr));
    sendto(udpSocket, buffer, sizeof(buffer), 0, (sockaddr *)&localhostAddr,
           sizeof(localhostAddr));

    for (int i = 0; i < 20 && isBeaconRunning; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  closesocket(udpSocket);
}

/**
 * @brief Reads an exact number of bytes from a TCP socket, handling partial reads.
 * @param sock The connected TCP socket.
 * @param buffer Destination buffer.
 * @param length Exact number of bytes to receive.
 * @param outErr Receives 0 on graceful close, or WSAGetLastError() on failure.
 * @return bool True if all requested bytes were read, false on disconnect or timeout.
 */
static bool RecvExact(SOCKET sock, void *buffer, size_t length, int &outErr) {
  size_t total = 0;
  char *ptr = static_cast<char *>(buffer);
  outErr = 0;
  while (total < length) {
    int r = recv(sock, ptr + total, static_cast<int>(length - total), 0);
    if (r <= 0) {
      outErr = (r == 0) ? 0 : WSAGetLastError();
      return false;
    }
    total += r;
  }
  return true;
}

/**
 * @brief Writes an exact number of bytes to a TCP socket, handling partial sends.
 * @param sock The connected TCP socket.
 * @param buffer Source buffer.
 * @param length Exact number of bytes to send.
 * @return bool True if all bytes were sent, false on socket error or SO_SNDTIMEO timeout.
 */
static bool SendExact(SOCKET sock, const void *buffer, size_t length) {
  // A single send() may write only part of the buffer under congestion or when
  // SO_SNDTIMEO expires mid-write. Leaving the remainder unsent desynchronizes
  // the TCP packet framing and forces the peer to drop the connection.
  size_t total = 0;
  const char *ptr = static_cast<const char *>(buffer);
  while (total < length) {
    int sent = send(sock, ptr + total, static_cast<int>(length - total), 0);
    if (sent <= 0)
      return false;
    total += sent;
  }
  return true;
}

/**
 * @brief Dedicated thread for an authenticated client to listen for return
 * control commands.
 * @param clientSocket The authenticated socket connection.
 * @param initialRxSequence The initial TCP sequence number received during authentication.
 * @param clientName Authenticated client hostname for diagnostics.
 * @param clientIp Authenticated client IPv4 address for diagnostics.
 * @param transportState Lock-free per-session transport flags shared with UDPListenerLoop.
 * @return void
 */
void ClientListenerLoop(SOCKET clientSocket, uint32_t initialRxSequence,
                        std::string clientName, std::string clientIp,
                        std::shared_ptr<ClientTransportState> transportState) {
  uint32_t rxSequence = initialRxSequence;
  static DebugLog::RateLimitState tcpReplayRate;
  static DebugLog::RateLimitState tcpDecryptRate;
  static DebugLog::RateLimitState clipBadRate;
  static DebugLog::RateLimitState retFocusRate;

  thread_local std::vector<uint8_t> tls_rawPayload;
  thread_local std::vector<uint8_t> tls_decrypted;
  tls_rawPayload.reserve(MAX_PAYLOAD_SIZE);
  tls_decrypted.reserve(MAX_PAYLOAD_SIZE);

  while (isRunning) {
    PacketHeader header;
    int errCode = 0;
    if (!RecvExact(clientSocket, &header, sizeof(header), errCode)) {
      if (errCode == WSAETIMEDOUT) {
        if (State::globalDebugMode)
          UI::LogDebug("[Network Server] Client '%s' (%s) timed out (No Heartbeat "
                       "received). WSA Error: %d",
                       clientName.c_str(), clientIp.c_str(), errCode);
      } else {
        if (State::globalDebugMode)
          UI::LogDebug("[Network Server] Client '%s' (%s) socket severed. WSA Error: %d",
                       clientName.c_str(), clientIp.c_str(), errCode);
      }
      break;
    }

    if (header.magic != PACKET_MAGIC) {
      if (State::globalDebugMode)
        UI::LogDebug("[Network Server] FATAL: Packet desync from '%s' (%s): bad magic 0x%08X. Dropping connection.",
                     clientName.c_str(), clientIp.c_str(), header.magic);
      break;
    }

    if (header.payloadSize > MAX_PAYLOAD_SIZE) {
      if (State::globalDebugMode)
        UI::LogDebug("[Network Server] CRITICAL: Payload size (%u bytes) exceeds safety "
                     "bounds from '%s' (%s)! Dropping connection.",
                     header.payloadSize, clientName.c_str(), clientIp.c_str());
      break;
    }

    tls_rawPayload.resize(header.payloadSize);
    if (header.payloadSize > 0) {
      if (!RecvExact(clientSocket, tls_rawPayload.data(), header.payloadSize, errCode)) {
        if (State::globalDebugMode)
          UI::LogDebug(
              "[Network Server] WARNING: Partial payload from '%s' (%s) (Expected %u bytes, Error: %d). Dropping connection.",
              clientName.c_str(), clientIp.c_str(), header.payloadSize, errCode);
        break;
      }
    }

    if (header.sequenceNumber <= rxSequence && header.sequenceNumber != 0) {
      if (State::globalDebugMode) {
        uint32_t suppressed = 0;
        if (tcpReplayRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
          char supBuf[32] = "";
          if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
          UI::LogDebug("[Security] Replay attack / stale TCP packet from '%s' (%s): type=%s seq=%u <= rxSeq=%u%s.",
                       clientName.c_str(), clientIp.c_str(),
                       MessageTypeToString(header.type), header.sequenceNumber, rxSequence, supBuf);
        }
      }
      continue;
    }
    rxSequence = header.sequenceNumber;

    const uint8_t *finalPayload = tls_rawPayload.data();
    size_t finalSize = tls_rawPayload.size();

    bool isEncrypted = (header.type != MessageType::EVENT_AUTH);

    if (isEncrypted) {
      if (!Security::DecryptPayload(tls_rawPayload.data(),
                                    tls_rawPayload.size(), &header,
                                    sizeof(header), tls_decrypted)) {
        if (State::globalDebugMode) {
          uint32_t suppressed = 0;
          if (tcpDecryptRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
            UI::LogDebug("[Security] Failed to decrypt packet from '%s' (%s): type=%s seq=%u size=%zu%s.",
                         clientName.c_str(), clientIp.c_str(),
                         MessageTypeToString(header.type), header.sequenceNumber,
                         tls_rawPayload.size(), supBuf);
          }
        }
        continue;
      }
      finalPayload = tls_decrypted.data();
      finalSize = tls_decrypted.size();
    }

    if (header.type == MessageType::EVENT_HEARTBEAT) {
      continue; // Keep-alive heartbeat acknowledged.
    }

    if (header.type == MessageType::EVENT_AUDIO_DATA) {
      // Discard late in-flight TCP audio packets once UDP is active so stale TCP backlog
      // cannot interleave or stack onto live UDP audio frames.
      const bool udpActive =
          transportState && transportState->udpActive.load(std::memory_order_acquire);
      if (ShouldAcceptTcpAudioPacket(udpActive)) {
        if (transportState) {
          transportState->sawTcpAudio.store(true, std::memory_order_relaxed);
        }
        // Pass the raw PCM data to the audio manager for mixing and playback.
        Audio::HandleAudioData(clientSocket, finalPayload, finalSize);
      }
    } else if (header.type == MessageType::EVENT_CLIPBOARD) {
      if (finalSize > 0 && State::enableClipboardSync) {
        bool clientClipboardAllowed = false;
        {
          std::lock_guard<std::mutex> lock(clientsMutex);
          for (const auto &c : activeClients) {
            if (c.socket == clientSocket) {
              clientClipboardAllowed = c.isClipboardEnabled;
              break;
            }
          }
        }
        if (!clientClipboardAllowed) {
          continue;
        }

        // Convert byte stream back to a wide string and set it locally.
        std::wstring text;
        if (finalSize % sizeof(wchar_t) == 0 && finalSize > 0) {
          text.assign((wchar_t *)finalPayload, finalSize / sizeof(wchar_t));
          // Ensure the string is properly terminated before passing to
          // clipboard
          if (!text.empty() && text.back() == L'\0') {
            text.pop_back();
          }
        } else if (State::globalDebugMode) {
          uint32_t suppressed = 0;
          if (clipBadRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            UI::LogDebug("[Network Server] WARNING: Malformed clipboard payload (%zu bytes) from '%s'.",
                         finalSize, clientName.c_str());
          }
        }

        if (!text.empty()) {
          ClipboardManager::SetRemoteClipboard(text);
          // Relay clipboard update to all other connected clients without echoing back to sender
          BroadcastClipboardMessage(finalPayload, finalSize, clientSocket);
        }
      }
    } else if (header.type == MessageType::EVENT_RETURN_CONTROL) {
      // Gate return control to only accept events from the client that currently holds KVM focus
      if (g_controllingClientSocket.load(std::memory_order_relaxed) != clientSocket) {
        if (State::globalDebugMode) {
          uint32_t suppressed = 0;
          if (retFocusRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), " (+%u suppressed)", suppressed);
            UI::LogDebug("[Network Server] Ignored RETURN_CONTROL from non-controlling client '%s'%s.",
                         clientName.c_str(), supBuf);
          }
        }
        continue;
      }

      if (finalSize == sizeof(ReturnControlPayload)) {
        ReturnControlPayload p;
        memcpy(&p, finalPayload, sizeof(p));

        int sourceX = 0, sourceY = 0;
        {
          std::lock_guard<std::mutex> lock(clientsMutex);
          for (const auto &c : activeClients) {
            if (c.socket == clientSocket) {
              sourceX = c.gridX;
              sourceY = c.gridY;
              break;
            }
          }
        }
        RouteCursorTransition(sourceX, sourceY, p.exitEdge, p.normalizedX,
                              p.normalizedY);
      } else if (State::globalDebugMode) {
        UI::LogDebug("[Network Server] WARNING: Malformed RETURN_CONTROL payload (%zu bytes) from '%s'.",
                     finalSize, clientName.c_str());
      }
    } else if (header.type == MessageType::EVENT_CLIENT_LOCKED) {
      if (State::IsRemote()) {
        if (State::globalDebugMode)
          UI::LogDebug("[Network Server] Client '%s' input locked (UAC / SYSTEM window). Auto-reverting control to Server.",
                       clientName.c_str());
        Input::HandleReturnControl(0, 0.5f, 0.5f);
        g_controllingClientSocket.store(INVALID_SOCKET,
                                        std::memory_order_relaxed);
        State::SetMode(State::ControlMode::LOCAL);
      }
    } else if (header.type == MessageType::EVENT_FILE_OFFER) {
      if (finalSize == sizeof(FileOfferPayload)) {
        FileOfferPayload p;
        memcpy(&p, finalPayload, sizeof(p));
        FileTransfer::HandleFileOffer(clientSocket, p);
      } else if (State::globalDebugMode) {
        UI::LogDebug("[Network Server] WARNING: Malformed FILE_OFFER payload (%zu bytes) from '%s'.",
                     finalSize, clientName.c_str());
      }
    } else if (header.type == MessageType::EVENT_FILE_ACCEPT) {
      if (finalSize == sizeof(FileAcceptPayload)) {
        FileAcceptPayload p;
        memcpy(&p, finalPayload, sizeof(p));
        FileTransfer::HandleFileAccept(clientSocket, p);
      } else if (State::globalDebugMode) {
        UI::LogDebug("[Network Server] WARNING: Malformed FILE_ACCEPT payload (%zu bytes) from '%s'.",
                     finalSize, clientName.c_str());
      }
    } else if (header.type == MessageType::EVENT_FILE_DECLINE) {
      if (finalSize == sizeof(FileDeclinePayload)) {
        FileDeclinePayload p;
        memcpy(&p, finalPayload, sizeof(p));
        FileTransfer::HandleFileDecline(clientSocket, p);
      } else if (State::globalDebugMode) {
        UI::LogDebug("[Network Server] WARNING: Malformed FILE_DECLINE payload (%zu bytes) from '%s'.",
                     finalSize, clientName.c_str());
      }
    }
  }

  if (State::globalDebugMode)
    UI::LogDebug("[Network Server] Client '%s' (%s) disconnected.",
                 clientName.c_str(), clientIp.c_str());

  // Remove the client from the active list under the lock, but release the lock
  // BEFORE calling SetMode or closesocket. SetMode → BroadcastMessage →
  // clientsMutex would deadlock if we still held the lock (std::mutex is
  // non-recursive).
  // Track whether this socket was already evicted by a reconnect. If the
  // same client re-authenticated while this old thread was still blocking on
  // recv(), AcceptClientsLoop already removed our entry and closed our socket.
  bool wasEvicted = true;
  UdpReplayStats finalStats = {};
  bool finalHasUdp = false;
  uint32_t finalTcpStalls = 0;
  uint64_t finalConnectTick = 0;
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (auto it = activeClients.begin(); it != activeClients.end(); ++it) {
      if (it->socket == clientSocket) {
        finalStats = it->udpStats;
        finalHasUdp = it->hasUdp;
        finalTcpStalls = it->tcpStallCount;
        finalConnectTick = it->connectTickMs;
        AccumulateRetiredUdpStats(it->udpStats);
        activeClients.erase(it);
        g_hasClients.store(!activeClients.empty(), std::memory_order_relaxed);
        clientSendMutexes.erase(clientSocket);
        g_clientsCacheDirty.store(true, std::memory_order_release);
        wasEvicted = false;
        break;
      }
    }
  }

  // Only perform cleanup if we weren't already evicted by a reconnect.
  // The eviction path already closed the socket and cleared the audio queue.
  if (!wasEvicted) {
    Audio::ClearClientAudioQueue(clientSocket);
    closesocket(clientSocket);
    if (State::globalDebugMode) {
      double durSec = (finalConnectTick > 0)
                          ? static_cast<double>(GetTickCount64() - finalConnectTick) / 1000.0
                          : 0.0;
      UI::LogDebug("[Session Summary] '%s' (%s) ended after %.1fs | Transport=%s | "
                   "UDP rx=%llu lost=%llu ooo=%llu stale=%llu dup=%llu | TCP stalls=%u",
                   clientName.c_str(), clientIp.c_str(), durSec,
                   finalHasUdp ? "UDP" : "TCP-Fallback",
                   static_cast<unsigned long long>(finalStats.packetsAccepted),
                   static_cast<unsigned long long>(finalStats.sequenceGaps),
                   static_cast<unsigned long long>(finalStats.packetsOutOfOrder),
                   static_cast<unsigned long long>(finalStats.staleDrops),
                   static_cast<unsigned long long>(finalStats.duplicateDrops),
                   finalTcpStalls);
    }
  }

  // CRITICAL SAFETY: If the disconnected client was the one that had control,
  // immediately revert to LOCAL to prevent input lockout.
  // Only revert for the controlling client — other clients disconnecting
  // should not affect the active KVM session.
  // Skip if evicted: the reconnect path already cleared g_controllingClientSocket
  // if needed, and the NEW connection may have already taken control.
  if (!wasEvicted && State::IsRemote() &&
      g_controllingClientSocket.load(std::memory_order_relaxed) ==
          clientSocket) {
    if (State::globalDebugMode)
      UI::LogDebug("[Network Server] Controlling client '%s' (%s) dropped. "
                   "Auto-reverting to LOCAL.",
                   clientName.c_str(), clientIp.c_str());
    g_controllingClientSocket.store(INVALID_SOCKET, std::memory_order_relaxed);
    State::SetMode(State::ControlMode::LOCAL);
  }
}

/**
 * @brief Main listener loop that accepts incoming TCP connections and handles
 * the authentication handshake.
 * @return void
 */
void AcceptClientsLoop() {
  while (isRunning) {
    // Clean up and reap finished client listener threads.
    {
      std::lock_guard<std::mutex> lock(clientThreadsListMutex);
      for (auto it = activeClientThreadsList.begin();
           it != activeClientThreadsList.end();) {
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
    SOCKET clientSocket =
        accept(serverSocket, (sockaddr *)&clientAddr, &clientAddrSize);

    if (clientSocket != INVALID_SOCKET) {
      // Allow 5s for encrypted AUTH to absorb post-wake Wi-Fi TCP retransmissions.
      DWORD timeout = PRE_AUTH_RECV_TIMEOUT_MS;
      setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
                 sizeof(timeout));

      char clientIpStr[INET_ADDRSTRLEN] = "Unknown";
      inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIpStr, INET_ADDRSTRLEN);
      int clientPort = ntohs(clientAddr.sin_port);

      if (State::globalDebugMode)
        UI::LogDebug("[Network Server] Client connection accepted from %s:%d. Awaiting AUTH...",
                     clientIpStr, clientPort);

      PacketHeader header;
      int errCode = 0;
      if (!RecvExact(clientSocket, &header, sizeof(header), errCode)) {
        if (State::globalDebugMode) {
          if (errCode == 0) {
            UI::LogDebug("[Network Server] Auth failed for %s:%d: client disconnected before sending header.",
                         clientIpStr, clientPort);
          } else {
            UI::LogDebug("[Network Server] Auth failed for %s:%d: header recv failed (WSA Error: %d).",
                         clientIpStr, clientPort, errCode);
          }
        }
      } else if (header.magic != PACKET_MAGIC) {
        if (State::globalDebugMode) {
          // Detect common HTTP probes (e.g., 0x20544547 is "GET ", 0x54534F50 is "POST")
          // and log them cleanly without alarming "Auth failed" messages.
          if (header.magic == 0x20544547 || header.magic == 0x54534F50) {
            UI::LogDebug("[Network Server] HTTP probe ignored from %s:%d (received HTTP verb on port %u).",
                         clientIpStr, clientPort,
                         static_cast<unsigned int>(g_serverPort.load(std::memory_order_relaxed)));
          } else {
            UI::LogDebug("[Network Server] Auth failed for %s:%d: bad magic 0x%08X (expected 0x%08X).",
                         clientIpStr, clientPort, header.magic, PACKET_MAGIC);
          }
        }
      } else if (header.type != MessageType::EVENT_AUTH) {
        if (State::globalDebugMode) {
          UI::LogDebug("[Network Server] Auth failed for %s:%d: unexpected message type %u (expected EVENT_AUTH).",
                       clientIpStr, clientPort, static_cast<unsigned int>(header.type));
        }
      } else if (header.payloadSize < sizeof(AuthPayload) + 28 ||
                 header.payloadSize > MAX_PAYLOAD_SIZE) {
        if (State::globalDebugMode) {
          UI::LogDebug("[Network Server] Auth failed for %s:%d: invalid encrypted payload size %u (expected %zu..%u).",
                       clientIpStr, clientPort, header.payloadSize, sizeof(AuthPayload) + 28, MAX_PAYLOAD_SIZE);
        }
      } else {
        std::vector<uint8_t> authBuffer(header.payloadSize);
        if (!RecvExact(clientSocket, authBuffer.data(), header.payloadSize, errCode)) {
          if (State::globalDebugMode) {
            UI::LogDebug("[Network Server] Auth failed for %s:%d: payload recv failed (WSA Error: %d).",
                         clientIpStr, clientPort, errCode);
          }
        } else {
          // Decrypt the EVENT_AUTH payload using the key derived from activeServerPin.
          // If the client used an incorrect PIN, AES-GCM authentication tag verification
          // fails here without the PIN ever traversing the network in plaintext.
          std::vector<uint8_t> decryptedAuth;
          if (!Security::DecryptPayload(authBuffer.data(), authBuffer.size(), &header, sizeof(header), decryptedAuth) ||
              decryptedAuth.size() < sizeof(AuthPayload)) {
            if (State::globalDebugMode) {
              UI::LogDebug("[Network Server] Auth rejected for %s:%d: AES-GCM tag mismatch (wrong Master PIN).",
                           clientIpStr, clientPort);
            }
            // Notify client explicitly so it halts wrong-PIN auto-reconnect loops immediately
            PacketHeader rejectHdr = {PACKET_MAGIC, MessageType::EVENT_AUTH_REJECTED, 0, 0};
            SendExact(clientSocket, &rejectHdr, sizeof(rejectHdr));
            // Brief rate-limiting delay to throttle online PIN brute-force attempts.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          } else {
            AuthPayload auth;
            memcpy(&auth, decryptedAuth.data(), sizeof(AuthPayload));
            auth.clientName[sizeof(auth.clientName) - 1] = '\0';

            if (auth.authVerifier != PACKET_MAGIC || auth.sessionToken == 0) {
              if (State::globalDebugMode) {
                UI::LogDebug("[Network Server] Auth rejected for client '%s' (%s:%d): verifier mismatch.",
                             auth.clientName, clientIpStr, clientPort);
              }
              PacketHeader rejectHdr = {PACKET_MAGIC, MessageType::EVENT_AUTH_REJECTED, 0, 0};
              SendExact(clientSocket, &rejectHdr, sizeof(rejectHdr));
              std::this_thread::sleep_for(std::chrono::milliseconds(200));
            } else {
              if (State::globalDebugMode)
                UI::LogDebug("[Network Server] Client Authenticated: '%s' from %s:%d.",
                             auth.clientName, clientIpStr, clientPort);

              // 10-second timeout gives 5× the heartbeat interval as margin,
              // preventing premature disconnects during gaming CPU stalls.
              timeout = 10000;
              setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO,
                         (const char *)&timeout, sizeof(timeout));
              DWORD sndTimeout = 10000;
              setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO,
                         (const char *)&sndTimeout, sizeof(sndTimeout));

              int flag = 1;
              setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                         sizeof(flag));

              char ipStr[INET_ADDRSTRLEN];
              inet_ntop(AF_INET, &(clientAddr.sin_addr), ipStr,
                        INET_ADDRSTRLEN);

              char safeName[33] = {0};
              memcpy(safeName, auth.clientName, 32);

              // --- Evict stale duplicate connections for the same machine ---
              // When a client auto-reconnects, the OLD ClientListenerLoop
              // thread may still be alive (blocking on recv() with
              // SO_RCVTIMEO). The old entry would make the saved grid position
              // appear "occupied", forcing the radial search to place the
              // reconnecting client diagonally. Evicting the old entry ensures
              // seamless position inheritance on reconnect.
              {
                std::lock_guard<std::mutex> lock(clientsMutex);
                for (auto it = activeClients.begin();
                     it != activeClients.end(); ++it) {
                  if (it->name == safeName &&
                      it->socket != clientSocket) {
                    SOCKET oldSocket = it->socket;
                    AccumulateRetiredUdpStats(it->udpStats);
                    activeClients.erase(it);
                    clientSendMutexes.erase(oldSocket);
                    g_clientsCacheDirty.store(true, std::memory_order_release);
                    // Force the old ClientListenerLoop to exit by aborting its
                    // socket. The old thread's cleanup will harmlessly no-op
                    // since the entry is already removed.
                    shutdown(oldSocket, SD_BOTH);
                    closesocket(oldSocket);
                    Audio::ClearClientAudioQueue(oldSocket);
                    // If the evicted socket had KVM control, clear it so the
                    // new connection can take over cleanly.
                    if (g_controllingClientSocket.load(
                            std::memory_order_relaxed) == oldSocket) {
                      g_controllingClientSocket.store(
                          INVALID_SOCKET, std::memory_order_relaxed);
                    }
                    if (State::globalDebugMode)
                      UI::LogDebug(
                          "[Network Server] Evicted stale connection for %s "
                          "(old socket replaced by reconnect).",
                          safeName);
                    break;
                  }
                }
              }

              int targetX = 0, targetY = 0;
              bool loaded =
                  Config::LoadClientLayout(safeName, targetX, targetY);
              bool usedSavedLayout = false;

              // Register the authenticated session in activeClients BEFORE Slow/Blocking WASAPI
              // format queries so the Client's immediate UDP handshake packet always matches.
              auto transportState = std::make_shared<ClientTransportState>();
              {
                std::lock_guard<std::mutex> lock(clientsMutex);
                bool occupied =
                    (targetX == 0 &&
                     targetY == 0); // Coordinate (0,0) is explicitly reserved
                                    // for the Server.
                if (loaded && !occupied) {
                  for (const auto &c : activeClients) {
                    if (c.gridX == targetX && c.gridY == targetY) {
                      occupied = true;
                      break;
                    }
                  }
                }
                usedSavedLayout = (loaded && !occupied);
                // Perform a radial search to auto-assign the nearest empty slot
                // to the Server.
                if (!loaded || occupied) {
                  bool found = false;
                  for (int radius = 1; radius < 10 && !found; ++radius) {
                    for (int dx = -radius; dx <= radius && !found; ++dx) {
                      for (int dy = -radius; dy <= radius && !found; ++dy) {
                        if (abs(dx) != radius && abs(dy) != radius)
                          continue;

                        // Only auto-place in slots orthogonally adjacent to the
                        // Server or an existing Client
                        bool isAdjacent = (abs(dx) + abs(dy) == 1);
                        for (const auto &c : activeClients) {
                          if (abs(c.gridX - dx) + abs(c.gridY - dy) == 1) {
                            isAdjacent = true;
                            break;
                          }
                        }
                        if (!isAdjacent)
                          continue;

                        bool isOccupied = false;
                        for (const auto &c : activeClients) {
                          if (c.gridX == dx && c.gridY == dy) {
                            isOccupied = true;
                            break;
                          }
                        }
                        if (!isOccupied) {
                          targetX = dx;
                          targetY = dy;
                          found = true;
                        }
                      }
                    }
                  }
                }
                sockaddr_in emptyUdpAddr;
                memset(&emptyUdpAddr, 0, sizeof(emptyUdpAddr));
                activeClients.push_back({clientSocket, emptyUdpAddr, false,
                                         ipStr, safeName, 1.0f, true, true,
                                         targetX, targetY, 0, 0, auth.sessionToken});
                activeClients.back().connectTickMs = GetTickCount64();
                activeClients.back().transportState = transportState;
                g_hasClients.store(true, std::memory_order_relaxed);
                clientSendMutexes[clientSocket] =
                    std::make_shared<std::mutex>();
                g_clientsCacheDirty.store(true, std::memory_order_release);
              }

              // Persist layout and initialize lock-free audio state outside clientsMutex.
              Config::SaveClientLayout(safeName, targetX, targetY);
              Audio::SetClientAudioState(clientSocket, true, 1.0f);

              if (State::globalDebugMode) {
                UI::LogDebug("[Network Server] Placed '%s' at grid (%d, %d) (%s).",
                             safeName, targetX, targetY,
                             usedSavedLayout ? "saved" : "auto-placed");
              }

              // Send the server's native audio format via SendToClient (serialized cleanly
              // with concurrent EVENT_UDP_HANDSHAKE_ACK replies on the client send mutex).
              {
                Audio::AudioFormat serverFormat;
                if (Audio::GetDefaultDeviceFormat(serverFormat)) {
                  AudioFormatPayload wireFmt = {};
                  wireFmt.sampleRate = serverFormat.sampleRate;
                  wireFmt.bitDepth = serverFormat.bitDepth;
                  wireFmt.channels = serverFormat.channels;
                  wireFmt.codec = serverFormat.codec;
                  if (!SendToClient(clientSocket, MessageType::EVENT_AUDIO_FORMAT,
                                    &wireFmt, sizeof(wireFmt)) &&
                      State::globalDebugMode) {
                    UI::LogDebug("[Network Server] WARNING: Failed to send audio format handshake to '%s' (WSA Error: %d).",
                                 safeName, WSAGetLastError());
                  }
                } else if (State::globalDebugMode) {
                  UI::LogDebug("[Network Server] WARNING: GetDefaultDeviceFormat failed; audio format handshake skipped for '%s'.",
                               safeName);
                }
              }

              auto threadInfo = std::make_shared<ClientThreadInfo>();
              threadInfo->thread = std::thread(
                  [clientSocket, seq = header.sequenceNumber, threadInfo,
                   nameStr = std::string(safeName), ipCopy = std::string(ipStr),
                   transportState]() {
                    ::SetThreadPriority(::GetCurrentThread(),
                                        THREAD_PRIORITY_TIME_CRITICAL);
                    ClientListenerLoop(clientSocket, seq, nameStr, ipCopy,
                                       transportState);
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
 * @brief Listener thread for connectionless UDP streaming (Mouse, Audio, and Return Control).
 * @return void
 */
void UDPListenerLoop() {
  std::vector<uint8_t> buffer(MAX_PAYLOAD_SIZE);
  std::vector<uint8_t> decrypted(MAX_PAYLOAD_SIZE);
  sockaddr_in senderAddr;
  int senderAddrSize = sizeof(senderAddr);

  static DebugLog::RateLimitState udpRecvErrRate;
  static DebugLog::RateLimitState udpDecryptRate;
  static DebugLog::RateLimitState udpHsMismatchRate;
  static DebugLog::RateLimitState udpUnrecogRate;

  while (isRunning) {
    int bytes =
        recvfrom(serverUdpSocket, (char *)buffer.data(), (int)buffer.size(), 0,
                 (sockaddr *)&senderAddr, &senderAddrSize);
    if (bytes <= 0) {
      if (bytes == SOCKET_ERROR && isRunning) {
        int err = WSAGetLastError();
        if (err != WSAETIMEDOUT && State::globalDebugMode) {
          uint32_t suppressed = 0;
          if (udpRecvErrRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
            UI::LogDebug("[Network Server] UDP recvfrom error (WSA Error: %d%s).", err, supBuf);
          }
        }
      }
      continue;
    }

    if (bytes < sizeof(PacketHeader))
      continue;

    PacketHeader header;
    memcpy(&header, buffer.data(), sizeof(PacketHeader));

    if (header.magic != PACKET_MAGIC)
      continue;

    size_t payloadSize = header.payloadSize;
    if (payloadSize > MAX_PAYLOAD_SIZE ||
        bytes < sizeof(PacketHeader) + payloadSize)
      continue;

    const uint8_t *rawPayload = buffer.data() + sizeof(PacketHeader);

    if (!Security::DecryptPayload(rawPayload, payloadSize, &header,
                                  sizeof(header), decrypted)) {
      g_serverUdpDecryptFails.fetch_add(1, std::memory_order_relaxed);
      if (State::globalDebugMode) {
        uint32_t suppressed = 0;
        if (udpDecryptRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
          char supBuf[32] = "";
          if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
          UI::LogDebug("[Security] Server UDP decrypt failed: type=%s seq=%u size=%zu%s.",
                       MessageTypeToString(header.type), header.sequenceNumber,
                       payloadSize, supBuf);
        }
      }
      continue; // Decryption failed, drop packet
    }

    if (header.type == MessageType::EVENT_UDP_HANDSHAKE) {
      if (decrypted.size() == sizeof(UDPHandshakePayload)) {
        UDPHandshakePayload p;
        memcpy(&p, decrypted.data(), sizeof(p));
        // Safely ensure null termination
        p.clientName[sizeof(p.clientName) - 1] = '\0';

        char senderIpStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &(senderAddr.sin_addr), senderIpStr, INET_ADDRSTRLEN);

        SOCKET ackSocket = INVALID_SOCKET;
        bool wasUdpActive = false;
        std::shared_ptr<ClientTransportState> matchedTransport;
        {
          std::lock_guard<std::mutex> lock(clientsMutex);
          for (auto &c : activeClients) {
            // Verify hostname, IP address, AND the cryptographic 64-bit session token
            // negotiated during the encrypted TCP handshake to prevent UDP stream hijacking.
            if (c.ip == senderIpStr &&
                c.sessionToken != 0 &&
                c.sessionToken == p.sessionToken &&
                strncmp(c.name.c_str(), p.clientName, 32) == 0) {
              if (!VerifyUdpReplayWindow(header.sequenceNumber, c.udpRxSequence,
                                         c.udpReplayMask, &c.udpStats)) {
                break;
              }
              wasUdpActive = c.hasUdp;
              matchedTransport = c.transportState;
              c.udpAddr = senderAddr;
              c.hasUdp = true;
              if (matchedTransport) {
                matchedTransport->udpActive.store(true, std::memory_order_release);
              }
              ackSocket = c.socket;
              if (State::globalDebugMode)
                UI::LogDebug("[Network Server] UDP Stream Handshake matched "
                             "for %s (%s). Connection verified.",
                             p.clientName, senderIpStr);
              break;
            }
          }
        } // Release mutex before calling ResetClientAudioQueue / SendToClient

        if (ackSocket != INVALID_SOCKET) {
          // Flush any pre-UDP TCP fallback audio backlog before switching to live UDP frames.
          if (!wasUdpActive && matchedTransport &&
              matchedTransport->sawTcpAudio.exchange(false, std::memory_order_acq_rel)) {
            Audio::ResetClientAudioQueue(ackSocket);
          }
          SendToClient(ackSocket, MessageType::EVENT_UDP_HANDSHAKE_ACK, nullptr,
                       0);
        } else if (State::globalDebugMode) {
          uint32_t suppressed = 0;
          if (udpHsMismatchRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            UI::LogDebug("[Network Server] UDP handshake mismatch from %s (client '%s', token=%llu) — no matching active TCP session.",
                         senderIpStr, p.clientName, static_cast<unsigned long long>(p.sessionToken));
          }
        }
      }
      continue;
    }

    SOCKET matchingSocket = INVALID_SOCKET;
    int sourceX = 0, sourceY = 0;

    {
      std::lock_guard<std::mutex> lock(clientsMutex);
      for (auto &c : activeClients) {
        if (c.hasUdp &&
            c.udpAddr.sin_addr.s_addr == senderAddr.sin_addr.s_addr &&
            c.udpAddr.sin_port == senderAddr.sin_port) {
          // Enforce 64-bit sliding bitmask anti-replay window (RFC 6479 style).
          // Tolerates minor UDP out-of-order delivery across audio and control
          // streams while strictly rejecting duplicate replays within the window.
          if (!VerifyUdpReplayWindow(header.sequenceNumber, c.udpRxSequence,
                                     c.udpReplayMask, &c.udpStats)) {
            matchingSocket = (SOCKET)-1; // Flag as stale or replayed drop
            break;
          }
          matchingSocket = c.socket;
          sourceX = c.gridX;
          sourceY = c.gridY;
          break;
        }
      }
    }

    if (matchingSocket == INVALID_SOCKET) {
      if (State::globalDebugMode) {
        uint32_t suppressed = 0;
        if (udpUnrecogRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
          char senderIpStr[INET_ADDRSTRLEN] = "Unknown";
          inet_ntop(AF_INET, &(senderAddr.sin_addr), senderIpStr, INET_ADDRSTRLEN);
          char supBuf[32] = "";
          if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
          UI::LogDebug("[Network Server] Dropped UDP packet from unrecognized endpoint %s:%u (type=%s%s).",
                       senderIpStr, ntohs(senderAddr.sin_port),
                       MessageTypeToString(header.type), supBuf);
        }
      }
      continue;
    }
    if (matchingSocket == (SOCKET)-1)
      continue;

    if (header.type == MessageType::EVENT_AUDIO_DATA ||
        header.type == MessageType::EVENT_MIC_DATA) {
      Audio::HandleAudioData(matchingSocket, decrypted.data(),
                             decrypted.size());
    } else if (header.type == MessageType::EVENT_RETURN_CONTROL) {
      // Gate UDP return control to only accept events from the actively controlling client
      if (g_controllingClientSocket.load(std::memory_order_relaxed) != matchingSocket) {
        continue;
      }

      if (decrypted.size() == sizeof(ReturnControlPayload)) {
        ReturnControlPayload p;
        memcpy(&p, decrypted.data(), sizeof(p));
        // Route the cursor dynamically based on the client's grid coordinates
        RouteCursorTransition(sourceX, sourceY, p.exitEdge, p.normalizedX,
                              p.normalizedY);
      }
    }
  }
}

/**
 * @brief Initializes WinSock2, binds the socket, and starts the background
 * listening thread.
 * @param port The TCP port to listen on.
 * @return true if the server started successfully, false otherwise.
 */
bool StartServer(uint16_t port) {
  serverTxSequence = 0; // Reset monotonic counter for new sessions
  serverUdpTxSequence = 0;
  g_serverUdpDecryptFails.store(0, std::memory_order_relaxed);
  g_serverUdpSendErrors.store(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    g_serverRetiredUdpStats = {};
  }

  if (!CredentialManager::LoadSecret("KAMFlow_LocalServer", activeServerPin)) {
    // First boot: Generate a secure 8-digit random PIN (per-digit, no leading
    // zero)
    uint8_t digits[8];
    BCryptGenRandom(NULL, digits, sizeof(digits),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    char pin[9];
    pin[0] = '1' + (digits[0] % 9);
    for (int i = 1; i < 8; ++i)
      pin[i] = '0' + (digits[i] % 10);
    pin[8] = '\0';
    activeServerPin = pin;
    CredentialManager::SaveSecret("KAMFlow_LocalServer", activeServerPin);
    if (State::globalDebugMode)
      UI::LogDebug("[Security] Generated new 8-digit Master PIN and stored in Credential Vault.");
  }

  // Reset the crypto engine before re-init to handle server restarts cleanly.
  // Security::Initialize() early-returns if already initialized, leaving a
  // stale key.
  Security::Shutdown();
  if (!Security::Initialize(activeServerPin.c_str())) {
    if (State::globalDebugMode)
      UI::LogDebug(
          "[Security] FATAL: Cryptography Engine failed to initialize.");
    return false;
  }

  if (!g_serverWsaInitialized.exchange(true)) {
    WSADATA wsa;
    int wsaErr = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wsaErr != 0) {
      g_serverWsaInitialized.store(false);
      if (State::globalDebugMode) {
        UI::LogDebug("[Network Server] ERROR: WSAStartup failed (%d).", wsaErr);
      }
      return false;
    }
  }
  serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (serverSocket == INVALID_SOCKET) {
    if (State::globalDebugMode) {
      UI::LogDebug("[Network Server] ERROR: TCP socket() failed (WSA Error: %d).", WSAGetLastError());
    }
    if (g_serverWsaInitialized.exchange(false)) WSACleanup();
    return false;
  }

  sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(serverSocket, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
    if (State::globalDebugMode) {
      UI::LogDebug("[Network Server] ERROR: TCP bind() failed on port %u (WSA Error: %d).",
                   port, WSAGetLastError());
    }
    closesocket(serverSocket);
    serverSocket = INVALID_SOCKET;
    if (g_serverWsaInitialized.exchange(false)) WSACleanup();
    return false;
  }
  if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
    if (State::globalDebugMode) {
      UI::LogDebug("[Network Server] ERROR: TCP listen() failed on port %u (WSA Error: %d).",
                   port, WSAGetLastError());
    }
    closesocket(serverSocket);
    serverSocket = INVALID_SOCKET;
    if (g_serverWsaInitialized.exchange(false)) WSACleanup();
    return false;
  }

  // Ensure accept() can be interrupted by closesocket() during shutdown
  DWORD acceptTimeout = 1000;
  setsockopt(serverSocket, SOL_SOCKET, SO_RCVTIMEO,
             (const char *)&acceptTimeout, sizeof(acceptTimeout));

  serverUdpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (serverUdpSocket == INVALID_SOCKET) {
    if (State::globalDebugMode) {
      UI::LogDebug("[Network Server] ERROR: UDP socket() failed (WSA Error: %d).", WSAGetLastError());
    }
    closesocket(serverSocket);
    serverSocket = INVALID_SOCKET;
    if (g_serverWsaInitialized.exchange(false)) WSACleanup();
    return false;
  }
  if (bind(serverUdpSocket, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
    if (State::globalDebugMode) {
      UI::LogDebug("[Network Server] ERROR: UDP bind() failed on port %u (WSA Error: %d).",
                   port, WSAGetLastError());
    }
    closesocket(serverUdpSocket);
    serverUdpSocket = INVALID_SOCKET;
    closesocket(serverSocket);
    serverSocket = INVALID_SOCKET;
    if (g_serverWsaInitialized.exchange(false)) WSACleanup();
    return false;
  }

  // Prevent indefinite blocking on recvfrom() to allow clean shutdown
  DWORD udpTimeout = 500;
  setsockopt(serverUdpSocket, SOL_SOCKET, SO_RCVTIMEO,
             (const char *)&udpTimeout, sizeof(udpTimeout));

  // 512 KB kernel buffer prevents UDP drops under simultaneous 1000Hz mouse + 48kHz audio bursts
  int udpBufSize = 524288;
  setsockopt(serverUdpSocket, SOL_SOCKET, SO_RCVBUF,
             (const char *)&udpBufSize, sizeof(udpBufSize));
  setsockopt(serverUdpSocket, SOL_SOCKET, SO_SNDBUF,
             (const char *)&udpBufSize, sizeof(udpBufSize));

  // Suppress Windows ICMP Port Unreachable (WSAECONNRESET) errors on shared UDP socket
  BOOL bNewBehavior = FALSE;
  DWORD dwBytesReturned = 0;
  WSAIoctl(serverUdpSocket, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior),
           nullptr, 0, &dwBytesReturned, nullptr, nullptr);

  g_serverPort.store(port, std::memory_order_relaxed);
  isRunning = true;
  serverThread = std::thread([]() {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    AcceptClientsLoop();
  });

  udpListenerThread = std::thread([]() {
    DWORD taskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!hMmcss) {
      ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    } else if (State::globalDebugMode) {
      UI::LogDebug("[Network Server] UDPListenerLoop: MMCSS registered (index=%u).", taskIndex);
    }
    UDPListenerLoop();
    if (hMmcss) {
      AvRevertMmThreadCharacteristics(hMmcss);
    }
  });

  isBeaconRunning = true;
  beaconThread = std::thread(UDPBeaconLoop, port);

  serverHeartbeatThread = std::thread([]() {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    int heartbeatCycles = 0;
    UdpReplayStats prevAgg = {};
    while (isRunning) {
      for (int i = 0; i < 20 && isRunning; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!isRunning) break;

      BroadcastMessage(MessageType::EVENT_HEARTBEAT, nullptr, 0);

      if (++heartbeatCycles >= 15) {
        heartbeatCycles = 0;
        UdpReplayStats currAgg = {};
        {
          std::lock_guard<std::mutex> lock(clientsMutex);
          currAgg = g_serverRetiredUdpStats;
          for (const auto &c : activeClients) {
            currAgg.packetsAccepted += c.udpStats.packetsAccepted;
            currAgg.packetsOutOfOrder += c.udpStats.packetsOutOfOrder;
            currAgg.sequenceGaps += c.udpStats.sequenceGaps;
            currAgg.staleDrops += c.udpStats.staleDrops;
            currAgg.duplicateDrops += c.udpStats.duplicateDrops;
          }
        }
        uint64_t decFail = g_serverUdpDecryptFails.exchange(0, std::memory_order_relaxed);
        uint64_t txErr = g_serverUdpSendErrors.exchange(0, std::memory_order_relaxed);

        auto deltaU64 = [](uint64_t cur, uint64_t prev) -> uint64_t {
          return (cur >= prev) ? (cur - prev) : 0;
        };
        uint64_t dRx = deltaU64(currAgg.packetsAccepted, prevAgg.packetsAccepted);
        uint64_t dOoo = deltaU64(currAgg.packetsOutOfOrder, prevAgg.packetsOutOfOrder);
        uint64_t dLost = deltaU64(currAgg.sequenceGaps, prevAgg.sequenceGaps);
        uint64_t dStale = deltaU64(currAgg.staleDrops, prevAgg.staleDrops);
        uint64_t dDup = deltaU64(currAgg.duplicateDrops, prevAgg.duplicateDrops);
        prevAgg = currAgg;

        if ((ShouldEmitPerfSummary(dLost + dStale + dDup + decFail + txErr) || dOoo > 50) &&
            State::globalDebugMode) {
          UI::LogDebug("[Network Perf] Server UDP 30s: rx=%llu lost=%llu ooo=%llu stale=%llu dup=%llu decFail=%llu txErr=%llu",
                       static_cast<unsigned long long>(dRx),
                       static_cast<unsigned long long>(dLost),
                       static_cast<unsigned long long>(dOoo),
                       static_cast<unsigned long long>(dStale),
                       static_cast<unsigned long long>(dDup),
                       static_cast<unsigned long long>(decFail),
                       static_cast<unsigned long long>(txErr));
        }
      }
    }
  });

  if (State::globalDebugMode) {
    UI::LogDebug("[Network Server] Listening on TCP & UDP port %u.", port);
  }
  return true;
}

/**
 * @brief Shuts down the server, disconnects all clients, and cleans up WinSock
 * resources.
 * @return void
 */
void StopServer() {
  isRunning = false;
  isBeaconRunning = false;

  // Clear control tracking to prevent stale state on restart
  g_controllingClientSocket.store(INVALID_SOCKET, std::memory_order_relaxed);

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
    for (const auto &client : activeClients) {
      shutdown(client.socket, SD_BOTH);
    }
    clientSendMutexes.clear();
  }

  if (serverThread.joinable())
    serverThread.join();
  if (beaconThread.joinable())
    beaconThread.join();
  if (serverHeartbeatThread.joinable())
    serverHeartbeatThread.join();
  if (udpListenerThread.joinable())
    udpListenerThread.join();

  // Securely wait for all client handlers to finish before destroying
  // subsystems
  {
    std::lock_guard<std::mutex> lock(clientThreadsListMutex);
    for (auto &info : activeClientThreadsList) {
      if (info->thread.joinable()) {
        info->thread.join();
      }
    }
    activeClientThreadsList.clear();
  }

  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    activeClients.clear();
    g_hasClients.store(false, std::memory_order_relaxed);
    g_clientsCacheDirty.store(true, std::memory_order_release);
  }

  if (g_serverWsaInitialized.exchange(false)) {
    WSACleanup();
  }
  Security::Shutdown();
}

/**
 * @brief Packages a payload with a header, optionally encrypts it, and
 * broadcasts it to all clients.
 * @param type The MessageType identifier.
 * @param payload Pointer to the raw payload structure.
 * @param payloadSize Size of the payload structure.
 * @return true if data was sent to at least one client.
 */
bool BroadcastMessage(MessageType type, const void *payload,
                      size_t payloadSize) {
  // Snapshot targets WITH their per-client send mutexes so we don't hold
  // clientsMutex during send().
  struct SendTarget {
    SOCKET sock;
    std::shared_ptr<std::mutex> mtx;
    bool useUdp;
    sockaddr_in udpAddr;
    uint32_t prevHbSkips;
  };
  // thread_local reuses the vector's internal buffer across calls, eliminating
  // ~1000 heap allocations/second at 1000Hz mouse polling. clear() only resets
  // the size without freeing memory.
  thread_local std::vector<SendTarget> targets;
  targets.clear();
  thread_local std::vector<std::string> fallbackLogNames;
  fallbackLogNames.clear();
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    if (activeClients.empty())
      return false;
    for (auto &c : activeClients) {
      SOCKET targetSock = c.socket;
      bool isMediaPacket = (type == MessageType::EVENT_MOUSE ||
                            type == MessageType::EVENT_AUDIO_DATA ||
                            type == MessageType::EVENT_MIC_DATA);
      bool useUdp = isMediaPacket && c.hasUdp;

      if (isMediaPacket && !c.hasUdp) {
        if (!c.hasLoggedTcpFallback) {
          c.hasLoggedTcpFallback = true;
          if (State::globalDebugMode) {
            fallbackLogNames.push_back(c.name);
          }
        }
      } else if (c.hasUdp) {
        c.hasLoggedTcpFallback = false; // Reset if UDP becomes available again
      }

      auto it = clientSendMutexes.find(targetSock);
      if (it != clientSendMutexes.end()) {
        targets.push_back({targetSock, it->second, useUdp, c.udpAddr, c.consecutiveHbSkips});
      }
    }
  }
  for (const auto &name : fallbackLogNames) {
    UI::LogDebug("[Network Server] UDP not available for '%s'. Falling back to "
                 "TCP for media streams.",
                 name.c_str());
  }

  // Per-client send: a congested Client A no longer blocks sends to Client B
  // from other threads. CRITICAL: Sequence generation AND encryption MUST
  // happen inside the sendLock. Otherwise, concurrent threads (Mouse,
  // Heartbeat) will interleave sequence numbers into the TCP stream, causing
  // the receiver to drop packets as false-positive Replay Attacks.
  for (auto &t : targets) {
    // Decouple UDP media sends from per-client TCP locks so large TCP clipboard/file
    // writes never stall latency-sensitive mouse or audio streams.
    // For TCP heartbeats, use try_to_lock so we skip clients whose send mutex is
    // held by a concurrent TCP stream.
    std::unique_lock<std::mutex> sendLock(t.useUdp ? serverUdpSendMutex : *t.mtx, std::defer_lock);
    if (!t.useUdp && type == MessageType::EVENT_HEARTBEAT) {
      if (!sendLock.try_lock()) {
        // sendLock is not held here; acquire clientsMutex briefly to track consecutive heartbeat starvation.
        uint32_t skips = 0;
        std::string skipName;
        {
          std::lock_guard<std::mutex> lock(clientsMutex);
          for (auto &c : activeClients) {
            if (c.socket == t.sock) {
              skips = ++c.consecutiveHbSkips;
              skipName = c.name;
              break;
            }
          }
        }
        if (skips >= 3 && State::globalDebugMode) {
          UI::LogDebug("[Network Perf] WARNING: %u consecutive heartbeats skipped for '%s' (~%us TCP send saturation).",
                       skips, skipName.c_str(), skips * 2);
        }
        continue; // Socket is actively streaming; heartbeat is unnecessary.
      }
    } else {
      sendLock.lock();
    }

    bool isUdp = t.useUdp;
    uint32_t seq = isUdp ? ++serverUdpTxSequence : ++serverTxSequence;
    PacketHeader h = {PACKET_MAGIC, type, static_cast<uint32_t>(payloadSize),
                      seq};

    thread_local std::vector<uint8_t> tls_sendBuffer;
    thread_local std::vector<uint8_t> tls_ciphertext;

    if (type != MessageType::EVENT_AUTH &&
        type != MessageType::EVENT_UDP_BEACON) {
      h.payloadSize =
          static_cast<uint32_t>(payloadSize + 28); // 12-byte IV + 16-byte Tag
      if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h),
                                   tls_ciphertext)) {
        tls_sendBuffer.resize(sizeof(h) + tls_ciphertext.size());
        memcpy(tls_sendBuffer.data(), &h, sizeof(h));
        memcpy(tls_sendBuffer.data() + sizeof(h), tls_ciphertext.data(),
               tls_ciphertext.size());
      } else {
        if (State::globalDebugMode) {
          static DebugLog::RateLimitState bcastEncRate;
          uint32_t suppressed = 0;
          if (bcastEncRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            UI::LogDebug("[Network Server] WARNING: Broadcast EncryptPayload failed for type=%s.",
                         MessageTypeToString(type));
          }
        }
        continue;
      }
    } else {
      tls_sendBuffer.resize(sizeof(h) + payloadSize);
      memcpy(tls_sendBuffer.data(), &h, sizeof(h));
      memcpy(tls_sendBuffer.data() + sizeof(h), payload, payloadSize);
    }

    uint64_t tStart = GetTickCount64();
    if (isUdp) {
      if (sendto(serverUdpSocket, (const char *)tls_sendBuffer.data(),
                 (int)tls_sendBuffer.size(), 0, (sockaddr *)&t.udpAddr,
                 sizeof(t.udpAddr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        g_serverUdpSendErrors.fetch_add(1, std::memory_order_relaxed);
        if (State::globalDebugMode) {
          static DebugLog::RateLimitState bcastUdpErrRate;
          uint32_t suppressed = 0;
          if (bcastUdpErrRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
            UI::LogDebug("[Network Server] WARNING: Broadcast UDP sendto failed for type=%s (WSA Error: %d%s).",
                         MessageTypeToString(type), err, supBuf);
          }
        }
      }
    } else {
      if (!SendExact(t.sock, tls_sendBuffer.data(), tls_sendBuffer.size())) {
        int err = WSAGetLastError();
        if (State::globalDebugMode) {
          static DebugLog::RateLimitState bcastTcpErrRate;
          uint32_t suppressed = 0;
          if (bcastTcpErrRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            UI::LogDebug("[Network Server] WARNING: Broadcast TCP SendExact failed for type=%s (WSA Error: %d).",
                         MessageTypeToString(type), err);
          }
        }
        shutdown(t.sock,
                 SD_BOTH); // Force the listener loop to sever the dead socket.
      }
    }
    uint64_t tEnd = GetTickCount64();
    bool tcpStalled = (!isUdp && (tEnd - tStart) > 200);
    bool resetHbSkips = (!isUdp && type == MessageType::EVENT_HEARTBEAT && t.prevHbSkips > 0);
    sendLock.unlock();

    if (tcpStalled || resetHbSkips) {
      std::lock_guard<std::mutex> lock(clientsMutex);
      for (auto &c : activeClients) {
        if (c.socket == t.sock) {
          if (tcpStalled) c.tcpStallCount++;
          if (resetHbSkips) c.consecutiveHbSkips = 0;
          break;
        }
      }
    }

    // 200ms threshold reduces log noise during normal gaming where brief
    // kernel-level stalls are expected from CPU/GPU contention.
    if (State::globalDebugMode && tcpStalled) {
      UI::LogDebug("[Network Server] WARNING: Broadcast (%s) to client blocked for "
                   "%llu ms! TCP congested.",
                   MessageTypeToString(type),
                   (unsigned long long)(tEnd - tStart));
    }
  }

  return true;
}

/**
 * @brief Packages clipboard data and broadcasts it to all enabled clients.
 * @param payload Pointer to the raw wide-character string data.
 * @param payloadSize Size of the string data in bytes.
 * @param excludeSocket Optional client socket to exclude from the broadcast (prevents echoing back to sender).
 * @return true if data was sent to at least one client.
 */
bool BroadcastClipboardMessage(const void *payload, size_t payloadSize,
                               SOCKET excludeSocket) {
  // Snapshot clipboard-enabled targets with their per-client send mutexes.
  struct SendTarget {
    SOCKET sock;
    std::shared_ptr<std::mutex> mtx;
  };
  std::vector<SendTarget> targets;
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    if (activeClients.empty())
      return false;
    for (const auto &c : activeClients) {
      if (c.isClipboardEnabled && c.socket != excludeSocket) {
        auto it = clientSendMutexes.find(c.socket);
        if (it != clientSendMutexes.end()) {
          targets.push_back({c.socket, it->second});
        }
      }
    }
  }
  if (targets.empty())
    return false;

  // Per-client send: clipboard delivery to each client is independently locked.
  for (auto &t : targets) {
    std::lock_guard<std::mutex> sendLock(*t.mtx);

    PacketHeader h = {PACKET_MAGIC, MessageType::EVENT_CLIPBOARD,
                      static_cast<uint32_t>(payloadSize), ++serverTxSequence};
    h.payloadSize =
        static_cast<uint32_t>(payloadSize + 28); // 12-byte IV + 16-byte Tag

    thread_local std::vector<uint8_t> tls_sendBuffer;
    thread_local std::vector<uint8_t> tls_ciphertext;

    if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h),
                                 tls_ciphertext)) {
      tls_sendBuffer.resize(sizeof(h) + tls_ciphertext.size());
      memcpy(tls_sendBuffer.data(), &h, sizeof(h));
      memcpy(tls_sendBuffer.data() + sizeof(h), tls_ciphertext.data(),
             tls_ciphertext.size());

      if (!SendExact(t.sock, tls_sendBuffer.data(), tls_sendBuffer.size())) {
        int err = WSAGetLastError();
        if (State::globalDebugMode) {
          UI::LogDebug("[Network Server] WARNING: Clipboard SendExact failed (WSA Error: %d).", err);
        }
        shutdown(t.sock, SD_BOTH);
      }
    } else if (State::globalDebugMode) {
      UI::LogDebug("[Network Server] WARNING: Failed to encrypt clipboard broadcast.");
    }
  }
  return true;
}

/**
 * @brief Packages a payload with a header, encrypts it via AES-GCM, and sends it directly to a specific client.
 * @param clientSocket The TCP socket handle identifying the target client.
 * @param type The MessageType identifier.
 * @param payload Pointer to the raw payload buffer (may be nullptr if payloadSize is 0).
 * @param payloadSize Size of the raw payload in bytes.
 * @return true if the packet was encrypted and queued/sent successfully, false otherwise.
 */
bool SendToClient(SOCKET clientSocket, MessageType type, const void *payload,
                  size_t payloadSize) {
  bool useUdp = false;
  sockaddr_in udpAddr;

  thread_local std::vector<uint8_t> tls_sendBuffer;
  thread_local std::vector<uint8_t> tls_ciphertext;

  // Lock only this specific client's send mutex, not a global lock.
  std::shared_ptr<std::mutex> sendMtx;
  SOCKET targetSock = clientSocket;
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (const auto &c : activeClients) {
      if (c.socket == clientSocket) {
        useUdp = (type == MessageType::EVENT_MOUSE ||
                  type == MessageType::EVENT_AUDIO_DATA ||
                  type == MessageType::EVENT_MIC_DATA) &&
                 c.hasUdp;
        udpAddr = c.udpAddr;
        break;
      }
    }
    auto it = clientSendMutexes.find(targetSock);
    if (it != clientSendMutexes.end())
      sendMtx = it->second;
  }
  if (!sendMtx)
    return false; // Client disconnected before we could send

  // Use the dedicated UDP send mutex for UDP media so blocking TCP transfers never starve mouse/mic traffic.
  std::unique_lock<std::mutex> sendLock(useUdp ? serverUdpSendMutex : *sendMtx);

  // Generate sequence and encrypt under the per-client lock
  uint32_t seq = useUdp ? ++serverUdpTxSequence : ++serverTxSequence;
  PacketHeader h = {PACKET_MAGIC, type, static_cast<uint32_t>(payloadSize),
                    seq};

  h.payloadSize = static_cast<uint32_t>(payloadSize + 28);
  if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h),
                               tls_ciphertext)) {
    tls_sendBuffer.resize(sizeof(h) + tls_ciphertext.size());
    memcpy(tls_sendBuffer.data(), &h, sizeof(h));
    memcpy(tls_sendBuffer.data() + sizeof(h), tls_ciphertext.data(),
           tls_ciphertext.size());

    uint64_t tStart = GetTickCount64();
    bool ok = false;
    if (useUdp) {
      ok = sendto(serverUdpSocket, (const char *)tls_sendBuffer.data(),
                  (int)tls_sendBuffer.size(), 0, (sockaddr *)&udpAddr,
                  sizeof(udpAddr)) != SOCKET_ERROR;
      if (!ok) {
        int err = WSAGetLastError();
        g_serverUdpSendErrors.fetch_add(1, std::memory_order_relaxed);
        if (State::globalDebugMode) {
          static DebugLog::RateLimitState sendUdpErrRate;
          uint32_t suppressed = 0;
          if (sendUdpErrRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
            UI::LogDebug("[Network Server] WARNING: SendToClient UDP sendto failed (type=%s, WSA Error: %d%s).",
                         MessageTypeToString(type), err, supBuf);
          }
        }
      }
    } else {
      ok = SendExact(targetSock, tls_sendBuffer.data(), tls_sendBuffer.size());
      if (!ok) {
        int err = WSAGetLastError();
        if (State::globalDebugMode) {
          static DebugLog::RateLimitState sendTcpErrRate;
          uint32_t suppressed = 0;
          if (sendTcpErrRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            UI::LogDebug("[Network Server] WARNING: SendToClient TCP SendExact failed (type=%s, WSA Error: %d).",
                         MessageTypeToString(type), err);
          }
        }
        // A failed/partial TCP write leaves the stream misframed; sever it so the
        // listener loop tears the client down cleanly (matches BroadcastMessage).
        shutdown(targetSock, SD_BOTH);
      }
    }
    uint64_t tEnd = GetTickCount64();
    bool tcpStalled = (!useUdp && (tEnd - tStart) > 200);
    sendLock.unlock();
    if (tcpStalled) {
      std::lock_guard<std::mutex> lock(clientsMutex);
      for (auto &c : activeClients) {
        if (c.socket == targetSock) {
          c.tcpStallCount++;
          break;
        }
      }
    }
    if (State::globalDebugMode && tcpStalled) {
      UI::LogDebug("[Network Server] WARNING: SendToClient (%s) blocked for %llu "
                   "ms! TCP congested.",
                   MessageTypeToString(type),
                   (unsigned long long)(tEnd - tStart));
    }
    return ok;
  } else if (State::globalDebugMode) {
    static DebugLog::RateLimitState sendEncErrRate;
    uint32_t suppressed = 0;
    if (sendEncErrRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
      UI::LogDebug("[Network Server] WARNING: SendToClient EncryptPayload failed (type=%s).",
                   MessageTypeToString(type));
    }
  }
  return false;
}

/**
 * @brief Calculates spatial grid trajectory and routes control to the appropriate machine.
 * @param sourceX The grid X coordinate of the originating machine.
 * @param sourceY The grid Y coordinate of the originating machine.
 * @param exitEdge The edge the cursor exited from (0=Left, 1=Right, 2=Top, 3=Bottom).
 * @param normX Normalized horizontal coordinate on the exit edge (0.0f to 1.0f).
 * @param normY Normalized vertical coordinate on the exit edge (0.0f to 1.0f).
 * @return bool True if the transition successfully reached a valid node, false if hit boundary.
 */
bool RouteCursorTransition(int sourceX, int sourceY, uint8_t exitEdge,
                           float normX, float normY) {
  int dx = 0, dy = 0;
  uint8_t entryEdge = 0;

  if (exitEdge == 0) {
    dx = -1;
    entryEdge = 1;
    normX = 1.0f;
  } // Left exit -> Right entry
  else if (exitEdge == 1) {
    dx = 1;
    entryEdge = 0;
    normX = 0.0f;
  } // Right exit -> Left entry
  else if (exitEdge == 2) {
    dy = -1;
    entryEdge = 3;
    normY = 1.0f;
  } // Top exit -> Bottom entry
  else if (exitEdge == 3) {
    dy = 1;
    entryEdge = 2;
    normY = 0.0f;
  } // Bottom exit -> Top entry

  int targetX = sourceX + dx;
  int targetY = sourceY + dy;

  // Cast a directional ray across the spatial grid limits to find the next
  // active node.
  while (targetX >= -10 && targetX <= 10 && targetY >= -10 && targetY <= 10) {
    if (targetX == 0 && targetY == 0) {
      // Notify the previously controlling client that it lost focus before switching to LOCAL
      SOCKET prevCtrl = g_controllingClientSocket.load(std::memory_order_relaxed);
      if (prevCtrl != INVALID_SOCKET) {
        StatePayload localPayload = { 0 }; // 0 represents ControlMode::LOCAL
        SendToClient(prevCtrl, MessageType::EVENT_STATE, &localPayload, sizeof(localPayload));
      }
      // Ray hit the Server's coordinates; pull control back locally.
      Input::HandleReturnControl(entryEdge, normX, normY);
      g_controllingClientSocket.store(INVALID_SOCKET,
                                      std::memory_order_relaxed);
      State::SetMode(State::ControlMode::LOCAL);
      if (State::globalDebugMode) {
        UI::LogDebug("[KVM] Focus -> LOCAL (entryEdge=%u).", entryEdge);
      }
      return true;
    }

    SOCKET targetSocket = INVALID_SOCKET;
    std::string targetName;
    {
      std::lock_guard<std::mutex> lock(clientsMutex);
      for (const auto &c : activeClients) {
        if (c.gridX == targetX && c.gridY == targetY) {
          targetSocket = c.socket;
          targetName = c.name;
          break;
        }
      }
    }

    if (targetSocket != INVALID_SOCKET) {
      // Ray hit an active client; send the synchronization command directly.
      CursorSyncPayload p = {entryEdge, normX, normY};
      if (SendToClient(targetSocket, MessageType::EVENT_SYNC_CURSOR, &p,
                       sizeof(p))) {
        // Notify the PREVIOUS controlling client that it lost control
        SOCKET prevCtrl = g_controllingClientSocket.load(std::memory_order_relaxed);
        if (prevCtrl != INVALID_SOCKET && prevCtrl != targetSocket) {
          StatePayload localPayload = { 0 }; // 0 represents ControlMode::LOCAL
          SendToClient(prevCtrl, MessageType::EVENT_STATE, &localPayload, sizeof(localPayload));
        }
        g_controllingClientSocket.store(targetSocket,
                                        std::memory_order_relaxed);
        State::SetMode(State::ControlMode::REMOTE);
        if (State::globalDebugMode) {
          UI::LogDebug("[KVM] Focus -> Client '%s' (%d,%d) (entryEdge=%u).",
                       targetName.c_str(), targetX, targetY, entryEdge);
        }
        return true;
      } else {
        if (State::globalDebugMode) {
          UI::LogDebug("[KVM] WARNING: Failed to send SYNC_CURSOR to '%s' (%d,%d); continuing raycast.",
                       targetName.c_str(), targetX, targetY);
        }
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

/**
 * @brief Fast non-blocking check returning true if an active node exists in the given exit direction.
 * Allows WH_MOUSE_LL to validate edge transitions without executing blocking TCP send() calls on the hook thread.
 * @param sourceX The grid X coordinate of the originating machine.
 * @param sourceY The grid Y coordinate of the originating machine.
 * @param exitEdge The exit direction (0=Left, 1=Right, 2=Top, 3=Bottom).
 * @return bool True if an active target node exists along the directional ray.
 */
bool HasTargetInDirection(int sourceX, int sourceY, uint8_t exitEdge) {
  int dx = 0, dy = 0;
  if (exitEdge == 0) dx = -1;
  else if (exitEdge == 1) dx = 1;
  else if (exitEdge == 2) dy = -1;
  else if (exitEdge == 3) dy = 1;
  else return false;

  int targetX = sourceX + dx;
  int targetY = sourceY + dy;

  std::lock_guard<std::mutex> lock(clientsMutex);
  while (targetX >= -10 && targetX <= 10 && targetY >= -10 && targetY <= 10) {
    if (targetX == 0 && targetY == 0) {
      return true;
    }
    for (const auto &c : activeClients) {
      if (c.gridX == targetX && c.gridY == targetY && c.socket != INVALID_SOCKET) {
        return true;
      }
    }
    targetX += dx;
    targetY += dy;
  }
  return false;
}
} // namespace Network