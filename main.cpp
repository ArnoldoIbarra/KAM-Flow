// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Main Application Entry Point
// =============================================================================

/**
 * @file main.cpp
 * @brief The entry point for the KAM-Flow Engine.
 * Uses Deferred Initialization: UI starts first, engine threads follow user selection.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

#include "MouseCapture.h"
#include "KeyboardCapture.h"
#include "NetworkServer.h"
#include "NetworkClient.h"
#include "StateManager.h"
#include "ConfigManager.h"
#include "UIManager.h"
#include "AudioManager.h"
#include "ClipboardManager.h"
#include "FileTransferManager.h"

/// Stores the Thread ID of the KVM Hook loop so we can post WM_QUIT safely.
std::atomic<DWORD> g_HookThreadId(0);

#define WM_TOGGLE_GAMEMODE (WM_APP + 1)

/**
 * @brief Background thread dedicated solely to processing low-level KVM hooks.
 * Isolating this prevents DirectX VSync from causing input latency.
 * @return void
 */
void KVMHookThreadLoop() {
    g_HookThreadId = GetCurrentThreadId();
    MSG msg;
    
    // Elevate the KVM hook thread to prevent starvation from OS scheduler during heavy/full-screen workloads
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    
    if (!State::enableGameMode) {
        if (!Input::StartMouseCapture()) {
            UI::LogDebug("[Error] Failed to install Mouse Hook!");
        }
    }
    if (!Input::StartKeyboardCapture()) {
        UI::LogDebug("[Error] Failed to install Keyboard Hook!");
        return;
    }
    
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TOGGLE_GAMEMODE) {
            if (msg.wParam == 1) {
                Input::StopMouseCapture();
            } else {
                Input::StartMouseCapture();
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    Input::StopMouseCapture();
    Input::StopKeyboardCapture();
}

/**
 * @brief Main entry point for the application.
 * @param argc The number of command-line arguments.
 * @param argv The array of command-line argument strings.
 * @return int Status code (0 for success).
 */
int main(int argc, char* argv[]) {
    // Load persisted settings from kamflow.ini (This naturally manages initial console state)
    Config::LoadConfig();

    HANDLE hMutex = NULL;
    if (!State::globalDebugMode) {
        hMutex = CreateMutexA(NULL, FALSE, "KAMFlow_SingleInstance_Mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (hMutex) CloseHandle(hMutex);
            return 0; // Silently exit to prevent multiple instances
        }
    }

    // Initialize Core Audio Subsystem
    Audio::Initialize();
    ClipboardManager::Initialize();
    FileTransfer::Initialize();

    if (State::globalDebugMode) {
        UI::LogDebug("KAM-Flow Engine Booting...");
    }
    
    char savedIp[256];
    const std::string& iniPath = Config::GetResolvedPath();
    GetPrivateProfileStringA("Network", "TargetIP", "127.0.0.1", savedIp, sizeof(savedIp), iniPath.c_str());
    std::string targetIp = savedIp;
    uint16_t port = (uint16_t)GetPrivateProfileIntA("Network", "Port", 8080, iniPath.c_str());

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-client") { 
            State::currentRole = State::AppRole::CLIENT; 
            if (i + 1 < argc) targetIp = argv[i + 1]; 
        }
        if (arg == "-server") { 
            State::currentRole = State::AppRole::SERVER; 
        }
    }

    // --- APPLY DEFAULT ROLE IF NO CLI OVERRIDE ---
    if (State::currentRole == State::AppRole::NONE && State::defaultRole != State::AppRole::NONE) {
        State::currentRole = State::defaultRole;
    }

    std::thread hookThread;
    bool subsystemsStarted = false;

    // --- MAIN EXECUTION LOOP ---
    if (UI::Initialize()) {
        while (UI::IsRunning()) {
            UI::RenderFrame();

            // Deferred Ignition: Start threads immediately once the UI sets the role
            if (!subsystemsStarted && State::currentRole != State::AppRole::NONE) {
                if (State::currentRole == State::AppRole::CLIENT) {
                    if (State::globalDebugMode) UI::LogDebug("[Role] CLIENT MODE locked in. Starting UDP Discovery.");
                    Network::StartDiscoveryListener();
                    ClipboardManager::Start();
                } else if (State::currentRole == State::AppRole::SERVER) {
                    if (State::globalDebugMode) UI::LogDebug("[Role] SERVER MODE locked in.");
                    Network::StartServer(port);
                    // If audio mixing is enabled in the config, start the renderer now.
                    if (State::enableServerAudioMix) {
                        Audio::StartAudioRenderer();
                    }
                    if (State::enableServerMicBroadcast) {
                        Audio::StartMicBroadcast();
                    }
                    ClipboardManager::Start();
                    hookThread = std::thread(KVMHookThreadLoop);
                    while (g_HookThreadId.load() == 0) { std::this_thread::yield(); }
                }
                subsystemsStarted = true;
            }
        }
        
        // --- GRACEFUL DATA PERSISTENCE ---
        // Save the precise state of all checkboxes right before the window collapses
        Config::SaveConfig(targetIp, port);
        
        UI::Shutdown();
    }

    // --- GRACEFUL TEARDOWN ---
    if (subsystemsStarted) {
        ClipboardManager::Stop();
        if (State::currentRole == State::AppRole::SERVER) {
            if (g_HookThreadId.load() != 0) {
                PostThreadMessage(g_HookThreadId.load(), WM_QUIT, 0, 0);
                if (hookThread.joinable()) hookThread.join();
            }
            Network::StopServer();
        } else if (State::currentRole == State::AppRole::CLIENT) {
            Network::StopClient(); 
            Network::StopDiscoveryListener();
        }
    }

    if (State::globalDebugMode) {
        UI::LogDebug("[KAM-Flow Engine] Terminated cleanly.");
    }

    Audio::Shutdown();
    ClipboardManager::Shutdown();
    FileTransfer::Shutdown();
    
    if (hMutex) {
        CloseHandle(hMutex);
    }
    return 0;
}