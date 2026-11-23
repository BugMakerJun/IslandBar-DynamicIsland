// =====================================================================
//  main.cpp
//  IslandBar entry point: read the command line, run the command or the bar.
//
//  Two modes:
//    island_bar.exe --self-test   run the phase 0-d foundation checks and exit
//                                 with the failure count (0 == pass)
//    island_bar.exe               show the bar and run until closed
//
//  The self-test mode exists because this subsystem has no visible behaviour to
//  judge by eye: what matters is whether Win32 actually gave the window the
//  styles it needs, whether the composition surface presents, and whether the
//  click-through region matches the painted pill. Those are all machine-checkable.
//
//  THE BAR ITSELF IS NOT HERE ANY MORE (2026-10-16). RunBar, the phases of its frame loop and the
//  helpers only they use live in app/BarLoop.cpp; this file is the argument line, the console, the
//  demo cycle and the self-test -- and every include below is one of the four actually needs, which
//  is why most of the bar's headers are gone from this list.
// =====================================================================
#include <windows.h>

#include <shellapi.h>   // CommandLineToArgvW: command-line values as real wide characters

#include <cstdio>
#include <cmath>        // fabs: comparing the last pushed slider fraction
#include <cstring>
#include <cwchar>       // wcstoul / wcstol / wcstof: the diagnostic environment settings
#include <cwctype>      // towlower: comparing an executable's name with a window's process image
#include <tlhelp32.h>   // CreateToolhelp32Snapshot: ending every process of one image
#include <memory>       // make_unique: the sound page's worker object
#include <atomic>       // the flag the tray-learning worker shares with the frame loop
#include <vector>

#include "app/AppBar.h"
#include "app/BarLoop.h"   // RunBar: the bar itself, and the phases of its frame loop
#include "app/IslandWindow.h"
#include "app/PlayerWindow.h"
#include "cli/Cli.h"
#include "core/Log.h"
#include "media/Lyrics.h"
#include "platform/NativeTaskbar.h"
#include "platform/Monitor.h"
#include "render/Renderer.h"
#include "selftest/Diagnostics.h"
#include "services/Notification.h"
#include "services/NotificationPipe.h"
#include "shell/TrayPress.h"
#include <dwmapi.h>
#include <thread>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "secur32.lib")   // GetUserNameExW, for the Start card's footer
#pragma comment(lib, "advapi32.lib")  // ConvertSidToStringSidW, to find the account picture
#pragma comment(lib, "powrprof.lib")  // SetSuspendState, for 睡眠

namespace {



// This is a Windows-subsystem executable (see src/CMakeLists.txt), so it has no
// console of its own: launched by double-click it shows only the bar, which is
// the point. Diagnostics still need somewhere to go, so:
//
//   * run from a terminal  -> attach to the parent's console, and the output
//                             appears in that terminal as usual;
//   * run by double-click   -> nobody is reading, so stay silent rather than
//                             popping up a console window.
//
// Only the self-test allocates a console when there is no parent to borrow,
// because a test that prints nothing is not a test.
//
// `detail` is filled in with what actually happened. Three separate "the program
// printed nothing" reports made it clear that guessing here is not good enough:
// the probe reports these values so the failure is visible instead of silent.
bool PrepareConsole(bool createIfMissing, wchar_t* detail, size_t detailCapacity) {
    const DWORD pid = GetConsoleProcessList(nullptr, 0);
    const bool inheritedConsole = (pid > 0);

    const BOOL attached = AttachConsole(ATTACH_PARENT_PROCESS);
    const DWORD attachError = attached ? 0 : GetLastError();
    const bool haveConsole = attached != FALSE || inheritedConsole;

    if (!haveConsole && createIfMissing) {
        AllocConsole();
    }

    if (detail && detailCapacity) {
        std::swprintf(detail, detailCapacity,
                      L"inherited=%d attach=%d err=%lu",
                      inheritedConsole ? 1 : 0, attached ? 1 : 0,
                      static_cast<unsigned long>(attachError));
    }

    // Redirect the C runtime's standard streams onto the console ONLY when we
    // created one ourselves.
    //
    // When a console was merely attached or inherited, stdout already points
    // wherever the caller aimed it -- a pipe, a file, the terminal. Reopening it
    // onto "CONOUT$" would silently break `island_bar.exe --self-test > log`,
    // which is exactly how the tests are run from a script.
    if (!haveConsole) {
        FILE* stream = nullptr;
        if (freopen_s(&stream, "CONOUT$", "w", stdout) != 0) return false;
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
    }

    SetConsoleOutputCP(CP_UTF8);

    // Line-buffered, not block-buffered: the bar's few startup lines would
    // otherwise sit in the runtime's buffer. setvbuf demands a real buffer when a
    // mode is given -- passing size 0 with _IOLBF trips the CRT's own
    // assertion (2 <= buffer_size_in_bytes) in setvbuf.cpp.
    static char stdoutBuffer[1024];
    std::setvbuf(stdout, stdoutBuffer, _IOLBF, sizeof(stdoutBuffer));

    return haveConsole;
}

// Per-monitor-v2, requested at runtime as well as in the manifest. The manifest
// is what normally applies it; doing it here as well makes the executable behave
// correctly even when it is launched in a way that ignores the manifest.
void EnablePerMonitorV2() {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadCtxFn)(DPI_AWARENESS_CONTEXT);
        auto setContext = reinterpret_cast<SetThreadCtxFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
        if (setContext) {
            setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
}

// Runs the three states on a fixed schedule with no input at all.
//
// This exists because the hover transition cannot be verified by hand from a
// script: the bar is deliberately click-through, so nothing can be driven through
// it programmatically, and "it looked fine when I moved the mouse" is not
// evidence. Driving the state machine on a clock exercises exactly the same code
// the hover path uses, and makes every state observable to a screenshot.
int RunDemo() {
    island::MonitorGeometry monitor = island::QueryMonitor(nullptr);

    island::IslandWindow window;
    if (!window.Create(monitor, /*show=*/true)) return 1;

    const RECT rect = window.WindowRect();
    island::Renderer renderer;
    if (!renderer.Initialize(window.Handle(),
                             static_cast<UINT>(rect.right - rect.left),
                             static_cast<UINT>(rect.bottom - rect.top),
                             window.Scale())) {
        window.Destroy();
        return 1;
    }

    island::TextRenderer text;
    if (text.Initialize()) renderer.SetTextRenderer(&text);

    island::LogAndPrint("IslandBar demo: collapsed -> hover -> expanded -> hover -> collapsed\n");
    std::fflush(stdout);

    struct Step {
        island::IslandState state;
        double seconds;
    };
    const Step steps[] = {
        {island::IslandState::Collapsed, 1.2},
        {island::IslandState::Hover,     1.5},
        {island::IslandState::Expanded,  2.0},
        {island::IslandState::Hover,     1.5},
        {island::IslandState::Collapsed, 1.2},
    };

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER previous = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previous);

    const bool clockDriven =
        renderer.WaitForCompositorClock(1) ==
        island::Renderer::CompositorWaitResult::Success;

    for (const Step& step : steps) {
        window.SetState(step.state);
        island::LogAndPrint("  -> %ls for %.1fs\n", island::StateName(step.state), step.seconds);
        std::fflush(stdout);

        const double deadline = step.seconds;
        double elapsed = 0.0;
        while (elapsed < deadline) {
            if (!window.PumpMessages()) break;

            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            const double dt = static_cast<double>(now.QuadPart - previous.QuadPart) /
                              static_cast<double>(frequency.QuadPart);
            previous = now;
            elapsed += dt;

            const bool animating = window.Tick(static_cast<float>(dt > 0.1 ? 0.1 : dt));

            island::FrameState frame;
            frame.pill.box = window.CurrentBox();
            frame.state = window.State();
            frame.clock = (frame.state == island::IslandState::Expanded)
                              ? island::ClockStyle::Card
                              : island::ClockStyle::Compact;
            // The CARD fades while the shape moves; the bar's own content does not. One number for both
            // is what made the real bar blink (see ContentStrengthsFor).
            frame.cardOpacity = window.IsAnimating() ? 0.5f : 1.0f;

            renderer.Render(frame);

            if (animating && clockDriven) {
                renderer.WaitForCompositorClock(8);
            } else {
                ::MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        }
    }

    renderer.SetTextRenderer(nullptr);
    text.Shutdown();
    window.Destroy();
    return 0;
}



// Retrieves a command-line value as UTF-8.
//
// NOT from `argv`. On Windows `argv` gives each argument converted to the process ANSI code
// page, so a Chinese song title arrives as GBK bytes -- and percent-encoding those produces
// `%B9%A9%CF%FA...`, which is what a search endpoint was actually sent while it expected
// `%E4%BE%9B...` in UTF-8. Every Chinese query was malformed, which is exactly the kind of
// failure that looks like "the service has no lyrics for this bar.track".
//
// `CommandLineToArgvW` returns the real wide characters, and the rest of the program works
// in UTF-8, so the conversion happens here, once. ASCII flags are unaffected either way.
bool ArgumentAsUtf8(const wchar_t* argument, char* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';
    if (!argument) return false;
    if (WideCharToMultiByte(CP_UTF8, 0, argument, -1, out, static_cast<int>(capacity),
                            nullptr, nullptr) <= 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    // Decide the console before anything prints. The self-test needs somewhere to
    // report even when double-clicked; the bar itself must never pop one up.
    bool selfTest = false;
    bool demo = false;
    bool keepTaskbar = false;
    bool hideTaskbar = false;
    bool nativeTray = false;  // --native-tray keeps the shell's own tray on screen (see NativeTaskbar.h)
    bool claimAppBar = false;
    bool keepStrip = false;
    bool windowList = false;
    bool probe = false;
    bool noConsole = false;
    bool notify = false;
    bool musicPreview = false;
    const wchar_t* forcedState = nullptr;
    bool forcedStartHover = false;
    bool forcedSystemPanel = false;
    bool lyricsTest = false;
    bool lyricsVerbose = false;
    bool volumeTest = false;
    bool iconDump = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
        if (std::strcmp(argv[i], "--demo") == 0) demo = true;
        if (std::strcmp(argv[i], "--keep-taskbar") == 0) keepTaskbar = true;
        // The two ends of the default: keep the real tray (the default, as of 2026-10-15) or hide the
        // native taskbar outright and have nothing where the tray used to be.
        if (std::strcmp(argv[i], "--hide-taskbar") == 0) hideTaskbar = true;
        if (std::strcmp(argv[i], "--no-native-tray") == 0) nativeTray = false;
        // ... and the switch that turns it ON. It is not the default: the user does not want the shell's
        // own tray strip on screen -- it looks like the Windows taskbar and carries the shell's clock --
        // so keeping it is opt-in (2026-10-15, HANDOFF 8.135).
        if (std::strcmp(argv[i], "--native-tray") == 0) nativeTray = true;
        if (std::strcmp(argv[i], "--appbar") == 0) claimAppBar = true;
        // Keeps the shell's strip reserved: a maximized window then stops below the band, exactly as it
        // did before the work area was ever released. Here for the case where the release misbehaves on a
        // machine, so the answer is a restart rather than a rebuild.
        if (std::strcmp(argv[i], "--keep-strip") == 0) keepStrip = true;
        if (std::strcmp(argv[i], "--window-list") == 0) windowList = true;
        if (std::strcmp(argv[i], "--probe-taskbar") == 0) probe = true;
        if (std::strcmp(argv[i], "--notify") == 0) notify = true;
        if (std::strcmp(argv[i], "--music-preview") == 0) musicPreview = true;
        if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            // A static buffer rather than an allocation: this is read once at startup and the pointer is
            // kept for the life of the process.
            static wchar_t stateName[64] = {};
            if (island::ArgumentAsUtf8ToWide(argv[++i], stateName, 64)) {
                forcedState = stateName;
            }
        }
        if (std::strcmp(argv[i], "--start-hover") == 0) forcedStartHover = true;
        if (std::strcmp(argv[i], "--system-panel") == 0) forcedSystemPanel = true;
        if (std::strcmp(argv[i], "--lyrics-test") == 0) lyricsTest = true;
        if (std::strcmp(argv[i], "--lyrics-verbose") == 0) lyricsVerbose = true;
        if (std::strcmp(argv[i], "--volume-test") == 0) volumeTest = true;
        if (std::strcmp(argv[i], "--icon-dump") == 0) iconDump = true;
        // Reproduces the console-less launch on purpose. That is the case where
        // stdout has an invalid descriptor and an unguarded write trips a CRT
        // assertion instead of printing nothing, so it has to be testable from a
        // shell rather than only by double-clicking the executable.
        if (std::strcmp(argv[i], "--no-console") == 0) noConsole = true;
    }

    wchar_t consoleDetail[128] = {};
    const bool haveConsole = noConsole
                                 ? false
                                 : PrepareConsole(selfTest || demo || probe, consoleDetail, 128);

    // The log is opened before anything else can fail, and every diagnostic line
    // goes there as well as to stdout. Output on the console has been lost three
    // times on this machine; the file has not.
    island::OpenLog();
    island::LogLine(L"console: %s -> %s; log = %s", consoleDetail,
                    haveConsole ? L"usable" : L"unavailable", island::LogPath());

    // What the token looks like, said the same way whether or not the taskbar is being hidden.
    //
    // This property decides whether the bar can act on the SHELL at all: UIPI discards window messages
    // and synthetic input sent from a lower integrity level to a higher one, so a bar started from a
    // sandboxed terminal can neither hide Explorer's taskbar nor open the Start menu -- and both of those
    // fail with no error anywhere. It used to be reported only along the hide path, which is silent
    // exactly when the operator is running with --keep-taskbar and looking for the reason.
    //
    // Determined here, before anything is decided or mutated, and cheap: it only reads two tokens.
    {
        island::NativeTaskbar integrityProbe;
        integrityProbe.Locate();
        const bool belowShell = integrityProbe.IsBlockedByIntegrity();
        island::LogLine(L"integrity: %s", belowShell
                            ? L"LOWER than the shell's -- synthetic input and window messages aimed at "
                              L"Explorer will be discarded (start the bar from your own shell)"
                            : L"same as the shell's -- this process may act on Explorer");
    }

    EnablePerMonitorV2();

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            const int failures = island::RunSelfTest();
            island::LogLine(L"self-test finished with %d failure(s)", failures);
            island::CloseLog();
            return failures;
        }
        if (std::strcmp(argv[i], "--probe-taskbar") == 0) {
            const int result = island::RunTaskbarProbe();
            island::LogLine(L"log written to %s", island::LogPath());
            island::CloseLog();
            return result;
        }
        if (std::strcmp(argv[i], "--window-list") == 0) {
            const bool activate = (i + 1 < argc) && std::strcmp(argv[i + 1], "--activate") == 0;
            const int listResult = island::RunWindowList(activate);
            island::CloseLog();
            return listResult;
        }
        if (std::strcmp(argv[i], "--volume-test") == 0) {
            const int testResult = island::RunVolumeTest();
            island::CloseLog();
            return testResult;
        }
        if (std::strcmp(argv[i], "--volume-set") == 0 && i + 1 < argc) {
            const int setResult = island::RunVolumeSet(static_cast<int>(std::strtol(argv[++i], nullptr, 10)));
            island::CloseLog();
            return setResult;
        }
        if (std::strcmp(argv[i], "--toast-dump") == 0) {
            const int count = (i + 1 < argc) ? static_cast<int>(std::strtol(argv[++i], nullptr, 10)) : 10;
            const int dumpResult = island::RunToastDump(count);
            island::CloseLog();
            return dumpResult;
        }
        if (std::strcmp(argv[i], "--tray-dump") == 0) {
            const int dumpResult = island::RunTrayDump();
            island::CloseLog();
            return dumpResult;
        }
        // `--tray-surface` is the old name of this command, kept accepted because scripts and HANDOFF
        // entries use it -- what it reports changed with the press (see island::RunTraySurface).
        if (std::strcmp(argv[i], "--tray-surface") == 0 || std::strcmp(argv[i], "--tray-reach") == 0) {
            const int surfaceResult = island::RunTraySurface();
            island::CloseLog();
            return surfaceResult;
        }
        if (std::strcmp(argv[i], "--tray-learn") == 0) {
            const int learnResult = island::RunTrayLearn();
            island::CloseLog();
            return learnResult;
        }
        // --tray-press N: press the Nth icon of the card's list, exactly as a click on its cell does.
        // The card's own cells cannot be clicked from a script on this machine (see ISLANDBAR_CLICK_AT),
        // so this is how the press is exercised without a hand on the mouse. `--fire` is accepted and no
        // longer means anything: it used to ask for a dry run of a route that had to open the shell's tray
        // first, and that route is gone (see TrayPress.h).
        if (std::strcmp(argv[i], "--tray-press") == 0 && i + 1 < argc) {
            const int wanted = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
            // THE FLAGS ARE READ WHEREVER THEY ARE WRITTEN (2026-10-16). They used to be read at fixed
            // positions -- `--fire` at the first slot, `--right` only in the second -- so `--tray-press 1
            // --right` was accepted, reported as a press, and performed a LEFT press in silence. That is the
            // kind of wrong answer this project keeps paying for: measured while verifying the right-click
            // fix, when a scripted right press returned "pressed" and had in fact pressed the left button.
            bool fire = false;
            bool right = false;
            int consumed = i;
            for (int k = i + 1; k < argc; ++k) {
                if (std::strcmp(argv[k], "--fire") == 0) {
                    fire = true;
                } else if (std::strcmp(argv[k], "--right") == 0) {
                    right = true;
                } else {
                    break;
                }
                consumed = k;
            }
            i = consumed;
            const int pressResult = island::RunTrayPress(wanted, fire, right);
            island::CloseLog();
            return pressResult;
        }
        if (std::strcmp(argv[i], "--icon-dump") == 0) {
            const char* appId = (i + 1 < argc) ? argv[i + 1] : "";
            // Shell icon extraction goes through the shell's COM objects, so an apartment is
            // required. Without it SHGetFileInfoW reports success and hands back a NULL icon,
            // which reads exactly like "this executable has no icon" -- that is what made this
            // command print "encoded 0 bytes" for a file the live path loads fine.
            const HRESULT iconHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(iconHr) && iconHr != RPC_E_CHANGED_MODE) return 1;
            const int dumpResult = island::RunIconDump(appId);
            CoUninitialize();
            island::CloseLog();
            return dumpResult;
        }
        if (std::strcmp(argv[i], "--lyrics-test") == 0) {
            // Taken from the WIDE command line, not from argv: see ArgumentAsUtf8. A
            // Chinese title in argv is GBK, and percent-encoding GBK is what made every
            // Chinese search query malformed.
            int wideCount = 0;
            LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
            char testTitle[512] = {};
            char testArtist[512] = {};
            if (wideArgs) {
                if (i + 1 < wideCount) ArgumentAsUtf8(wideArgs[i + 1], testTitle, sizeof(testTitle));
                if (i + 2 < wideCount) ArgumentAsUtf8(wideArgs[i + 2], testArtist, sizeof(testArtist));
                LocalFree(wideArgs);
            }
            const HRESULT testHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(testHr) && testHr != RPC_E_CHANGED_MODE) return 1;
            const int testResult = island::RunLyricsTest(testTitle, testArtist, lyricsVerbose);
            CoUninitialize();
            island::CloseLog();
            return testResult;
        }        if (std::strcmp(argv[i], "--music-preview") == 0) {
            const HRESULT previewHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(previewHr) && previewHr != RPC_E_CHANGED_MODE) return 1;
            const int previewResult = island::RunMusicPreview();
            CoUninitialize();
            island::CloseLog();
            return previewResult;
        }        if (std::strcmp(argv[i], "--notify") == 0) {
            const int result = island::RunNotify(argc, argv);
            island::CloseLog();
            return result;
        }
        if (std::strcmp(argv[i], "--demo") == 0) {
            const HRESULT demoHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(demoHr) && demoHr != RPC_E_CHANGED_MODE) return 1;
            const int demoResult = RunDemo();
            CoUninitialize();
            island::CloseLog();
            return demoResult;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            island::LogAndPrint("usage: island_bar.exe [options]");
            island::LogAndPrint("  (no arguments)   show the bar and hide the native taskbar");
            island::LogAndPrint("  --keep-taskbar   leave the native taskbar visible");
            island::LogAndPrint("  --hide-taskbar   hide the native taskbar (the default until"
                                " 2026-10-15; the tray is then not on screen at all)");
            island::LogAndPrint("  --keep-strip     do not release the strip the shell taskbar reserves");
            island::LogAndPrint("                   (a maximized window then keeps a band at the top)");
            island::LogAndPrint("  --self-test      run the runtime assertions; exit code = failures");
            island::LogAndPrint("  --probe-taskbar  report whether this process may hide the shell taskbar");
            island::LogAndPrint("  --demo           cycle collapsed/hover/expanded with no input needed");
            island::LogAndPrint("  --music-preview  cycle the three music states with sample data");
            island::LogAndPrint("  --lyrics-test <title> [artist]  fetch lyrics and report what came back");
            island::LogAndPrint("  --no-console     behave as if there were no console (diagnostic)");
            island::LogAndPrint("  --tray-reach     list every tray icon and whether a press can reach it");
            island::LogAndPrint("  --tray-learn     ask every running tray application to register again");
            island::LogAndPrint("  --tray-press N [--right]  press the Nth icon exactly as its cell does");
            island::LogAndPrint("");
            island::LogAndPrint("Every run also writes a timestamped log, which is the reliable way to");
            island::LogAndPrint("see what happened -- the console has been known to swallow output:");
            island::LogAndPrint("      %%TEMP%%\\island_bar.log");
            island::LogAndPrint("");
            island::LogAndPrint("HOW TO RUN THIS");
            island::LogAndPrint("  From PowerShell, & is the call operator and is required for a quoted");
            island::LogAndPrint("  path:");
            island::LogAndPrint("      & \"C:\\path\\to\\island_bar.exe\" --probe-taskbar");
            island::LogAndPrint("  From cmd.exe, & is a command separator and CANNOT precede the path.");
            island::LogAndPrint("  Quote the path directly, or use start /wait:");
            island::LogAndPrint("      \"C:\\path\\to\\island_bar.exe\" --probe-taskbar");
            island::LogAndPrint("      start /wait \"\" \"C:\\path\\to\\island_bar.exe\" --probe-taskbar");
            island::LogAndPrint("  To capture the output from either shell, redirect it:");
            island::LogAndPrint("      island_bar.exe --probe-taskbar > probe.txt 2>&1");
            island::LogAndPrint("");
            island::LogAndPrint("The native taskbar is hidden on startup and restored on exit. If this");
            island::LogAndPrint("process is killed instead of closed, bring it back with either:");
            island::LogAndPrint("  taskkill /f /im explorer.exe && start explorer.exe");
            island::LogAndPrint("or, without restarting the shell, this one-liner:");
            island::LogAndPrint("  powershell -c \"Add-Type -Name W -Namespace N -MemberDefinition "
                              "'[DllImport(\\\"user32.dll\\\")]public static extern bool "
                              "ShowWindow(IntPtr h,int c);[DllImport(\\\"user32.dll\\\")]public static "
                              "extern IntPtr FindWindow(string c,string n);'; "
                              "[N.W]::ShowWindow([N.W]::FindWindow('Shell_TrayWnd',$null),5)\"");
            return 0;
        }
    }

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!comOk) {
        island::LogAndPrint("FATAL: CoInitializeEx failed (0x%08lX)\n",
                    static_cast<unsigned long>(hr));
        return 1;
    }

    // One bar at a time.
    //
    // Two instances draw two bars on top of each other, and the symptom does not look like "two
    // programs are running": it looks like ONE bar drawn in the wrong place, because the second copy
    // is somewhere the first one is not. That is exactly how it was reported -- a panel anchored to its
    // button AND a second card-shaped box in the middle of the screen -- and it was diagnosed as a
    // placement bug for two rounds.
    //
    // A named mutex is used rather than a window lookup because it is the same check whether the other
    // instance is starting, running, or shutting down, and it cannot be defeated by the other instance
    // being on a different desktop or having renamed its window class.
    //
    // The bar is a single global thing (it hides the shell's taskbar and may claim a screen edge), so
    // refusing to start a second copy is the correct behaviour rather than a limitation.
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"IslandBar.SingleInstance");
    const DWORD mutexError = GetLastError();
    if (singleInstance && mutexError == ERROR_ALREADY_EXISTS) {
        island::LogAndPrint("IslandBar is already running -- this copy exits now.\n");

        // REPORTED AND GONE. No dialog at all.
        //
        // The first version popped a MessageBox, and `MessageBoxTimeoutW` -- the undocumented call that
        // was supposed to dismiss it on a timer -- did not do so here: three launches in a row left TWO
        // refused copies alive, each sitting in a modal box. That is not a cosmetic wart, it is the same
        // defect the guard exists to prevent: the copies that were refused were still drawing their own
        // bars, which is exactly the "two panels on screen" that took three rounds to find.
        //
        // So the refusal is now a log line and an immediate exit. A guard that can itself linger is worse
        // than no guard, because it produces the very state it was written to prevent.
        island::CloseLog();
        CloseHandle(singleInstance);
        CoUninitialize();
        return 1;
    }

    // THE NATIVE TASKBAR IS NOT HIDDEN ANY MORE, IT IS CLIPPED (2026-10-15). The default keeps the
    // real system tray on screen -- the shell's own icons, menus, overflow and clock -- because the
    // three measurements in HANDOFF 8.133 left no cheaper way to have a tray that behaves exactly like
    // the system one. `--hide-taskbar` (or `--no-native-tray`) is the old behaviour, and
    // `--keep-taskbar` still means "do not touch it at all".
    const int result = RunBar(/*hideNativeTaskbar=*/!keepTaskbar, claimAppBar, keepStrip, forcedState,
                           forcedStartHover, forcedSystemPanel,
                           /*keepNativeTray=*/keepTaskbar ? false : (hideTaskbar ? false : nativeTray));
    // The status reader is closed by RunBar, where it was opened.
    CoUninitialize();
    return result;
}
