cmake_minimum_required(VERSION 3.15)

project(KAM-Flow VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# --- NEW: Export Compile Commands for VS Code IntelliSense ---
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- STATIC RUNTIME LINKING ---
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- DEAR IMGUI FETCH & INTEGRATION ---
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4
)
FetchContent_MakeAvailable(imgui)

# --- LIBOPUS FETCH & STATIC INTEGRATION (BSD-3-Clause) ---
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(opus)

set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

set(SOURCES 
    main.cpp
    MouseCapture.cpp
    KeyboardCapture.cpp
    StateManager.cpp
    NetworkServer.cpp
    NetworkClient.cpp
    ConfigManager.cpp
    UIManager.cpp
    DebugLog.cpp
    SecurityManager_Win.cpp
    CredentialManager_Win.cpp
    AudioCodec.cpp
    AudioManager_Win.cpp
    NetworkMessages.h
    ClipboardManager_Win.cpp
    FileTransferManager.cpp
    app.manifest
    KAM-Flow.rc
    ${IMGUI_SOURCES}
)

# Added WIN32 to define this as a GUI Application (No Console by default)
add_executable(KAM-Flow WIN32 ${SOURCES})

# Force the linker to use standard main() instead of WinMain
set_target_properties(KAM-Flow PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup /MANIFESTUAC:NO")

# Include ImGui headers explicitly so the compiler and VS Code find them
target_include_directories(KAM-Flow PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

# Link Windows subsystems, multimedia timers, DirectX 11, and static libopus
target_link_libraries(KAM-Flow PRIVATE user32 ws2_32 d3d11 d3dcompiler comdlg32 bcrypt advapi32 ole32 shell32 winmm opus)

# Non-elevated console unit test executable for CTest
enable_testing()
add_executable(KAMFlowTests
    tests/KAMFlowTests.cpp
    AudioCodec.cpp
    DebugLog.cpp
)
target_include_directories(KAMFlowTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KAMFlowTests PRIVATE opus)
add_test(NAME KAMFlowTests COMMAND KAMFlowTests)