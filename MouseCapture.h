// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/// @file MouseCapture.h
/// Manages global mouse interception and edge detection.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>

/// Namespace handling global input interception and processing.
namespace Input {

    /// Action selected by WH_MOUSE_LL based on remote/transition state.
    enum class MouseHookAction : uint8_t {
        PassThrough,
        SwallowOnly,
        ForwardRemote
    };

    /// Outcome of evaluating a raw remote WM_MOUSEMOVE coordinate against cursor and anchor positions.
    struct RemoteDeltaResult {
        int dx;
        int dy;
        bool discardTeleport;
        bool needsReanchor;
        bool fromDrift;
    };

    /// Resolves whether WH_MOUSE_LL should pass through, swallow during an in-flight transition, or forward remotely.
    inline constexpr MouseHookAction ResolveMouseHookAction(bool isRemote, bool transitionPending) {
        if (transitionPending) {
            return MouseHookAction::SwallowOnly;
        }
        if (isRemote) {
            return MouseHookAction::ForwardRemote;
        }
        return MouseHookAction::PassThrough;
    }

    /// Returns true only when it is safe to clear isTransitionedOut without disrupting an in-flight transition.
    inline constexpr bool ShouldClearTransitionedOut(bool isRemote, bool transitionPending, bool isTransitionedOut) {
        return !isRemote && !transitionPending && isTransitionedOut;
    }

    /// Evaluates relative delta from live cursor coordinates and flags teleport discards or center re-anchoring.
    inline constexpr RemoteDeltaResult EvaluateRemoteMouseDelta(
        int eventX, int eventY,
        int cursorX, int cursorY,
        int anchorX, int anchorY,
        int driftThresholdPx = 50,
        int flickReanchorPx = 150,
        int teleportDiscardPx = 1500) {
        auto abs64 = [](int64_t v) constexpr { return v < 0 ? -v : v; };
        const int64_t rawDx = static_cast<int64_t>(eventX) - static_cast<int64_t>(cursorX);
        const int64_t rawDy = static_cast<int64_t>(eventY) - static_cast<int64_t>(cursorY);
        const bool discardTeleport = (abs64(rawDx) > teleportDiscardPx || abs64(rawDy) > teleportDiscardPx);
        const bool fromDrift = (abs64(static_cast<int64_t>(cursorX) - static_cast<int64_t>(anchorX)) > driftThresholdPx ||
                                abs64(static_cast<int64_t>(cursorY) - static_cast<int64_t>(anchorY)) > driftThresholdPx);
        const bool fromFlick = !discardTeleport && (abs64(rawDx) > flickReanchorPx || abs64(rawDy) > flickReanchorPx);
        const bool needsReanchor = discardTeleport || fromDrift || fromFlick;
        return { static_cast<int>(rawDx), static_cast<int>(rawDy), discardTeleport, needsReanchor, fromDrift };
    }

    /// Maps a virtual-desktop coordinate to rounded, clamped Win32 SendInput absolute coordinates [0, 65535].
    inline constexpr int ComputeAbsoluteVirtualCoord(int coord, int vOrigin, int vSize) {
        if (vSize <= 1) {
            return 0;
        }
        const int64_t rel = static_cast<int64_t>(coord) - static_cast<int64_t>(vOrigin);
        const int64_t denom = static_cast<int64_t>(vSize - 1);
        const int64_t scaled = (rel * 65535LL + (denom / 2)) / denom;
        return static_cast<int>(std::clamp<int64_t>(scaled, 0LL, 65535LL));
    }
    
    /// Installs the low-level global mouse hook (WH_MOUSE_LL).
    bool StartMouseCapture();

    /// Removes the low-level mouse hook safely and cleans up resources.
    void StopMouseCapture();

    /// Teleports the Server cursor to the physical edge based on 4-way coordinate tracking.
    void HandleReturnControl(uint8_t entryEdge, float normalizedX, float normalizedY);

    /// Discards all pending mouse events in the queue. Called on sleep/wake resume to prevent stale burst playback.
    void DrainMouseQueue();
}