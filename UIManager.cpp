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
#include <string>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <shellapi.h>
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <commdlg.h>
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

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
    bool focusPairingTab = false;
    
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
                    if (State::globalDebugMode) std::cout << "[UI] File dropped: " << filePath << "\n";
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
                        if (State::globalDebugMode) std::cout << "[KAM-Flow UI] Emergency Override. Reverting to LOCAL.\n";
                        State::SetMode(State::ControlMode::LOCAL);
                    } else if (State::currentRole == State::AppRole::CLIENT) {
                        if (State::globalDebugMode) std::cout << "[KAM-Flow UI] Emergency Override. Disconnecting Client.\n";
                        Network::StopClient();
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
            case WM_DESTROY:
                ::PostQuitMessage(0);
                isAppRunning = false;
                return 0;
        }
        return ::DefWindowProc(hWnd, msg, wParam, lParam);
    }

    /**
     * @brief Initializes the Win32 window, DirectX 11 device, and Dear ImGui context.
     * @return true if initialization succeeded, false if any subsystem failed.
     */
    bool Initialize() {
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
        ::RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, State::emergencyHotkey);

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
        ImGui::StyleColorsDark();
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

        ImGui::Text("KAM-Flow %s", (State::currentRole == State::AppRole::SERVER ? "SERVER" : "CLIENT"));
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
                        snprintf(btnText, sizeof(btnText), "FORCE LOCAL CONTROL (Ctrl+Alt+%c)", State::emergencyHotkey);
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
                    auto targets = CredentialManager::GetSavedTargets("KAMFlow_Server_");
                    bool isPaired = !targets.empty();

                    std::string currentStatus = State::GetClientStatus();
                    ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // Default White
                    if (currentStatus == "Connected") {
                        statusColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Green
                    } else if (currentStatus.find("Connecting") != std::string::npos || 
                               currentStatus.find("Authenticating") != std::string::npos || 
                               currentStatus.find("reconnecting") != std::string::npos) {
                        statusColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow
                    } else if (currentStatus.find("Failed") != std::string::npos || 
                               currentStatus.find("Disconnected") != std::string::npos ||
                               currentStatus.find("dropped") != std::string::npos) {
                        statusColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
                    }

                    ImGui::Text("Connection Status: ");
                    ImGui::SameLine();
                    ImGui::TextColored(statusColor, "%s", currentStatus.c_str());
                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    if (!Network::IsClientConnected()) {
                        if (!isPaired) {
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "No Server Paired.");
                            ImGui::Text("Please go to the 'Security & Pairing' tab to pair a server.");
                        } else {
                            std::string ip = targets[0].substr(15);
                            auto servers = Network::GetDiscoveredServers();
                            bool foundOnline = false;
                            uint16_t targetPort = 8080;
                            std::string targetHostname = "Unknown";

                            for (const auto& srv : servers) {
                                if (srv.ip == ip) {
                                    targetPort = srv.tcpPort;
                                    targetHostname = srv.hostname;
                                    foundOnline = true;
                                    break;
                                }
                            }

                            ImGui::Text("Currently Linked to Server:");
                            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s", targetHostname.c_str());
                            ImGui::SameLine(); ImGui::Text("(%s)", ip.c_str());
                            ImGui::Spacing();

                            if (foundOnline) {
                                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Server is ONLINE!");
                                std::string btnText = "Connect to " + targetHostname + " (" + ip + ")";
                                if (ImGui::Button(btnText.c_str(), ImVec2(-1, 40))) {
                                    Network::StartClient(ip, targetPort);
                                }
                                DrawWrappedTooltip("Establish a secure KVM and Audio connection to this Server.");
                            } else {
                                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Server is offline or not discoverable.");
                                if (ImGui::Button(("Attempt Manual Connection to " + ip).c_str(), ImVec2(-1, 40))) {
                                    Network::StartClient(ip, targetPort);
                                }
                                DrawWrappedTooltip("Try to connect even though the server is not broadcasting on the local network.");
                            }
                        }
                    } else {
                        ImGui::TextWrapped("Receiving Input and KVM Commands from Server.");
                        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                        
                        char btnText[64];
                        snprintf(btnText, sizeof(btnText), "Disconnect (Ctrl+Alt+%c)", State::emergencyHotkey);
                        if (ImGui::Button(btnText, ImVec2(-1, 40))) {
                            Network::StopClient();
                        }
                        DrawWrappedTooltip("Safely terminate the encrypted TCP connection and return to standby.");
                    }
                }
                ImGui::EndTabItem();
            }

            bool tabSecurity = ImGui::BeginTabItem("Security & Pairing");
            if (State::currentRole == State::AppRole::SERVER) {
                DrawWrappedTooltip("Manage your Master PIN to control which clients are authorized to connect.");
            } else {
                DrawWrappedTooltip("Discover local KAM-Flow servers, manage trusted devices, and securely save connection credentials.");
            }
            if (tabSecurity) {
                if (State::currentRole == State::AppRole::SERVER) {
                    ImGui::Text("Security & Access Control");
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
                } else { // Client
                    int tabFlags = 0;
                    if (focusPairingTab) {
                        tabFlags = ImGuiTabItemFlags_SetSelected;
                        focusPairingTab = false;
                    }

                    ImGui::Text("Pairing Status");
                    ImGui::Separator();
                    
                    auto targets = CredentialManager::GetSavedTargets("KAMFlow_Server_");
                    if (!targets.empty()) {
                        std::string ip = targets[0].substr(15);
                        
                        std::string targetHostname = "Unknown";
                        auto servers = Network::GetDiscoveredServers();
                        for (const auto& srv : servers) {
                            if (srv.ip == ip) {
                                targetHostname = srv.hostname;
                                break;
                            }
                        }
                        
                        ImGui::Text("Currently Linked to Server:");
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", targetHostname.c_str());
                        ImGui::SameLine(); ImGui::Text("(%s)", ip.c_str());

                        ImGui::Spacing();
                        ImGui::TextWrapped("A Client can only be linked to one Server at a time. To link to a different Server, or if the Master PIN changed, you must forget the current Server.");
                        ImGui::Spacing();
                        if (ImGui::Button("Forget Server & Reset Pairing", ImVec2(-1, 30))) {
                            for(const auto& t : targets) {
                                CredentialManager::DeleteSecret(t);
                            }
                        }
                        DrawWrappedTooltip("Delete the saved PIN for this server from the Windows Credential Vault to allow pairing with a new one.");
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "No Server Linked.");
                        ImGui::Spacing(); ImGui::Spacing();

                        ImGui::Text("Discovered Servers (Local Network)");
                        ImGui::Separator();
                        auto servers = Network::GetDiscoveredServers();
                        if (servers.empty()) {
                            ImGui::TextDisabled("Scanning... No servers found.");
                        } else {
                            for (const auto& srv : servers) {
                                ImGui::Text("%s (%s)", srv.hostname.c_str(), srv.ip.c_str());
                                ImGui::SameLine(ImGui::GetWindowWidth() - 80);
                                ImGui::PushID(srv.ip.c_str());
                                if (ImGui::Button("Select", ImVec2(60, 20))) {
                                    strncpy_s(newServerIP, sizeof(newServerIP), srv.ip.c_str(), _TRUNCATE);
                                }
                                DrawWrappedTooltip("Auto-fill the IP address below.");
                                ImGui::PopID();
                            }
                        }

                        ImGui::Spacing(); ImGui::Spacing();
                        ImGui::Text("Link to Server");
                        ImGui::Separator();
                        ImGui::InputText("Server IP", newServerIP, sizeof(newServerIP));
                        ImGui::InputText("Master PIN", newServerPIN, sizeof(newServerPIN), ImGuiInputTextFlags_Password);
                        
                        if (ImGui::Button("Save Pairing", ImVec2(-1, 30))) {
                            if (strlen(newServerIP) > 0 && strlen(newServerPIN) > 0) {
                                CredentialManager::SaveSecret("KAMFlow_Server_" + std::string(newServerIP), std::string(newServerPIN));
                                memset(newServerIP, 0, sizeof(newServerIP));
                                memset(newServerPIN, 0, sizeof(newServerPIN));
                            }
                        }
                        DrawWrappedTooltip("Save this Server's IP and PIN to the Windows Credential Vault for future connections.");
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
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
                                static char serverName[MAX_COMPUTERNAME_LENGTH + 1] = "";
                                if (serverName[0] == '\0') {
                                    DWORD size = sizeof(serverName);
                                    GetComputerNameA(serverName, &size);
                                }
                                char serverLabel[128];
                                snprintf(serverLabel, sizeof(serverLabel), "%s\nServer", serverName);
                                ImGui::Button(serverLabel, ImVec2(cellSize, cellSize));
                                ImGui::PopStyleColor(3);
                            } else if (matchedClient) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.8f, 1.0f));
                                char clientLabel[128];
                                snprintf(clientLabel, sizeof(clientLabel), "%s\nClient", matchedClient->name.c_str());
                                ImGui::Button(clientLabel, ImVec2(cellSize, cellSize));
                                ImGui::PopStyleColor();

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
                ImGui::SliderInt("Corner Deadzone (%)", &State::edgeDeadzonePercent, 0, 10);
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

                ImGui::Checkbox("Minimize to System Tray", &State::minimizeToTray);
                DrawWrappedTooltip("If checked, minimizing the window will hide it from the taskbar and run KAM-Flow silently in the background.");
                
                if (State::currentRole == State::AppRole::CLIENT) {
                    ImGui::Checkbox("Auto-Reconnect to Last Server", &State::enableClientAutoReconnect);
                    DrawWrappedTooltip("If checked, KAM-Flow will automatically attempt a single reconnect if the connection drops unintentionally (e.g., PC sleep/wake).");
                }

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::Text("Input Synchronization");
                ImGui::Separator();
                ImGui::Checkbox("Enable Keyboard Control", &State::enableKeyboardSync);
                DrawWrappedTooltip("If checked, keyboard keystrokes will be securely transmitted to the Client(s).");
                ImGui::Checkbox("Enable Clipboard Sync", &State::enableClipboardSync);
                DrawWrappedTooltip("If checked, copied text will automatically sync across the Server and Client(s).");
                ImGui::Checkbox("Enable File Transfers", &State::enableFileTransfer);
                DrawWrappedTooltip("If checked, allows drag-and-drop file sending and receiving via the encrypted out-of-band stream.");
                
                if (State::currentRole == State::AppRole::SERVER) {
                    ImGui::Spacing();
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::SliderFloat("Mouse Sensitivity", &State::mouseSensitivity, 0.1f, 3.0f, "%.2f");
                    DrawWrappedTooltip("Adjust the pointer speed on the Client screens (Global).");
                }

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::Text("Audio Subsystem");
                ImGui::Separator();

                ImGui::SetNextItemWidth(200.0f);
                ImGui::SliderInt("Network Jitter Buffer (ms)", &State::audioJitterBufferMs, 20, 200, "%d ms");
                if (State::currentRole == State::AppRole::SERVER) {
                    DrawWrappedTooltip("Increase to stop audio skipping on unstable networks (adds latency). Decrease for instant audio response when listening to Clients.");
                } else {
                    DrawWrappedTooltip("Increase to stop audio skipping when receiving the Server's Microphone. Decrease for instant audio response.");
                }
                ImGui::Spacing();

                if (State::currentRole == State::AppRole::SERVER) {
                    bool lastServerAudioMixState = State::enableServerAudioMix;
                    ImGui::Checkbox("Enable Master Audio Mix (Listen to Clients)", &State::enableServerAudioMix);
                    DrawWrappedTooltip("If checked, the server will mix and play audio received from connected clients.");
                    if (lastServerAudioMixState != State::enableServerAudioMix) {
                        if (State::enableServerAudioMix) {
                            Audio::StartAudioRenderer();
                        } else {
                            Audio::StopAudioRenderer();
                        }
                    }

                    bool lastServerMicBroadcastState = State::enableServerMicBroadcast;
                    static char serverMicBuf[256] = "";
                static bool serverMicInit = false;
                if (!serverMicInit && State::enableServerMicBroadcast) {
                    std::string micName = Audio::GetDefaultMicName();
                    strncpy_s(serverMicBuf, sizeof(serverMicBuf), micName.c_str(), _TRUNCATE);
                    serverMicInit = true;
                }
                    ImGui::Checkbox("Broadcast Server Microphone to Clients", &State::enableServerMicBroadcast);
                DrawWrappedTooltip("If checked, the server's default microphone will be captured and sent to all clients.");

                    if (lastServerMicBroadcastState != State::enableServerMicBroadcast) {
                        if (State::enableServerMicBroadcast) {
                            Audio::StartMicBroadcast();
                            std::string micName = Audio::GetDefaultMicName();
                            strncpy_s(serverMicBuf, sizeof(serverMicBuf), micName.c_str(), _TRUNCATE);
                        } else {
                            Audio::StopMicBroadcast();
                        }
                    }
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
                                ImGui::SetNextItemWidth(150.0f);
                                float vol = client.audioVolume;
                                if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.0f, "Vol: %.2f")) {
                                    Network::SetClientVolume(client.socket, vol);
                                }
                            DrawWrappedTooltip("Adjust the individual audio mix volume for this specific client.");
                                ImGui::PopID();
                            }
                        }
                    }
                } else { // Client
                    // This checkbox now acts as a state toggle. The network thread handles the
                    // initial start. This logic handles runtime changes by the user.
                    if (ImGui::Checkbox("Send Client Audio to Server", &State::enableClientAudioStream)) {
                        // Only attempt to start/stop if we are already connected.
                        if (Network::IsClientConnected()) {
                            if (State::enableClientAudioStream) Audio::StartLoopbackCapture();
                            else Audio::StopLoopbackCapture();
                        }
                    }
                    DrawWrappedTooltip("If checked, all system audio playing on this Client will be streamed to the Server for centralized mixing.");

                    if (ImGui::Checkbox("Receive Server Microphone", &State::enableClientMicReceive)) {
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
                int currentItem = State::emergencyHotkey - 'A';
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Emergency Hotkey (Ctrl+Alt+)");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::Combo("##EmergencyHotkey", &currentItem, hotkeyOptions)) {
                    State::emergencyHotkey = 'A' + currentItem;
                    ::UnregisterHotKey(hwnd, 1);
                    ::RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, State::emergencyHotkey);
                }
                DrawWrappedTooltip("Customize the global OS hotkey used to instantly regain local control or sever a connection.");

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::Text("Engine & Debug");
                ImGui::Separator();
                
                if (ImGui::Checkbox("Show Debug Console", &State::globalDebugMode)) {
                    State::UpdateConsoleVisibility();
                }
                DrawWrappedTooltip("Show or hide the real-time diagnostic console window for troubleshooting.");
                ImGui::EndChild();
                ImGui::EndTabItem();
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
                if (State::globalDebugMode) std::cout << "[UI] Initiating transfer for: " << g_pendingDropFile << "\n";
                
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

        // Render incoming file offers notification.
        auto pendingTransfers = FileTransfer::GetPendingTransfers();
        if (!pendingTransfers.empty()) {
            const auto& offer = pendingTransfers[0]; // Process one offer at a time
            
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 340, ImGui::GetIO().DisplaySize.y - 140), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(320, 120));
            
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.3f, 0.1f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
            ImGui::Begin("Incoming File Transfer", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
            
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
            
            ImGui::End(); ImGui::PopStyleColor(2);
        }

        // Render active file transfers progress visualization.
        auto activeTransfers = FileTransfer::GetActiveTransfers();
        if (!activeTransfers.empty()) {
            // Dynamically stack the progress UI directly underneath the pending offer notification if it's visible
            float yOffset = FileTransfer::GetPendingTransfers().empty() ? 140.0f : 270.0f;
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 340, ImGui::GetIO().DisplaySize.y - yOffset), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(320, 0)); // Auto-height to support multiple active transfers
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.15f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.4f, 0.8f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.0f, 0.6f, 1.0f, 1.0f)); // Bright, opaque blue fill
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));       // Solid black track for contrast
            ImGui::Begin("Active Transfers", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
            for (const auto& t : activeTransfers) {
                float progress = t.fileSize > 0 ? (float)t.transferredSize / (float)t.fileSize : 0.0f;
                ImGui::TextWrapped("%s", t.fileName.c_str());
                char overlay[64];
                snprintf(overlay, sizeof(overlay), "%.2f MB / %.2f MB", (float)t.transferredSize / (1024.0f*1024.0f), (float)t.fileSize / (1024.0f*1024.0f));
                ImGui::ProgressBar(progress, ImVec2(-40, 0), overlay);
                ImGui::SameLine();
                ImGui::PushID(t.transferId);
                if (ImGui::Button("X", ImVec2(35, 0))) {
                    FileTransfer::CancelTransfer(t.transferId);
                }
                DrawWrappedTooltip("Cancel Transfer");
                ImGui::PopID();
            }
            ImGui::End();
            ImGui::PopStyleColor(4);
        }

        ImGui::End();
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
        pSwapChain->Present(1, 0); 
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
            ::DestroyWindow(hwnd); 
            ::UnregisterClass("KAMFlowUIClass", GetModuleHandle(nullptr)); 
            hwnd = nullptr; 
        }
    }

    /**
     * @brief Checks if the UI window is still active and hasn't received a close request.
     * @return true if the UI should continue running.
     */
    bool IsRunning() { return isAppRunning; }
}