// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
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
#include "DebugLog.h"
#include "FileTransferManager.h"
#include "NetworkMessages.h"
#include "SecurityManager.h"
#include "StateManager.h"
#include "UIManager.h"
#include <algorithm>
#include <atomic>
#include <avrt.h>
#include <bcrypt.h>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <ws2tcpip.h>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

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
/// 64-bit sliding bitmask tracking seen UDP sequence numbers within the anti-replay window.
uint64_t udpRxReplayMask = 0;
/// Cumulative client UDP delivery, reorder, loss, stale, and duplicate counters.
static UdpReplayStats g_clientUdpStats = {};
/// Protects concurrent reads/writes of g_clientUdpStats, udpRxSequence, and udpRxReplayMask.
static std::mutex g_clientUdpStatsMutex;
static std::atomic<uint64_t> g_clientUdpDecryptFails{0};
static std::atomic<uint64_t> g_clientUdpSendErrors{0};
static std::atomic<uint32_t> g_clientTcpStalls{0};
static std::atomic<uint32_t> g_clientConsecutiveHbSkips{0};
static std::atomic<bool> g_loggedUdpTimeout{false};
static std::atomic<bool> g_sessionUsedUdp{false};

/// Cryptographic 64-bit token binding UDP handshakes to the current TCP session.
std::atomic<uint64_t> g_clientSessionToken{0};

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
/// Ensures TCP media fallback is logged once per session rather than permanently silenced.
std::atomic<bool> g_hasLoggedTcpFallback{false};
/// Timestamp (GetTickCount64) when post-auth UDP handshake window began.
std::atomic<uint64_t> g_udpHandshakeStartMs{0};
/// Timestamp (GetTickCount64) of the last dispatched EVENT_RETURN_CONTROL for edge-burst debouncing.
std::atomic<uint64_t> g_lastReturnControlTickMs{0};
/// Signals EvaluateClientEdgeReturn (including ClientUDPListenerLoop) to reset its edge pull-back accumulator.
std::atomic<bool> g_resetPullBackAccumulator{false};
/// Synchronizes interruptible waits on the client heartbeat / UDP retry loop.
std::mutex g_heartbeatCvMutex;
std::condition_variable g_heartbeatCv;

// UDP Discovery state variables.
SOCKET udpListenerSocket = INVALID_SOCKET;
std::thread discoveryThread;
std::atomic<bool> isDiscoveryRunning(false);
std::vector<DiscoveredServer> discoveredServers;
std::mutex discoveryMutex;

// Audio state variables.
Audio::AudioFormat g_serverAudioFormat;
std::mutex g_serverAudioFormatMutex;
std::atomic<bool> g_hasServerAudioFormat(false);
std::atomic<uint8_t> g_serverAudioCodec{0};

// Auto-Reconnect state variables.
std::string lastConnectedIp = "";
uint16_t lastConnectedPort = 0;
std::thread autoReconnectThread;
std::atomic<bool> isIntentionalDisconnect(false);
std::atomic<bool> g_authRejected(false);
std::atomic<bool> isAutoReconnecting(false);
std::atomic<bool> isCurrentSessionAutoReconnected(false);
uint64_t connectionStartTime = 0;

/// Exponential backoff delay for auto-reconnect (seconds). Doubles on each
/// short-lived session, capped at 30s. Resets after a stable connection (>30s).
std::atomic<int> reconnectBackoffSeconds{ReconnectPolicy::kInitialDelaySec};
/// Signals that the OS resumed from sleep/Modern Standby so reconnect skips backoff penalties.
std::atomic<bool> g_systemJustResumed{false};
/// Distinguishes non-recoverable errors (missing PIN, crypto/WinSock init) from transient offline retries.
std::atomic<bool> g_fatalConnectError{false};
/// Triggers an immediate heartbeat probe on wake to detect dead pre-sleep TCP sockets immediately.
std::atomic<bool> g_forceImmediateHeartbeat{false};
/// Synchronizes interruptible exponential-backoff waits in autoReconnectThread.
std::mutex g_reconnectCvMutex;
std::condition_variable g_reconnectCv;

/// Tracks WinSock reference ownership independently for discovery and client sessions.
static std::atomic<bool> g_discoveryWsaInitialized{false};
static std::atomic<bool> g_clientWsaInitialized{false};

/**
 * @brief Injects KEYUP and mouse button UP events to prevent modifiers or mouse drags
 * from getting stuck when the Server suddenly revokes control or disconnects.
 * @return void
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

  // Release any held mouse buttons to prevent stuck drag/selection states
  DWORD mouseUpFlags = 0;
  if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
    mouseUpFlags |= MOUSEEVENTF_LEFTUP;
  if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
    mouseUpFlags |= MOUSEEVENTF_RIGHTUP;
  if (GetAsyncKeyState(VK_MBUTTON) & 0x8000)
    mouseUpFlags |= MOUSEEVENTF_MIDDLEUP;

  if (mouseUpFlags != 0) {
    INPUT mi = {0};
    mi.type = INPUT_MOUSE;
    mi.mi.dwFlags = mouseUpFlags;
    SendInput(1, &mi, sizeof(INPUT));
  }
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
 * @brief Packages a payload with a header, encrypts it via AES-GCM, and sends it
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
    } else {
      if (type == MessageType::EVENT_AUDIO_DATA) {
        uint64_t nowMs = GetTickCount64();
        uint64_t startMs = g_udpHandshakeStartMs.load(std::memory_order_relaxed);
        uint64_t elapsedMs = (nowMs >= startMs) ? (nowMs - startMs) : 0;
        if (ShouldDeferTcpAudioFallback(false, clientUdpSocket != INVALID_SOCKET,
                                        elapsedMs)) {
          // Allow the post-auth UDP handshake brief RTT to complete before falling
          // back to TCP, preventing stale TCP audio from stacking onto UDP.
          return true;
        }
      }
      if (!g_hasLoggedTcpFallback.exchange(true, std::memory_order_relaxed) &&
          State::globalDebugMode) {
        UI::LogDebug("[Network Client] UDP not active. Falling back to TCP for "
                     "media streams.");
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

  // All client-to-server packets (including EVENT_AUTH) are encrypted with AES-GCM.
  h.payloadSize = static_cast<uint32_t>(
      payloadSize + 28); // 12-byte IV + 16-byte Tag overhead
  if (Security::EncryptPayload(payload, payloadSize, &h, sizeof(h),
                               tls_ciphertext)) {
    tls_sendBuffer.resize(sizeof(h) + tls_ciphertext.size());
    memcpy(tls_sendBuffer.data(), &h, sizeof(h));
    memcpy(tls_sendBuffer.data() + sizeof(h), tls_ciphertext.data(),
           tls_ciphertext.size());
  } else {
    if (State::globalDebugMode) {
      static DebugLog::RateLimitState cliEncRate;
      uint32_t suppressed = 0;
      if (cliEncRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
        UI::LogDebug("[Network Client] WARNING: EncryptPayload failed for type=%s.",
                     MessageTypeToString(type));
      }
    }
    return false;
  }

  if (!tls_sendBuffer.empty()) {
    if (isUdp) {
      int res = sendto(clientUdpSocket, (const char *)tls_sendBuffer.data(),
                       (int)tls_sendBuffer.size(), 0, (sockaddr *)&serverUdpAddr,
                       sizeof(serverUdpAddr));
      if (res == SOCKET_ERROR) {
        int err = WSAGetLastError();
        g_clientUdpSendErrors.fetch_add(1, std::memory_order_relaxed);
        if (State::globalDebugMode) {
          static DebugLog::RateLimitState cliUdpSendRate;
          uint32_t suppressed = 0;
          if (cliUdpSendRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
            UI::LogDebug("[Network Client] WARNING: UDP sendto failed (type=%s, WSA Error: %d%s).",
                         MessageTypeToString(type), err, supBuf);
          }
        }
        return false;
      }
      return true;
    } else {
      uint64_t tStart = GetTickCount64();
      bool sentOk = SendExact(targetSock, tls_sendBuffer.data(), tls_sendBuffer.size());
      uint64_t tEnd = GetTickCount64();
      if ((tEnd - tStart) > 200) {
        g_clientTcpStalls.fetch_add(1, std::memory_order_relaxed);
        if (State::globalDebugMode) {
          static DebugLog::RateLimitState cliTcpStallRate;
          uint32_t suppressed = 0;
          if (cliTcpStallRate.TryAcquire(tEnd, 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
            UI::LogDebug("[Network Client] WARNING: send() (%s) blocked for %llu ms! TCP congested%s.",
                         MessageTypeToString(type),
                         static_cast<unsigned long long>(tEnd - tStart), supBuf);
          }
        }
      }
      if (!sentOk) {
        if (State::globalDebugMode) {
          UI::LogDebug("[Network Client] send() failed on TCP socket (type=%s, WSA Error: %d).",
                       MessageTypeToString(type), WSAGetLastError());
        }
        return false;
      }
      return true;
    }
  }
  return false;
}

/**
 * @brief Specially sends a Keep-Alive heartbeat on the specified socket to
 * prevent SO_RCVTIMEO disconnects.
 * @param sock The TCP socket connected to the Server.
 * @param sockMutex Per-socket mutex protecting TCP framing against interleaved writes.
 * @return void
 */
void SendHeartbeatToSocket(SOCKET sock, std::mutex &sockMutex) {
  if (sock == INVALID_SOCKET)
    return;

  std::unique_lock<std::mutex> lock(sockMutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    uint32_t skips = g_clientConsecutiveHbSkips.fetch_add(1, std::memory_order_relaxed) + 1;
    if (skips >= 3 && State::globalDebugMode) {
      static DebugLog::RateLimitState cliHbSkipRate;
      uint32_t suppressed = 0;
      if (cliHbSkipRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
        UI::LogDebug("[Network Perf] WARNING: %u consecutive heartbeats skipped (~%us TCP send saturation).",
                     skips, skips * 2);
      }
    }
    return; // Socket is actively streaming data, heartbeat is unnecessary.
  }
  g_clientConsecutiveHbSkips.store(0, std::memory_order_relaxed);

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

    if (!SendExact(sock, tls_sendBuffer.data(), tls_sendBuffer.size())) {
      int err = WSAGetLastError();
      if (State::globalDebugMode) {
        static DebugLog::RateLimitState hbSendErrRate;
        uint32_t suppressed = 0;
        if (hbSendErrRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
          UI::LogDebug("[Network Client] WARNING: Heartbeat SendExact failed (WSA Error: %d).", err);
        }
      }
      shutdown(sock, SD_BOTH);
    }
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
  if (SendInput(1, &i, sizeof(INPUT)) == 0 && State::globalDebugMode) {
    DWORD err = GetLastError();
    static DebugLog::RateLimitState mouseInjectRate;
    uint32_t suppressed = 0;
    if (mouseInjectRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
      char supBuf[32] = "";
      if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
      UI::LogDebug("[Network Client] WARNING: SendInput(MOUSE) failed/blocked (err=%lu%s).", err, supBuf);
    }
  }
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
  if (SendInput(1, &i, sizeof(INPUT)) == 0 && State::globalDebugMode) {
    DWORD err = GetLastError();
    static DebugLog::RateLimitState kbdInjectRate;
    uint32_t suppressed = 0;
    if (kbdInjectRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
      char supBuf[32] = "";
      if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
      UI::LogDebug("[Network Client] WARNING: SendInput(KEYBOARD) failed/blocked (err=%lu%s).", err, supBuf);
    }
  }
}

/**
 * @brief Checks if synthetic mouse movement is actively stalled by a SYSTEM-integrity
 * or protected window (such as NVIDIA App tray context menu) and auto-reverts control to Server.
 * @param p The MousePayload containing movement deltas and flags.
 * @param currentPt The current cursor position on the client screen.
 * @return void
 */
void CheckForBlockedInput(const MousePayload &p, const POINT &currentPt) {
  // Only evaluate on pure relative movement commands with non-zero movement
  if (p.flags != MOUSEEVENTF_MOVE || (p.deltaX == 0 && p.deltaY == 0)) {
    return;
  }

  static POINT lastPt = { -1, -1 };
  static int stallCount = 0;
  static auto lastNotifyTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);
  static HWND lastCheckedFg = NULL;
  static bool lastFgWasProtected = false;
  static std::string lastBlockingProc = "SYSTEM";
  static auto lastFgCheckTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);

  // Fast-path integer comparison: exactly 1 CPU cycle during normal movement
  if (currentPt.x == lastPt.x && currentPt.y == lastPt.y) {
    stallCount++;

    // After 60 consecutive frozen movement packets (~60ms at 1000Hz), check whether an actual
    // SYSTEM/UIPI-protected foreground window owns focus. Never revert blindly without confirming
    // a protected window (fixes false REMOTE->LOCAL bounces under heavy GPU/video load).
    if (stallCount >= 60) {
      auto now = std::chrono::steady_clock::now();
      HWND fg = GetForegroundWindow();
      auto sinceFgCheckMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFgCheckTime).count();

      if (fg != lastCheckedFg || sinceFgCheckMs >= 250) {
        lastCheckedFg = fg;
        lastFgCheckTime = now;
        lastFgWasProtected = false;
        lastBlockingProc = "SYSTEM";

        if (fg) {
          DWORD pid = 0;
          GetWindowThreadProcessId(fg, &pid);
          if (pid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
              char procPath[MAX_PATH] = { 0 };
              DWORD pathSize = sizeof(procPath);
              if (QueryFullProcessImageNameA(hProc, 0, procPath, &pathSize)) {
                std::string rawPath(procPath);
                size_t slashPos = rawPath.find_last_of("\\/");
                lastBlockingProc = (slashPos != std::string::npos) ? rawPath.substr(slashPos + 1) : rawPath;

                std::string path = rawPath;
                std::transform(path.begin(), path.end(), path.begin(), ::tolower);
                if (path.find("nvcontainer") != std::string::npos ||
                    path.find("nvdisplay") != std::string::npos) {
                  lastFgWasProtected = true;
                }
              }

              if (!lastFgWasProtected) {
                HANDLE hToken = NULL;
                if (OpenProcessToken(hProc, TOKEN_QUERY, &hToken)) {
                  DWORD len = 0;
                  GetTokenInformation(hToken, TokenIntegrityLevel, NULL, 0, &len);
                  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
                    std::vector<uint8_t> buffer(len);
                    if (GetTokenInformation(hToken, TokenIntegrityLevel, buffer.data(), len, &len)) {
                      TOKEN_MANDATORY_LABEL* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
                      DWORD subAuth = *GetSidSubAuthority(label->Label.Sid, (DWORD)(UCHAR)(*GetSidSubAuthorityCount(label->Label.Sid) - 1));
                      if (subAuth >= SECURITY_MANDATORY_SYSTEM_RID) {
                        lastFgWasProtected = true; // SYSTEM integrity level
                      }
                    }
                  }
                  CloseHandle(hToken);
                }
              }
              CloseHandle(hProc);
            }
          }
        }
      }

      if (lastFgWasProtected &&
          std::chrono::duration_cast<std::chrono::milliseconds>(now - lastNotifyTime).count() >= 500) {
        lastNotifyTime = now;
        stallCount = 0;
        if (State::globalDebugMode) {
          UI::LogDebug("[Network Client] Synthetic input blocked by SYSTEM/UIPI window (%s). Auto-reverting control to Server.",
                       lastBlockingProc.c_str());
        }
        SendToServer(MessageType::EVENT_CLIENT_LOCKED, nullptr, 0);
      }
    }
  } else {
    stallCount = 0;
  }

  lastPt = currentPt;
}

/**
 * @brief Evaluates multi-monitor edge boundaries after injecting mouse movement
 * and sends EVENT_RETURN_CONTROL when the cursor pushes past an outer desktop edge.
 * @param p The injected MousePayload containing deltas.
 * @param pullBackAccumulator Reference to the caller's edge pull-back accumulator.
 * @param pullBackThreshold Delta threshold required to trigger a transition.
 * @return void
 */
static void EvaluateClientEdgeReturn(const MousePayload &p,
                                     int &pullBackAccumulator,
                                     int pullBackThreshold) {
  if (g_resetPullBackAccumulator.exchange(false, std::memory_order_acq_rel)) {
    pullBackAccumulator = 0;
  }
  POINT pt;
  if (!GetCursorPos(&pt))
    return;
  CheckForBlockedInput(p, pt);

  HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(mi)};
  if (!GetMonitorInfo(hMon, &mi))
    return;

  int monWidth = mi.rcMonitor.right - mi.rcMonitor.left;
  int monHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
  if (monWidth <= 0 || monHeight <= 0)
    return;

  int deadzoneX = (monWidth * State::edgeDeadzonePercent) / 100;
  int deadzoneY = (monHeight * State::edgeDeadzonePercent) / 100;

  bool safeY = pt.y > (mi.rcMonitor.top + deadzoneY) &&
               pt.y < (mi.rcMonitor.bottom - deadzoneY);
  bool safeX = pt.x > (mi.rcMonitor.left + deadzoneX) &&
               pt.x < (mi.rcMonitor.right - deadzoneX);

  uint8_t exitEdge = 255;
  float normX = 0.0f, normY = 0.0f;

  if (pt.x <= mi.rcMonitor.left && p.deltaX < 0 && safeY) {
    exitEdge = 0;
    normX = 0.0f;
    normY = (float)(pt.y - mi.rcMonitor.top) / monHeight;
  } else if (pt.x >= mi.rcMonitor.right - 1 && p.deltaX > 0 && safeY) {
    exitEdge = 1;
    normX = 1.0f;
    normY = (float)(pt.y - mi.rcMonitor.top) / monHeight;
  } else if (pt.y <= mi.rcMonitor.top && p.deltaY < 0 && safeX) {
    exitEdge = 2;
    normX = (float)(pt.x - mi.rcMonitor.left) / monWidth;
    normY = 0.0f;
  } else if (pt.y >= mi.rcMonitor.bottom - 1 && p.deltaY > 0 && safeX) {
    exitEdge = 3;
    normX = (float)(pt.x - mi.rcMonitor.left) / monWidth;
    normY = 1.0f;
  }

  if (exitEdge != 255) {
    // Only trigger return control if there is no adjacent local monitor beyond this edge
    POINT testPt = pt;
    if (exitEdge == 0)
      testPt.x -= 2;
    else if (exitEdge == 1)
      testPt.x += 2;
    else if (exitEdge == 2)
      testPt.y -= 2;
    else if (exitEdge == 3)
      testPt.y += 2;

    if (MonitorFromPoint(testPt, MONITOR_DEFAULTTONULL) == NULL) {
      pullBackAccumulator += (exitEdge <= 1) ? abs(p.deltaX) : abs(p.deltaY);
      if (pullBackAccumulator > pullBackThreshold) {
        uint64_t nowMs = GetTickCount64();
        uint64_t lastMs = g_lastReturnControlTickMs.load(std::memory_order_relaxed);
        if (CanSendReturnControl(nowMs, lastMs)) {
          g_lastReturnControlTickMs.store(nowMs, std::memory_order_relaxed);
          ReturnControlPayload retPayload = {exitEdge, normX, normY};
          SendToServer(MessageType::EVENT_RETURN_CONTROL, &retPayload,
                       sizeof(retPayload));
        }
        pullBackAccumulator = 0;
      }
      return;
    }
  }
  pullBackAccumulator = 0;
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
  std::lock_guard<std::mutex> lock(g_serverAudioFormatMutex);
  if (!g_hasServerAudioFormat)
    return false;
  outFormat = g_serverAudioFormat;
  return true;
}

/**
 * @brief Retrieves the lock-free active audio wire codec ID dictated by the Server.
 * @return Wire codec ID (0 = PCM, 1 = Opus).
 */
uint8_t GetServerAudioCodec() {
  return g_serverAudioCodec.load(std::memory_order_relaxed);
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
      UI::LogDebug("[Network Client] UDP Socket creation failed (WSA Error: %d).",
                   WSAGetLastError());
    return;
  }

  int reuseAddr = 1;
  setsockopt(udpListenerSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&reuseAddr,
             sizeof(reuseAddr));

  sockaddr_in localAddr = {0};
  localAddr.sin_family = AF_INET;
  localAddr.sin_port = htons(DEFAULT_DISCOVERY_PORT);
  localAddr.sin_addr.s_addr = INADDR_ANY;

  if (bind(udpListenerSocket, (sockaddr *)&localAddr, sizeof(localAddr)) ==
      SOCKET_ERROR) {
    if (State::globalDebugMode)
      UI::LogDebug(
          "[Network Client] UDP Bind failed on port %u (WSA Error: %d).",
          static_cast<unsigned int>(DEFAULT_DISCOVERY_PORT), WSAGetLastError());
    closesocket(udpListenerSocket);
    udpListenerSocket = INVALID_SOCKET;
    return;
  }

  DWORD timeout = 500;
  setsockopt(udpListenerSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));

  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Listening for UDP Beacons on port %u...",
                 static_cast<unsigned int>(DEFAULT_DISCOVERY_PORT));

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
          if (payload->tcpPort == 0) {
            continue;
          }

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
                           bool expired = (currentTick - srv.lastUpdateTick) > 5000;
                           if (expired && State::globalDebugMode) {
                             UI::LogDebug("[Network Client] Discovered Server offline/expired: %s (%s).",
                                          srv.hostname.c_str(), srv.ip.c_str());
                           }
                           return expired;
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
  if (!g_discoveryWsaInitialized.exchange(true)) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
  }
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
  {
    std::lock_guard<std::mutex> lock(discoveryMutex);
    discoveredServers.clear();
  }
  if (g_discoveryWsaInitialized.exchange(false)) {
    WSACleanup();
  }
}

/**
 * @brief Dedicated thread for handling incoming connectionless UDP packets
 * (Audio and Mouse deltas).
 * @return void
 */
void ClientUDPListenerLoop() {
  int pullBackAccumulator = 0;
  const int PULL_BACK_THRESHOLD = 150;

  std::vector<uint8_t> buffer(MAX_PAYLOAD_SIZE);
  std::vector<uint8_t> decrypted(MAX_PAYLOAD_SIZE);
  sockaddr_in senderAddr;
  int senderAddrSize = sizeof(senderAddr);

  static DebugLog::RateLimitState cliUdpRecvErrRate;
  static DebugLog::RateLimitState cliUdpIpMismatchRate;
  static DebugLog::RateLimitState cliUdpDecryptRate;

  while (isClientRunning && clientUdpSocket != INVALID_SOCKET) {
    int bytes =
        recvfrom(clientUdpSocket, (char *)buffer.data(), (int)buffer.size(), 0,
                 (sockaddr *)&senderAddr, &senderAddrSize);
    if (bytes <= 0) {
      if (bytes == SOCKET_ERROR && isClientRunning) {
        int err = WSAGetLastError();
        if (err != WSAETIMEDOUT && State::globalDebugMode) {
          uint32_t suppressed = 0;
          if (cliUdpRecvErrRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
            UI::LogDebug("[Network Client] UDP recvfrom error (WSA Error: %d%s).", err, supBuf);
          }
        }
      }
      continue;
    }

    if (bytes < sizeof(PacketHeader))
      continue;

    // Only accept UDP traffic from the Server we explicitly connected to
    if (senderAddr.sin_addr.s_addr != serverUdpAddr.sin_addr.s_addr) {
      if (State::globalDebugMode) {
        uint32_t suppressed = 0;
        if (cliUdpIpMismatchRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
          char gotIp[INET_ADDRSTRLEN] = "Unknown";
          char expIp[INET_ADDRSTRLEN] = "Unknown";
          inet_ntop(AF_INET, &(senderAddr.sin_addr), gotIp, INET_ADDRSTRLEN);
          inet_ntop(AF_INET, &(serverUdpAddr.sin_addr), expIp, INET_ADDRSTRLEN);
          UI::LogDebug("[Network Client] Ignored UDP packet from unexpected IP %s (expected %s).",
                       gotIp, expIp);
        }
      }
      continue;
    }

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
      g_clientUdpDecryptFails.fetch_add(1, std::memory_order_relaxed);
      if (State::globalDebugMode) {
        uint32_t suppressed = 0;
        if (cliUdpDecryptRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
          char supBuf[32] = "";
          if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
          UI::LogDebug("[Security] Client UDP decrypt failed: type=%s seq=%u size=%zu%s.",
                       MessageTypeToString(header.type), header.sequenceNumber,
                       payloadSize, supBuf);
        }
      }
      continue; // Decryption failed, drop packet
    }

    // Enforce 64-bit sliding bitmask anti-replay window (RFC 6479 style)
    // AFTER AES-GCM verification so unauthenticated packets cannot advance the window.
    // Tolerates minor UDP reordering between 1000Hz mouse and 100Hz audio packets
    // while strictly rejecting duplicate replays within the 64-packet window.
    bool validUdp = false;
    {
      std::lock_guard<std::mutex> statsLock(g_clientUdpStatsMutex);
      validUdp = VerifyUdpReplayWindow(header.sequenceNumber, udpRxSequence,
                                       udpRxReplayMask, &g_clientUdpStats);
    }
    if (!validUdp) {
      continue;
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
        EvaluateClientEdgeReturn(p, pullBackAccumulator, PULL_BACK_THRESHOLD);
      }
    }
  }
}

/**
 * @brief Sends an encrypted UDP hole-punching handshake packet bound to the current TCP session token.
 * @return true if the UDP handshake packet was dispatched, false if UDP is unavailable or already active.
 */
static bool SendUdpHandshake() {
  if (!isClientRunning || isUdpActive.load(std::memory_order_relaxed) ||
      clientUdpSocket == INVALID_SOCKET) {
    return false;
  }
  uint64_t token = g_clientSessionToken.load(std::memory_order_relaxed);
  if (token == 0) {
    return false;
  }
  char hostname[MAX_COMPUTERNAME_LENGTH + 1] = "KAM-Flow-Client";
  DWORD size = sizeof(hostname);
  if (!GetComputerNameA(hostname, &size)) {
    strncpy_s(hostname, sizeof(hostname), "KAM-Flow-Client", _TRUNCATE);
  }
  UDPHandshakePayload udpHandshake;
  memset(&udpHandshake, 0, sizeof(udpHandshake));
  udpHandshake.sessionToken = token;
  strncpy_s(udpHandshake.clientName, sizeof(udpHandshake.clientName),
            hostname, _TRUNCATE);
  return SendToServer(MessageType::EVENT_UDP_HANDSHAKE, &udpHandshake,
                      sizeof(udpHandshake));
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
  bool sentPostAuthUdpHandshake = false;

  thread_local std::vector<uint8_t> tls_rawPayload;
  thread_local std::vector<uint8_t> tls_decrypted;
  tls_rawPayload.reserve(MAX_PAYLOAD_SIZE);
  tls_decrypted.reserve(MAX_PAYLOAD_SIZE);

  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] TCP Listener Thread active.");

  while (isClientRunning) {
    PacketHeader h;
    int errCode = 0;
    if (!RecvExact(clientSocket, &h, sizeof(h), errCode)) {
      if (errCode == 0) {
        if (!g_authRejected.load(std::memory_order_relaxed)) {
          State::SetClientStatus("Disconnected (Server Closed Connection).");
        }
        if (State::globalDebugMode)
          UI::LogDebug("[Network Client] Server closed connection (graceful close / auth rejected).");
      } else if (errCode == WSAETIMEDOUT) {
        State::SetClientStatus("Disconnected (Timeout).");
        if (State::globalDebugMode)
          UI::LogDebug("[Network Client] Connection timed out (No Heartbeat "
                       "received). WSA Error: %d",
                       errCode);
      } else {
        State::SetClientStatus("Disconnected (Connection Severed).");
        if (State::globalDebugMode)
          UI::LogDebug("[Network Client] Connection severed. WSA Error: %d",
                       errCode);
      }
      break;
    }

    if (h.magic != PACKET_MAGIC) {
      State::SetClientStatus("Disconnected (Packet Desync).");
      if (State::globalDebugMode)
        UI::LogDebug("[Network Client] FATAL: Packet desync detected (bad magic 0x%08X)!", h.magic);
      break;
    }

    // Enforce payload size limits to prevent memory allocation attacks
    // (CWE-400).
    if (h.payloadSize > MAX_PAYLOAD_SIZE) {
      State::SetClientStatus("Disconnected (Payload Size Violation).");
      if (State::globalDebugMode)
        UI::LogDebug("[Network Client] CRITICAL: Payload size (%u bytes, type=%s) exceeds safety "
                     "bounds! Dropping connection.",
                     h.payloadSize, MessageTypeToString(h.type));
      break;
    }

    tls_rawPayload.resize(h.payloadSize);
    if (h.payloadSize > 0) {
      // Ensure full payload is received to avoid processing corrupted state
      // data.
      if (!RecvExact(clientSocket, tls_rawPayload.data(), h.payloadSize, errCode)) {
        if (State::globalDebugMode)
          UI::LogDebug("[Network Client] WARNING: Partial payload received. "
                       "(Expected %u bytes, type=%s, Error: %d). Dropping connection.",
                       h.payloadSize, MessageTypeToString(h.type), errCode);
        break;
      }
    }

    if (h.sequenceNumber <= rxSequence && h.sequenceNumber != 0) {
      if (State::globalDebugMode) {
        static DebugLog::RateLimitState cliTcpReplayRate;
        uint32_t suppressed = 0;
        if (cliTcpReplayRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
          char supBuf[32] = "";
          if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
          UI::LogDebug("[Security] Replay attack / stale TCP packet: type=%s seq=%u <= rxSeq=%u%s.",
                       MessageTypeToString(h.type), h.sequenceNumber, rxSequence, supBuf);
        }
      }
      continue; // Safe to continue since the raw payload was already cleared
                // from the socket buffer.
    }
    rxSequence = h.sequenceNumber;

    const uint8_t *finalPayload = tls_rawPayload.data();
    size_t finalSize = tls_rawPayload.size();

    bool isEncrypted = (h.type != MessageType::EVENT_AUTH) &&
                       (h.type != MessageType::EVENT_UDP_BEACON) &&
                       (h.type != MessageType::EVENT_AUTH_REJECTED);

    if (isEncrypted) {
      if (!Security::DecryptPayload(tls_rawPayload.data(),
                                    tls_rawPayload.size(), &h, sizeof(h),
                                    tls_decrypted)) {
        if (State::globalDebugMode) {
          static DebugLog::RateLimitState cliTcpDecryptRate;
          uint32_t suppressed = 0;
          if (cliTcpDecryptRate.TryAcquire(GetTickCount64(), 5000, suppressed)) {
            char supBuf[32] = "";
            if (suppressed > 0) snprintf(supBuf, sizeof(supBuf), ", +%u suppressed", suppressed);
            UI::LogDebug("[Security] Failed to decrypt server packet: type=%s seq=%u size=%zu%s.",
                         MessageTypeToString(h.type), h.sequenceNumber,
                         tls_rawPayload.size(), supBuf);
          }
        }
        continue;
      }
      finalPayload = tls_decrypted.data();
      finalSize = tls_decrypted.size();
    }

    if (h.type == MessageType::EVENT_AUTH_REJECTED) {
      g_authRejected.store(true, std::memory_order_relaxed);
      isIntentionalDisconnect = true; // Stop auto-reconnect loop immediately on wrong PIN
      State::SetClientStatus("Authentication Failed (Wrong PIN - Please Re-Pair).");
      if (State::globalDebugMode) {
        UI::LogDebug("[Security] Server rejected authentication (wrong Master PIN). Auto-reconnect stopped; please re-pair via UI.");
      }
      break;
    }

    // Filter high-frequency and routine events BEFORE the generic log
    // to prevent flooding the debug window with heartbeat/input/audio noise.
    if (h.type == MessageType::EVENT_UDP_HANDSHAKE_ACK) {
      isUdpActive.store(true, std::memory_order_relaxed);
      g_sessionUsedUdp.store(true, std::memory_order_relaxed);
      g_hasLoggedTcpFallback.store(false, std::memory_order_relaxed);
      if (State::globalDebugMode)
        UI::LogDebug(
            "[Network Client] UDP Connection fully established with Server.");
      continue;
    }

    // Fire the initial UDP handshake only after receiving the Server's first
    // authenticated packet (e.g. EVENT_AUDIO_FORMAT), guaranteeing the Server
    // has already registered our session token in activeClients.
    if (isEncrypted && !sentPostAuthUdpHandshake &&
        !isUdpActive.load(std::memory_order_relaxed) &&
        clientUdpSocket != INVALID_SOCKET) {
      sentPostAuthUdpHandshake = true;
      g_udpHandshakeStartMs.store(GetTickCount64(), std::memory_order_relaxed);
      SendUdpHandshake();
    }

    if (h.type == MessageType::EVENT_HEARTBEAT) {
      continue;
    }

    // Log non-routine TCP packets with human-readable type names
    if (State::globalDebugMode && !IsHighFrequencyMessage(h.type)) {
      UI::LogDebug("[Network Client] RX -> %s | %zu bytes",
                   MessageTypeToString(h.type), finalSize);
    }

    if (h.type == MessageType::EVENT_MOUSE) {
      if (finalSize == sizeof(MousePayload)) {
        MousePayload p;
        memcpy(&p, finalPayload, sizeof(p));
        InjectMouseInput(p);
        EvaluateClientEdgeReturn(p, pullBackAccumulator, PULL_BACK_THRESHOLD);
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

        POINT curPt = {0, 0};
        GetCursorPos(&curPt);
        HMONITOR hMon = MonitorFromPoint(curPt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfo(hMon, &mi)) {
          int w = mi.rcMonitor.right - mi.rcMonitor.left;
          int hMonPx = mi.rcMonitor.bottom - mi.rcMonitor.top;
          int tx = mi.rcMonitor.left + (int)(w * p.normalizedX);
          int ty = mi.rcMonitor.top + (int)(hMonPx * p.normalizedY);

          if (p.entryEdge == 0)
            tx = mi.rcMonitor.left + 15; // Left
          else if (p.entryEdge == 1)
            tx = mi.rcMonitor.right - 15; // Right
          else if (p.entryEdge == 2)
            ty = mi.rcMonitor.top + 15; // Top
          else if (p.entryEdge == 3)
            ty = mi.rcMonitor.bottom - 15; // Bottom

          tx = std::clamp(tx, (int)mi.rcMonitor.left + 10,
                          (int)mi.rcMonitor.right - 10);
          ty = std::clamp(ty, (int)mi.rcMonitor.top + 10,
                          (int)mi.rcMonitor.bottom - 10);
          SetCursorPos(tx, ty);
        } else {
          int sw = GetSystemMetrics(SM_CXSCREEN);
          int sh = GetSystemMetrics(SM_CYSCREEN);
          int ty = std::clamp((int)(sh * p.normalizedY), 10, sh - 10);
          int tx = std::clamp((int)(sw * p.normalizedX), 15, sw - 15);
          if (p.entryEdge == 0)
            tx = 15;
          else if (p.entryEdge == 1)
            tx = sw - 15;
          else if (p.entryEdge == 2)
            ty = 15;
          else if (p.entryEdge == 3)
            ty = sh - 15;
          SetCursorPos(tx, ty);
        }
        pullBackAccumulator = 0;
        g_resetPullBackAccumulator.store(true, std::memory_order_release);
        g_lastReturnControlTickMs.store(0, std::memory_order_relaxed);
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
        AudioFormatPayload wireFmt;
        memcpy(&wireFmt, finalPayload, sizeof(wireFmt));

        Audio::AudioFormat newFormat;
        newFormat.sampleRate = wireFmt.sampleRate;
        newFormat.bitDepth = wireFmt.bitDepth;
        newFormat.channels = wireFmt.channels;
        newFormat.codec = wireFmt.codec;

        bool formatChanged = false;
        bool codecChanged = false;
        {
          std::lock_guard<std::mutex> lock(g_serverAudioFormatMutex);
          const bool hadFmt = g_hasServerAudioFormat.load(std::memory_order_relaxed);
          formatChanged = hadFmt &&
                          ((g_serverAudioFormat.sampleRate != newFormat.sampleRate) ||
                           (g_serverAudioFormat.bitDepth != newFormat.bitDepth) ||
                           (g_serverAudioFormat.channels != newFormat.channels));
          codecChanged = hadFmt && (g_serverAudioFormat.codec != newFormat.codec);
          g_serverAudioFormat = newFormat;
          g_serverAudioCodec.store(newFormat.codec, std::memory_order_relaxed);
          g_hasServerAudioFormat = true;
        }

        if (State::globalDebugMode) {
          const char* suffix = formatChanged ? " (Format Changed)"
                             : (codecChanged ? " (Codec Changed)" : "");
          UI::LogDebug(
              "[Audio] Received Server Audio Format: %uHz, %u-bit, %uch, Codec=%s%s",
              newFormat.sampleRate, newFormat.bitDepth,
              newFormat.channels, newFormat.codec == 1 ? "Opus" : "PCM", suffix);
        }

        if (formatChanged) {
          // If capture or mic receiver was already running, stop first so StartLoopbackCapture
          // and StartMicReceiver re-initialize with the updated format instead of returning early.
          if (State::enableClientAudioStream) {
            Audio::StopLoopbackCapture();
            Audio::StartLoopbackCapture();
          }
          if (State::enableClientMicReceive) {
            Audio::StopMicReceiver();
            Audio::StartMicReceiver();
          }
        } else {
          // Format dimensions unchanged (or codec toggled in-place); start streams if needed.
          if (State::enableClientAudioStream) {
            Audio::StartLoopbackCapture();
          }
          if (State::enableClientMicReceive) {
            Audio::StartMicReceiver();
          }
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
      if (finalSize > 0) {
        Audio::HandleMicData(finalPayload, finalSize);
      }
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
  {
    std::lock_guard<std::mutex> cvLock(g_heartbeatCvMutex);
  }
  g_heartbeatCv.notify_all();
  ReleaseStuckModifiers(); // Catch all hard disconnects or timeouts
  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Listener Thread exiting.");

  uint64_t sessionDuration = GetTickCount64() - connectionStartTime;
  {
    UdpReplayStats snap = {};
    {
      std::lock_guard<std::mutex> statsLock(g_clientUdpStatsMutex);
      snap = g_clientUdpStats;
    }
    if (State::globalDebugMode) {
      UI::LogDebug("[Session Summary] Client session to %s:%u ended after %.1fs | Transport=%s | "
                   "UDP rx=%llu lost=%llu ooo=%llu stale=%llu dup=%llu | TCP stalls=%u",
                   lastConnectedIp.c_str(), lastConnectedPort,
                   static_cast<double>(sessionDuration) / 1000.0,
                   g_sessionUsedUdp.load(std::memory_order_relaxed) ? "UDP" : "TCP-Fallback",
                   static_cast<unsigned long long>(snap.packetsAccepted),
                   static_cast<unsigned long long>(snap.sequenceGaps),
                   static_cast<unsigned long long>(snap.packetsOutOfOrder),
                   static_cast<unsigned long long>(snap.staleDrops),
                   static_cast<unsigned long long>(snap.duplicateDrops),
                   g_clientTcpStalls.load(std::memory_order_relaxed));
    }
  }

  if (!isIntentionalDisconnect && !g_authRejected.load(std::memory_order_relaxed) &&
      State::enableClientAutoReconnect && !isAutoReconnecting) {
    const bool resumed = g_systemJustResumed.exchange(false, std::memory_order_acq_rel);
    const int nextBackoff = ReconnectPolicy::NextDelayAfterDisconnect(
        reconnectBackoffSeconds.load(std::memory_order_relaxed),
        isCurrentSessionAutoReconnected.load(std::memory_order_relaxed),
        sessionDuration,
        resumed);
    reconnectBackoffSeconds.store(nextBackoff, std::memory_order_relaxed);

    if (!resumed && isCurrentSessionAutoReconnected &&
        sessionDuration < ReconnectPolicy::kShortSessionMs) {
      if (State::globalDebugMode)
        UI::LogDebug("[Network Client] Short-lived session (%llums). "
                     "Increasing backoff to %ds.",
                     (unsigned long long)sessionDuration, nextBackoff);
    }

    isAutoReconnecting.store(true, std::memory_order_release);
    State::SetClientStatus("Connection dropped. Auto-reconnecting in " +
                           std::to_string(nextBackoff) + "s...");
    if (State::globalDebugMode)
      UI::LogDebug("[Network Client] Connection dropped unintentionally. "
                   "Preparing to auto-reconnect in %ds...",
                   nextBackoff);

    // Stop audio capture/mic receiver before reconnecting to prevent stale
    // threads from accumulating across reconnect cycles.
    Audio::StopLoopbackCapture();
    Audio::StopMicReceiver();

    if (autoReconnectThread.joinable()) {
      autoReconnectThread.join();
    }

    std::string capturedIp = lastConnectedIp;
    uint16_t capturedPort = lastConnectedPort;
    int initialDelaySec = nextBackoff;
    autoReconnectThread = std::thread([capturedIp, capturedPort,
                                       initialDelaySec]() {
      int delaySec = initialDelaySec;
      while (!isIntentionalDisconnect.load(std::memory_order_acquire) &&
             !g_authRejected.load(std::memory_order_relaxed) &&
             State::enableClientAutoReconnect.load(std::memory_order_relaxed)) {
        auto wakeAt = std::chrono::steady_clock::now() + std::chrono::seconds(delaySec);
        {
          std::unique_lock<std::mutex> rkLock(g_reconnectCvMutex);
          g_reconnectCv.wait_until(rkLock, wakeAt, []() {
            return isIntentionalDisconnect.load(std::memory_order_acquire) ||
                   g_authRejected.load(std::memory_order_relaxed) ||
                   !State::enableClientAutoReconnect.load(std::memory_order_relaxed) ||
                   g_systemJustResumed.load(std::memory_order_acquire);
          });
        }

        if (isIntentionalDisconnect.load(std::memory_order_acquire) ||
            g_authRejected.load(std::memory_order_relaxed) ||
            !State::enableClientAutoReconnect.load(std::memory_order_relaxed)) {
          break;
        }

        if (g_systemJustResumed.exchange(false, std::memory_order_acq_rel)) {
          delaySec = ReconnectPolicy::kWakeDelaySec;
          auto wakeRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(delaySec);
          std::unique_lock<std::mutex> rkLock(g_reconnectCvMutex);
          g_reconnectCv.wait_until(rkLock, wakeRetryAt, []() {
            return isIntentionalDisconnect.load(std::memory_order_acquire) ||
                   g_authRejected.load(std::memory_order_relaxed) ||
                   !State::enableClientAutoReconnect.load(std::memory_order_relaxed);
          });
          if (isIntentionalDisconnect.load(std::memory_order_acquire) ||
              g_authRejected.load(std::memory_order_relaxed) ||
              !State::enableClientAutoReconnect.load(std::memory_order_relaxed)) {
            break;
          }
        }

        if (State::globalDebugMode) {
          UI::LogDebug("[Network Client] Attempting auto-reconnect to %s:%u...",
                       capturedIp.c_str(), capturedPort);
        }
        if (StartClient(capturedIp, capturedPort, true)) {
          // StartClient clears isAutoReconnecting before spawning the new ClientLoop.
          return;
        }

        if (g_fatalConnectError.load(std::memory_order_acquire) ||
            isIntentionalDisconnect.load(std::memory_order_acquire) ||
            g_authRejected.load(std::memory_order_relaxed) ||
            !State::enableClientAutoReconnect.load(std::memory_order_relaxed)) {
          break;
        }

        delaySec = ReconnectPolicy::NextDelayAfterConnectFailure(
            delaySec, g_systemJustResumed.exchange(false, std::memory_order_acq_rel));
        reconnectBackoffSeconds.store(delaySec, std::memory_order_relaxed);
        State::SetClientStatus("Server unreachable. Retrying in " +
                               std::to_string(delaySec) + "s...");
        if (State::globalDebugMode) {
          UI::LogDebug("[Network Client] Auto-reconnect failed for %s:%u; retrying in %ds.",
                       capturedIp.c_str(), capturedPort, delaySec);
        }
      }
      isAutoReconnecting.store(false, std::memory_order_release);
    });
  }
}

/**
 * @brief Initializes WinSock2 and connects to the Server's IP and port.
 * @param ip The IPv4 address of the Server PC.
 * @param port The TCP port the Server is listening on.
 * @param isAutoReconnect True when invoked from the background exponential-backoff reconnect loop.
 * @return true if connected successfully, false otherwise.
 */
bool StartClient(const std::string &ip, uint16_t port, bool isAutoReconnect) {
  g_fatalConnectError.store(false, std::memory_order_relaxed);
  if (!isAutoReconnect) {
    isIntentionalDisconnect = true;
    {
      std::lock_guard<std::mutex> rkLock(g_reconnectCvMutex);
    }
    g_reconnectCv.notify_all();
    if (autoReconnectThread.joinable()) {
      autoReconnectThread.join();
    }
    isAutoReconnecting = false;
    isIntentionalDisconnect = false;
  } else if (isIntentionalDisconnect.load(std::memory_order_acquire)) {
    return false;
  }

  if (isClientRunning)
    return false;
  g_hasServerAudioFormat =
      false; // Reset audio format state for the new connection.

  g_authRejected.store(false, std::memory_order_relaxed);
  lastConnectedIp = ip;
  lastConnectedPort = port;
  isCurrentSessionAutoReconnected = isAutoReconnect;
  connectionStartTime = GetTickCount64();

  State::SetClientStatus("Connecting...");

  clientTxSequence = 0; // Reset monotonic counter for new connection
  clientUdpTxSequence = 0;
  udpRxSequence = 0;
  udpRxReplayMask = 0;
  isUdpActive.store(false, std::memory_order_relaxed);
  g_hasLoggedTcpFallback.store(false, std::memory_order_relaxed);
  g_udpHandshakeStartMs.store(GetTickCount64(), std::memory_order_relaxed);
  g_lastReturnControlTickMs.store(0, std::memory_order_relaxed);
  g_resetPullBackAccumulator.store(true, std::memory_order_release);

  if (clientThread.joinable()) {
    if (State::globalDebugMode)
      UI::LogDebug("[Network Client] Reaping zombie thread from previous "
                   "connection attempt...");
    clientThread.join();
  }
  if (clientHeartbeatThread.joinable()) {
    clientHeartbeatThread.join();
  }
  if (clientSocket != INVALID_SOCKET) {
    closesocket(clientSocket);
    clientSocket = INVALID_SOCKET;
  }
  if (clientUdpSocket != INVALID_SOCKET) {
    closesocket(clientUdpSocket);
    clientUdpSocket = INVALID_SOCKET;
  }

  if (clientUdpListenerThread.joinable()) {
    clientUdpListenerThread.join();
  }

  {
    std::lock_guard<std::mutex> statsLock(g_clientUdpStatsMutex);
    udpRxSequence = 0;
    udpRxReplayMask = 0;
    g_clientUdpStats = {};
  }
  g_clientUdpDecryptFails.store(0, std::memory_order_relaxed);
  g_clientUdpSendErrors.store(0, std::memory_order_relaxed);
  g_clientTcpStalls.store(0, std::memory_order_relaxed);
  g_clientConsecutiveHbSkips.store(0, std::memory_order_relaxed);
  g_loggedUdpTimeout.store(false, std::memory_order_relaxed);
  g_sessionUsedUdp.store(false, std::memory_order_relaxed);

  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Attempting TCP Connection to %s:%u...",
                 ip.c_str(), port);

  std::string targetPin;
  std::string targetName = "KAMFlow_Server_" + ip;
  if (!CredentialManager::LoadSecret(targetName, targetPin)) {
    g_fatalConnectError.store(true, std::memory_order_release);
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
    g_fatalConnectError.store(true, std::memory_order_release);
    State::SetClientStatus("Security Initialization Failed.");
    if (State::globalDebugMode)
      UI::LogDebug(
          "[Security] FATAL: Cryptography Engine failed to initialize.");
    return false;
  }

  if (!g_clientWsaInitialized.exchange(true)) {
    WSADATA wsa;
    int wsaErr = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wsaErr != 0) {
      g_clientWsaInitialized.store(false);
      g_fatalConnectError.store(true, std::memory_order_release);
      State::SetClientStatus("Connection Failed (WinSock Error).");
      if (State::globalDebugMode) {
        UI::LogDebug("[Network Client] ERROR: WSAStartup failed (%d).", wsaErr);
      }
      return false;
    }
  }

  clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (clientSocket == INVALID_SOCKET) {
    if (State::globalDebugMode) {
      UI::LogDebug("[Network Client] ERROR: TCP socket() failed (WSA Error: %d).", WSAGetLastError());
    }
    return false;
  }

  sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

  if (connect(clientSocket, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
    int err = WSAGetLastError();
    State::SetClientStatus("Connection Failed. Server is offline or blocked.");
    if (State::globalDebugMode)
      UI::LogDebug("[Network Client] Connection Failed to %s:%u (WSA Error: %d).",
                   ip.c_str(), port, err);
    closesocket(clientSocket);
    // Reset the handle so the stale-socket cleanup at the top of the next
    // StartClient() call cannot close a recycled descriptor owned by another socket.
    clientSocket = INVALID_SOCKET;
    return false;
  }

  if (isAutoReconnect && isIntentionalDisconnect.load(std::memory_order_acquire)) {
    closesocket(clientSocket);
    clientSocket = INVALID_SOCKET;
    return false;
  }

  int flag = 1;
  setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
             sizeof(flag));

  // 10-second timeout gives 5× the heartbeat interval as margin,
  // preventing premature disconnects during gaming CPU stalls.
  DWORD timeout = 10000;
  setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));
  DWORD sndTimeout = 10000;
  setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&sndTimeout,
             sizeof(sndTimeout));

  isClientRunning = true;

  // Generate a non-zero 64-bit cryptographic session token to bind our UDP stream
  // to this authenticated TCP session.
  uint64_t sessionToken = 0;
  while (sessionToken == 0) {
    BCryptGenRandom(NULL, (PUCHAR)&sessionToken, sizeof(sessionToken),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  }
  g_clientSessionToken.store(sessionToken, std::memory_order_relaxed);

  AuthPayload p;
  memset(&p, 0, sizeof(p));
  p.authVerifier = PACKET_MAGIC;
  p.sessionToken = sessionToken;

  char hostname[32] = "KAM-Flow-Client";
  DWORD size = sizeof(hostname);
  GetComputerNameA(hostname, &size);
  strncpy_s(p.clientName, sizeof(p.clientName), hostname, _TRUNCATE);

  sockaddr_in localAddr;
  int localAddrLen = sizeof(localAddr);
  int localPort = 0;
  if (getsockname(clientSocket, (sockaddr *)&localAddr, &localAddrLen) != SOCKET_ERROR) {
    localPort = ntohs(localAddr.sin_port);
  }

  State::SetClientStatus("Authenticating...");
  if (State::globalDebugMode) {
    UI::LogDebug("[Network Client] Connected from local port %u to %s:%u. Sending encrypted AUTH (%zu bytes)...",
                 localPort, ip.c_str(), port, sizeof(p));
  }

  if (!SendToServer(MessageType::EVENT_AUTH, &p, sizeof(p))) {
    if (State::globalDebugMode)
      UI::LogDebug("[Network Client] CRITICAL: Failed to send AUTH handshake (WSA Error: %d).",
                   WSAGetLastError());
    State::SetClientStatus("Disconnected (Auth Send Failed).");
    isClientRunning = false;
    closesocket(clientSocket);
    clientSocket = INVALID_SOCKET;
    Security::Shutdown();
    return false;
  }

  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Handshake Sent. AES-GCM Active.");

  // Configure the Server's UDP address
  memset(&serverUdpAddr, 0, sizeof(serverUdpAddr));
  serverUdpAddr.sin_family = AF_INET;
  serverUdpAddr.sin_port =
      htons(port); // Server UDP listener is bound to the same port
  inet_pton(AF_INET, ip.c_str(), &serverUdpAddr.sin_addr);

  // Bind the UDP listener socket
  clientUdpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (clientUdpSocket == INVALID_SOCKET) {
    if (State::globalDebugMode) {
      UI::LogDebug("[Network Client] WARNING: UDP socket() failed (WSA Error: %d); using TCP fallback.",
                   WSAGetLastError());
    }
  } else {
    sockaddr_in localAddr = {0};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0; // Ephemeral port
    if (bind(clientUdpSocket, (sockaddr *)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
      if (State::globalDebugMode) {
        UI::LogDebug("[Network Client] WARNING: UDP bind() failed (WSA Error: %d); using TCP fallback.",
                     WSAGetLastError());
      }
      closesocket(clientUdpSocket);
      clientUdpSocket = INVALID_SOCKET;
    }
  }
  if (clientUdpSocket != INVALID_SOCKET) {

    // Set SO_RCVTIMEO on the UDP socket so recvfrom() in ClientUDPListenerLoop
    // periodically unblocks and checks isClientRunning, preventing zombie
    // threads when the socket is closed during reconnect.
    DWORD udpTimeout = 500;
    setsockopt(clientUdpSocket, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&udpTimeout, sizeof(udpTimeout));

    // 512 KB kernel buffer prevents UDP drops under simultaneous 1000Hz mouse + 48kHz audio bursts
    int udpBufSize = 524288;
    setsockopt(clientUdpSocket, SOL_SOCKET, SO_RCVBUF,
               (const char *)&udpBufSize, sizeof(udpBufSize));
    setsockopt(clientUdpSocket, SOL_SOCKET, SO_SNDBUF,
               (const char *)&udpBufSize, sizeof(udpBufSize));

    // Suppress Windows ICMP Port Unreachable (WSAECONNRESET) errors on UDP socket
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    WSAIoctl(clientUdpSocket, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior),
             nullptr, 0, &dwBytesReturned, nullptr, nullptr);

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
  }

  State::SetClientStatus("Connected");

  if (isAutoReconnect) {
    // Clear before spawning ClientLoop so an instant early disconnect can re-arm auto-reconnect.
    isAutoReconnecting.store(false, std::memory_order_release);
  }

  clientThread = std::thread([]() {
    // Elevate thread priority to prevent starvation from the OS scheduler
    // during heavy workloads.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ClientLoop();
  });
  clientHeartbeatThread = std::thread([]() {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    bool wasLocked = false;
    int heartbeatCycles = 0;
    uint32_t udpRetryAttempt = 0;
    UdpReplayStats prevStats = {};

    const auto sessionStart = std::chrono::steady_clock::now();
    auto lastLoopTick = sessionStart;
    auto nextHeartbeat = sessionStart + std::chrono::milliseconds(2000);
    auto nextUdpRetry = sessionStart + std::chrono::milliseconds(ComputeUdpHandshakeRetryDelayMs(0));

    while (isClientRunning) {
      bool udpActiveNow = isUdpActive.load(std::memory_order_relaxed);
      bool hasUdpSocket = (clientUdpSocket != INVALID_SOCKET);
      auto wakeTarget = (!udpActiveNow && hasUdpSocket)
                            ? (std::min)(nextHeartbeat, nextUdpRetry)
                            : nextHeartbeat;

      {
        std::unique_lock<std::mutex> cvLock(g_heartbeatCvMutex);
        g_heartbeatCv.wait_until(cvLock, wakeTarget, []() {
          return !isClientRunning.load(std::memory_order_relaxed) ||
                 g_forceImmediateHeartbeat.load(std::memory_order_acquire);
        });
      }
      if (!isClientRunning) break;

      auto now = std::chrono::steady_clock::now();
      // Fallback standby detector if OEM power policy suppresses WM_POWERBROADCAST.
      if ((now - lastLoopTick) > std::chrono::seconds(3) &&
          isClientRunning.load(std::memory_order_relaxed)) {
        g_systemJustResumed.store(true, std::memory_order_release);
        g_forceImmediateHeartbeat.store(true, std::memory_order_release);
      }
      lastLoopTick = now;

      udpActiveNow = isUdpActive.load(std::memory_order_relaxed);
      hasUdpSocket = (clientUdpSocket != INVALID_SOCKET);

      if (udpActiveNow) {
        udpRetryAttempt = 0;
      } else if (hasUdpSocket && now >= nextUdpRetry) {
        if ((now - sessionStart) >= std::chrono::seconds(10) &&
            !g_loggedUdpTimeout.exchange(true, std::memory_order_relaxed) &&
            State::globalDebugMode) {
          UI::LogDebug("[Network Client] WARNING: UDP handshake unacknowledged after 10s (firewall/NAT?); running in TCP fallback mode.");
        }
        SendUdpHandshake();
        ++udpRetryAttempt;
        nextUdpRetry = now + std::chrono::milliseconds(ComputeUdpHandshakeRetryDelayMs(udpRetryAttempt));
      }

      const bool forceHb = g_forceImmediateHeartbeat.exchange(false, std::memory_order_acq_rel);
      if (!forceHb && now < nextHeartbeat) {
        continue;
      }
      nextHeartbeat = now + std::chrono::milliseconds(2000);

      SendHeartbeatToSocket(clientSocket, clientSendMutex);

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

      if (++heartbeatCycles >= 15) {
        heartbeatCycles = 0;
        UdpReplayStats currStats = {};
        {
          std::lock_guard<std::mutex> statsLock(g_clientUdpStatsMutex);
          currStats = g_clientUdpStats;
        }
        uint64_t decFail = g_clientUdpDecryptFails.exchange(0, std::memory_order_relaxed);
        uint64_t txErr = g_clientUdpSendErrors.exchange(0, std::memory_order_relaxed);

        auto deltaU64 = [](uint64_t cur, uint64_t prev) -> uint64_t {
          return (cur >= prev) ? (cur - prev) : 0;
        };
        uint64_t dRx = deltaU64(currStats.packetsAccepted, prevStats.packetsAccepted);
        uint64_t dOoo = deltaU64(currStats.packetsOutOfOrder, prevStats.packetsOutOfOrder);
        uint64_t dLost = deltaU64(currStats.sequenceGaps, prevStats.sequenceGaps);
        uint64_t dStale = deltaU64(currStats.staleDrops, prevStats.staleDrops);
        uint64_t dDup = deltaU64(currStats.duplicateDrops, prevStats.duplicateDrops);
        prevStats = currStats;

        if ((ShouldEmitPerfSummary(dLost + dStale + dDup + decFail + txErr) || dOoo > 50) &&
            State::globalDebugMode) {
          UI::LogDebug("[Network Perf] Client UDP 30s: rx=%llu lost=%llu ooo=%llu stale=%llu dup=%llu decFail=%llu txErr=%llu",
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
  g_systemJustResumed.store(true, std::memory_order_release);
  reconnectBackoffSeconds.store(ReconnectPolicy::kWakeDelaySec, std::memory_order_relaxed);
  g_forceImmediateHeartbeat.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> rkLock(g_reconnectCvMutex);
  }
  g_reconnectCv.notify_all();
  {
    std::lock_guard<std::mutex> hbLock(g_heartbeatCvMutex);
  }
  g_heartbeatCv.notify_all();
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
  {
    std::lock_guard<std::mutex> rkLock(g_reconnectCvMutex);
  }
  g_reconnectCv.notify_all();
  {
    std::lock_guard<std::mutex> cvLock(g_heartbeatCvMutex);
  }
  g_heartbeatCv.notify_all();
  isUdpActive.store(false, std::memory_order_relaxed);
  // Ensure audio capture streams are always stopped on disconnect.
  Audio::StopLoopbackCapture();
  Audio::StopMicReceiver();

  // Signal graceful abort on the TCP socket to unblock recv() in ClientLoop
  if (clientSocket != INVALID_SOCKET) {
    shutdown(clientSocket, SD_BOTH);
  }

  // Join autoReconnectThread first so no concurrent StartClient() call can race
  // against joining/closing sockets below.
  if (autoReconnectThread.joinable())
    autoReconnectThread.join();

  isClientRunning = false;
  {
    std::lock_guard<std::mutex> cvLock(g_heartbeatCvMutex);
  }
  g_heartbeatCv.notify_all();
  if (clientSocket != INVALID_SOCKET) {
    shutdown(clientSocket, SD_BOTH);
  }

  // Join all threads BEFORE closing sockets to prevent race conditions or crashes
  // from in-flight send/recv operations on invalidated or OS-recycled socket descriptors.
  if (clientThread.joinable())
    clientThread.join();
  if (clientUdpListenerThread.joinable())
    clientUdpListenerThread.join();
  if (clientHeartbeatThread.joinable())
    clientHeartbeatThread.join();

  // Clean up socket descriptors now that all network threads have safely exited
  if (clientSocket != INVALID_SOCKET) {
    closesocket(clientSocket);
    clientSocket = INVALID_SOCKET;
  }
  if (clientUdpSocket != INVALID_SOCKET) {
    closesocket(clientUdpSocket);
    clientUdpSocket = INVALID_SOCKET;
  }

  Security::Shutdown();
  if (g_clientWsaInitialized.exchange(false)) {
    WSACleanup();
  }
  if (State::globalDebugMode)
    UI::LogDebug("[Network Client] Client successfully stopped.");
}
} // namespace Network