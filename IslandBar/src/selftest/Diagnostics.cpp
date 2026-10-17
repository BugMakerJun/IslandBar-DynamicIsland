// =====================================================================
//  Diagnostics.cpp -- the two entry points: --self-test and --probe-taskbar
//
//  IT USED TO BE THE WHOLE SELF-TEST, 8,968 lines of it. The checks now live one file per subject
//  (selftest/Checks*.cpp, declared in selftest/Checks.h) and the printing lives in
//  selftest/Harness.cpp; what is left here is what an operator actually invokes, plus the
//  environment each entry point sets up. Nothing about the checks changed -- they moved.
// =====================================================================
// =====================================================================
//  Diagnostics.cpp
//  The runtime self-test and the native-taskbar probe.
//
//  Every assertion below is deliberately phrased as a question about what the
//  system actually did, not about what was requested. The interesting failures
//  in this area of Windows are the silent ones -- a per-monitor-v2 awareness
//  that never took effect, a z-order that quietly dropped out of topmost, a
//  swap chain that presented black outside the pill -- and none of them raise
//  an HRESULT.
//
//  The probe's report goes to the log file as well as to stdout, because
//  stdout has been lost on this machine three separate times.
// =====================================================================
#include "selftest/Diagnostics.h"

#include <windows.h>
#include <shellapi.h>   // APPBARDATA / ABM_* for the native-taskbar probe

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>   // strtoul: unpacking the hex of a captured registration in the tray checks
#include <vector>
#include <cwchar>
#include <string>

#include "domain/Geometry.h"
#include "core/Log.h"
#include "domain/MiniJson.h"
#include "domain/TrayRegistration.h"
#include "domain/TrayLedger.h"     // which registrations the shell is holding (see the tray checks)
#include "platform/TrayRing.h"     // the channel the module publishes registrations on
#include "domain/CardLayout.h"
#include "domain/StatePolicy.h"
#include "domain/TrayEncoding.h"   // the only place a tray click is encoded (see its header)
#include "media/Lyrics.h"
#include "app/AppBar.h"
#include "domain/TaskStripLayout.h"
#include "platform/WindowList.h"
#include "media/MusicSession.h"
#include "app/PlayerWindow.h"
#include "media/WicImage.h"
#include "services/Notification.h"
#include "services/NotificationIcon.h"
#include "services/ToastStore.h"
#include "services/WeChatWatch.h"
#include "shell/Weather.h"
#include "platform/ThumbnailPreview.h"
#include "app/IslandWindow.h"
#include "platform/Monitor.h"
#include "platform/NativeTaskbar.h"   // the three answers to "what happens to the shell's taskbar"
#include "shell/SystemToggles.h"
#include "render/Theme.h"
#include "render/GlassMaterial.h"
#include "shell/Weather.h"   // SkyMarkForCode: the word and the mark come from the same WMO code
#include "domain/Motion.h"
#include "render/Renderer.h"
#include "render/TextRenderer.h"
#include "shell/TrayIcons.h"
#include "shell/TrayPress.h"
#include "shell/TrayLearn.h"   // CountCoveredIcons: how much of the tray a press can reach (see the tray checks)

#include "selftest/Checks.h"
#include "selftest/Harness.h"

namespace island {

int RunSelfTest() {
    ResetFailures();

    LogAndPrint("IslandBar runtime self-test\n");
    LogAndPrint("-----------------------------------------------\n");

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    Check(comOk, L"CoInitializeEx");

    const MonitorGeometry monitor = QueryMonitor(nullptr);
    Section(L"monitor metrics");

    // THE TASKBAR'S MODES (2026-10-15): the object has three answers -- clip the taskbar to the tray and
    // keep the real tray on screen (the default), hide it, or leave it alone -- and the frame loop asks
    // two of them every tick. The WINDOW work is not asserted here (it needs the real shell and it is
    // what `--probe-taskbar` is for); what is asserted is that the object starts in hiding mode and
    // remembers what it is told, because a flag that silently fails to take would look like "the tray
    // mode does not work" rather than like "the flag was lost".
    {
        island::NativeTaskbar taskbar;          // never located: only the flags are exercised
        Check(!taskbar.KeepTrayVisible() && !taskbar.Suppressed(),
              L"the taskbar object starts in hiding mode");
        taskbar.SetKeepTrayVisible(true);
        Check(taskbar.KeepTrayVisible(), L"... and takes the tray mode when it is asked for");
        taskbar.SetSuppressed(true);
        Check(taskbar.Suppressed(), L"... and the suppression that a full-screen app asks for");
        taskbar.SetSuppressed(false);
        Check(!taskbar.Suppressed(), L"... and lets the suppression go again");
    }

    // THE ONE PLACE THAT ENCODES A TRAY CLICK (2026-10-16). The version-4 numbers are the bytes the shell
    // ITSELF was caught sending to a version-4 application on a real right click (HANDOFF 8.142: pointer
    // 2394,69 -> wParam 0x0045095A; uid 1000 -> HIWORD(lParam) 0x03E8). They are asserted rather than
    // described because getting this backwards crashed WeChat twice and got read as a "modal menu loop".
    Section(L"tray click encoding");
    {
        const unsigned version4 = 4;
        const int x = 2394;
        const int y = 69;
        const unsigned uid = 1000;

        const island::TrayClickPlan right = island::PlanTrayClick(version4, uid, true, x, y);
        Check(right.version4, L"a uVersion 4 record takes the version-4 protocol");
        Check(right.first.wParam == 0x0045095Au, L"v4 right click: wParam is the pointer position");
        Check(right.first.lParam == 0x03E8007Bu, L"v4 right click: lParam is (WM_CONTEXTMENU, uid)");
        Check(island::TrayLow(right.first.wParam) == 2394 && island::TrayHigh(right.first.wParam) == 69,
              L"v4: the receiver can read x and y back out of wParam");
        Check(island::TrayLow(right.first.lParam) == island::kTrayMouseContextMenu &&
                  island::TrayHigh(right.first.lParam) == uid,
              L"v4: the receiver can read the event and the uid back out of lParam");
        Check(!right.hasSecond, L"a v4 click is ONE message, not a down/up pair");

        const island::TrayClickPlan select = island::PlanTrayClick(version4, uid, false, x, y);
        Check(island::TrayLow(select.first.lParam) == island::kTraySelect,
              L"v4 left click asks for NIN_SELECT");

        const island::TrayClickPlan classicRight = island::PlanTrayClick(0, uid, true, x, y);
        Check(!classicRight.version4 && classicRight.first.wParam == uid,
              L"v0 right click: the uid goes in wParam");
        // THE PAIR, and in this order: down, then up (2026-10-16, HANDOFF 8.142 -- the shell itself was
        // caught sending exactly this to an application on a real right click, and the probe that brought up
        // ChatGPT's own menu sends the same pair). It is asserted because a single POSTED message stood here
        // for a round and was never what the shell sends.
        Check(classicRight.first.lParam == island::kTrayMouseRightDown && classicRight.hasSecond &&
                  classicRight.second.lParam == island::kTrayMouseRightUp,
              L"v0 right click: down and then up, both in lParam");
        Check(classicRight.sent, L"v0 right click: the pair is SENT, not posted");

        // ... and the delivery is part of the shape: the version-4 context-menu notification is POSTED (what
        // clash-verge answered in 78 ms and WeChat in HANDOFF 8.104), while every other press is sent.
        Check(!island::PlanTrayClick(version4, uid, true, x, y).sent,
              L"v4 right click: the context-menu notification is POSTED");
        Check(island::PlanTrayClick(version4, uid, false, x, y).sent &&
                  island::PlanTrayClick(0, uid, false, x, y).sent,
              L"a left click is sent, in both protocols");

        const island::TrayClickPlan classicLeft = island::PlanTrayClick(3, uid, false, x, y);
        Check(classicLeft.first.lParam == island::kTrayMouseLeftDown &&
                  classicLeft.second.lParam == island::kTrayMouseLeftUp,
              L"v3 left click: the same pair, with the left button");

        // WHICH SHAPE A RIGHT PRESS SENDS, AND WHEN IT STOPS ASKING (2026-10-16, round 44). One shape per
        // press and at most two per icon, in a fixed order: the pair the shell sends, then the context-menu
        // notification, then this bar's own menu. The rule is asserted because the version that shipped
        // before it sent two shapes in a row, which is how a round was spent finding out which of them the
        // applications were ignoring.
        const auto fresh3 = island::PlanTrayRightShape(0, island::TrayRightNote::Untried);
        Check(fresh3 == island::TrayRightShape::DownUpPair,
              L"a v0 icon nobody has tried gets the shell's own pair first");
        Check(island::PlanTrayRightShape(0, island::TrayRightNote::PairFailed) ==
                  island::TrayRightShape::ContextMenu,
              L"after the pair did nothing, the NEXT press sends the context-menu notification");
        Check(island::PlanTrayRightShape(0, island::TrayRightNote::BothFailed) ==
                  island::TrayRightShape::None,
              L"after both did nothing, no press sends anything again");
        Check(island::PlanTrayRightShape(4, island::TrayRightNote::Untried) ==
                  island::TrayRightShape::ContextMenu &&
                  island::PlanTrayRightShape(4, island::TrayRightNote::PairFailed) ==
                      island::TrayRightShape::ContextMenu,
              L"a v4 icon is never sent the pair, whatever has been tried");
        Check(island::PlanTrayRightShape(0, island::TrayRightNote::ContextMenuWorks) ==
                  island::TrayRightShape::ContextMenu &&
                  island::PlanTrayRightShape(0, island::TrayRightNote::PairWorks) ==
                      island::TrayRightShape::DownUpPair,
              L"the shape that worked is the shape the next press sends");
        Check(island::NextTrayRightNote(island::TrayRightNote::Untried, island::TrayRightShape::DownUpPair,
                                        false) == island::TrayRightNote::PairFailed &&
                  island::NextTrayRightNote(island::TrayRightNote::PairFailed,
                                            island::TrayRightShape::ContextMenu, false) ==
                      island::TrayRightNote::BothFailed &&
                  island::NextTrayRightNote(island::TrayRightNote::Untried,
                                            island::TrayRightShape::DownUpPair, true) ==
                      island::TrayRightNote::PairWorks,
              L"one failure moves to the second shape, the second failure ends it, an answer is remembered");

        Check(island::PlanTrayClick(version4, uid, true, x, y).first.wParam !=
                  island::PlanTrayClick(0, uid, true, x, y).first.wParam,
              L"the two protocols must never produce the same first message");
    }

    // THE RULE THAT DECIDES WHICH CELLS EXIST (2026-10-16, round 45).
    //
    // The registry keeps an entry for every icon an application has EVER registered, and the card was built
    // from that plus "is the executable running" -- thirteen cells against the nine the shell was actually
    // holding, the surplus being three NVIDIA entries (its container process runs all day and registered
    // nothing that session) and one dead explorer GUID whose stored picture is a blank 210-byte PNG. The
    // replacement is this ledger, and its behaviour is asserted here with no shell, no tray and no
    // application in the room: what an ADD, a MODIFY, a DELETE, a dead window and a dead process each do,
    // and why a uid on its own is not an identity (ChatGPT and RazerAppEngine both register uid 3 on this
    // machine, which is exactly the collision a uid-only match would have got wrong).
    Section(L"tray live ledger");
    {
        const unsigned char bluetooth[16] = {0x6D, 0xA6, 0x8F, 0x06};
        const unsigned char otherGuid[16] = {0x78, 0x20, 0xAE, 0x78};
        island::TrayLedger ledger;

        Check(ledger.Count() == 0 && ledger.Find(L"chatgpt.exe", 3, false, nullptr) == nullptr,
              L"nothing is held until something registers");

        ledger.Apply(island::kTrayCopyAdd, 100, 0x2001, 3, false, nullptr, L"ChatGPT", L"chatgpt.exe");
        const island::TrayLiveRegistration* live = ledger.Find(L"chatgpt.exe", 3, false, nullptr);
        Check(ledger.Count() == 1 && live != nullptr && std::wcscmp(live->tip, L"ChatGPT") == 0,
              L"an ADD is held, with the tip the application sent");
        Check(ledger.Find(L"razerappengine.exe", 3, false, nullptr) == nullptr,
              L"uid 3 belongs to the application that used it, not to any of them");

        ledger.Apply(island::kTrayCopyModify, 100, 0x2001, 3, false, nullptr, L"", L"chatgpt.exe");
        Check(ledger.Count() == 1, L"a MODIFY refreshes that registration instead of adding one");
        Check(std::wcscmp(ledger.At(0).tip, L"ChatGPT") == 0,
              L"... and a MODIFY carrying no tip does not erase the name");

        ledger.Apply(island::kTrayCopyAdd, 101, 0x2002, 3, false, nullptr, L"Razer", L"razerappengine.exe");
        Check(ledger.Count() == 2, L"the same uid from another application is a second registration");

        // A registration whose tray window was already gone when the bar resolved its owner arrives with no
        // executable at all (measured: the probe registering uid 7). The uid is then the only thing left, and
        // refusing to match it would drop an icon the shell is holding.
        ledger.Apply(island::kTrayCopyAdd, 103, 0x2004, 7, false, nullptr, L"ISLANDBAR-PROBE-4711", L"");
        Check(ledger.Find(L"probe-tray-registrar.exe", 7, false, nullptr) != nullptr,
              L"a registration with no executable name still matches by uid");
        ledger.ForgetWindow(0x2004);

        ledger.Apply(island::kTrayCopyAdd, 102, 0x2003, 0, true, bluetooth, L"蓝牙设备", L"explorer.exe");
        Check(ledger.Find(nullptr, 0, true, bluetooth) != nullptr,
              L"a guid identifies a registration on its own");
        Check(ledger.Find(nullptr, 0, true, otherGuid) == nullptr, L"... and another guid is not it");

        ledger.Apply(island::kTrayCopyDelete, 101, 0x2002, 3, false, nullptr, L"", L"");
        Check(ledger.Count() == 2 && ledger.Find(L"razerappengine.exe", 3, false, nullptr) == nullptr,
              L"a DELETE takes away exactly the registration it names");

        ledger.Apply(island::kTrayCopyDelete, 999, 0x9999, 7, false, nullptr, L"", L"");
        Check(ledger.Count() == 2, L"a DELETE for something nobody held changes nothing");

        ledger.ForgetWindow(0x2001);
        Check(ledger.Count() == 1 && ledger.Find(L"chatgpt.exe", 3, false, nullptr) == nullptr,
              L"a window that has gone takes its registration with it");
        ledger.ForgetProcess(102);
        Check(ledger.Count() == 0, L"a process that has gone takes every registration of its own");

        for (int i = 0; i < island::kTrayLedgerCapacity + 5; ++i) {
            ledger.Apply(island::kTrayCopyAdd, 200, static_cast<unsigned long long>(0x3000 + i),
                         static_cast<unsigned>(i), false, nullptr, L"", L"spammer.exe");
        }
        Check(ledger.Count() == island::kTrayLedgerCapacity && ledger.Refused() == 5,
              L"an application that re-registers in a loop is capped, and the refusals are counted");

        ledger.Reset();
        Check(ledger.Count() == 0 && ledger.Refused() == 0, L"a replaced shell starts from nothing");
    }

    {
        wchar_t metrics[192];
        std::swprintf(metrics, 192,
                      L"%ldx%ld screen, %ldx%ld work, %u dpi, inset %ld px",
                      ScreenWidth(monitor), ScreenHeight(monitor),
                      WorkWidth(monitor), WorkHeight(monitor),
                      monitor.dpi, TaskbarThickness(monitor));
        Info(L"monitor", metrics);
    }

    Check(TaskbarThickness(monitor) >= 0 && TaskbarThickness(monitor) < 200,
          L"work-area inset is plausible");
    Check(monitor.dpi >= 96 && monitor.dpi <= 480, L"dpi is plausible");
    Check(monitor.work.right <= monitor.screen.right &&
          monitor.work.bottom <= monitor.screen.bottom,
          L"work area lies inside the screen");

    // The band the bar draws in, read from the shell taskbar's own window. It is the number the work
    // area stops carrying once the strip is released, so it is reported next to it: on a desktop where
    // the two disagree, one of them is being read wrong.
    {
        wchar_t band[192];
        std::swprintf(band, 192,
                      L"top band from the taskbar window: %ld px (work-area inset: %ld px)",
                      ShellTopBandThicknessPx(monitor.monitor), TaskbarThickness(monitor));
        Info(L"band", band);
        Check(ShellTopBandThicknessPx(monitor.monitor) >= 0 &&
              ShellTopBandThicknessPx(monitor.monitor) < 200,
              L"the band read from the taskbar is plausible");
    }

    // Layout numbers as the window constructor will see them.
    const float originX = PixelsToDip(monitor.screen.left, monitor.dpi);
    const float originY = PixelsToDip(monitor.work.top - DipToPixels(kPillTopMarginDip, monitor.dpi),
                                      monitor.dpi);
    const float monitorWidthDip = PixelsToDip(ScreenWidth(monitor), monitor.dpi);
    const float workTopDip = PixelsToDip(monitor.work.top, monitor.dpi);

    TestGeometryPredicate(originX, originY, monitorWidthDip, workTopDip);
    TestCardLayout();
    TestIslandSurface();
    TestMusicCardLayout();
    TestHoverSequence();
    TestPointerStateCycle();
    TestLyrics();
    TestImageCodec();
    TestAccentColour();
    TestMiniJson();
    TestNotifications();
    TestMotion(originX, originY, monitorWidthDip);
    TestClock();
    TestWorkerThread();

    // Diagnostics has no direct access to the window's private clock; the
    // renderer's own check covers the compositor wait.
    {
        IslandWindow window;
        const bool created = window.Create(monitor, /*show=*/false);
        Check(created, L"IslandWindow::Create (hidden)");
        if (created) {
            TestWindow(window, monitor);
            TestRenderer(window);

            // Bring it up for real: the pass-through/click-through behaviour is
            // only meaningful on a visible window.
            ShowWindow(window.Handle(), SW_SHOWNOACTIVATE);
            SetWindowPos(window.Handle(), HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            Check(IsWindowVisible(window.Handle()) != FALSE, L"window became visible");
            Check((GetWindowLongPtrW(window.Handle(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
                  L"still topmost while visible");

            TestClickThrough(window, monitor);
            window.Destroy();
        }
    }

    CoUninitialize();

    LogAndPrint("\n-----------------------------------------------\n");
    if (Failures() == 0) {
        LogAndPrint("RESULT: PASS - the island behaves as specified\n");
    } else {
        LogAndPrint("RESULT: FAIL - %d check(s) failed\n", Failures());
    }
    return Failures();
}

int RunTaskbarProbe() {
    LogAndPrint("IslandBar native-taskbar probe\n");
    LogAndPrint("-----------------------------------------------\n");

    const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        LogAndPrint("  Shell_TrayWnd not found -- nothing to hide on this desktop\n");
        return 0;
    }

    RECT before = {};
    GetWindowRect(tray, &before);
    LogAndPrint("  Shell_TrayWnd %p  visible=%d  rect=%ld,%ld %ldx%ld\n",
                reinterpret_cast<void*>(tray),
                IsWindowVisible(tray) ? 1 : 0,
                before.left, before.top,
                before.right - before.left, before.bottom - before.top);

    DWORD trayPid = 0;
    GetWindowThreadProcessId(tray, &trayPid);
    LogAndPrint("  owned by pid %lu\n", static_cast<unsigned long>(trayPid));

    // Whether this process may even talk to that window. UIPI drops window
    // messages sent from a lower integrity level to a higher one, and ShowWindow
    // on another process's window is exactly that kind of message: it returns
    // FALSE and nothing happens, with no error to inspect.
    HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, trayPid);
    LogAndPrint("  OpenProcess(QUERY_LIMITED) -> %s (err %lu)\n",
                probe ? "allowed" : "DENIED",
                probe ? 0UL : static_cast<unsigned long>(GetLastError()));
    if (probe) CloseHandle(probe);

    // Integrity levels decide whether UIPI is the reason window messages get
    // dropped: a lower-integrity sender cannot drive a higher-integrity window.
    // This is what distinguishes "we are not permitted" from "the shell refuses".
    const auto integrityOf = [](DWORD pid, wchar_t* out, size_t capacity) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) { std::swprintf(out, capacity, L"unavailable"); return; }
        HANDLE token = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
            CloseHandle(process);
            std::swprintf(out, capacity, L"token denied");
            return;
        }
        BYTE buffer[256] = {};
        DWORD returned = 0;
        wchar_t label[48] = L"?";
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &returned)) {
            auto* mandatory = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
            const DWORD count = *GetSidSubAuthorityCount(mandatory->Label.Sid);
            const DWORD rid = *GetSidSubAuthority(mandatory->Label.Sid, count - 1);
            const wchar_t* name = L"other";
            if (rid < 0x1000) name = L"untrusted";
            else if (rid < 0x2000) name = L"low";
            else if (rid < 0x2100) name = L"medium";
            else if (rid < 0x3000) name = L"medium-plus";
            else if (rid < 0x4000) name = L"high";
            else name = L"system";
            std::swprintf(label, 48, L"%s (0x%lX)", name, static_cast<unsigned long>(rid));
        }
        std::swprintf(out, capacity, L"%s", label);
        CloseHandle(token);
        CloseHandle(process);
    };

    wchar_t ours[64] = {};
    wchar_t theirs[64] = {};
    integrityOf(GetCurrentProcessId(), ours, 64);
    integrityOf(trayPid, theirs, 64);
    LogAndPrint("  integrity: island_bar = %ls, taskbar = %ls\n", ours, theirs);

    // --- method 1: ShowWindow(SW_HIDE) ---
    const BOOL showRc = ShowWindow(tray, SW_HIDE);
    Sleep(150);
    const bool hiddenByShow = IsWindowVisible(tray) == FALSE;
    LogAndPrint("  [%s] ShowWindow(SW_HIDE)  returned=%d  visible=%d\n",
                hiddenByShow ? " OK " : "FAIL", showRc ? 1 : 0, IsWindowVisible(tray) ? 1 : 0);

    bool anyWorked = hiddenByShow;

    // Every method is tried even after one succeeds, because the point of this
    // probe is to learn WHICH method this build of Windows honours, and an early
    // exit would hide exactly that answer.
    if (!hiddenByShow) {
        // --- method 2: SetWindowPos(SWP_HIDEWINDOW) ---
        const BOOL posRc = SetWindowPos(tray, nullptr, 0, 0, 0, 0,
                                        SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE |
                                        SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(150);
        const bool hiddenByPos = IsWindowVisible(tray) == FALSE;
        LogAndPrint("  [%s] SetWindowPos(SWP_HIDEWINDOW) returned=%d  visible=%d\n",
                    hiddenByPos ? " OK " : "FAIL", posRc ? 1 : 0, IsWindowVisible(tray) ? 1 : 0);
        anyWorked = anyWorked || hiddenByPos;
    }

    if (!anyWorked) {
        // --- method 3: move it off the top of the screen ---
        //
        // Hiding a window and moving it are different operations: a build can
        // refuse the first while allowing the second. Moving it above the top
        // edge puts it out of reach of the pointer, which also defeats the
        // reveal-on-hover strip that auto-hide leaves behind.
        RECT moved = {};
        const BOOL moveRc = SetWindowPos(tray, nullptr, before.left, -400,
                                         before.right - before.left,
                                         before.bottom - before.top,
                                         SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(200);
        GetWindowRect(tray, &moved);
        const bool offScreen = (moved.bottom <= 0);
        LogAndPrint("  [%s] SetWindowPos(off-screen)   returned=%d  rect top=%ld\n",
                    offScreen ? " OK " : "FAIL", moveRc ? 1 : 0, moved.top);

        // Put it back before the next method, since they all start from the
        // visible state.
        SetWindowPos(tray, nullptr, before.left, before.top,
                     before.right - before.left, before.bottom - before.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(150);

        // --- method 4: make it fully transparent ---
        //
        // WS_EX_LAYERED + zero alpha makes the window draw nothing while leaving
        // it present, so the taskbar's own logic still runs and Explorer does not
        // re-create or fight it. On a build that refuses to hide the bar, this at
        // least removes it visually. The rectangle still receives clicks, which is
        // why the probe reports it rather than treating it as a full solution.
        const LONG_PTR oldEx = GetWindowLongPtrW(tray, GWL_EXSTYLE);
        const BOOL layeredRc = SetWindowLongPtrW(tray, GWL_EXSTYLE, oldEx | WS_EX_LAYERED) != 0;
        const BOOL alphaRc = SetLayeredWindowAttributes(tray, 0, 0, LWA_ALPHA);
        Sleep(200);

        // Sample the top-left corner, where the taskbar paints its own background.
        HDC screenDc = GetDC(nullptr);
        COLORREF pixel = CLR_INVALID;
        if (screenDc) {
            pixel = GetPixel(screenDc, before.left + 4, before.top + 4);
            ReleaseDC(nullptr, screenDc);
        }
        const bool transparent = alphaRc != FALSE;
        LogAndPrint("  [%s] SetLayeredWindowAttributes(alpha 0) layered=%d alpha=%d "
                    "pixel=0x%06lX\n",
                    transparent ? " OK " : "FAIL", layeredRc ? 1 : 0, alphaRc ? 1 : 0,
                    static_cast<unsigned long>(pixel));

        // Restore the original extended style exactly, then visibility.
        SetWindowLongPtrW(tray, GWL_EXSTYLE, oldEx);
        SetWindowPos(tray, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
        anyWorked = anyWorked || transparent;
    }

    // --- the documented AppBar state request, always reported ---
    {
        APPBARDATA state = {};
        state.cbSize = sizeof(state);
        const ULONG_PTR readState = SHAppBarMessage(ABM_GETSTATE, &state);

        APPBARDATA set = {};
        set.cbSize = sizeof(set);
        set.lParam = ABS_AUTOHIDE | ABS_ALWAYSONTOP;
        const ULONG_PTR setRc = SHAppBarMessage(ABM_SETSTATE, &set);
        Sleep(400);

        APPBARDATA after = {};
        after.cbSize = sizeof(after);
        const ULONG_PTR newState = SHAppBarMessage(ABM_GETSTATE, &after);

        const bool autoHideApplied = (static_cast<LONG>(newState) & ABS_AUTOHIDE) != 0;
        LogAndPrint("  ABM_GETSTATE before=0x%llX after=0x%llX  ABM_SETSTATE returned=%llu\n",
                    static_cast<unsigned long long>(readState),
                    static_cast<unsigned long long>(newState),
                    static_cast<unsigned long long>(setRc));
        LogAndPrint("  [%s] ABM_SETSTATE(ABS_AUTOHIDE) applied\n",
                    autoHideApplied ? " OK " : "FAIL");
    }

    // --- restore, unconditionally ---
    ShowWindow(tray, SW_SHOW);
    SetWindowPos(tray, nullptr, before.left, before.top,
                 before.right - before.left, before.bottom - before.top,
                 SWP_SHOWWINDOW | SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(200);

    RECT restored = {};
    GetWindowRect(tray, &restored);
    const bool back = IsWindowVisible(tray) != FALSE;
    LogAndPrint("  [%s] restored: visible=%d rect=%ld,%ld %ldx%ld\n",
                back ? " OK " : "FAIL", back ? 1 : 0,
                restored.left, restored.top,
                restored.right - restored.left, restored.bottom - restored.top);

    // The exit code answers "can this process hide the taskbar", not "did the
    // restore work". Every failure would otherwise look like success to a script:
    // returning 0 while printing FAIL is how a broken check survives unnoticed.
    LogAndPrint("\n-----------------------------------------------\n");
    if (!back) {
        LogAndPrint("RESULT: FAIL - the taskbar could not be restored\n");
        return 2;
    }
    if (!anyWorked) {
        LogAndPrint("RESULT: FAIL - this process cannot hide the taskbar\n");
        LogAndPrint("  Most likely the process runs at a lower integrity level than the\n");
        LogAndPrint("  shell, in which case Windows blocks the request (UIPI) and no error\n");
        LogAndPrint("  is reported. Run island_bar.exe normally, outside any sandbox.\n");
        return 1;
    }
    LogAndPrint("RESULT: PASS - this process can hide the taskbar\n");
    return 0;
}

} // namespace island
