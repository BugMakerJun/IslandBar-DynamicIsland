// =====================================================================
//  TrayPress.h
//  Pressing one of the tray icons for the user, from the bar's tray card.
//
//  WHAT A PRESS IS NOW (2026-10-15): the application's OWN callback message, delivered to the application's
//  OWN tray window. Nothing is clicked, nothing is injected, and the user's pointer is never moved -- which is
//  the whole requirement, in the user's words: "把真实点击这个去掉不要 … 我就想要 MyDockFinder 一样的效果".
//
//  THIS FILE USED TO MOVE THE POINTER, and the history is worth exactly one paragraph because it is why the
//  route that replaced it can be trusted. Earlier rounds established that a real click on the shell's tray
//  icon is the only thing the shell forwards to an application, and that a UI Automation Invoke on the same
//  button is accepted and then ignored by the application (measured three times; the log line that would have
//  said so was itself broken for one round, which is a lesson this project has now learned twice). So the
//  press was a real click: pointer moved onto the shell's icon, button down and up, pointer restored.
//
//  What ended it: with the module of src/trayhook inside the application, watching what the SHELL itself sends
//  when a tray icon is really pressed, the answer turned out to be two SENT messages -- `WM_LBUTTONDOWN` then
//  `WM_LBUTTONUP` with the icon's uid in wParam (HANDOFF 8.109). Reproducing that pair, synchronously,
//  restores and activates the application's window with the pointer untouched. The same values POSTED had
//  produced nothing at all, in eight shapes, which is why this took as long as it did.
//
//  WHAT A PRESS NEEDS: the application's callback message number, the window to send it to, and the uid --
//  learned automatically by being inside the application when it registers (see shell/TrayLearn.h, and the
//  cache file it keeps). An application that has never been learned has no entry, and a press reports that
//  instead of doing something to the user's pointer: `NoEntry`. The card answers such a cell with its OWN
//  menu, which is where the "open it for me" actions live.
//
//  WHAT IS GONE BY DEFAULT, and may not come back without a measurement that says the messages cannot work:
//
//    * the real click: `SetCursorPos`, `SendInput`, the shell's overflow panel being made hit-testable for a
//      moment, and the alpha-1 reveal that went with it;
//    * the automatic UI Automation fallback: it can press otherwise unreachable icons, but it has to reveal
//      the native taskbar for the length of the lookup. That flash is worse than the missing action for the
//      default experience, so it is now an explicit opt-in (`ISLANDBAR_TRAY_UIA=1`);
//    * `ISLANDBAR_TRAY_PRESS_VISIBLE` and `ISLANDBAR_TRAY_NO_CLICK`, the two switches that existed to look at
//      the old route.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>

namespace island {

// One press request: what the registry said about the icon, copied out of the list that produced it.
//
// A COPY, and that is not tidiness: the press runs on a worker thread while the tray list refreshes
// underneath it every five seconds, and the list's own storage (the PNG bytes, the strings) does not
// survive that.
struct TrayPressRequest {
    unsigned uid = 0;
    bool hasUid = false;
    bool hasGuid = false;
    GUID guid = {};
    wchar_t exePath[260] = {};
    wchar_t name[96] = {};
};

enum class TrayPressResult {
    Pressed,    // the application's own callback was delivered and it answered
    NoEntry,    // nothing is known about this icon: it has never been learned (the card offers its own menu)
    NoWindow,   // an entry exists but the application has no such window right now (not running, or not yet
                // registered)
    Refused,    // the message could not be delivered at all
};

struct TrayPressOutcome {
    TrayPressResult result = TrayPressResult::Refused;
    // WHICH RECIPE RAN, for the log: "its own callback" (the application answered), and the spelling used --
    // see TrayPress.cpp for the three recipes and why the packing is part of them.
    const wchar_t* route = L"?";
    // How long the application's window procedure held the message, in milliseconds: the one number that
    // separates "the system dropped it" (microseconds) from "the application read it and did nothing" (tens of
    // milliseconds). Zero for a shape that is POSTED, which is delivered without waiting -- worth knowing too,
    // so that a zero is not read as a failure.
    long long busyMs = 0;
    wchar_t target[192] = {};   // the window it went to, described: "class 'X' of pid N"
    wchar_t answer[192] = {};   // what the application was seen to do, in words
    bool appAnswered = false;
    long long ms = 0;           // the whole press
};

// Presses the icon `request` names. Safe to call from a worker thread that has called
// CoInitializeEx(COINIT_MULTITHREADED); needs no UI thread of its own.
//
// `rightClick` asks for the application's own menu instead of its default action, which is what a right click
// means in a tray. The RIGHT press is posted; a LEFT press is the pair of sent mouse messages described at
// the top of this file. Both are measured rather than chosen (HANDOFF 8.104, 8.109).
TrayPressOutcome PressTrayIcon(const TrayPressRequest& request, bool rightClick);

// Whether a press of this icon has anything to send: the application's own callback is known AND the window
// it registered exists right now. Asked without pressing anything, which is what the command line's coverage
// report is built on -- and what the card reads when it decides whether a cell is "pressable" or opens the
// bar's own menu instead.
bool TrayPressReachable(const TrayPressRequest& request);

// Whether this icon's own menu has been shown to be out of reach: both shapes a right press may send (the
// shell's pair, then the context-menu notification) have been tried and the application answered neither. The
// press itself then stops asking and the card's menu says so in words -- see TrayMenuLabel.
bool TrayRightClickExhausted(const TrayPressRequest& request);

const wchar_t* TrayPressResultName(TrayPressResult result);

// The one decision that keeps an unlearned icon from flashing the native taskbar by default. Pure, so the
// right/left and enabled/disabled combinations are asserted without touching the machine.
inline bool ShouldTryTrayUiaFallback(TrayPressResult result, bool rightClick, bool enabled) {
    return enabled && !rightClick && result == TrayPressResult::NoEntry;
}

} // namespace island
