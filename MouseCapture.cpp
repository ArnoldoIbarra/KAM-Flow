// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Mouse Capture Hook Implementation
// =============================================================================

/**
 * @file MouseCapture.cpp
 * @brief Handles mouse edge detection, tethered movement, clicks, and scrolling.
 */

#include "MouseCapture.h"
#include "StateManager.h"
#include "NetworkMessages.h"
#include "NetworkServer.h"
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace Input {
    /// Global handle for the low-level mouse hook.
    HHOOK globalMouseHook = NULL;

    /// Background queue to decouple the OS hook from the network socket
    std::queue<Network::MousePayload> g_mouseQueue;
    std::mutex g_mouseQueueMutex;
    std::condition_variable g_mouseQueueCv;
    std::atomic<bool> g_isMouseNetworkThreadRunning{false};
    std::thread g_mouseNetworkThread;

    /// Coordinates where the mouse entered the boundary.
    POINT entryPoint = { 0, 0 };
    /// Coordinates where the mouse is locked during Remote Mode.
    POINT lockedPoint = { 0, 0 };
    
    /// Flags indicating which edge was breached.
    bool isTransitionedOut = false;
    
    /**
     * @brief Teleports the cursor deterministically prior to returning to LOCAL control.
     * @param entryEdge The edge the cursor is entering from.
     * @param normalizedX Normalized horizontal position (0.0 to 1.0).
     * @param normalizedY Normalized vertical position (0.0 to 1.0).
     * @return void
     */
    void HandleReturnControl(uint8_t entryEdge, float normalizedX, float normalizedY) {
        if (isTransitionedOut) {
            isTransitionedOut = false; 

            HMONITOR hMon = MonitorFromPoint(entryPoint, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfo(hMon, &mi)) {
                int w = mi.rcMonitor.right - mi.rcMonitor.left;
                int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
                
                int tx = mi.rcMonitor.left + (int)(normalizedX * w);
                int ty = mi.rcMonitor.top + (int)(normalizedY * h);
                
                if (entryEdge == 0) tx = mi.rcMonitor.left + 5;
                else if (entryEdge == 1) tx = mi.rcMonitor.right - 5;
                else if (entryEdge == 2) ty = mi.rcMonitor.top + 5;
                else if (entryEdge == 3) ty = mi.rcMonitor.bottom - 5;

                tx = std::clamp(tx, (int)mi.rcMonitor.left + 5, (int)mi.rcMonitor.right - 5);
                ty = std::clamp(ty, (int)mi.rcMonitor.top + 5, (int)mi.rcMonitor.bottom - 5);
                
                SetCursorPos(tx, ty);
            } else {
                SetCursorPos(entryPoint.x + 30, entryPoint.y); 
            }
        }
    }

    /**
     * @brief Background thread loop that consumes mouse payloads and broadcasts them over the network.
     * This fully decouples the high-frequency OS mouse hook from blocking network calls.
     * @return void
     */
    void MouseNetworkThreadLoop() {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (g_isMouseNetworkThreadRunning) {
            std::queue<Network::MousePayload> localQueue;
            {
                std::unique_lock<std::mutex> lock(g_mouseQueueMutex);
                // Coalesce mouse deltas for up to 2ms before sending.
                // This replaces the old wait() + sleep(7ms) double-throttle pattern.
                // With UDP streaming, TCP congestion is no longer a concern, so 2ms
                // provides near-native responsiveness while still batching a few deltas.
                g_mouseQueueCv.wait_for(lock, std::chrono::milliseconds(2), [] { 
                    return !g_mouseQueue.empty() || !g_isMouseNetworkThreadRunning; 
                });

                if (!g_isMouseNetworkThreadRunning && g_mouseQueue.empty()) {
                    break;
                }

                // Swap queues instantly to release the mutex back to the hook thread
                std::swap(localQueue, g_mouseQueue);
            }

            if (localQueue.empty()) continue; // Timeout with no events, just loop back

            // Perform the blocking network calls outside of the OS hook chain
            Network::MousePayload coalescedMove = { 0, 0, 0, 0 };
            bool hasCoalescedMove = false;

            while (!localQueue.empty()) {
                auto p = localQueue.front();
                localQueue.pop();

                // If it is a pure relative move, sum the deltas to compress multiple events into one packet
                if (p.flags == MOUSEEVENTF_MOVE && p.mouseData == 0) {
                    coalescedMove.deltaX += p.deltaX;
                    coalescedMove.deltaY += p.deltaY;
                    coalescedMove.flags = MOUSEEVENTF_MOVE;
                    hasCoalescedMove = true;
                } else {
                    // Send any accumulated movement BEFORE sending the discrete click/scroll
                    if (hasCoalescedMove) {
                        Network::BroadcastMessage(Network::MessageType::EVENT_MOUSE, &coalescedMove, sizeof(coalescedMove));
                        coalescedMove = { 0, 0, 0, 0 };
                        hasCoalescedMove = false;
                    }
                    // Send the discrete action
                    Network::BroadcastMessage(Network::MessageType::EVENT_MOUSE, &p, sizeof(p));
                }
            }

            // Flush any remaining accumulated movement
            if (hasCoalescedMove) {
                Network::BroadcastMessage(Network::MessageType::EVENT_MOUSE, &coalescedMove, sizeof(coalescedMove));
            }
        }
    }

    /**
     * @brief Processes all mouse input during Remote mode, translating clicks and scrolls to flags.
     * @param wParam Message identifier representing the specific mouse action.
     * @param ms Pointer to the low-level mouse struct containing exact hardware data.
     * @return void
     */
    void HandleRemoteMouseInput(WPARAM wParam, const MSLLHOOKSTRUCT* ms) {
        uint32_t flags = 0;
        uint32_t mouseData = 0;
        int dx = 0, dy = 0;

        static float fracX = 0.0f;
        static float fracY = 0.0f;

        if (wParam == WM_MOUSEMOVE) {
            int rawDx = ms->pt.x - lockedPoint.x;
            int rawDy = ms->pt.y - lockedPoint.y;

            // Failsafe: Re-anchor the cursor if a background app forcefully moved the OS cursor
            if (abs(rawDx) > 300 || abs(rawDy) > 300) {
                SetCursorPos(lockedPoint.x, lockedPoint.y);
                return;
            }

            if (rawDx != 0 || rawDy != 0) {
                float sx = rawDx * State::mouseSensitivity + fracX;
                float sy = rawDy * State::mouseSensitivity + fracY;
                dx = static_cast<int>(sx);
                dy = static_cast<int>(sy);
                fracX = sx - dx;
                fracY = sy - dy;

                if (dx != 0 || dy != 0) {
                    Network::MousePayload p = { dx, dy, 0, MOUSEEVENTF_MOVE };
                    std::lock_guard<std::mutex> lock(g_mouseQueueMutex);
                    g_mouseQueue.push(p);
                    g_mouseQueueCv.notify_one();
                }
            }
            return; 
        } else {
            switch (wParam) {
                case WM_LBUTTONDOWN: flags = MOUSEEVENTF_LEFTDOWN; break;
                case WM_LBUTTONUP:   flags = MOUSEEVENTF_LEFTUP;   break;
                case WM_RBUTTONDOWN: flags = MOUSEEVENTF_RIGHTDOWN; break;
                case WM_RBUTTONUP:   flags = MOUSEEVENTF_RIGHTUP;   break;
                case WM_MBUTTONDOWN: flags = MOUSEEVENTF_MIDDLEDOWN; break;
                case WM_MBUTTONUP:   flags = MOUSEEVENTF_MIDDLEUP;   break;
                case WM_MOUSEWHEEL:  
                    flags = MOUSEEVENTF_WHEEL; 
                    mouseData = static_cast<uint32_t>(static_cast<short>(HIWORD(ms->mouseData))); 
                    break;
                case WM_MOUSEHWHEEL: 
                    flags = MOUSEEVENTF_HWHEEL; 
                    mouseData = static_cast<uint32_t>(static_cast<short>(HIWORD(ms->mouseData))); 
                    break;
                case WM_XBUTTONDOWN: 
                    flags = MOUSEEVENTF_XDOWN; 
                    mouseData = HIWORD(ms->mouseData); 
                    break;
                case WM_XBUTTONUP:   
                    flags = MOUSEEVENTF_XUP;   
                    mouseData = HIWORD(ms->mouseData); 
                    break;
                default: return; 
            }
        }

        Network::MousePayload p = { 0, 0, mouseData, flags };
        
        std::lock_guard<std::mutex> lock(g_mouseQueueMutex);
        g_mouseQueue.push(p);
        g_mouseQueueCv.notify_one();
    }

    /**
     * @brief Processes mouse events intercepted by the hook.
     * @param nCode Hook code.
     * @param wParam Message identifier.
     * @param lParam Pointer to MSLLHOOKSTRUCT.
     * @return Result of the next hook in the chain or 1 to block.
     */
    LRESULT CALLBACK MouseHookCallback(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0) {
            // Absolute Fast-Path: Instantly return to OS scheduler if Game Mode is active
            if (State::enableGameMode) return CallNextHookEx(globalMouseHook, nCode, wParam, lParam);

            if (!Network::g_hasClients.load(std::memory_order_relaxed)) return CallNextHookEx(globalMouseHook, nCode, wParam, lParam);

            MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
            if (ms->flags & LLMHF_INJECTED) return CallNextHookEx(globalMouseHook, nCode, wParam, lParam);

            if (!State::IsRemote() && isTransitionedOut) {
                isTransitionedOut = false;
                SetCursorPos(entryPoint.x + 30, entryPoint.y); 
            }

            if (State::IsRemote()) {
                HandleRemoteMouseInput(wParam, ms);
                return 1; 
            }

            if (wParam == WM_MOUSEMOVE) {
                static HMONITOR cachedMonitor = NULL;
                static MONITORINFO cachedMi = { sizeof(cachedMi) };
                
                HMONITOR hMon = MonitorFromPoint(ms->pt, MONITOR_DEFAULTTONEAREST);
                if (hMon != cachedMonitor) {
                    cachedMi.cbSize = sizeof(cachedMi);
                    if (GetMonitorInfo(hMon, &cachedMi)) {
                        cachedMonitor = hMon;
                    } else {
                        cachedMonitor = NULL;
                    }
                }
                
                if (cachedMonitor) {
                    int monWidth = cachedMi.rcMonitor.right - cachedMi.rcMonitor.left;
                    int monHeight = cachedMi.rcMonitor.bottom - cachedMi.rcMonitor.top;
                    
                    int deadzoneX = (monWidth * State::edgeDeadzonePercent) / 100;
                    int deadzoneY = (monHeight * State::edgeDeadzonePercent) / 100;

                    bool hitEdge = false;
                    uint8_t exitEdge = 0;
                    float normX = 0.0f, normY = 0.0f;

                    bool safeY = ms->pt.y > (cachedMi.rcMonitor.top + deadzoneY) && ms->pt.y < (cachedMi.rcMonitor.bottom - deadzoneY);
                    bool safeX = ms->pt.x > (cachedMi.rcMonitor.left + deadzoneX) && ms->pt.x < (cachedMi.rcMonitor.right - deadzoneX);

                    if (ms->pt.x <= cachedMi.rcMonitor.left && safeY) {
                        exitEdge = 0; normX = 0.0f; normY = (float)(ms->pt.y - cachedMi.rcMonitor.top) / monHeight; hitEdge = true;
                    } else if (ms->pt.x >= cachedMi.rcMonitor.right - 1 && safeY) {
                        exitEdge = 1; normX = 1.0f; normY = (float)(ms->pt.y - cachedMi.rcMonitor.top) / monHeight; hitEdge = true;
                    } else if (ms->pt.y <= cachedMi.rcMonitor.top && safeX) {
                        exitEdge = 2; normX = (float)(ms->pt.x - cachedMi.rcMonitor.left) / monWidth; normY = 0.0f; hitEdge = true;
                    } else if (ms->pt.y >= cachedMi.rcMonitor.bottom - 1 && safeX) {
                        exitEdge = 3; normX = (float)(ms->pt.x - cachedMi.rcMonitor.left) / monWidth; normY = 1.0f; hitEdge = true;
                    }
                        
                    static int consecutiveEdgeHits = 0;
                    if (hitEdge && !isTransitionedOut) {
                        consecutiveEdgeHits++;
                        
                        // Require 15 consecutive edge-push events to prevent 3D games from triggering 
                        // accidental transitions during single-frame cursor recentering.
                        if (consecutiveEdgeHits >= 15) {
                            POINT testPt = ms->pt;
                            if (exitEdge == 0) testPt.x -= 2;
                            else if (exitEdge == 1) testPt.x += 2;
                            else if (exitEdge == 2) testPt.y -= 2;
                            else if (exitEdge == 3) testPt.y += 2;

                            if (MonitorFromPoint(testPt, MONITOR_DEFAULTTONULL) == NULL) {
                                if (Network::RouteCursorTransition(0, 0, exitEdge, normX, normY)) {
                                    isTransitionedOut = true;
                                    entryPoint = ms->pt;
                                    lockedPoint = { (cachedMi.rcMonitor.left + cachedMi.rcMonitor.right) / 2, (cachedMi.rcMonitor.top + cachedMi.rcMonitor.bottom) / 2 };
                                    SetCursorPos(lockedPoint.x, lockedPoint.y);
                                    consecutiveEdgeHits = 0;
                                    return 1;
                                }
                            }
                        }
                    } else {
                        consecutiveEdgeHits = 0;
                    }
                }
            }
        }
        return CallNextHookEx(globalMouseHook, nCode, wParam, lParam);
    }

    /**
     * @brief Installs the low-level global mouse hook (WH_MOUSE_LL).
     * @return true if the hook was successfully installed, false otherwise.
     */
    bool StartMouseCapture() {
        if (globalMouseHook != NULL) {
            return true; // Prevent handle leaking from double-hooks
        }
        
        g_isMouseNetworkThreadRunning = true;
        g_mouseNetworkThread = std::thread(MouseNetworkThreadLoop);
        
        globalMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookCallback, NULL, 0);
        return (globalMouseHook != NULL);
    }

    /**
     * @brief Removes the low-level mouse hook safely.
     * @return void
     */
    void StopMouseCapture() {
        if (globalMouseHook) {
            UnhookWindowsHookEx(globalMouseHook);
            globalMouseHook = NULL;
        }

        g_isMouseNetworkThreadRunning = false;
        g_mouseQueueCv.notify_all();
        if (g_mouseNetworkThread.joinable()) {
            g_mouseNetworkThread.join();
        }
    }
}