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
#include <tlhelp32.h>

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

    // === SINGLE INSTANCE ENFORCEMENT ===
    // Always check, regardless of debug mode. The previous code skipped this when
    // globalDebugMode was true, allowing phantom processes from debug-mode crashes
    // to coexist with new instances.
    HANDLE hMutex = CreateMutexA(NULL, FALSE, "KAMFlow_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        
        // Check if the existing instance is a healthy process with a visible window.
        HWND existingWindow = FindWindowA("KAMFlowUIClass", nullptr);
        
        if (existingWindow) {
            // Healthy instance found — bring it to the foreground and exit.
            ::ShowWindow(existingWindow, SW_SHOW);
            ::ShowWindow(existingWindow, SW_RESTORE);
            ::SetForegroundWindow(existingWindow);
            ::MessageBoxA(NULL, 
                "KAM-Flow is already running.\n\n"
                "The existing instance has been brought to the foreground.\n"
                "Check your System Tray if you don't see it.",
                "KAM-Flow", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        
        // No visible window — this is a phantom process from a previous crash.
        // Step 1: Terminate the phantom process.
        DWORD currentPid = GetCurrentProcessId();
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32 pe32;
            pe32.dwSize = sizeof(pe32);
            if (Process32First(hSnap, &pe32)) {
                do {
                    if (pe32.th32ProcessID != currentPid && 
                        _stricmp(pe32.szExeFile, "KAM-Flow.exe") == 0) {
                        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                        if (hProc) {
                            TerminateProcess(hProc, 1);
                            CloseHandle(hProc);
                        }
                    }
                } while (Process32Next(hSnap, &pe32));
            }
            CloseHandle(hSnap);
        }
        
        // Step 2: Wait for the mutex to be released by the terminated phantom.
        Sleep(500);
        
        // Step 3: Verify the phantom is actually dead.
        bool phantomStillAlive = false;
        hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32 pe32;
            pe32.dwSize = sizeof(pe32);
            if (Process32First(hSnap, &pe32)) {
                do {
                    if (pe32.th32ProcessID != currentPid && 
                        _stricmp(pe32.szExeFile, "KAM-Flow.exe") == 0) {
                        phantomStillAlive = true;
                        break;
                    }
                } while (Process32Next(hSnap, &pe32));
            }
            CloseHandle(hSnap);
        }
        
        if (phantomStillAlive) {
            // Phantom survived TerminateProcess (elevated/protected process?).
            // Kill ALL instances including ourselves and let the user restart.
            ::MessageBoxA(NULL, 
                "A phantom KAM-Flow process from a previous crash could not be terminated.\n\n"
                "All KAM-Flow instances will now be closed.\n"
                "Please relaunch the application.",
                "KAM-Flow", MB_OK | MB_ICONERROR);
            
            // Force-terminate everything
            hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (hSnap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32 pe32;
                pe32.dwSize = sizeof(pe32);
                if (Process32First(hSnap, &pe32)) {
                    do {
                        if (_stricmp(pe32.szExeFile, "KAM-Flow.exe") == 0) {
                            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                            if (hProc) {
                                TerminateProcess(hProc, 1);
                                CloseHandle(hProc);
                            }
                        }
                    } while (Process32Next(hSnap, &pe32));
                }
                CloseHandle(hSnap);
            }
            return 1;
        }
        
        // Step 4: Phantom terminated successfully. Re-acquire the mutex and continue startup.
        hMutex = CreateMutexA(NULL, FALSE, "KAMFlow_SingleInstance_Mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            // Mutex still held (race condition or OS delay). Bail out.
            if (hMutex) CloseHandle(hMutex);
            ::MessageBoxA(NULL, 
                "Failed to acquire the single-instance lock after terminating the phantom process.\n\n"
                "Please wait a moment and try again.",
                "KAM-Flow", MB_OK | MB_ICONWARNING);
            return 1;
        }
        // Success! Continue startup with the fresh mutex.
    }

    // Initialize Core Audio Subsystem
    Audio::Initialize();
    ClipboardManager::Initialize();
    FileTransfer::Initialize();

    if (State::globalDebugMode) {
        UI::LogDebug("KAM-Flow Engine Booting...");
    }
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-client") { 
            State::currentRole = State::AppRole::CLIENT; 
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
                    Network::StartServer(8080);
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
        Config::SaveConfig("127.0.0.1", 8080);
        
        UI::Shutdown();
    }

    // --- GRACEFUL TEARDOWN ---
    if (subsystemsStarted) {
        ClipboardManager::Stop();
        if (State::currentRole == State::AppRole::SERVER) {
            // Stop audio threads BEFORE network shutdown to prevent
            // audio threads from calling BroadcastMessage() on destroyed sockets.
            Audio::StopAudioRenderer();
            Audio::StopMicBroadcast();

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