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

namespace Input {
    /// Global handle for the low-level mouse hook.
    HHOOK globalMouseHook = NULL;

    /// Coordinates where the mouse entered the boundary.
    POINT entryPoint = { 0, 0 };
    /// Coordinates where the mouse is locked during Remote Mode.
    POINT lockedPoint = { 0, 0 };
    /// Frames to skip to avoid self-triggering from SetCursorPos.
    int ignorePackets = 0;
    
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
            ignorePackets = 2; 
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
            if (ignorePackets > 0) { ignorePackets--; return; }

            dx = ms->pt.x - lockedPoint.x;
            dy = ms->pt.y - lockedPoint.y;
            if (dx == 0 && dy == 0) return;

            flags = MOUSEEVENTF_MOVE;
            SetCursorPos(lockedPoint.x, lockedPoint.y);
            ignorePackets = 2;
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

        if (flags & MOUSEEVENTF_MOVE) {
            float sx = dx * State::mouseSensitivity + fracX;
            float sy = dy * State::mouseSensitivity + fracY;
            dx = static_cast<int>(sx);
            dy = static_cast<int>(sy);
            fracX = sx - dx;
            fracY = sy - dy;
            if (dx == 0 && dy == 0) flags &= ~MOUSEEVENTF_MOVE;
        }

        if (flags != 0) {
            Network::MousePayload p = { dx, dy, mouseData, flags };
            Network::BroadcastMessage(Network::MessageType::EVENT_MOUSE, &p, sizeof(p));
        }
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
            if (!Network::HasAuthenticatedClients()) return CallNextHookEx(globalMouseHook, nCode, wParam, lParam);

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
                HMONITOR hMon = MonitorFromPoint(ms->pt, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfo(hMon, &mi)) {
                    int monWidth = mi.rcMonitor.right - mi.rcMonitor.left;
                    int monHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
                    
                    int deadzoneX = (monWidth * State::edgeDeadzonePercent) / 100;
                    int deadzoneY = (monHeight * State::edgeDeadzonePercent) / 100;

                    bool hitEdge = false;
                    uint8_t exitEdge = 0;
                    float normX = 0.0f, normY = 0.0f;

                    bool safeY = ms->pt.y > (mi.rcMonitor.top + deadzoneY) && ms->pt.y < (mi.rcMonitor.bottom - deadzoneY);
                    bool safeX = ms->pt.x > (mi.rcMonitor.left + deadzoneX) && ms->pt.x < (mi.rcMonitor.right - deadzoneX);

                    if (ms->pt.x <= mi.rcMonitor.left && safeY) {
                        exitEdge = 0; normX = 0.0f; normY = (float)(ms->pt.y - mi.rcMonitor.top) / monHeight; hitEdge = true;
                    } else if (ms->pt.x >= mi.rcMonitor.right - 1 && safeY) {
                        exitEdge = 1; normX = 1.0f; normY = (float)(ms->pt.y - mi.rcMonitor.top) / monHeight; hitEdge = true;
                    } else if (ms->pt.y <= mi.rcMonitor.top && safeX) {
                        exitEdge = 2; normX = (float)(ms->pt.x - mi.rcMonitor.left) / monWidth; normY = 0.0f; hitEdge = true;
                    } else if (ms->pt.y >= mi.rcMonitor.bottom - 1 && safeX) {
                        exitEdge = 3; normX = (float)(ms->pt.x - mi.rcMonitor.left) / monWidth; normY = 1.0f; hitEdge = true;
                    }
                        
                    if (hitEdge && !isTransitionedOut) {
                        POINT testPt = ms->pt;
                        if (exitEdge == 0) testPt.x -= 2;
                        else if (exitEdge == 1) testPt.x += 2;
                        else if (exitEdge == 2) testPt.y -= 2;
                        else if (exitEdge == 3) testPt.y += 2;

                        if (MonitorFromPoint(testPt, MONITOR_DEFAULTTONULL) == NULL) {
                            if (Network::RouteCursorTransition(0, 0, exitEdge, normX, normY)) {
                                isTransitionedOut = true;
                                entryPoint = ms->pt;
                                lockedPoint = { (mi.rcMonitor.left + mi.rcMonitor.right) / 2, (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2 };
                                SetCursorPos(lockedPoint.x, lockedPoint.y);
                                ignorePackets = 3;
                                return 1;
                            }
                        }
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
        globalMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookCallback, NULL, 0);
        return (globalMouseHook != NULL);
    }

    /**
     * @brief Removes the low-level mouse hook safely.
     * @return void
     */
    void StopMouseCapture() {
        if (globalMouseHook) UnhookWindowsHookEx(globalMouseHook);
    }
}