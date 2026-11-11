// =====================================================================
//  TrayUia.h -- PRESSING A TRAY ICON BY ASKING THE SHELL TO PRESS IT
//
//  WHAT PROBLEM THIS SOLVES. Most cells are pressed by sending the application its own callback message, which
//  is what TrayPress does and it costs nothing visible. That works for every application the module can learn,
//  and there is one class of icon where it can never work: NOTHING EVER REGISTERS IT. NVIDIA's icons were
//  registered before this bar started listening and never re-register, the shell's own network/volume/clock
//  buttons are not applications at all, and AppContainer and elevated applications cannot be injected into. For
//  those, "what does the real tray do" is the only honest answer -- so this asks the SHELL to do it.
//
//  HOW. Windows 11's tray is `SystemTray_Main`, a XAML island whose icons are UI Automation elements: every one
//  of them exposes InvokePattern, and Invoke() IS the shell performing that icon's own left-click. Measured on
//  this machine 2026-10-15: invoking ChatGPT's element brought its window to the front in 17 ms with the pointer
//  untouched, and icons that have no callback we could ever learn (NVIDIA, the security centre) are in the same
//  tree. It works for AppContainer and elevated applications for the same reason: the shell is the one pressing.
//
//  WHAT IT COSTS, AND WHY IT IS NOT THE DEFAULT PATH. UIA only sees the tray when the native taskbar is actually
//  ALIVE -- measured: with the bar hiding it, `Shell_TrayWnd` is not in the tree at all. So a fallback press has
//  to make the shell's taskbar exist for the length of one press (a few hundred milliseconds), which is visible.
//  That is a real cost on the user's screen, which is why learned cells never pay it and why the whole path is
//  now opt-in: set ISLANDBAR_TRAY_UIA=1 to enable it. By default every cell keeps opening the bar's own menu
//  without showing the native taskbar.
//
//  RIGHT CLICKS ARE NOT SUPPORTED. UI Automation publishes no context-menu pattern for these elements, and the
//  measurement agrees: this route is for the left button only. A right press on an unlearned cell keeps opening
//  the bar's own menu.
// =====================================================================
#pragma once

#include <windows.h>

namespace island {

struct TrayUiaOutcome {
    bool pressed = false;          // the shell was asked to press the icon, and answered
    bool foundIcon = false;        // the icon was seen in the shell's tree at all
    bool openedOverflow = false;   // the chevron had to be invoked first
    bool refused = false;          // the tray could not be made visible, or UIA could not be reached
    wchar_t detail[192] = {};      // one line for the log, in the caller's words
};

// Presses the icon the shell calls `iconName` -- the tooltip, which is what the shell's own element is named.
//
// `secondName` is a second candidate, tried when the first finds nothing, and it exists because the two names
// the bar has for a cell are usually the same string and are not always: a GUID-registered icon (the security
// centre, Explorer's network and volume buttons) has an EMPTY tooltip in the shell's registry record, so the
// card falls back to the executable's name -- "SecurityHealthSystray.exe" -- while the shell's element is named
// "Windows 安全中心 - 建议的操作。". The shell told us that name itself, in the registration it sent when the
// icon appeared, and the report file still holds it (see TrayPress.cpp). Pass null when there is no second
// candidate; matching is by containment in both directions, so an empty string matches nothing.
//
// Blocking, on a worker thread: it makes the shell's taskbar exist, walks the tree, invokes, and puts the
// taskbar back. Returns what happened, in enough detail for the caller to write a log line that says which
// route was taken.
TrayUiaOutcome PressTrayIconWithUia(const wchar_t* iconName, const wchar_t* secondName);

} // namespace island
