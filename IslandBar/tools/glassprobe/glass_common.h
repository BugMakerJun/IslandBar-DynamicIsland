// =====================================================================
//  glass_common.h -- the two window-material calls both probe windows need
//
//  Kept in one place because the two windows ask the SAME question with a different window kind, and
//  the answer is only comparable if the material is applied by the same code: exactly the same accent
//  state, tint and corner preference, and the same return values reported.
//
//  The composition attribute setter is not declared in any SDK header; the structure is the one every
//  acrylic sample in the world uses, and the fields are documented only by those samples.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================

#pragma once

#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace glassprobe {

enum AccentState {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};

struct AccentPolicy {
    AccentState state;
    int flags;
    unsigned int colour;   // ABGR, and only the alpha matters to acrylic
    int animationId;
};

struct WindowCompositionAttributeData {
    int attribute;
    void* data;
    size_t size;
};

inline constexpr int kAccentPolicy = 19;
inline constexpr int kCornerPreference = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
inline constexpr int kCornerRound = 2;         // DWMWCP_ROUND

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

// Returns S_OK only when the call itself said it worked. The value is REPORTED, never trusted: this
// project has been burned by calls that returned success and changed nothing.
inline HRESULT SetAcrylic(HWND hwnd, unsigned int tint) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setAttribute = reinterpret_cast<SetWindowCompositionAttributeFn>(
        user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowCompositionAttribute"))
               : nullptr);
    if (!setAttribute) return E_NOTIMPL;

    AccentPolicy policy = {};
    policy.state = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    policy.colour = tint;
    WindowCompositionAttributeData data = {kAccentPolicy, &policy, sizeof(policy)};
    return setAttribute(hwnd, &data) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

inline HRESULT SetRoundCorners(HWND hwnd) {
    return DwmSetWindowAttribute(hwnd, kCornerPreference, &kCornerRound, sizeof(kCornerRound));
}

}  // namespace glassprobe
