// =====================================================================
//  UiWake.h -- coalesced notifications to the bar's existing UI message pump.
//
//  Background workers own their data and publish snapshots. They never touch
//  UI state directly; they set a bit here and post one ordinary Win32 message.
//  Different sources sharing a bit collapse naturally, while the message keeps
//  the existing MsgWaitForMultipleObjectsEx path reactive without a second
//  message loop.
// =====================================================================
#pragma once

#include <windows.h>

#include <atomic>

namespace island {

inline constexpr UINT kUiWakeMessage = WM_APP + 0x1A0;

inline constexpr unsigned kUiWakeToast = 1u << 0;
inline constexpr unsigned kUiWakeTray = 1u << 1;
inline constexpr unsigned kUiWakeStatus = 1u << 2;

inline std::atomic<unsigned> g_uiWakeBits{0};

inline void PostUiWake(HWND window, unsigned bits) {
    if (!window || bits == 0) return;
    const unsigned previous = g_uiWakeBits.fetch_or(bits, std::memory_order_relaxed);
    if ((previous & bits) == bits) return;
    PostMessageW(window, kUiWakeMessage, static_cast<WPARAM>(bits), 0);
}

inline unsigned TakeUiWakeBits() {
    return g_uiWakeBits.exchange(0, std::memory_order_acq_rel);
}

} // namespace island
