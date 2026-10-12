// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// UI Manager Windows/DX11 Implementation
// =============================================================================

/**
 * @file UIManager.cpp
 * @brief Implementation of the Win32/DX11 UI Manager utilizing Dear ImGui.
 * Connects directly to the StateManager and Network roles for real-time visualization.
 */

#include "UIManager.h"
#include "StateManager.h"
#include "NetworkServer.h"
#include "NetworkClient.h" 
#include "CredentialManager.h"
#include "AudioManager.h"
#include "ConfigManager.h"
#include "FileTransferManager.h"
#include "MouseCapture.h"
#include <string>
#include <string_view>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <shellapi.h>
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <streambuf>
#include <mutex>
#include <commdlg.h>
#include <cstdio>
#include <cmath>
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern std::atomic<DWORD> g_HookThreadId;
#define WM_TOGGLE_GAMEMODE (WM_APP + 1)

namespace UI {
    /// Win32 Window Handle.
    HWND hwnd = nullptr;
    /// D3D11 Device interface.
    ID3D11Device* pd3dDevice = nullptr;
    /// D3D11 Device Context interface.
    ID3D11DeviceContext* pd3dDeviceContext = nullptr;
    /// D3D11 Swap Chain for double buffering.
    IDXGISwapChain* pSwapChain = nullptr;
    /// D3D11 Render Target View.
    ID3D11RenderTargetView* pMainRenderTargetView = nullptr;
    /// Flag tracking if the window remains open.
    bool isAppRunning = false;
    char newServerIP[64] = "";
    char newServerPIN[16] = "";
    bool showServerPin = false;

    // Pairing flow state for the client mode Connections tab
    char g_pairingTargetIP[64] = "";
    char g_pairingTargetHostname[64] = "";
    char g_pairingPinInput[16] = "";
    bool g_showPinPopup = false;
    bool g_showAlreadyPairedWarning = false;
    bool g_showNotPairedPopup = false;
    bool g_editingPairedIP = false;
    char g_editIPBuffer[64] = "";
    bool g_editIPInvalid = false;
    bool g_showPairingPin = false;

    /**
     * @brief Validates that a C-string is a well-formed IPv4 address (4 octets, 0-255).
     * @param ip The null-terminated string to validate.
     * @return true if the string is a valid IPv4 address.
     */
    bool IsValidIPv4(const char* ip) {
        if (!ip || ip[0] == '\0') return false;
        int octets[4];
        char extra;
        // sscanf with trailing %c detects garbage after the last octet
        int matched = sscanf(ip, "%d.%d.%d.%d%c", &octets[0], &octets[1], &octets[2], &octets[3], &extra);
        if (matched != 4) return false;
        for (int i = 0; i < 4; ++i) {
            if (octets[i] < 0 || octets[i] > 255) return false;
        }
        // Reject leading zeros in octets (e.g., "192.168.01.1")
        const char* p = ip;
        for (int i = 0; i < 4; ++i) {
            if (*p == '0' && *(p + 1) != '.' && *(p + 1) != '\0') return false;
            while (*p && *p != '.') ++p;
            if (*p == '.') ++p;
        }
        return true;
    }
    
    /**
     * @brief Adapts an std::atomic<bool> state variable to the Dear ImGui Checkbox widget.
     * @param label UI label string for the checkbox.
     * @param val Reference to the std::atomic<bool> variable to read and update.
     * @return bool True if the checkbox value was toggled in the current frame, false otherwise.
     */
    static bool AtomicCheckbox(const char* label, std::atomic<bool>& val) {
        bool current = val.load(std::memory_order_relaxed);
        if (ImGui::Checkbox(label, &current)) {
            val.store(current, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    /**
     * @brief Adapts an std::atomic<int> state variable to the Dear ImGui SliderInt widget.
     * @param label UI label string for the slider.
     * @param val Reference to the std::atomic<int> variable to read and update.
     * @param v_min Minimum integer range bound.
     * @param v_max Maximum integer range bound.
     * @param format Optional format specifier for the slider display text.
     * @return bool True if the slider value was modified in the current frame, false otherwise.
     */
    static bool AtomicSliderInt(const char* label, std::atomic<int>& val, int v_min, int v_max, const char* format = "%d") {
        int current = val.load(std::memory_order_relaxed);
        if (ImGui::SliderInt(label, &current, v_min, v_max, format)) {
            val.store(current, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    /**
     * @brief Adapts an std::atomic<float> state variable to the Dear ImGui SliderFloat widget.
     * @param label UI label string for the slider.
     * @param val Reference to the std::atomic<float> variable to read and update.
     * @param v_min Minimum floating-point range bound.
     * @param v_max Maximum floating-point range bound.
     * @param format Optional format specifier for the slider display text.
     * @return bool True if the slider value was modified in the current frame, false otherwise.
     */
    static bool AtomicSliderFloat(const char* label, std::atomic<float>& val, float v_min, float v_max, const char* format = "%.2f") {
        float current = val.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat(label, &current, v_min, v_max, format)) {
            val.store(current, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // --- IN-ENGINE DEBUG CONSOLE ---
    std::mutex g_logMutex;
    std::vector<char> g_logBufferVec; // Starts entirely empty to save memory
    std::string g_lineBuffer;
    size_t g_logBufferLen = 0;
    std::atomic<bool> g_logDirty(false); // Tracks if new data was written

    class ImGuiLogStreamBuf : public std::streambuf {
    public:
        std::streambuf* oldCout;
        std::streambuf* oldCerr;
        std::streambuf* oldClog;
        
        ImGuiLogStreamBuf() {
            oldCout = std::cout.rdbuf(this);
            oldCerr = std::cerr.rdbuf(this);
            oldClog = std::clog.rdbuf(this);
        }
        ~ImGuiLogStreamBuf() {
            std::cout.rdbuf(oldCout);
            std::cerr.rdbuf(oldCerr);
            std::clog.rdbuf(oldClog);
        }
    protected:
        virtual std::streamsize xsputn(const char* s, std::streamsize count) override {
            if (!State::globalDebugMode) return count; // Instant short-circuit! CPU cost is 0.
            
            g_lineBuffer.append(s, count);
            size_t newline_pos;
            while ((newline_pos = g_lineBuffer.find('\n')) != std::string::npos) {
                std::string line = g_lineBuffer.substr(0, newline_pos);
                g_lineBuffer.erase(0, newline_pos + 1);

                SYSTEMTIME st;
                GetLocalTime(&st);
                char time_buf[64];
                sprintf_s(time_buf, sizeof(time_buf), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
                
                std::string final_line = std::string(time_buf) + line + "\n";

                std::lock_guard<std::mutex> lock(g_logMutex);
                if (g_logBufferVec.empty()) g_logBufferVec.resize(1024 * 1024, 0); // Lazy allocation
                
                if (final_line.length() > g_logBufferVec.size() - 1) continue;

                if (g_logBufferLen + final_line.length() > g_logBufferVec.size() - 1) {
                    size_t shift = (g_logBufferLen + final_line.length()) - (g_logBufferVec.size() - 1);
                    std::memmove(g_logBufferVec.data(), g_logBufferVec.data() + shift, g_logBufferLen - shift);
                    g_logBufferLen -= shift;
                }
                std::memcpy(g_logBufferVec.data() + g_logBufferLen, final_line.c_str(), final_line.length());
                g_logBufferLen += final_line.length();
                g_logBufferVec[g_logBufferLen] = '\0';
                g_logDirty = true;
            }
            return count;
        }
        virtual int_type overflow(int_type c) override {
            if (!State::globalDebugMode) return c;
            if (c != EOF) { char ch = static_cast<char>(c); xsputn(&ch, 1); }
            return c;
        }
    };

    ImGuiLogStreamBuf* g_logInterceptor = nullptr;

    /**
     * @brief Blazing fast, thread-safe, atomic logger for time-critical threads.
     * Bypasses std::ostream overhead and locks the mutex exactly once per line.
     */
    void LogDebug(const char* fmt, ...) {
        if (!State::globalDebugMode) return;
        
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char time_buf[64];
        int timeLen = sprintf_s(time_buf, sizeof(time_buf), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        std::string finalLine = std::string(time_buf) + buf + "\n";

        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logBufferVec.empty()) g_logBufferVec.resize(1024 * 1024, 0); // Lazy allocation
        if (g_logBufferLen + finalLine.length() > g_logBufferVec.size() - 1) {
            size_t shift = (g_logBufferLen + finalLine.length()) - (g_logBufferVec.size() - 1);
            std::memmove(g_logBufferVec.data(), g_logBufferVec.data() + shift, g_logBufferLen - shift);
            g_logBufferLen -= shift;
        }
        std::memcpy(g_logBufferVec.data() + g_logBufferLen, finalLine.c_str(), finalLine.length());
        g_logBufferLen += finalLine.length();
        g_logBufferVec[g_logBufferLen] = '\0';
        g_logDirty = true;
    }

    // File transfer state variables.
    std::string g_pendingDropFile = "";
    bool g_showDropModal = false;
    bool g_triggerDropModal = false;
    std::vector<SOCKET> g_selectedTransferClients;

    // Window dimension tracking variables.
    int g_windowX = 100;
    int g_windowY = 100;
    int g_windowW = 450;
    int g_windowH = 480;

    // System tray state variables.
    #define WM_TRAYICON (WM_USER + 1)
    #define ID_TRAY_SHOW 2001
    #define ID_TRAY_EXIT 2002
    NOTIFYICONDATAA g_nid = {};
    bool g_isMinimizedToTray = false;

    /**
     * @brief Initializes the System Tray icon structure.
     * @param hWnd The handle to the main application window.
     * @return void
     */
    void InitTrayIcon(HWND hWnd) {
        ZeroMemory(&g_nid, sizeof(NOTIFYICONDATAA));
        g_nid.cbSize = sizeof(NOTIFYICONDATAA);
        g_nid.hWnd = hWnd;
        g_nid.uID = 1001;
        g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = ::LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)); // Custom Application Icon
        strncpy_s(g_nid.szTip, "KAM-Flow (Click to restore)", _TRUNCATE);
    }

    /**
     * @brief Helper to draw text-wrapped tooltips to prevent cutoff on small windows.
     * @param text The tooltip text to display.
     * @return void
     */
    void DrawWrappedTooltip(const char* text) {
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
            ImGui::TextWrapped("%s", text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    /**
     * @brief Extracts the back buffer from the swap chain and creates a render target view.
     * @return void
     */
    void CreateRenderTarget() {
        ID3D11Texture2D* pBackBuffer;
        pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pMainRenderTargetView);
        pBackBuffer->Release();
    }

    /**
     * @brief Safely releases the Render Target View resources before window resizes or shutdown.
     * @return void
     */
    void CleanupRenderTarget() {
        if (pMainRenderTargetView) { pMainRenderTargetView->Release(); pMainRenderTargetView = nullptr; }
    }

    /**
     * @brief Internal Window Procedure for the UI Window.
     * Integrates global safety hotkey handling (Ctrl+Alt+M).
     * @param hWnd Handle to the window.
     * @param msg The message to process (e.g., WM_SIZE, WM_DESTROY).
     * @param wParam Additional message info.
     * @param lParam Additional message info.
     * @return Result of the message processing.
     */
    LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
        switch (msg) {
            case WM_DROPFILES: {
                if (!State::enableFileTransfer) {
                    ::DragFinish((HDROP)wParam);
                    return 0; // Ignore file drops entirely if disabled
                }
                HDROP hDrop = (HDROP)wParam;
                UINT fileCount = ::DragQueryFileA(hDrop, 0xFFFFFFFF, nullptr, 0);
                if (fileCount > 0) {
                    char filePath[MAX_PATH] = { 0 };
                    ::DragQueryFileA(hDrop, 0, filePath, MAX_PATH); // Take the first file
                    g_pendingDropFile = filePath;
                    g_showDropModal = true;
                    g_triggerDropModal = true;
                    g_selectedTransferClients.clear(); // Reset selections
                    if (State::globalDebugMode) UI::LogDebug("[UI] File dropped: %s", filePath);
                }
                ::DragFinish(hDrop);
                return 0;
            }
            case WM_SYSCOMMAND:
                if ((wParam & 0xFFF0) == SC_MINIMIZE && State::minimizeToTray) {
                    ::ShowWindow(hWnd, SW_HIDE);
                    ::Shell_NotifyIconA(NIM_ADD, &g_nid);
                    g_isMinimizedToTray = true;
                    return 0; // Handled, prevent default taskbar minimization
                }
                break;
            case WM_TRAYICON:
                if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
                    ::Shell_NotifyIconA(NIM_DELETE, &g_nid);
                    ::ShowWindow(hWnd, SW_SHOW);
                    ::ShowWindow(hWnd, SW_RESTORE);
                    ::SetForegroundWindow(hWnd);
                    g_isMinimizedToTray = false;
                } else if (lParam == WM_RBUTTONUP) {
                    POINT pt;
                    ::GetCursorPos(&pt);
                    HMENU hMenu = ::CreatePopupMenu();
                    ::InsertMenuA(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_TRAY_SHOW, "Show KAM-Flow");
                    ::InsertMenuA(hMenu, 1, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, "Exit");
                    
                    // Required to fix a Windows behavior where popup menus from tray icons don't dismiss when clicking away
                    ::SetForegroundWindow(hWnd);
                    
                    int cmd = ::TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr);
                    ::DestroyMenu(hMenu);
                    
                    if (cmd == ID_TRAY_SHOW) {
                        ::Shell_NotifyIconA(NIM_DELETE, &g_nid);
                        ::ShowWindow(hWnd, SW_SHOW);
                        ::ShowWindow(hWnd, SW_RESTORE);
                        ::SetForegroundWindow(hWnd);
                        g_isMinimizedToTray = false;
                    } else if (cmd == ID_TRAY_EXIT) {
                        ::PostQuitMessage(0);
                        isAppRunning = false;
                    }
                }
                return 0;
            case WM_HOTKEY:
                // Universal Safety Override (Ctrl+Alt+M) caught at the OS level
                if (wParam == 1) {
                    if (State::currentRole == State::AppRole::SERVER) {
                        if (State::globalDebugMode) UI::LogDebug("[KAM-Flow UI] Emergency Override. Reverting to LOCAL.");
                        State::SetMode(State::ControlMode::LOCAL);
                    } else if (State::currentRole == State::AppRole::CLIENT) {
                        if (State::globalDebugMode) UI::LogDebug("[KAM-Flow UI] Emergency Override. Disconnecting Client.");
                        Network::StopClient();
                    }
                } else if (wParam == 2) {
                    if (State::currentRole == State::AppRole::SERVER) {
                        State::enableGameMode = !State::enableGameMode;
                        if (State::enableGameMode) {
                            if (State::globalDebugMode) UI::LogDebug("[KAM-Flow UI] Game Mode ENABLED. Forcing LOCAL.");
                            State::SetMode(State::ControlMode::LOCAL); // Instantly pull control back if remote
                            if (g_HookThreadId != 0) ::PostThreadMessage(g_HookThreadId.load(), WM_TOGGLE_GAMEMODE, 1, 0);
                        } else {
                            if (State::globalDebugMode) UI::LogDebug("[KAM-Flow UI] Game Mode DISABLED.");
                            if (g_HookThreadId != 0) ::PostThreadMessage(g_HookThreadId.load(), WM_TOGGLE_GAMEMODE, 0, 0);
                        }
                    }
                }
                return 0;
    case WM_MOVE:
        if (!IsIconic(hWnd) && !IsZoomed(hWnd)) {
            RECT rect;
            if (GetWindowRect(hWnd, &rect)) {
                g_windowX = rect.left;
                g_windowY = rect.top;
            }
        }
        break; // Let it fall through to DefWindowProc
            case 0x02E0: // WM_DPICHANGED
                if (lParam) {
                    RECT* const prcNewWindow = (RECT*)lParam;
                    ::SetWindowPos(hWnd, nullptr,
                        prcNewWindow->left, prcNewWindow->top,
                        prcNewWindow->right - prcNewWindow->left,
                        prcNewWindow->bottom - prcNewWindow->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                return 0;
            case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && wParam != SIZE_MAXIMIZED) {
            RECT rect;
            if (GetWindowRect(hWnd, &rect)) {
                g_windowW = rect.right - rect.left;
                g_windowH = rect.bottom - rect.top;
            }
        }
                if (wParam == SIZE_MINIMIZED) return 0;
                if (pd3dDevice != nullptr) {
                    CleanupRenderTarget();
                    pSwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
                    CreateRenderTarget();
                }
                return 0;
            case WM_POWERBROADCAST:
                if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                    if (State::globalDebugMode) UI::LogDebug("[System] Resumed from sleep/hibernate.");

                    // Drain all stale mouse events that accumulated while the system was
                    // suspended. Without this, hundreds/thousands of deltas would burst-fire
                    // on wake, teleporting the cursor to an arbitrary position.
                    if (State::currentRole == State::AppRole::SERVER) {
                        Input::DrainMouseQueue();
                        // Force LOCAL to prevent input lockout — the client's TCP socket
                        // is likely dead after sleep, so remote control would be a black hole.
                        if (State::IsRemote()) {
                            if (State::globalDebugMode) UI::LogDebug("[System] Forcing LOCAL after sleep/wake.");
                            State::SetMode(State::ControlMode::LOCAL);
                        }
                    } else if (State::currentRole == State::AppRole::CLIENT) {
                        // Reset reconnect backoff so the client reconnects within 1s
                        // instead of waiting 5-30s from the exponential backoff.
                        // Sleep-triggered disconnects are expected and should not be
                        // penalized with escalating delays.
                        Network::NotifySystemResumed();
                    }
                }
                return TRUE;
            case WM_DESTROY:
                ::PostQuitMessage(0);
                isAppRunning = false;
                return 0;
        }
        return ::DefWindowProc(hWnd, msg, wParam, lParam);
    }

    /**
     * @brief Configures a cohesive modern dark palette and ergonomic geometry for Dear ImGui.
     * @param style Reference to the ImGuiStyle structure to configure.
     * @return void
     */
    void ApplyDarkTheme(ImGuiStyle& style) {
        ImVec4* colors = style.Colors;

        // Window and backgrounds
        colors[ImGuiCol_WindowBg]           = ImVec4(0.08f, 0.09f, 0.12f, 1.00f); // Deep sleek obsidian
        colors[ImGuiCol_ChildBg]            = ImVec4(0.10f, 0.11f, 0.15f, 0.90f); // Subtle card background
        colors[ImGuiCol_PopupBg]            = ImVec4(0.09f, 0.10f, 0.14f, 0.98f); // Solid popup surface
        colors[ImGuiCol_Border]             = ImVec4(0.20f, 0.23f, 0.30f, 0.50f); // Subtle slate border
        colors[ImGuiCol_BorderShadow]       = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

        // Text
        colors[ImGuiCol_Text]               = ImVec4(0.92f, 0.93f, 0.95f, 1.00f); // Crisp white
        colors[ImGuiCol_TextDisabled]       = ImVec4(0.50f, 0.53f, 0.60f, 1.00f); // Muted slate

        // Headers & Tabs
        colors[ImGuiCol_Header]             = ImVec4(0.16f, 0.22f, 0.32f, 0.80f);
        colors[ImGuiCol_HeaderHovered]      = ImVec4(0.22f, 0.32f, 0.48f, 0.90f);
        colors[ImGuiCol_HeaderActive]       = ImVec4(0.18f, 0.28f, 0.44f, 1.00f);

        colors[ImGuiCol_Tab]                = ImVec4(0.12f, 0.14f, 0.19f, 1.00f);
        colors[ImGuiCol_TabHovered]         = ImVec4(0.22f, 0.32f, 0.48f, 1.00f);
        colors[ImGuiCol_TabActive]          = ImVec4(0.17f, 0.25f, 0.38f, 1.00f);
        colors[ImGuiCol_TabUnfocused]       = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.18f, 0.26f, 1.00f);

        // Frame (input boxes, checkboxes, sliders background)
        colors[ImGuiCol_FrameBg]            = ImVec4(0.13f, 0.15f, 0.20f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
        colors[ImGuiCol_FrameBgActive]      = ImVec4(0.22f, 0.28f, 0.38f, 1.00f);

        // Title & Menu
        colors[ImGuiCol_TitleBg]            = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
        colors[ImGuiCol_TitleBgActive]      = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.08f, 0.09f, 0.12f, 0.60f);
        colors[ImGuiCol_MenuBarBg]          = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);

        // Buttons (modern royal blue / slate accent)
        colors[ImGuiCol_Button]             = ImVec4(0.16f, 0.24f, 0.38f, 1.00f);
        colors[ImGuiCol_ButtonHovered]      = ImVec4(0.22f, 0.34f, 0.54f, 1.00f);
        colors[ImGuiCol_ButtonActive]       = ImVec4(0.14f, 0.28f, 0.48f, 1.00f);

        // Sliders & Scrollbars
        colors[ImGuiCol_SliderGrab]         = ImVec4(0.26f, 0.52f, 0.88f, 1.00f); // Electric royal blue
        colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.35f, 0.62f, 0.98f, 1.00f);
        colors[ImGuiCol_CheckMark]          = ImVec4(0.35f, 0.72f, 1.00f, 1.00f); // Bright cyan checkmark
        colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.08f, 0.09f, 0.12f, 0.50f);
        colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.20f, 0.24f, 0.32f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.28f, 0.34f, 0.45f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]= ImVec4(0.35f, 0.44f, 0.58f, 1.00f);

        // Separators & Resize Grips
        colors[ImGuiCol_Separator]          = ImVec4(0.20f, 0.23f, 0.30f, 0.60f);
        colors[ImGuiCol_SeparatorHovered]   = ImVec4(0.26f, 0.52f, 0.88f, 0.80f);
        colors[ImGuiCol_SeparatorActive]    = ImVec4(0.35f, 0.62f, 0.98f, 1.00f);
        colors[ImGuiCol_ResizeGrip]         = ImVec4(0.20f, 0.24f, 0.32f, 0.30f);
        colors[ImGuiCol_ResizeGripHovered]  = ImVec4(0.26f, 0.52f, 0.88f, 0.67f);
        colors[ImGuiCol_ResizeGripActive]   = ImVec4(0.35f, 0.62f, 0.98f, 0.95f);

        // Modal dimming
        colors[ImGuiCol_ModalWindowDimBg]   = ImVec4(0.00f, 0.00f, 0.00f, 0.65f);

        // Modern Rounding & Sizing
        style.WindowRounding    = 6.0f;
        style.ChildRounding     = 6.0f;
        style.FrameRounding     = 4.0f;
        style.PopupRounding     = 6.0f;
        style.ScrollbarRounding = 4.0f;
        style.GrabRounding      = 4.0f;
        style.TabRounding       = 4.0f;

        style.WindowBorderSize  = 1.0f;
        style.ChildBorderSize   = 1.0f;
        style.PopupBorderSize   = 1.0f;
        style.FrameBorderSize   = 1.0f;

        style.WindowPadding     = ImVec2(10.0f, 10.0f);
        style.FramePadding      = ImVec2(8.0f, 4.0f);
        style.ItemSpacing       = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    }

    /**
     * @brief Initializes the Win32 window, DirectX 11 device, and Dear ImGui context.
     * @return true if initialization succeeded, false if any subsystem failed.
     */
    bool Initialize() {
        g_logInterceptor = new ImGuiLogStreamBuf();

        // Ensure KAM-Flow survives extreme system load by elevating the entire process priority.
        // This guarantees KVM inputs and audio streams don't stutter when minimized during heavy gaming/workloads.
        ::SetPriorityClass(::GetCurrentProcess(), HIGH_PRIORITY_CLASS);

        // Prevent Windows 11 from putting the minimized/tray process into Efficiency Mode (EcoQoS)
        PROCESS_POWER_THROTTLING_STATE PowerThrottling;
        ZeroMemory(&PowerThrottling, sizeof(PowerThrottling));
        PowerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        PowerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        PowerThrottling.StateMask = 0; // 0 explicitly disables throttling
        ::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &PowerThrottling, sizeof(PowerThrottling));

        WNDCLASSEX wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, "KAMFlowUIClass", nullptr };
        wc.hIcon = ::LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON));
        wc.hIconSm = ::LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON));
        ::RegisterClassEx(&wc);

        const std::string& iniPath = Config::GetResolvedPath();
        g_windowX = GetPrivateProfileIntA("UI", "WindowX", 100, iniPath.c_str());
        g_windowY = GetPrivateProfileIntA("UI", "WindowY", 100, iniPath.c_str());
        g_windowW = GetPrivateProfileIntA("UI", "WindowW", 450, iniPath.c_str());
        g_windowH = GetPrivateProfileIntA("UI", "WindowH", 480, iniPath.c_str());

        int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vRight = vLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vBottom = vTop + GetSystemMetrics(SM_CYVIRTUALSCREEN);

        if (g_windowX < vLeft || g_windowX > vRight - 100) g_windowX = 100;
        if (g_windowY < vTop || g_windowY > vBottom - 100) g_windowY = 100;
        if (g_windowW < 300) g_windowW = 450; if (g_windowH < 300) g_windowH = 480;

        hwnd = ::CreateWindow(wc.lpszClassName, "KAM-Flow", WS_OVERLAPPEDWINDOW, g_windowX, g_windowY, g_windowW, g_windowH, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;

        // Register the Global Hotkey for Emergency Override
        ::RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, State::emergencyHotkey.load(std::memory_order_relaxed));
        ::RegisterHotKey(hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, State::gameModeHotkey.load(std::memory_order_relaxed));

        InitTrayIcon(hwnd);

        // Enable Win32 Drag and Drop for the main KAM-Flow window
        ::DragAcceptFiles(hwnd, TRUE);
        
        // Bypass UIPI (User Interface Privilege Isolation) to allow drops from non-elevated Windows Explorer
        ::ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
        ::ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ::ChangeWindowMessageFilterEx(hwnd, 0x0049, MSGFLT_ALLOW, nullptr); // WM_COPYGLOBALDATA

        DXGI_SWAP_CHAIN_DESC sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createDeviceFlags = 0;
        D3D_FEATURE_LEVEL featureLevel;
        const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
        
        HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &pSwapChain, &pd3dDevice, &featureLevel, &pd3dDeviceContext);
        if (res == DXGI_ERROR_UNSUPPORTED || res == E_FAIL) {
            res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &pSwapChain, &pd3dDevice, &featureLevel, &pd3dDeviceContext);
        }
        if (res != S_OK) return false;

        CreateRenderTarget();
        
        // Force the window to start non-minimized and capture focus
        ::ShowWindow(hwnd, SW_SHOWNORMAL);
        ::SetForegroundWindow(hwnd);
        ::SetFocus(hwnd);
        ::UpdateWindow(hwnd);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGuiStyle& style = ImGui::GetStyle();
        ApplyDarkTheme(style);

        // Dynamic High-DPI detection
        float dpiScale = 1.0f;
        HMODULE hUser32 = GetModuleHandleA("user32.dll");
        if (hUser32) {
            typedef UINT(WINAPI* PFN_GetDpiForWindow)(HWND);
            PFN_GetDpiForWindow pfnGetDpiForWindow = (PFN_GetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
            if (pfnGetDpiForWindow && hwnd) {
                UINT dpi = pfnGetDpiForWindow(hwnd);
                if (dpi > 0) dpiScale = static_cast<float>(dpi) / 96.0f;
            }
        }
        if (dpiScale > 1.0f) {
            style.ScaleAllSizes(dpiScale);
            io.FontGlobalScale = dpiScale;
        }

        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX11_Init(pd3dDevice, pd3dDeviceContext);

        isAppRunning = true;
        return true;
    }

    /**
     * @brief Renders the Pre-Flight Launcher screen where the user selects their system role.
     * @return void
     */
    void RenderPreFlightScreen() {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Pre-Flight Launcher", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

        ImGui::Text("Welcome to KAM-Flow");
        ImGui::Separator();
        ImGui::Spacing(); ImGui::Spacing();

        ImGui::TextWrapped("Select the operational role for this machine:");
        ImGui::Spacing();

        if (ImGui::Button("START AS SERVER (Main PC)", ImVec2(-1, 50))) {
            State::currentRole = State::AppRole::SERVER;
        }
        DrawWrappedTooltip("Start as the Sever PC. This machine's keyboard and mouse will control all others.");
        ImGui::Spacing();
        if (ImGui::Button("START AS CLIENT", ImVec2(-1, 50))) {
            State::currentRole = State::AppRole::CLIENT;
        }
        DrawWrappedTooltip("Start as a Client PC. This machine will be controlled by the Server.");

        ImGui::End();
    }

    /**
     * @brief Renders the active operational dashboard with all its tabs and controls.
     * @return void
     */
    void RenderDashboardScreen() {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("KAM-Flow Control Center", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

        // --- UNIFIED STATUS HEADER & BADGES ---
        ImGui::AlignTextToFramePadding();
        ImGui::Text("KAM-Flow %s", (State::currentRole == State::AppRole::SERVER ? "SERVER" : "CLIENT"));
        ImGui::SameLine();

        if (State::currentRole == State::AppRole::SERVER) {
            bool isRemote = State::IsRemote();
            if (isRemote) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.0f, 1.0f));
                ImGui::SmallButton("REMOTE");
                ImGui::PopStyleColor();
                DrawWrappedTooltip("Remote mode active: Physical keyboard & mouse input is currently directed to the Client PC.");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.55f, 0.22f, 1.0f));
                ImGui::SmallButton("LOCAL");
                ImGui::PopStyleColor();
                DrawWrappedTooltip("Local mode active: You have physical control of the Server PC.");
            }
            ImGui::SameLine();

            auto connectedClients = Network::GetConnectedClients();
            char clientBadge[32];
            snprintf(clientBadge, sizeof(clientBadge), "Clients: %zu", connectedClients.size());
            if (!connectedClients.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.75f, 1.0f));
                ImGui::SmallButton(clientBadge);
                ImGui::PopStyleColor();
                DrawWrappedTooltip("Active client sessions currently authenticated and connected.");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
                ImGui::SmallButton("No Clients");
                ImGui::PopStyleColor();
                DrawWrappedTooltip("Waiting for Client machines to connect and pair.");
            }
        } else {
            // Client Status Badges
            std::string currentStatus = State::GetClientStatus();
            bool isConnected = Network::IsClientConnected();
            if (isConnected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.55f, 0.22f, 1.0f));
                ImGui::SmallButton("CONNECTED");
                ImGui::PopStyleColor();
                DrawWrappedTooltip("Client is securely connected to the Server.");
            } else if (currentStatus.find("Connecting") != std::string::npos || 
                       currentStatus.find("Authenticating") != std::string::npos || 
                       currentStatus.find("reconnecting") != std::string::npos) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.65f, 0.1f, 1.0f));
                ImGui::SmallButton(currentStatus.c_str());
                ImGui::PopStyleColor();
                DrawWrappedTooltip("Client is negotiating a connection.");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.2f, 0.2f, 1.0f));
                ImGui::SmallButton(currentStatus.c_str());
                ImGui::PopStyleColor();
                DrawWrappedTooltip("Client is currently disconnected.");
            }
        }

        // Persistent File Transfer Header Pill
        auto activeTransfers = FileTransfer::GetActiveTransfers();
        if (!activeTransfers.empty()) {
            ImGui::SameLine();
            const auto& t0 = activeTransfers[0];
            float pct = t0.fileSize > 0 ? ((float)t0.transferredSize / (float)t0.fileSize) * 100.0f : 0.0f;
            char pillText[128];
            snprintf(pillText, sizeof(pillText), "Transfer: %.0f%% (%zu)", pct, activeTransfers.size());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.85f, 1.0f));
            ImGui::SmallButton(pillText);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                char tip[256];
                snprintf(tip, sizeof(tip), "Active File: %s\nTransferred: %.2f / %.2f MB (%.1f%%)",
                         t0.fileName.c_str(),
                         (float)t0.transferredSize / (1024.0f * 1024.0f),
                         (float)t0.fileSize / (1024.0f * 1024.0f), pct);
                DrawWrappedTooltip(tip);
            }
        }

        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {
            bool tabConnections = ImGui::BeginTabItem("Connections");
            if (State::currentRole == State::AppRole::SERVER) {
                DrawWrappedTooltip("Monitor network status, view connected clients, and forcefully terminate active connections.");
            } else {
                DrawWrappedTooltip("View paired server status, establish secure KVM connections, or safely disconnect.");
            }
            if (tabConnections) {
                if (State::currentRole == State::AppRole::SERVER) {
                    // === MASTER PIN SECTION (moved from Security & Pairing tab) ===
                    ImGui::Text("Master PIN");
                    ImGui::Separator();
                    ImGui::Spacing();

                    std::string pin = Network::GetMasterPin();
                    if (showServerPin) {
                        ImGui::Text("Master PIN: %s", pin.c_str());
                    } else {
                        ImGui::Text("Master PIN: ********");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(showServerPin ? "Hide" : "Reveal")) { showServerPin = !showServerPin; }
                    DrawWrappedTooltip("Toggle visibility of the Master PIN.");

                    ImGui::Spacing();
                    ImGui::TextWrapped("Clients must use this PIN to connect. Regenerating it will instantly disconnect all active clients and require them to re-pair.");
                    ImGui::Spacing();

                    if (ImGui::Button("Regenerate Master PIN", ImVec2(-1, 30))) {
                        Network::RegenerateMasterPin();
                    }
                    DrawWrappedTooltip("Generate a new random PIN and instantly disconnect all currently active clients.");

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    // === CONTROL STATUS & CONNECTED CLIENTS ===
                    bool isRemote = State::IsRemote();
                    ImGui::Text("Control Status: "); ImGui::SameLine();
                    if (isRemote) { ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "REMOTE (Client controls input)"); }
                    else { ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "LOCAL (You have control)"); }

                    ImGui::Text("Network: "); ImGui::SameLine();
                    if (Network::HasAuthenticatedClients()) { ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Client Connected & Authenticated"); }
                    else { ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Awaiting Client Connection..."); }

                    if (isRemote) {
                        ImGui::Spacing();
                        char btnText[64];
                        snprintf(btnText, sizeof(btnText), "FORCE LOCAL CONTROL (Ctrl+Alt+%c)", State::emergencyHotkey.load(std::memory_order_relaxed));
                        if (ImGui::Button(btnText, ImVec2(-1, 40))) { State::SetMode(State::ControlMode::LOCAL); }
                        DrawWrappedTooltip("Instantly regain control of the Server PC, same as the hotkey.");
                    }

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                    ImGui::Text("Connected Clients");
                    auto connectedClients = Network::GetConnectedClients();
                    if (connectedClients.empty()) {
                        ImGui::TextDisabled("No active sessions.");
                    } else {
                        for (const auto& client : connectedClients) {
                            ImGui::Text("%s (%s)", client.name.c_str(), client.ip.c_str());
                            
                            ImGui::SameLine(ImGui::GetWindowWidth() - 100);
                            ImGui::PushID(client.socket);
                            if (ImGui::Button("Disconnect", ImVec2(80, 20))) {
                                Network::DisconnectClient(client.socket);
                            }
                            DrawWrappedTooltip("Forcefully terminate the encrypted TCP connection with this client.");
                            ImGui::PopID();
                        }
                    }
                } else {
                    // === CLIENT MODE CONNECTIONS TAB ===
                    auto targets = CredentialManager::GetSavedTargets("KAMFlow_Server_");
                    bool isPaired = !targets.empty();

                    std::string currentStatus = State::GetClientStatus();
                    ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                    if (currentStatus == "Connected") {
                        statusColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                    } else if (currentStatus.find("Connecting") != std::string::npos || 
                               currentStatus.find("Authenticating") != std::string::npos || 
                               currentStatus.find("reconnecting") != std::string::npos) {
                        statusColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
                    } else if (currentStatus.find("Failed") != std::string::npos || 
                               currentStatus.find("Disconnected") != std::string::npos ||
                               currentStatus.find("dropped") != std::string::npos) {
                        statusColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                    }

                    ImGui::Text("Connection Status: ");
                    ImGui::SameLine();
                    ImGui::TextColored(statusColor, "%s", currentStatus.c_str());
                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    if (!Network::IsClientConnected()) {
                        // --- CURRENTLY PAIRED SERVER SECTION ---
                        ImGui::Text("Currently Paired Server:");

                        // Resolve the paired server's hostname for display
                        std::string pairedIP = "";
                        std::string pairedHostname = "Unknown";
                        uint16_t pairedPort = 8080;
                        bool pairedOnline = false;
                        auto discoveredServers = Network::GetDiscoveredServers();

                        if (isPaired) {
                            pairedIP = targets[0].substr(15);
                            for (const auto& srv : discoveredServers) {
                                if (srv.ip == pairedIP) {
                                    pairedHostname = srv.hostname;
                                    pairedPort = srv.tcpPort;
                                    pairedOnline = true;
                                    break;
                                }
                            }
                            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", pairedHostname.c_str());
                            ImGui::SameLine(); ImGui::Text("(%s)", pairedIP.c_str());
                            if (pairedOnline) {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), " [ONLINE]");
                            } else {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), " [OFFLINE]");
                            }
                        } else {
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "No Server Paired.");
                        }
                        ImGui::Spacing();

                        // Connect button — behavior depends on pairing state
                        if (isPaired) {
                            std::string connectText = "Connect to " + pairedHostname + " (" + pairedIP + ")";
                            if (ImGui::Button(connectText.c_str(), ImVec2(-120, 35))) {
                                Network::StartClient(pairedIP, pairedPort);
                            }
                            DrawWrappedTooltip("Establish a secure KVM and Audio connection to the paired Server.");
                        } else {
                            if (ImGui::Button("Connect to Server", ImVec2(-120, 35))) {
                                g_showNotPairedPopup = true;
                            }
                            DrawWrappedTooltip("Pair a server first using the 'Pair' button below.");
                        }
                        ImGui::SameLine();
                        // Edit IP button — only enabled when paired
                        ImGui::BeginDisabled(!isPaired);
                        if (ImGui::Button("Edit IP", ImVec2(100, 35))) {
                            g_editingPairedIP = !g_editingPairedIP;
                            if (g_editingPairedIP && isPaired) {
                                strncpy_s(g_editIPBuffer, sizeof(g_editIPBuffer), pairedIP.c_str(), _TRUNCATE);
                                g_editIPInvalid = false;
                            }
                        }
                        DrawWrappedTooltip("Edit the IP address of the paired server (useful when the server's IP changes due to DHCP).");
                        ImGui::EndDisabled();

                        // Inline IP editor (shown when Edit IP is clicked)
                        if (g_editingPairedIP && isPaired) {
                            ImGui::Spacing();
                            ImGui::PushItemWidth(-80);
                            ImGui::InputText("##EditIP", g_editIPBuffer, sizeof(g_editIPBuffer));
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            if (ImGui::Button("Save", ImVec2(60, 0))) {
                                if (IsValidIPv4(g_editIPBuffer)) {
                                    std::string newIP = g_editIPBuffer;
                                    if (newIP != pairedIP) {
                                        // Re-key the credential: load PIN from old, save under new, delete old
                                        std::string savedPin;
                                        if (CredentialManager::LoadSecret("KAMFlow_Server_" + pairedIP, savedPin)) {
                                            CredentialManager::SaveSecret("KAMFlow_Server_" + newIP, savedPin);
                                            CredentialManager::DeleteSecret("KAMFlow_Server_" + pairedIP);
                                        }
                                    }
                                    g_editingPairedIP = false;
                                    g_editIPInvalid = false;
                                } else {
                                    g_editIPInvalid = true;
                                }
                            }
                            DrawWrappedTooltip("Save the new IP address. The Master PIN will be preserved.");
                            if (g_editIPInvalid) {
                                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid IPv4 address.");
                            }
                        }

                        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                        // --- DISCOVERED SERVERS LIST ---
                        ImGui::Text("Discovered Servers");
                        ImGui::Separator();

                        if (discoveredServers.empty()) {
                            ImGui::TextDisabled("Scanning... No servers found.");
                        } else {
                            for (const auto& srv : discoveredServers) {
                                ImGui::PushID(srv.ip.c_str());

                                // Check if THIS server is the one we're paired to
                                bool isPairedToThis = (isPaired && pairedIP == srv.ip);

                                if (isPairedToThis) {
                                    // Red "Forget" button
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.05f, 0.05f, 1.0f));
                                    if (ImGui::Button("Forget", ImVec2(60, 20))) {
                                        for (const auto& t : targets) {
                                            CredentialManager::DeleteSecret(t);
                                        }
                                        g_editingPairedIP = false;
                                    }
                                    DrawWrappedTooltip("Remove the saved pairing for this server.");
                                    ImGui::PopStyleColor(3);
                                } else {
                                    // Green "Pair" button
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.1f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.05f, 0.4f, 0.05f, 1.0f));
                                    if (ImGui::Button("Pair", ImVec2(60, 20))) {
                                        // Store the target server info for the popup flow
                                        strncpy_s(g_pairingTargetIP, sizeof(g_pairingTargetIP), srv.ip.c_str(), _TRUNCATE);
                                        strncpy_s(g_pairingTargetHostname, sizeof(g_pairingTargetHostname), srv.hostname.c_str(), _TRUNCATE);
                                        memset(g_pairingPinInput, 0, sizeof(g_pairingPinInput));
                                        g_showPairingPin = false;

                                        if (isPaired) {
                                            // Already paired to a DIFFERENT server — show warning
                                            g_showAlreadyPairedWarning = true;
                                        } else {
                                            // Not paired to anything — go straight to PIN entry
                                            g_showPinPopup = true;
                                        }
                                    }
                                    DrawWrappedTooltip("Pair to this server by entering its Master PIN.");
                                    ImGui::PopStyleColor(3);
                                }

                                ImGui::SameLine();
                                ImGui::Text("%s (%s)", srv.hostname.c_str(), srv.ip.c_str());

                                ImGui::PopID();
                            }
                        }

                        // === POPUP MODALS ===

                        // Open popups outside the loop (ImGui requires OpenPopup at the same level)
                        if (g_showNotPairedPopup) {
                            ImGui::OpenPopup("NotPairedPopup");
                            g_showNotPairedPopup = false;
                        }
                        if (g_showAlreadyPairedWarning) {
                            ImGui::OpenPopup("AlreadyPairedPopup");
                            g_showAlreadyPairedWarning = false;
                        }
                        if (g_showPinPopup) {
                            ImGui::OpenPopup("PairServerPopup");
                            g_showPinPopup = false;
                        }

                        // --- NOT PAIRED POPUP ---
                        if (ImGui::BeginPopupModal("NotPairedPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::Text("No Server Paired");
                            ImGui::Separator();
                            ImGui::Spacing();
                            ImGui::TextWrapped("No server is currently paired.\nClick the 'Pair' button next to a discovered server to get started.");
                            ImGui::Spacing();
                            if (ImGui::Button("OK", ImVec2(-1, 30))) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }

                        // --- ALREADY PAIRED WARNING POPUP ---
                        if (ImGui::BeginPopupModal("AlreadyPairedPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::Text("Already Paired");
                            ImGui::Separator();
                            ImGui::Spacing();
                            ImGui::Text("You are currently paired to:");
                            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s (%s)", pairedHostname.c_str(), pairedIP.c_str());
                            ImGui::Spacing();
                            ImGui::TextWrapped("Only one server can be paired at a time. Continuing will unpair the current server.");
                            ImGui::Spacing();
                            if (ImGui::Button("Cancel", ImVec2(120, 30))) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Continue", ImVec2(120, 30))) {
                                // Unpair old server and proceed to PIN entry
                                for (const auto& t : targets) {
                                    CredentialManager::DeleteSecret(t);
                                }
                                g_editingPairedIP = false;
                                ImGui::CloseCurrentPopup();
                                g_showPinPopup = true; // Will trigger OpenPopup on next frame
                            }
                            ImGui::EndPopup();
                        }

                        // --- PIN ENTRY POPUP ---
                        if (ImGui::BeginPopupModal("PairServerPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::Text("Pair to Server");
                            ImGui::Separator();
                            ImGui::Spacing();
                            ImGui::Text("Enter the 8-digit Master PIN for:");
                            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s (%s)", g_pairingTargetHostname, g_pairingTargetIP);
                            ImGui::Spacing();

                            // Automatically focus keyboard into the PIN input on first appearance
                            if (ImGui::IsWindowAppearing()) {
                                ImGui::SetKeyboardFocusHere();
                            }

                            ImGuiInputTextFlags pinFlags = (g_showPairingPin ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password) | ImGuiInputTextFlags_EnterReturnsTrue;
                            bool enterPressed = ImGui::InputText("Master PIN", g_pairingPinInput, sizeof(g_pairingPinInput), pinFlags);
                            ImGui::SameLine();
                            if (ImGui::Button(g_showPairingPin ? "Hide" : "Show")) { g_showPairingPin = !g_showPairingPin; }
                            DrawWrappedTooltip("Toggle visibility of the PIN you are typing.");

                            // Validate: exactly 8 digits
                            size_t pinLen = strlen(g_pairingPinInput);
                            bool pinValid = (pinLen == 8);
                            if (pinValid) {
                                for (size_t i = 0; i < pinLen; ++i) {
                                    if (g_pairingPinInput[i] < '0' || g_pairingPinInput[i] > '9') {
                                        pinValid = false;
                                        break;
                                    }
                                }
                            }

                            if (pinLen > 0 && !pinValid) {
                                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "PIN must be exactly 8 digits (0-9).");
                            } else {
                                ImGui::TextDisabled("8 digits required.");
                            }
                            ImGui::Spacing();

                            if (ImGui::Button("Cancel", ImVec2(120, 30))) {
                                memset(g_pairingPinInput, 0, sizeof(g_pairingPinInput));
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SameLine();
                            ImGui::BeginDisabled(!pinValid);
                            bool pairClicked = ImGui::Button("Pair", ImVec2(120, 30));
                            ImGui::EndDisabled();

                            // Allow submission via clicking 'Pair' button or pressing Enter
                            if (pinValid && (pairClicked || enterPressed)) {
                                CredentialManager::SaveSecret(
                                    "KAMFlow_Server_" + std::string(g_pairingTargetIP),
                                    std::string(g_pairingPinInput));
                                memset(g_pairingPinInput, 0, sizeof(g_pairingPinInput));
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }

                    } else {
                        // === CONNECTED STATE ===
                        ImGui::TextWrapped("Receiving Input and KVM Commands from Server.");
                        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                        
                        char btnText[64];
                        snprintf(btnText, sizeof(btnText), "Disconnect (Ctrl+Alt+%c)", State::emergencyHotkey.load(std::memory_order_relaxed));
                        if (ImGui::Button(btnText, ImVec2(-1, 40))) {
                            Network::StopClient();
                        }
                        DrawWrappedTooltip("Safely terminate the encrypted TCP connection and return to standby.");
                    }
                }
                ImGui::EndTabItem();
            }

            bool tabSpatial = ImGui::BeginTabItem("Spatial Layout");
            if (State::currentRole == State::AppRole::SERVER) {
                DrawWrappedTooltip("Visually arrange multiple clients in a 2D grid and configure physical screen edge routing.");
            } else {
                DrawWrappedTooltip("Configure local safety deadzones for physical screen edges to prevent accidental cursor transitions.");
            }
            if (tabSpatial) {
                if (State::currentRole == State::AppRole::SERVER) {
                    ImGui::Text("Client Placement Matrix");
                    ImGui::TextWrapped("Drag and drop client screens to arrange their spatial relationship to the Server.");
                    ImGui::Separator();
                    ImGui::Spacing();
                    
                    auto clients = Network::GetConnectedClients();
                    int minX = 0, maxX = 0, minY = 0, maxY = 0;
                    for (const auto& c : clients) {
                        minX = (std::min)(minX, c.gridX); maxX = (std::max)(maxX, c.gridX);
                        minY = (std::min)(minY, c.gridY); maxY = (std::max)(maxY, c.gridY);
                    }
                    minX--; maxX++; minY--; maxY++; // Add padding for drop targets

                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.1f, 1.0f));
                    // A negative Y value dynamically stretches the child window, leaving 75px at the bottom for the edge controls
                    ImGui::BeginChild("GridArea", ImVec2(0, -75.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
                    
                    const float cellSize = 80.0f;
                    const float spacing = 5.0f;

                    for (int y = minY; y <= maxY; ++y) {
                        for (int x = minX; x <= maxX; ++x) {
                            if (x > minX) ImGui::SameLine(0, spacing);
                            
                            ImGui::PushID(x * 100 + y);
                            
                            bool isServer = (x == 0 && y == 0);
                            Network::ConnectedClientInfo* matchedClient = nullptr;
                            bool isAdjacent = (abs(x) + abs(y) == 1);

                            for (auto& c : clients) {
                                if (c.gridX == x && c.gridY == y) { matchedClient = &c; }
                                if (abs(c.gridX - x) + abs(c.gridY - y) == 1) { isAdjacent = true; }
                            }

                            if (isServer) {
                                // Server is active when in local mode (not routing to a remote client)
                                bool isServerActive = !State::IsRemote();
                                if (isServerActive) {
                                    // Vibrant emerald green button styling for active local control
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.62f, 0.28f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.72f, 0.35f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.55f, 0.22f, 1.0f));
                                } else {
                                    // Muted olive green styling when control is delegated to a remote client
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.30f, 0.14f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.38f, 0.18f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.26f, 0.12f, 1.0f));
                                }
                                static char serverName[MAX_COMPUTERNAME_LENGTH + 1] = "";
                                if (serverName[0] == '\0') {
                                    DWORD size = sizeof(serverName);
                                    GetComputerNameA(serverName, &size);
                                }
                                char serverLabel[128];
                                if (isServerActive) {
                                    snprintf(serverLabel, sizeof(serverLabel), "%s\nServer\n[ACTIVE]", serverName);
                                } else {
                                    snprintf(serverLabel, sizeof(serverLabel), "%s\nServer", serverName);
                                }
                                ImGui::Button(serverLabel, ImVec2(cellSize, cellSize));
                                ImGui::PopStyleColor(3);

                                // Render glowing outline when Server is actively controlling input
                                if (isServerActive) {
                                    ImVec2 pMin = ImGui::GetItemRectMin();
                                    ImVec2 pMax = ImGui::GetItemRectMax();
                                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                                    float pulse = 0.65f + 0.35f * sinf(static_cast<float>(ImGui::GetTime()) * 4.0f);
                                    drawList->AddRect(ImVec2(pMin.x - 2, pMin.y - 2), ImVec2(pMax.x + 2, pMax.y + 2), IM_COL32(40, 230, 100, static_cast<int>(120 * pulse)), 6.0f, 0, 2.0f);
                                    drawList->AddRect(pMin, pMax, IM_COL32(60, 255, 130, 255), 4.0f, 0, 2.0f);
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip(isServerActive ? "Local Server: Currently controlling mouse and keyboard." : "Local Server: Input currently delegated to a Remote Client.");
                                }
                            } else if (matchedClient) {
                                // Client is active when in remote mode and this client's socket matches the controlling socket
                                bool isClientActive = State::IsRemote() && (matchedClient->socket == Network::g_controllingClientSocket.load(std::memory_order_relaxed));
                                if (isClientActive) {
                                    // Vibrant royal/cyan blue styling for actively controlled client
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.48f, 0.88f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.58f, 0.98f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.04f, 0.40f, 0.78f, 1.0f));
                                } else {
                                    // Muted steel blue styling for idle connected clients
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.28f, 0.48f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.36f, 0.58f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.22f, 0.40f, 1.0f));
                                }
                                char clientLabel[128];
                                if (isClientActive) {
                                    snprintf(clientLabel, sizeof(clientLabel), "%s\nClient\n[ACTIVE]", matchedClient->name.c_str());
                                } else {
                                    snprintf(clientLabel, sizeof(clientLabel), "%s\nClient", matchedClient->name.c_str());
                                }
                                ImGui::Button(clientLabel, ImVec2(cellSize, cellSize));
                                ImGui::PopStyleColor(3);

                                // Render glowing pulsing outline when this client is actively receiving input
                                if (isClientActive) {
                                    ImVec2 pMin = ImGui::GetItemRectMin();
                                    ImVec2 pMax = ImGui::GetItemRectMax();
                                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                                    float pulse = 0.65f + 0.35f * sinf(static_cast<float>(ImGui::GetTime()) * 4.0f);
                                    drawList->AddRect(ImVec2(pMin.x - 2, pMin.y - 2), ImVec2(pMax.x + 2, pMax.y + 2), IM_COL32(0, 200, 255, static_cast<int>(130 * pulse)), 6.0f, 0, 2.0f);
                                    drawList->AddRect(pMin, pMax, IM_COL32(0, 235, 255, 255), 4.0f, 0, 2.0f);
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip(isClientActive ? "Client %s: Actively receiving mouse and keyboard input." : "Client %s: Connected (drag to rearrange in layout).", matchedClient->name.c_str());
                                }

                                if (ImGui::BeginDragDropSource()) {
                                    ImGui::SetDragDropPayload("CLIENT_DRAG", &matchedClient->socket, sizeof(SOCKET));
                                    ImGui::Text("Moving %s", matchedClient->name.c_str());
                                    ImGui::EndDragDropSource();
                                }
                            } else if (isAdjacent) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
                                ImGui::Button("Empty", ImVec2(cellSize, cellSize));
                                ImGui::PopStyleColor();
                            } else {
                                ImGui::Dummy(ImVec2(cellSize, cellSize)); // Invisible spacer to maintain grid shape
                            }

                            if (!isServer && (matchedClient || isAdjacent)) {
                                if (ImGui::BeginDragDropTarget()) {
                                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CLIENT_DRAG")) {
                                        SOCKET draggedSocket = *(const SOCKET*)payload->Data;
                                        if (matchedClient) { // Collision detected, perform a coordinate swap
                                            SOCKET targetSocket = matchedClient->socket;
                                            int oldX = 0, oldY = 0;
                                            for (auto& c : clients) { if (c.socket == draggedSocket) { oldX = c.gridX; oldY = c.gridY; break; } }
                                            Network::UpdateClientGridPosition(targetSocket, oldX, oldY);
                                        }
                                        Network::UpdateClientGridPosition(draggedSocket, x, y);
                                    }
                                    ImGui::EndDragDropTarget();
                                }
                            }
                            ImGui::PopID();
                        }
                        ImGui::Spacing();
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                }
                ImGui::Text("Edge Control Toggles");
                ImGui::Separator();
                ImGui::SetNextItemWidth(150.0f);
                AtomicSliderInt("##CornerDeadzone", State::edgeDeadzonePercent, 0, 10);
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##Deadzone")) {
                    State::edgeDeadzonePercent.store(5, std::memory_order_relaxed);
                }
                DrawWrappedTooltip("Reset corner deadzone to factory default (5%).");
                ImGui::SameLine();
                ImGui::Text("Corner Deadzone (%%)");
                DrawWrappedTooltip("Creates safety zones in the corners to prevent accidental screen jumps when using the Start Menu or closing windows.");
                ImGui::EndTabItem();
            }

            bool tabPreferences = ImGui::BeginTabItem("Preferences");
            if (State::currentRole == State::AppRole::SERVER) {
                DrawWrappedTooltip("Customize startup behavior, input synchronization, global audio mixing, and debug settings.");
            } else {
                DrawWrappedTooltip("Customize startup behavior, input synchronization, local audio loopback streaming, and debug settings.");
            }
            if (tabPreferences) {
                ImGui::BeginChild("PreferencesContent", ImVec2(0, 0), false);
                
                ImGui::Text("Startup Configuration");
                ImGui::Separator();
                const char* startModeOptions = "Ask on Startup\0Server\0Client\0\0";
                int currentStartMode = static_cast<int>(State::defaultRole);
                if (ImGui::Combo("Default Start Mode", &currentStartMode, startModeOptions)) {
                    State::defaultRole = static_cast<State::AppRole>(currentStartMode);
                }
                DrawWrappedTooltip("Choose which mode KAM-Flow will automatically initialize when launched.");

                AtomicCheckbox("Minimize to System Tray", State::minimizeToTray);
                DrawWrappedTooltip("If checked, minimizing the window will hide it from the taskbar and run KAM-Flow silently in the background.");
                
                if (State::currentRole == State::AppRole::CLIENT) {
                    AtomicCheckbox("Auto-Reconnect to Last Server", State::enableClientAutoReconnect);
                    DrawWrappedTooltip("If checked, KAM-Flow will automatically attempt a single reconnect if the connection drops unintentionally (e.g., PC sleep/wake).");
                }

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::Text("Input Synchronization");
                ImGui::Separator();
                AtomicCheckbox("Enable Keyboard Control", State::enableKeyboardSync);
                DrawWrappedTooltip("If checked, keyboard keystrokes will be securely transmitted to the Client(s).");
                AtomicCheckbox("Enable Clipboard Sync", State::enableClipboardSync);
                DrawWrappedTooltip("If checked, copied text will automatically sync across the Server and Client(s).");
                AtomicCheckbox("Enable File Transfers", State::enableFileTransfer);
                DrawWrappedTooltip("If checked, allows drag-and-drop file sending and receiving via the encrypted out-of-band stream.");
                
                if (State::currentRole == State::AppRole::SERVER) {
                    ImGui::Spacing();
                    ImGui::SetNextItemWidth(150.0f);
                    AtomicSliderFloat("##MouseSensitivity", State::mouseSensitivity, 0.1f, 3.0f, "%.2f");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset##MouseSens")) {
                        State::mouseSensitivity.store(1.0f, std::memory_order_relaxed);
                    }
                    DrawWrappedTooltip("Reset mouse sensitivity to default (1.00).");
                    ImGui::SameLine();
                    ImGui::Text("Mouse Sensitivity");
                    DrawWrappedTooltip("Adjust the pointer speed on the Client screens (Global).");
                }

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::Text("Audio Subsystem");
                ImGui::Separator();

                ImGui::SetNextItemWidth(150.0f);
                AtomicSliderInt("##JitterBuffer", State::audioJitterBufferMs, 20, 200, "%d ms");
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##JitterBuffer")) {
                    State::audioJitterBufferMs.store(80, std::memory_order_relaxed);
                }
                DrawWrappedTooltip("Reset network jitter buffer to default (80 ms).");
                ImGui::SameLine();
                ImGui::Text("Network Jitter Buffer");
                if (State::currentRole == State::AppRole::SERVER) {
                    DrawWrappedTooltip("Increase to stop audio skipping on unstable networks (adds latency). Decrease for instant audio response when listening to Clients.");
                } else {
                    DrawWrappedTooltip("Increase to stop audio skipping when receiving the Server's Microphone. Decrease for instant audio response.");
                }
                ImGui::Spacing();

                if (State::currentRole == State::AppRole::SERVER) {
                    if (AtomicCheckbox("Enable Master Audio Mix (Listen to Clients)", State::enableServerAudioMix)) {
                        if (State::enableServerAudioMix) {
                            Audio::StartAudioRenderer();
                        } else {
                            Audio::StopAudioRenderer();
                        }
                    }
                    DrawWrappedTooltip("If checked, the server will mix and play audio received from connected clients.");

                    static char serverMicBuf[256] = "";
                    static bool serverMicInit = false;
                    if (!serverMicInit && State::enableServerMicBroadcast) {
                        std::string micName = Audio::GetDefaultMicName();
                        strncpy_s(serverMicBuf, sizeof(serverMicBuf), micName.c_str(), _TRUNCATE);
                        serverMicInit = true;
                    }
                    if (AtomicCheckbox("Broadcast Server Microphone to Clients", State::enableServerMicBroadcast)) {
                        if (State::enableServerMicBroadcast) {
                            Audio::StartMicBroadcast();
                            std::string micName = Audio::GetDefaultMicName();
                            strncpy_s(serverMicBuf, sizeof(serverMicBuf), micName.c_str(), _TRUNCATE);
                        } else {
                            Audio::StopMicBroadcast();
                        }
                    }
                    DrawWrappedTooltip("If checked, the server's default microphone will be captured and sent to all clients.");
                    if (State::enableServerMicBroadcast) {
                        ImGui::BeginDisabled(); ImGui::InputText("Capturing", serverMicBuf, sizeof(serverMicBuf)); ImGui::EndDisabled();
                    }

                    if (State::enableServerAudioMix) {
                        ImGui::Spacing(); ImGui::Spacing();
                        ImGui::Text("Client Sync Toggles");
                        ImGui::Separator();

                        auto connectedClients = Network::GetConnectedClients();
                        if (connectedClients.empty()) {
                            ImGui::TextDisabled("No clients connected.");
                        } else {
                            for (auto& client : connectedClients) {
                                ImGui::PushID(client.socket);
                                ImGui::Text("%s:", client.name.c_str());

                                // Clipboard Toggle
                                bool clipboardEnabled = client.isClipboardEnabled;
                                if (ImGui::Checkbox("Clipboard", &clipboardEnabled)) {
                                    Network::ToggleClientClipboard(client.socket, clipboardEnabled);
                                }
                            DrawWrappedTooltip("Toggle clipboard synchronization with this specific client.");
                                ImGui::SameLine();

                                // Audio Toggle
                                bool audioEnabled = client.isAudioEnabled;
                                if (ImGui::Checkbox("Audio", &audioEnabled)) {
                                    Network::ToggleClientAudio(client.socket, audioEnabled);
                                }
                            DrawWrappedTooltip("Toggle audio streaming from this specific client.");
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(120.0f);
                                float vol = client.audioVolume;
                                if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.0f, "Vol: %.2f")) {
                                    Network::SetClientVolume(client.socket, vol);
                                }
                                DrawWrappedTooltip("Adjust the individual audio mix volume for this specific client.");
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Reset##vol")) {
                                    Network::SetClientVolume(client.socket, 1.0f);
                                }
                                DrawWrappedTooltip("Reset client audio volume to default (1.00).");
                                ImGui::PopID();
                            }
                        }
                    }
                } else { // Client
                    // This checkbox now acts as a state toggle. The network thread handles the
                    // initial start. This logic handles runtime changes by the user.
                    if (AtomicCheckbox("Send Client Audio to Server", State::enableClientAudioStream)) {
                        // Only attempt to start/stop if we are already connected.
                        if (Network::IsClientConnected()) {
                            if (State::enableClientAudioStream) Audio::StartLoopbackCapture();
                            else Audio::StopLoopbackCapture();
                        }
                    }
                    DrawWrappedTooltip("If checked, all system audio playing on this Client will be streamed to the Server for centralized mixing.");

                    if (AtomicCheckbox("Receive Server Microphone", State::enableClientMicReceive)) {
                        if (Network::IsClientConnected()) {
                            if (State::enableClientMicReceive) {
                                Audio::StartMicReceiver();
                            } else {
                                Audio::StopMicReceiver();
                            }
                        }
                    }
                    DrawWrappedTooltip("If checked, audio from the Server's microphone will be received and injected into a local Virtual Audio Cable.");
                    
                    if (State::enableClientMicReceive) {
                        if (!Audio::IsVirtualAudioCableInstalled()) {
                            ImGui::Spacing();
                            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
                            ImGui::BeginChild("VBCableInfo", ImVec2(0, 200), true);
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Virtual Audio Cable Required");
                            ImGui::TextWrapped("Due to strict Windows security architecture, applications cannot programmatically create 'fake' hardware microphones. To use the Server's microphone in your local apps (Zoom, Discord, etc.), a safe, free Virtual Audio Cable is required.");
                            ImGui::Spacing();
                            ImGui::TextWrapped("Benefits: 100%% legal, zero-latency, digitally signed by Microsoft WHQL, and zero kernel-level security risks.");
                            ImGui::Spacing();
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 1.0f, 1.0f));
                            ImGui::TextWrapped("1. Download & Install 'VB-CABLE' from vb-audio.com");
                            ImGui::TextWrapped("2. IMPORTANT: In Windows Sound Settings, set Default Output to Speakers and Default Input to your physical Mic.");
                            ImGui::TextWrapped("3. Restart KAM-Flow. It will automatically target the Virtual Cable.");
                            ImGui::PopStyleColor();
                            ImGui::EndChild();
                            ImGui::PopStyleColor();
                        }
                        
                        ImGui::BeginDisabled();
                        static char clientTargetBuf[128] = "CABLE Input (VB-Audio Virtual Cable)";
                        ImGui::InputText("Target Device", clientTargetBuf, sizeof(clientTargetBuf));
                        ImGui::EndDisabled();
                    }
                }

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::Text("Safety & Control");
                ImGui::Separator();

                const char* hotkeyOptions = "A\0B\0C\0D\0E\0F\0G\0H\0I\0J\0K\0L\0M\0N\0O\0P\0Q\0R\0S\0T\0U\0V\0W\0X\0Y\0Z\0\0";
                int currentItem = State::emergencyHotkey.load(std::memory_order_relaxed) - 'A';
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Emergency Hotkey (Ctrl+Alt+)");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::Combo("##EmergencyHotkey", &currentItem, hotkeyOptions)) {
                    State::emergencyHotkey = 'A' + currentItem;
                    ::UnregisterHotKey(hwnd, 1);
                    ::RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, State::emergencyHotkey.load(std::memory_order_relaxed));
                }
                DrawWrappedTooltip("Customize the global OS hotkey used to instantly regain local control or sever a connection.");

                if (State::currentRole == State::AppRole::SERVER) {
                    ImGui::Spacing();
                    
                    if (AtomicCheckbox("Enable Game Mode", State::enableGameMode)) {
                        if (State::enableGameMode) {
                            State::SetMode(State::ControlMode::LOCAL);
                            if (g_HookThreadId != 0) ::PostThreadMessage(g_HookThreadId.load(), WM_TOGGLE_GAMEMODE, 1, 0);
                        } else {
                            if (g_HookThreadId != 0) ::PostThreadMessage(g_HookThreadId.load(), WM_TOGGLE_GAMEMODE, 0, 0);
                        }
                    }
                    DrawWrappedTooltip("When active, the mouse will not transition to Client screens, preventing games from losing focus.");
                    
                    int gmCurrentItem = State::gameModeHotkey.load(std::memory_order_relaxed) - 'A';
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Game Mode Toggle Hotkey (Ctrl+Alt+)");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    if (ImGui::Combo("##GameModeHotkey", &gmCurrentItem, hotkeyOptions)) {
                        State::gameModeHotkey = 'A' + gmCurrentItem;
                        ::UnregisterHotKey(hwnd, 2);
                        ::RegisterHotKey(hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, State::gameModeHotkey.load(std::memory_order_relaxed));
                    }
                    DrawWrappedTooltip("Customize the global OS hotkey used to toggle Game Mode on/off.");
                }

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::Text("Engine & Debug");
                ImGui::Separator();
                
                AtomicCheckbox("Show Debug Console Tab", State::globalDebugMode);
                DrawWrappedTooltip("Show or hide the real-time diagnostic console tab for troubleshooting.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (State::globalDebugMode) {
                bool tabDebug = ImGui::BeginTabItem("Debug Log");
                if (tabDebug) {
                    static std::vector<char> localLogCopy;
                    static ImGuiTextFilter logFilter;
                    static bool autoScroll = true;

                    if (ImGui::Button("Clear Log")) {
                        std::lock_guard<std::mutex> lock(g_logMutex);
                        g_logBufferLen = 0;
                        if (!g_logBufferVec.empty()) g_logBufferVec[0] = '\0';
                        if (!localLogCopy.empty()) localLogCopy[0] = '\0';
                    }
                    DrawWrappedTooltip("Clear all current debug messages.");

                    ImGui::SameLine();
                    if (ImGui::Button("Copy All")) {
                        if (!localLogCopy.empty() && localLogCopy[0] != '\0') {
                            if (logFilter.IsActive()) {
                                std::string filteredLog;
                                const char* s = localLogCopy.data();
                                while (*s) {
                                    const char* e = strchr(s, '\n');
                                    if (!e) e = s + strlen(s);
                                    if (logFilter.PassFilter(s, e)) {
                                        filteredLog.append(s, e - s);
                                        filteredLog.push_back('\n');
                                    }
                                    if (*e == '\n') s = e + 1;
                                    else break;
                                }
                                ImGui::SetClipboardText(filteredLog.c_str());
                            } else {
                                ImGui::SetClipboardText(localLogCopy.data());
                            }
                        }
                    }
                    DrawWrappedTooltip("Copy debug log messages to clipboard (copies filtered lines when filter is active).");

                    ImGui::SameLine();
                    ImGui::Checkbox("Auto-Scroll", &autoScroll);

                    ImGui::SameLine();
                    logFilter.Draw("Filter", 150.0f);
                    DrawWrappedTooltip("Filter logs by keyword (comma-separated tokens, e.g. 'Network, -ping').");

                    ImGui::Separator();
                    
                    // Only lock and copy if new text was actually appended to the buffer
                    bool needCopy = g_logDirty.exchange(false);
                    if (needCopy || (localLogCopy.empty() && g_logBufferLen > 0)) {
                        std::lock_guard<std::mutex> lock(g_logMutex);
                        if (localLogCopy.empty()) localLogCopy.resize(1024 * 1024, 0);
                        if (!g_logBufferVec.empty()) {
                            std::memcpy(localLogCopy.data(), g_logBufferVec.data(), g_logBufferLen + 1);
                        } else {
                            localLogCopy[0] = '\0';
                        }
                    }
                    
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
                    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

                    if (!localLogCopy.empty() && localLogCopy[0] != '\0') {
                        const char* line_start = localLogCopy.data();
                        while (*line_start) {
                            const char* line_end = strchr(line_start, '\n');
                            if (!line_end) line_end = line_start + strlen(line_start);

                            if (logFilter.PassFilter(line_start, line_end)) {
                                ImVec4 col(0.82f, 0.82f, 0.82f, 1.0f); // Default light gray
                                std::string_view sv(line_start, line_end - line_start);

                                if (sv.find("[Security]") != std::string_view::npos ||
                                    sv.find("PIN") != std::string_view::npos ||
                                    sv.find("Blocked") != std::string_view::npos) {
                                    col = ImVec4(1.0f, 0.75f, 0.22f, 1.0f); // Amber / Yellow
                                } else if (sv.find("ERROR") != std::string_view::npos ||
                                           sv.find("Error") != std::string_view::npos ||
                                           sv.find("failed") != std::string_view::npos ||
                                           sv.find("Failed") != std::string_view::npos ||
                                           sv.find("CRITICAL") != std::string_view::npos ||
                                           sv.find("dropped") != std::string_view::npos ||
                                           sv.find("Dropped") != std::string_view::npos) {
                                    col = ImVec4(1.0f, 0.35f, 0.35f, 1.0f); // Coral Red
                                } else if (sv.find("[Network") != std::string_view::npos) {
                                    col = ImVec4(0.28f, 0.80f, 1.0f, 1.0f); // Bright Cyan
                                } else if (sv.find("[Audio]") != std::string_view::npos ||
                                           sv.find("Audio") != std::string_view::npos) {
                                    col = ImVec4(0.82f, 0.55f, 1.0f, 1.0f); // Purple / Violet
                                } else if (sv.find("[KAM-Flow UI]") != std::string_view::npos ||
                                           sv.find("[UI]") != std::string_view::npos) {
                                    col = ImVec4(0.35f, 0.85f, 0.45f, 1.0f); // Mint Green
                                }

                                ImGui::PushStyleColor(ImGuiCol_Text, col);
                                ImGui::TextUnformatted(line_start, line_end);
                                ImGui::PopStyleColor();
                            }

                            if (*line_end == '\n') line_start = line_end + 1;
                            else break;
                        }
                    }

                    if (autoScroll && (needCopy || ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) {
                        ImGui::SetScrollHereY(1.0f);
                    }

                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::EndTabItem();
                }
            }

            ImGui::EndTabBar();
        }

        // Render file transfer confirmation modal.
        if (g_triggerDropModal) {
            ImGui::OpenPopup("File Transfer Offer");
            g_triggerDropModal = false;
        }
        
        if (ImGui::BeginPopupModal("File Transfer Offer", &g_showDropModal, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("File selected for transfer:");
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s", g_pendingDropFile.c_str());
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            
            if (State::currentRole == State::AppRole::SERVER) {
                ImGui::Text("Select Target Client(s):");
                auto clients = Network::GetConnectedClients();
                if (clients.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No clients connected.");
                } else {
                    for (const auto& c : clients) {
                        bool isSelected = std::find(g_selectedTransferClients.begin(), g_selectedTransferClients.end(), c.socket) != g_selectedTransferClients.end();
                        if (ImGui::Checkbox(c.name.c_str(), &isSelected)) {
                            if (isSelected) g_selectedTransferClients.push_back(c.socket);
                            else g_selectedTransferClients.erase(std::remove(g_selectedTransferClients.begin(), g_selectedTransferClients.end(), c.socket), g_selectedTransferClients.end());
                        }
                    }
                }
            } else {
                ImGui::Text("Target: "); ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Server");
            }
            
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            
            if (ImGui::Button("Send", ImVec2(120, 0))) {
                if (State::globalDebugMode) UI::LogDebug("[UI] Initiating transfer for: %s", g_pendingDropFile.c_str());
                
                if (State::currentRole == State::AppRole::CLIENT) {
                    g_selectedTransferClients.clear();
                    g_selectedTransferClients.push_back(INVALID_SOCKET); // SendToServer ignores this, but loop needs 1 element to fire
                }
                FileTransfer::InitiateTransfer(g_pendingDropFile, g_selectedTransferClients);
                g_showDropModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                g_showDropModal = false;
                g_pendingDropFile.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Close main dashboard window FIRST so subsequent overlay windows reliably render on top
        ImGui::End();

        // --- OVERLAY RENDERING (Rendered outside the main window to ensure proper Z-stacking) ---
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        auto pendingTransfers = FileTransfer::GetPendingTransfers();
        float pendingHeight = 0.0f;

        // Render incoming file offers notification (anchored bottom-right, growing upwards)
        if (!pendingTransfers.empty()) {
            const auto& offer = pendingTransfers[0]; // Process one offer at a time
            ImGui::SetNextWindowPos(ImVec2(displaySize.x - 20.0f, displaySize.y - 20.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(340, 0)); // Auto-height growing upwards
            
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.22f, 0.14f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.85f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.15f, 0.30f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.18f, 0.36f, 0.22f, 1.0f));
            if (ImGui::Begin("Incoming File Transfer", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("File: %s", offer.fileName.c_str());
                ImGui::Text("Size: %.2f MB", offer.fileSize / (1024.0f * 1024.0f));
                
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                
                if (ImGui::Button("Accept", ImVec2(140, 0))) {
                    char szFileName[MAX_PATH] = "";
                    strncpy_s(szFileName, offer.fileName.c_str(), _TRUNCATE);
                    
                    OPENFILENAMEA ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = "All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = szFileName;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
                    
                    if (GetSaveFileNameA(&ofn)) FileTransfer::AcceptTransfer(offer.transferId, std::string(szFileName));
                    else FileTransfer::DeclineTransfer(offer.transferId); // User cancelled dialog
                }
                ImGui::SameLine();
                if (ImGui::Button("Decline", ImVec2(140, 0))) FileTransfer::DeclineTransfer(offer.transferId);
                pendingHeight = ImGui::GetWindowHeight();
            }
            ImGui::End();
            ImGui::PopStyleColor(4);
        }

        // Render active file transfers progress visualization (anchored bottom-right, stacked above pending offers if visible)
        auto activeTransfersList = FileTransfer::GetActiveTransfers();
        if (!activeTransfersList.empty()) {
            float yPos = displaySize.y - 20.0f;
            if (pendingHeight > 0.0f) {
                yPos -= (pendingHeight + 10.0f);
            }
            ImGui::SetNextWindowPos(ImVec2(displaySize.x - 20.0f, yPos), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(340, 0)); // Auto-height growing upwards
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.16f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.55f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.15f, 0.22f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.18f, 0.28f, 0.45f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.0f, 0.65f, 1.0f, 1.0f)); // Bright, opaque blue fill
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.08f, 1.0f));       // Solid dark track for contrast
            if (ImGui::Begin("Active Transfers", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
                for (const auto& t : activeTransfersList) {
                    float progress = t.fileSize > 0 ? (float)t.transferredSize / (float)t.fileSize : 0.0f;
                    ImGui::TextWrapped("%s", t.fileName.c_str());
                    char overlay[64];
                    snprintf(overlay, sizeof(overlay), "%.2f / %.2f MB (%.0f%%)", 
                             (float)t.transferredSize / (1024.0f*1024.0f), 
                             (float)t.fileSize / (1024.0f*1024.0f),
                             progress * 100.0f);
                    ImGui::ProgressBar(progress, ImVec2(-45, 0), overlay);
                    ImGui::SameLine();
                    ImGui::PushID(t.transferId);
                    if (ImGui::Button("X", ImVec2(35, 0))) {
                        FileTransfer::CancelTransfer(t.transferId);
                    }
                    DrawWrappedTooltip("Cancel Transfer");
                    ImGui::PopID();
                }
            }
            ImGui::End();
            ImGui::PopStyleColor(6);
        }
    }

    /**
     * @brief Executes the rendering pipeline for a single frame.
     * @return void
     */
    void RenderFrame() {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) isAppRunning = false;
        }
        if (!isAppRunning) return;

        // Process background notifications (runs even if minimized).
        static uint32_t s_lastNotifiedTransferId = 0;
        auto currentPending = FileTransfer::GetPendingTransfers();
        if (!currentPending.empty()) {
            if (currentPending[0].transferId != s_lastNotifiedTransferId) {
                s_lastNotifiedTransferId = currentPending[0].transferId;
                
                if (g_isMinimizedToTray) {
                    g_nid.uFlags |= NIF_INFO;
                    strncpy_s(g_nid.szInfoTitle, "Incoming File Transfer", _TRUNCATE);
                    std::string infoMsg = "File: " + currentPending[0].fileName + "\nClick here to review and save.";
                    strncpy_s(g_nid.szInfo, infoMsg.c_str(), _TRUNCATE);
                    g_nid.dwInfoFlags = NIIF_INFO;
                    ::Shell_NotifyIconA(NIM_MODIFY, &g_nid);
                } else {
                    FLASHWINFO fw = { sizeof(FLASHWINFO), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0 };
                    ::FlashWindowEx(&fw);
                }
            }
        } else {
            s_lastNotifiedTransferId = 0;
        }

        // Skip D3D rendering completely when minimized to save GPU cycles
        if (g_isMinimizedToTray) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60hz throttle
            return;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (State::currentRole == State::AppRole::NONE) {
            RenderPreFlightScreen();
        } else {
            RenderDashboardScreen();
        }

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        pd3dDeviceContext->OMSetRenderTargets(1, &pMainRenderTargetView, nullptr);
        pd3dDeviceContext->ClearRenderTargetView(pMainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        HRESULT hrPresent = pSwapChain->Present(1, 0); 
        if (hrPresent == DXGI_ERROR_DEVICE_REMOVED || hrPresent == DXGI_ERROR_DEVICE_RESET) {
            // DX11 device lost (typically caused by sleep/wake, driver update, or GPU reset).
            // Rebuild the entire D3D pipeline to recover gracefully instead of crashing.
            if (State::globalDebugMode) UI::LogDebug("[UI] DX11 Device Lost. Attempting recovery...");
            CleanupRenderTarget();
            if (pSwapChain) { pSwapChain->Release(); pSwapChain = nullptr; }
            if (pd3dDeviceContext) { pd3dDeviceContext->Release(); pd3dDeviceContext = nullptr; }
            if (pd3dDevice) { pd3dDevice->Release(); pd3dDevice = nullptr; }

            DXGI_SWAP_CHAIN_DESC sd;
            ZeroMemory(&sd, sizeof(sd));
            sd.BufferCount = 2;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferDesc.RefreshRate.Numerator = 60;
            sd.BufferDesc.RefreshRate.Denominator = 1;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hwnd;
            sd.SampleDesc.Count = 1;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            D3D_FEATURE_LEVEL featureLevel;
            const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
            HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &pSwapChain, &pd3dDevice, &featureLevel, &pd3dDeviceContext);
            if (res == DXGI_ERROR_UNSUPPORTED || res == E_FAIL) {
                res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &pSwapChain, &pd3dDevice, &featureLevel, &pd3dDeviceContext);
            }
            if (SUCCEEDED(res)) {
                CreateRenderTarget();
                ImGui_ImplDX11_InvalidateDeviceObjects();
                ImGui_ImplDX11_CreateDeviceObjects();
            } else {
                isAppRunning = false; // Unrecoverable — exit gracefully
            }
            return;
        }
    }

    /**
     * @brief Cleans up D3D11 resources, destroys the ImGui context, and closes the window.
     * @return void
     */
    void Shutdown() {
        if (g_isMinimizedToTray) {
            ::Shell_NotifyIconA(NIM_DELETE, &g_nid);
        }
        
        const std::string& iniPath = Config::GetResolvedPath();
        WritePrivateProfileStringA("UI", "WindowX", std::to_string(g_windowX).c_str(), iniPath.c_str());
        WritePrivateProfileStringA("UI", "WindowY", std::to_string(g_windowY).c_str(), iniPath.c_str());
        WritePrivateProfileStringA("UI", "WindowW", std::to_string(g_windowW).c_str(), iniPath.c_str());
        WritePrivateProfileStringA("UI", "WindowH", std::to_string(g_windowH).c_str(), iniPath.c_str());

        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
        CleanupRenderTarget();
        if (pSwapChain) { pSwapChain->Release(); pSwapChain = nullptr; }
        if (pd3dDeviceContext) { pd3dDeviceContext->Release(); pd3dDeviceContext = nullptr; }
        if (pd3dDevice) { pd3dDevice->Release(); pd3dDevice = nullptr; }
        if (hwnd) { 
            ::UnregisterHotKey(hwnd, 1);
            ::UnregisterHotKey(hwnd, 2);
            ::DestroyWindow(hwnd); 
            ::UnregisterClass("KAMFlowUIClass", GetModuleHandle(nullptr)); 
            hwnd = nullptr; 
        }

        if (g_logInterceptor) {
            delete g_logInterceptor;
            g_logInterceptor = nullptr;
        }
    }

    /**
     * @brief Checks if the UI window is still active and hasn't received a close request.
     * @return true if the UI should continue running.
     */
    bool IsRunning() { return isAppRunning; }
}