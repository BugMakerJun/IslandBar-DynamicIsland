// =====================================================================
//  Cli.cpp -- THE COMMANDS THAT RUN INSTEAD OF THE BAR
//
//  These twelve commands lived in main.cpp until 2026-10-15, mixed in with the frame loop, the assembly of
//  every subsystem, and the tray rules. They are not part of the bar: each one is a question the workflow
//  asks a build ("what does the shell's record hold", "is there anything to send to that icon", "did the
//  media session arrive"), and each one prints through core/Log.h and answers with an exit code.
//
//  THEY TOUCH EVERYTHING, ON PURPOSE. A command that asks about the tray still stands up a window and a
//  renderer to answer, because that is what "run the bar and ask it one question" means -- the alternative
//  would be a second implementation of the bar that could disagree with the real one. What they must not do
//  is grow into a second program: the argument parsing stays in main.cpp, which knows the flags.
//
//  THE ONES THE WORKFLOW DEPENDS ON are --tray-dump, --tray-surface, --tray-learn and --tray-press: they are
//  the contract a script can hold this program to (see ARCHITECTURE.md), and they are named in HANDOFF as the
//  way every tray measurement was taken.
//
//  Nothing here is new code: it is the same text, moved, with island:: added at the call sites.
// =====================================================================
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/IslandWindow.h"
#include "cli/Cli.h"
#include "core/FrameStats.h"   // the frame clock --music-preview reports through
#include "core/Log.h"
#include "domain/CardLayout.h"
#include "domain/StatePolicy.h"
#include "media/AudioVolume.h"
#include "media/Lyrics.h"
#include "media/MusicSession.h"
#include "media/WicImage.h"
#include "platform/Monitor.h"
#include "platform/WindowList.h"
#include "render/Renderer.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"           // CurrentTheme: the colours the card is drawn from
#include "services/Notification.h"
#include "services/NotificationIcon.h"
#include "services/NotificationPipe.h"
#include "services/ToastStore.h"
#include "shell/TrayIcons.h"
#include "shell/TrayLearn.h"
#include "shell/TrayMenu.h"
#include "shell/TrayPress.h"

namespace island {

// MOVED from main.cpp's anonymous namespace: one definition, two users (main's --state parsing and
// --icon-dump), and it is command-line plumbing rather than a subsystem's business.
//
// The name says "already UTF-8" because the caller is the command line, where `argv` arrives in the process
// ANSI codepage unless it was converted -- and on this machine the ANSI codepage is 936, so widening an
// application identifier with the wrong conversion produced mojibake that read as "this app has no icon".
bool ArgumentAsUtf8ToWide(const char* utf8, wchar_t* out, size_t capacity) {
    if (!utf8 || !out || capacity == 0) return false;
    out[0] = L'\0';
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, static_cast<int>(capacity)) > 0;
}

int RunIconDump(const char* appId) {
    if (!appId || appId[0] == '\0') {
        island::LogAndPrint("icon dump: an app id is required, e.g. QQMusic.exe");
        return 2;
    }

    wchar_t wideAppId[512] = {};
    if (!ArgumentAsUtf8ToWide(appId, wideAppId, 512)) {
        island::LogAndPrint("icon dump: the app id could not be read");
        return 2;
    }

    wchar_t resolved[MAX_PATH] = {};
    if (!island::ResolveAppIconPath(wideAppId, resolved, MAX_PATH)) {
        island::LogAndPrint("icon dump: '%s' did not resolve to an executable", appId);
        return 1;
    }
    island::LogAndPrint("icon dump: resolved to %ls", resolved);

    uint8_t png[island::kMusicArtworkCapacity] = {};
    const size_t pngSize = island::FetchAppIconForTest(resolved, png, sizeof(png));
    island::LogAndPrint("icon dump: encoded %zu bytes of PNG", pngSize);
    if (pngSize == 0) return 1;

    island::DecodedImage decoded;
    if (!island::DecodeImage(png, pngSize, &decoded)) {
        island::LogAndPrint("icon dump: the PNG did not decode");
        return 1;
    }

    // What the pixels actually contain. A fully transparent or all-black decode looks exactly
    // like "no icon" on screen, and this is what tells the difference.
    size_t opaque = 0;
    uint64_t alphaSum = 0;
    uint8_t maxAlpha = 0;
    for (int i = 0; i < decoded.width * decoded.height; ++i) {
        const uint8_t alpha = decoded.pixels[static_cast<size_t>(i) * 4 + 3];
        alphaSum += alpha;
        if (alpha > maxAlpha) maxAlpha = alpha;
        if (alpha > 8) ++opaque;
    }
    const size_t total = static_cast<size_t>(decoded.width) * static_cast<size_t>(decoded.height);
    island::LogAndPrint("icon dump: decoded %dx%d, %zu of %zu pixels visible, max alpha %u, "
                        "mean alpha %.1f",
                        decoded.width, decoded.height, opaque, total,
                        static_cast<unsigned>(maxAlpha),
                        total ? static_cast<double>(alphaSum) / static_cast<double>(total)
                              : 0.0);

    // Written next to the log, so it is found the same way the log is.
    wchar_t outPath[MAX_PATH] = {};
    const wchar_t* logPath = island::LogPath();
    wcsncpy_s(outPath, MAX_PATH, logPath, _TRUNCATE);
    wchar_t* dot = wcsrchr(outPath, L'.');
    if (dot) *dot = L'\0';
    wcscat_s(outPath, MAX_PATH, L"-icon.png");

    const size_t written = island::EncodeBgraToPng(decoded.pixels, decoded.width,
                                                   decoded.height, png, sizeof(png));
    if (written == 0) {
        island::LogAndPrint("icon dump: could not re-encode the decoded pixels");
        return 1;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, outPath, L"wb") != 0 || !file) {
        island::LogAndPrint("icon dump: could not write %ls", outPath);
        return 1;
    }
    // The encoder reports the buffer capacity rather than its output length, so the PNG end
    // marker is what says where the image stops; the decoder stops there too.
    fwrite(png, 1, written, file);
    fclose(file);
    island::LogAndPrint("icon dump: wrote %ls (%zu bytes)", outPath, written);
    return 0;
}

// Reports what the bar would put on itself as buttons.
//
// The filtering rules are covered by the self-test, but a rule only decides about a window that has
// been DESCRIBED correctly, and the describing is done by a dozen Win32 calls that no test can
// exercise without a desktop full of real windows. This prints the result of those calls, so the
// two halves can be told apart: a wrong list means either the rules are wrong or the descriptions
// are, and this says which.
int RunWindowList(bool activate) {
    island::WindowList windows;
    const int count = windows.Refresh();

    island::LogAndPrint("IslandBar window list\n");
    island::LogAndPrint("-----------------------------------------------\n");
    island::LogAndPrint("  %ls\n", windows.LastSummary());
    island::LogAndPrint("  foreground: %p\n", reinterpret_cast<void*>(windows.ForegroundHandle()));

    if (count == 0) {
        island::LogAndPrint("\n  Nothing would be listed. If application windows ARE open, the\n");
        island::LogAndPrint("  DESCRIPTIONS are wrong rather than the rules.\n");
        return 0;
    }

    island::LogAndPrint("\n  these would appear as buttons, front to back:\n");
    for (int i = 0; i < count; ++i) {
        const island::TaskEntry& entry = windows.At(i);
        // Narrowed for the log: window titles are UTF-16 and the log is UTF-8.
        char title[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, entry.title, -1, title, sizeof(title) - 1, nullptr,
                            nullptr);
        island::LogAndPrint("    %2d. pid %-6lu %-6s %s\n", i + 1,
                            static_cast<unsigned long>(entry.processId),
                            entry.iconic ? "[min]" : "", title);
    }

    // Activating is the half no unit test can reach: it changes the desktop, so it can only be
    // judged by looking at it. Kept behind a flag and done last.
    if (activate) {
        const HWND current = windows.ForegroundHandle();
        int index = -1;
        for (int i = 0; i < count && index < 0; ++i) {
            if (windows.At(i).handle != current) index = i;
        }
        if (index < 0) {
            island::LogAndPrint("\n  nothing to activate\n");
            return 0;
        }

        const island::TaskEntry& target = windows.At(index);
        island::LogAndPrint("\n  activating entry %d (pid %lu, %sminimised)...\n", index + 1,
                            static_cast<unsigned long>(target.processId),
                            target.iconic ? "" : "not ");
        const bool raised = windows.Activate(target.handle);
        island::LogAndPrint("  activate %s\n", raised ? "SUCCEEDED" : "was REFUSED by Windows");
        if (!raised) {
            // SetForegroundWindow is allowed to fail and does so silently. Windows restricts who
            // may take the foreground to stop programs stealing focus; a real taskbar click
            // qualifies, a background shell may not.
            island::LogAndPrint("    Windows only lets a process take the foreground under the "
                                "conditions meant\n");
            island::LogAndPrint("    to stop programs stealing focus. A taskbar click qualifies; "
                                "a command\n");
            island::LogAndPrint("    run from another process may not.\n");
            return 1;
        }
    }
    return 0;
}

// Exercises the volume control: reads it, changes it, reads it back, restores it.
//
// Reads the newest toasts out of Windows' own notification database and prints them.
//
// A command line rather than only a card on screen, because this channel's evidence has to be obtainable
// without a screenshot: "no toasts appeared" and "the database was not read" look identical on a bar, and
// they are different problems.
int RunToastDump(int limit) {
    if (limit <= 0) limit = 10;

    auto reader = std::make_shared<island::ToastReader>();
    if (!reader->Start()) {
        island::LogAndPrint("toast dump: the system toast database is not readable\n");
        return 1;
    }

    std::vector<wchar_t> text(64 * 1024);
    const int rows = reader->Dump(limit, text.data(), text.size());
    // The WIDE overload: the dump is full of Chinese message text, and the narrow one would print it as
    // byte-widened mojibake -- the same trap that once swallowed a tile's own log line.
    island::LogAndPrint(L"toast dump: %d of the newest %d toast(s) in %ls\n", rows, limit,
                        reader->Available() ? L"the live database" : L"nowhere");
    if (rows > 0) {
        island::LogAndPrint(L"%ls", text.data());
    }
    reader->Stop();
    return rows > 0 ? 0 : 1;
}

// Prints the tray icons the bar would draw, and where each one comes from.
//
// A command line for the same reason --toast-dump is one: "there is no tray" and "the bar could not read
// the tray" look identical on screen, and they are different problems. The registry key and the PNG
// magic are printed so the answer can be checked against regedit by hand.
int RunTrayDump() {
    island::TrayIcons icons;
    // A PASS FIRST, because what the card draws is the shell's own answer and this process has not seen one
    // (see shell/TrayIcons.h). Without it the dump would print the registry list and call it the tray --
    // which is the thirteen-against-nine mistake this command exists to make visible.
    icons.Refresh();
    island::LearnTrayCallbacks(icons, true);
    const int count = icons.Refresh();
    island::LogAndPrint(L"tray dump: %d icon(s) %ls\n", count, icons.SourceClause().c_str());
    island::LogAndPrint(L"  from %ls (looked at %d entr(ies) in the key, %d of them kept)\n",
                        island::TrayIcons::SettingsKeyPath(), icons.Examined(), icons.Kept());
    for (int i = 0; i < count; ++i) {
        const island::TrayIcon& icon = icons.At(i);
        island::LogAndPrint(L"  [%d] %ls -- %zu byte PNG, promoted=%d, %ls\n", i, icon.name,
                            icon.pngSize, icon.promoted ? 1 : 0, icons.ExePathAt(i));
    }
    return count > 0 ? 0 : 1;
}


// THIS COMMAND USED TO ASK THE SHELL WHERE EACH ICON WAS, which was the question a press depended on while
// the press was a real click on the shell's own button. Since 2026-10-15 a press is the application's own
// callback message (see TrayPress.h), so the question became "is there anything to send" -- and the answer
// separates the same three states in a way that needs no shell, no taskbar and no UI Automation: an icon with
// a learned callback whose window exists is pressable; an icon that has been learned but whose application is
// not running is not; an icon nothing has been learned about opens the bar's own menu instead.
int RunTraySurface() {
    island::TrayIcons icons;
    const int count = icons.Refresh();
    island::LogAndPrint(L"tray reach: %d icon(s) on the card\n", count);

    int reachable = 0;
    for (int i = 0; i < count; ++i) {
        const island::TrayIcon& icon = icons.At(i);
        const island::TrayPressRequest request = island::MakeTrayPressRequest(icon, icons.ExePathAt(i));
        const bool reach = island::TrayPressReachable(request);
        if (reach) ++reachable;
        wchar_t flat[128] = {};
        std::swprintf(flat, 128, L"uid %u", icon.uid);
        island::LogAndPrint(L"  [%d] %ls (%ls) -- %ls\n", i, island::FlatName(icon.name, flat, 128),
                            icon.hasGuid ? L"GUID" : flat,
                            reach ? L"its own callback is known and its window is there"
                                  : L"nothing learned yet: a press opens this bar's own menu");
    }
    island::LogAndPrint(L"  %d of %d can be pressed without touching the pointer\n", reachable, count);
    return reachable > 0 ? 0 : 1;
}

// Presses one icon from the command line, for the same reason: a click cannot be delivered from a script
// here. Everything the live click does is done, including the walk through the shell's tray, so what this
// prints is what a click produces -- the difference being `--fire`, without which the button is found and
// reported but not pressed.
// Learns the tray callbacks of everything running, and prints what it got. Its own command because the
// learning is otherwise a background step of a live session, and a question about it should not need a
// running bar to be answered.
int RunTrayLearn() {
    island::TrayIcons icons;
    const int count = icons.Refresh();
    const int coveredBefore = island::CountCoveredIcons(icons);
    island::LogAndPrint(L"tray learn: %d icon(s) on the card, %d of them known; offering the module to every "
                        L"application and asking each to register again\n",
                        count, coveredBefore);
    const island::TrayLearnResult learned = island::LearnTrayCallbacks(icons, true);
    const std::wstring observer = island::ObserverClause(learned);
    island::LogAndPrint(L"tray learn: the shell (%d process(es) offered) was %ls, %d registration(s) "
                        L"captured, %d new entr(ies) -- %ls\n",
                        learned.appsTried,
                        learned.appsAlreadyWatched > 0 ? L"already carrying the module" : L"given the module",
                        learned.captured, learned.newEntries, observer.c_str());
    island::LogAndPrint(L"tray learn: %d of %d icon(s) now have a callback entry (before: %d), %lld ms\n",
                        learned.covered, learned.icons, coveredBefore, learned.ms);
    wchar_t home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    island::LogAndPrint(L"tray learn: the cache is %ls\\IslandBar\\tray_callbacks.txt and the raw capture is "
                        L"%ls\\IslandBar\\tray_learned.txt\n",
                        home, home);
    return learned.covered > coveredBefore ? 0 : 1;
}

int RunTrayPress(int index, bool fire, bool rightClick) {
    island::TrayIcons icons;
    const int count = icons.Refresh();
    if (index < 0 || index >= count) {
        island::LogAndPrint(L"tray press: [%d] is not one of the %d icon(s) on the card\n", index, count);
        return 1;
    }
    const island::TrayIcon& icon = icons.At(index);
    const island::TrayPressRequest request = island::MakeTrayPressRequest(icon, icons.ExePathAt(index));
    island::LogAndPrint(L"tray press: [%d] '%ls' (%ls)\n", index, icon.name,
                        icon.hasGuid ? L"registered by GUID" : L"registered by UID");

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // `fire` is accepted and ignored: it used to mean "do not actually press" for a route that opened the
    // shell's panel to find the icon first, and there is nothing to find any more -- see TrayPress.h. Keeping
    // the flag accepted means every instruction in HANDOFF and in the scripts that call it still runs.
    (void)fire;
    const island::TrayPressOutcome outcome = island::PressTrayIcon(request, rightClick);
    if (SUCCEEDED(comHr)) CoUninitialize();
    island::LogAndPrint(L"  -> %ls%ls: %ls (press %lld ms)\n", island::TrayPressResultName(outcome.result),
                        rightClick ? L" [right click]" : L"", outcome.route, outcome.ms);
    island::LogAndPrint(L"  delivered to %ls -- %ls\n", outcome.target, outcome.answer);
    if (outcome.result == island::TrayPressResult::Pressed) {
        return 0;
    }
    return 1;
}

// Sets the system output volume to a percentage and reports what the device says afterwards.
//
// A command line rather than a click, because the click cannot be delivered from every context: the bar
// runs as an overlay and a script's synthetic mouse input is dropped whenever the session is not in the
// state the input assumes (locked, asleep, another desktop). The volume itself, though, can always be set
// -- and a test that changes the machine's own setting has to be able to put it back afterwards.
int RunVolumeSet(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    auto volume = std::make_shared<island::AudioVolume>();
    if (!volume->Start()) {
        island::LogAndPrint("volume set: no audio endpoint available\n");
        return 1;
    }

    float before = 0.0f;
    const bool hadBefore = volume->Get(&before);
    volume->SetLevel(static_cast<float>(percent) / 100.0f);

    // The write goes through the worker, so the level is read back rather than assumed: this reports what
    // the DEVICE ended up at, which is the only claim worth making about someone else's machine.
    float after = 0.0f;
    for (int i = 0; i < 200; ++i) {
        Sleep(10);
        if (volume->Get(&after) && std::fabs(after - static_cast<float>(percent) / 100.0f) < 0.011f) {
            break;
        }
    }
    island::LogAndPrint("volume set: %s%.0f%% -> %.0f%% (asked for %d%%)\n",
                        hadBefore ? "" : "unknown -> ", hadBefore ? before * 100.0f : 0.0f,
                        after * 100.0f, percent);
    return 0;
}

// The card's volume control is the one feature whose effect is on the AUDIO DEVICE rather than
// on the screen, so no screenshot can show that it works. This does the round trip and reports
// both ends, which is the only way to tell "the bar moved" apart from "the volume actually
// changed".
int RunVolumeTest() {
    auto volume = std::make_shared<island::AudioVolume>();
    if (!volume->Start()) {
        island::LogAndPrint("volume test: no audio endpoint available");
        return 1;
    }

    float original = 0.0f;
    if (!volume->Get(&original)) {
        island::LogAndPrint("volume test: the endpoint gave no level");
        volume->Stop();
        return 1;
    }
    island::LogAndPrint("volume test: read %.0f%% (muted=%d)", original * 100.0f,
                        volume->IsMuted() ? 1 : 0);

    int failures = 0;

    // Step down, then up, checking the DEVICE agrees each time rather than trusting the value
    // this process happens to hold.
    struct Step {
        float level;
        const char* label;
    };
    const Step steps[] = {
        {0.25f, "set to 25%"},
        {0.75f, "set to 75%"},
    };
    for (const Step& step : steps) {
        volume->SetLevel(step.level);
        Sleep(900);   // long enough for the worker to write and read back

        float read = 0.0f;
        volume->Get(&read);
        island::LogAndPrint("volume test: %s -> device reports %.0f%%", step.label,
                            read * 100.0f);
        if (std::fabs(read - step.level) > 0.05f) ++failures;
    }

    // Mute, and confirm the device flipped.
    const bool mutedBefore = volume->IsMuted();
    volume->ToggleMute();
    Sleep(900);
    const bool mutedAfter = volume->IsMuted();
    island::LogAndPrint("volume test: mute %d -> %d", mutedBefore ? 1 : 0, mutedAfter ? 1 : 0);
    if (mutedAfter == mutedBefore) ++failures;
    volume->ToggleMute();   // put mute back before restoring the level
    Sleep(400);

    // Restore: a diagnostic must not leave the machine's volume somewhere else.
    volume->SetLevel(original);
    Sleep(900);
    float restored = 0.0f;
    volume->Get(&restored);
    island::LogAndPrint("volume test: restored to %.0f%% (was %.0f%%)", restored * 100.0f,
                        original * 100.0f);
    if (std::fabs(restored - original) > 0.05f) ++failures;

    volume->Stop();
    island::LogAndPrint("volume test: %s", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

// Loads the source application's icon exactly as the live path does, for the preview.
//
// The preview has no media session to read one from, and a card drawn with the fallback dot
// would not show whether the icon pipeline works at all -- which is the thing being checked.
// The executable is passed in so the preview can use a real player's icon.
size_t MakeSampleAppIcon(const wchar_t* executablePath, uint8_t* out, size_t capacity) {
    return island::FetchAppIconForTest(executablePath, out, capacity);
}


// Sends one notification and exits -- the client half of the push channel.
//
// Having this in the same binary means the channel can be exercised end to end
// with no external tooling: run the bar, then run `island_bar.exe --notify ...`.
// It also gives other programs a ready-made sender.
//
// Flags: --notify <title> [--body <text>] [--app <name>] [--urgency low|normal|high]
//        [--sticky]
int RunNotify(int argc, char** argv) {
    island::Notification notification;

    // THE TEXT ARGUMENTS COME FROM THE WIDE COMMAND LINE, not from `argv`.
    //
    // `main(int, char**)` hands out the command line in the CONSOLE'S ANSI CODE PAGE, so on this machine a
    // Chinese title arrives as GBK bytes -- eight of them for four characters -- and every one of them is an
    // invalid UTF-8 sequence by the time the card tries to draw it. Measured: the notice strip showed
    // `DSH ·` followed by EIGHT boxes, which is what eight undecypherable bytes look like. The same message
    // raised through the notification platform rendered perfectly, which is how the text was cleared and the
    // drawing acquitted.
    //
    // Reading the command line again with CommandLineToArgvW gives the real characters, and the program is
    // UTF-8 everywhere else, so the conversion happens once, here.
    int wideArgc = 0;
    wchar_t** wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    const auto wideValueOf = [&](const char* flag, wchar_t* out, size_t capacity) -> bool {
        if (!out || capacity == 0) return false;
        out[0] = L'\0';
        if (!wideArgv) return false;
        for (int i = 1; i + 1 < wideArgc; ++i) {
            wchar_t wideFlag[32] = {};
            if (MultiByteToWideChar(CP_UTF8, 0, flag, -1, wideFlag, 32) <= 0) continue;
            if (std::wcscmp(wideArgv[i], wideFlag) == 0) {
                wcscpy_s(out, capacity, wideArgv[i + 1]);
                return true;
            }
        }
        return false;
    };
    // UTF-8 out, so the rest of the pipeline (the JSON wire format and the store) is unchanged.
    const auto wideValueAsUtf8 = [&](const char* flag, char* out, size_t capacity) -> bool {
        if (!out || capacity == 0) return false;
        out[0] = '\0';
        wchar_t wide[512] = {};
        if (!wideValueOf(flag, wide, 512)) return false;
        return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(capacity), nullptr,
                                   nullptr) > 0;
    };

    const auto valueOf = [&](const char* flag) -> const char* {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
        }
        return nullptr;
    };

    // The title is the first bare argument after --notify that is not a flag.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--notify") == 0 && i + 1 < argc) {
            if (!wideValueAsUtf8("--notify", notification.title, island::kTitleCapacity)) {
                strncpy_s(notification.title, island::kTitleCapacity, argv[i + 1], _TRUNCATE);
            }
            break;
        }
    }

    if (const char* body = valueOf("--body")) {
        if (!wideValueAsUtf8("--body", notification.body, island::kBodyCapacity)) {
            strncpy_s(notification.body, island::kBodyCapacity, body, _TRUNCATE);
        }
    }
    if (const char* app = valueOf("--app")) {
        if (!wideValueAsUtf8("--app", notification.app, island::kAppNameCapacity)) {
            strncpy_s(notification.app, island::kAppNameCapacity, app, _TRUNCATE);
        }
    }
    // The icon is optional on purpose: with none, the card draws the SENDER's own executable icon, because
    // the pipe says who connected. --icon is for a sender that wants a particular picture instead.
    if (const char* icon = valueOf("--icon")) {
        strncpy_s(notification.iconPath, 260, icon, _TRUNCATE);
    }
    if (valueOf("--sticky")) {
        notification.sticky = true;
    }
    if (const char* urgency = valueOf("--urgency")) {
        if (_stricmp(urgency, "high") == 0) notification.urgency = island::Urgency::High;
        else if (_stricmp(urgency, "low") == 0) notification.urgency = island::Urgency::Low;
    }
    // A caller that only supplies a body is still legitimate.
    if (notification.title[0] == '\0') {
        if (const char* body = valueOf("--body")) {
            strncpy_s(notification.title, island::kTitleCapacity, body, _TRUNCATE);
            notification.body[0] = '\0';
        }
    }

    if (notification.title[0] == '\0' && notification.body[0] == '\0') {
        island::LogAndPrint("usage: island_bar.exe --notify <title> [--body <text>] "
                            "[--app <name>] [--icon <path>] [--urgency low|normal|high] [--sticky]");
        if (wideArgv) LocalFree(wideArgv);
        return 2;
    }

    char response[512] = {};
    const bool sent = island::SendNotification(notification, island::kPipeName,
                                               response, sizeof(response));
    if (sent) {
        island::LogAndPrint("sent: %s", response[0] ? response : "(no response body)");
        if (wideArgv) LocalFree(wideArgv);
        return 0;
    }

    island::LogAndPrint("could not reach the bar on \\\\.\\pipe\\%ls: %s",
                        island::kPipeName,
                        response[0] ? response : "(no reason reported)");
    if (wideArgv) LocalFree(wideArgv);
    return 1;
}

// Builds a small PNG in memory for the music preview.
//
// The preview must exercise the real artwork path -- encode, decode through WIC,
// draw through Direct2D -- rather than a special case that skips it. Generating the
// image at runtime does that without shipping a test file.
//
// The pattern is a diagonal gradient with a dark block in it, so a decode that
// mangled stride, channel order or orientation would be visible rather than
// plausible-looking.
size_t MakeSampleArtworkPng(uint8_t* out, size_t capacity) {
    constexpr int kSize = 128;
    std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4);

    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const size_t index = (static_cast<size_t>(y) * kSize + x) * 4;
            // BGRA, premultiplied (alpha is 255, so premultiplied == straight).
            pixels[index + 0] = static_cast<uint8_t>(60 + (x * 140) / kSize);
            pixels[index + 1] = static_cast<uint8_t>(40 + (y * 120) / kSize);
            pixels[index + 2] = static_cast<uint8_t>(180 - (x * 90) / kSize);
            pixels[index + 3] = 255;

            if (x > 20 && x < 56 && y > 20 && y < 56) {
                pixels[index + 0] = 20;
                pixels[index + 1] = 20;
                pixels[index + 2] = 20;
            }
        }
    }

    return island::EncodeBgraToPng(pixels.data(), kSize, kSize, out, capacity);
}

// Shows the four island shapes in order with sample data, then exits.
//
// This exists because the shapes cannot be reached on demand from a test -- they depend
// on what the system media session reports and where the pointer is -- and because a
// pointer cannot always be moved: this sandbox refuses SetCursorPos (error 203), so a
// hover CANNOT be reproduced by driving the mouse. What this does instead is drive the
// state machine directly and, on every stage, print the two facts that decide whether a
// real hover would work:
//
//   * where the pointer actually is, and
//   * whether the window would accept it there (ShouldAcceptPointer).
//
// A stage that reports "would accept: no" is a stage the user could never hover, which
// is exactly the class of bug that makes a card appear unreachable.
int RunMusicPreview() {
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

    island::MusicInfo sample;
    sample.present = true;
    sample.state = island::PlaybackState::Playing;
    // The track from the reference image, so the preview can be compared with it
    // directly rather than approximated.
    strncpy_s(sample.title, island::kMusicTitleCapacity, "\xe4\xb8\x9c\xe5\xa4\xa7\xe8\xa1\x97",
              _TRUNCATE);
    strncpy_s(sample.artist, island::kMusicArtistCapacity,
              "\xe5\xbc\xa0\xe8\xb6\x85\xe6\xb4\x8b", _TRUNCATE);
    strncpy_s(sample.album, island::kMusicAlbumCapacity,
              "\xe4\xb8\x9c\xe5\xa4\xa7\xe8\xa1\x97", _TRUNCATE);
    strncpy_s(sample.app, island::kMusicAppCapacity, "QQ\xe9\x9f\xb3\xe4\xb9\x90", _TRUNCATE);

    // The times from the reference image too, so the card can be held next to it.
    sample.positionMs = 93000;
    sample.durationMs = 340000;

    // Real artwork, built in memory and passed through the same WIC decode the live
    // path uses. A preview that skipped the decode would not be testing anything.
    sample.artworkSize = MakeSampleArtworkPng(sample.artwork, island::kMusicArtworkCapacity);
    if (sample.artworkSize > 0) sample.artworkRevision = 1;

    island::DecodedImage artwork;
    const bool haveArtwork =
        sample.artworkSize > 0 &&
        island::DecodeImage(sample.artwork, sample.artworkSize, &artwork,
                            island::DecodeSlot::Artwork);

    // The source application's icon, loaded through the SAME function the live path uses.
    //
    // A preview drawn with the fallback dot would not show whether the icon path works at all,
    // which is the thing being checked here. The real player's executable is preferred; if it is
    // not installed, this executable's own icon is used, so the path is still exercised.
    {
        wchar_t playerPath[MAX_PATH] = {};
        // Through the SAME lookup the live path uses, so this exercises App Paths and the
        // install-root search rather than a path that only exists on one machine.
        const bool havePlayer = island::ResolveAppIconPath(L"QQMusic.exe", playerPath, MAX_PATH);
        if (havePlayer) {
            sample.iconSize = island::FetchAppIconForTest(playerPath, sample.icon,
                                                          island::kMusicArtworkCapacity);
        }
        if (sample.iconSize == 0) {
            wchar_t selfPath[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH)) {
                sample.iconSize = island::FetchAppIconForTest(selfPath, sample.icon,
                                                              island::kMusicArtworkCapacity);
            }
        }
        if (sample.iconSize > 0) sample.iconRevision = 1;
        island::LogAndPrint("music preview: source icon %s (%zu bytes)",
                            sample.iconSize > 0 ? "loaded" : "NOT loaded", sample.iconSize);
        // Which icon is actually on screen: the player's, or this program's as a fallback.
        island::LogAndPrint("music preview: icon resolved to %ls",
                            havePlayer ? playerPath : L"(fallback: this executable)");
    }

    // The background wash, through the same blur the live path uses.
    static uint8_t backgroundPng[island::kMusicArtworkCapacity] = {};
    const size_t backgroundSize = island::MakeBlurredBackgroundPng(
        sample.artwork, sample.artworkSize, 105, 59, backgroundPng, sizeof(backgroundPng));
    island::DecodedImage iconImage;
    const bool haveIcon =
        sample.iconSize > 0 &&
        island::DecodeImage(sample.icon, sample.iconSize, &iconImage, island::DecodeSlot::Icon);

    island::DecodedImage background;
    const bool haveBackground =
        backgroundSize > 0 && island::DecodeImage(backgroundPng, backgroundSize, &background,
                                                  island::DecodeSlot::Background);

    island::LogAndPrint("music preview: five shapes, 4 seconds each "
                        "(pill, strip+lyric, strip+title, hover, card; decoded %s, wash %s)",
                        haveArtwork ? "yes" : "no", haveBackground ? "yes" : "no");

    // A lyric long enough to need scrolling, so the marquee is exercised rather than
    // sitting still in the middle of the strip.
    const char* lyric =
        "\xe6\x88\x91\xe6\x83\xb3\xe8\xa6\x81\xe5\xb8\xa6\xe7\x9d\x80\xe4\xbd\xa0"
        "\xe5\x8e\xbb\xe6\x88\x91\xe7\x9a\x84\xe5\xb0\x8f\xe5\x9f\x8e\xe5\xb8\x82"
        "\xe7\x9c\x8b\xe6\x97\xa5\xe8\x90\xbd\xe7\x9a\x84\xe6\x97\xb6\xe5\x80\x99";

    // The shapes the user sees, in order: the plain pill, the lyric strip with actual
    // lyrics, the strip falling back to the track NAME (which is what shows when no
    // lyrics arrive -- reported as rendering only a single character), the plain hover
    // shape, and the details card.
    struct Stage {
        island::IslandState state;
        const char* name;
        bool stripShowsFallback;   // strip only: show the title instead of the lyric
    };
    const Stage stages[5] = {
        {island::IslandState::Collapsed, "resting pill", false},
        {island::IslandState::MusicStrip, "strip + lyric", false},
        {island::IslandState::MusicStrip, "strip + title", true},
        {island::IslandState::Hover, "hover", false},
        {island::IslandState::Expanded, "card", false},
    };

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER previous = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previous);

    FrameStats frameStats;
    frameStats.Start();

    float scrollDip = 0.0f;
    for (const Stage& stage : stages) {
        window.SetState(stage.state);
        // Let the transition settle before reporting and photographing the stage.
        for (int i = 0; i < 40; ++i) {
            if (!window.PumpMessages()) break;
            window.Tick(0.02f);
            ::MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }

        // Report whether this shape could actually be hovered by a real pointer, using
        // the same predicate the window uses. A "no" here means the user cannot reach
        // the shape, which is the bug class this preview exists to catch.
        POINT cursor = {};
        GetCursorPos(&cursor);
        const bool acceptsCursor =
            island::IslandWindow::ShouldAcceptPointer(monitor, window.WindowRect(), cursor,
                                                      window.State());
        // Probed at the shape's own centre as well as at the physical cursor: the cursor
        // position only says "the pointer is elsewhere", while the centre says whether the
        // shape is reachable at all. Conflating the two is how "the pointer was never over
        // it" gets mistaken for "unreachable".
        const island::RoundedBox box = window.CurrentBox();
        const RECT windowRect = window.WindowRect();
        const POINT centre = {
            static_cast<LONG>((box.rect.left + box.rect.right) * 0.5f) + windowRect.left,
            static_cast<LONG>((box.rect.top + box.rect.bottom) * 0.5f) + windowRect.top};
        const bool acceptsCentre =
            island::IslandWindow::ShouldAcceptPointer(monitor, windowRect, centre,
                                                      window.State());
        island::LogAndPrint(
            "  music preview: %-13s box x=[%.0f..%.0f] y=[%.0f..%.0f] | "
            "centre (%ld,%ld) accepts=%s | pointer (%ld,%ld) accepts=%s",
            stage.name, box.rect.left, box.rect.right, box.rect.top, box.rect.bottom,
            centre.x, centre.y, acceptsCentre ? "yes" : "NO", cursor.x, cursor.y,
            acceptsCursor ? "yes" : "no");
        std::fflush(stdout);

        // How long a card frame takes to draw, reported once at the end.
        //
        // The card is the only shape that uploads images, and "hovering it feels laggy"
        // was the report. A number makes that falsifiable: the fix caches the GPU bitmaps
        // instead of rebuilding them per frame, and this says by how much.
        renderer.ResetFrameStats();

        frameStats.Reset();

        const DWORD stageStart = GetTickCount();
        while (GetTickCount() - stageStart < 4000) {
            if (!window.PumpMessages()) break;

            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            const double dt = static_cast<double>(now.QuadPart - previous.QuadPart) /
                              static_cast<double>(frequency.QuadPart);
            previous = now;
            const float step = static_cast<float>(dt > 0.1 ? 0.1 : dt);
            window.Tick(step);
            // The window's own region, per frame -- and this is why the card never appeared here before.
            //
            // The region is what CLIPS the window, and the live loop applies it every frame for exactly
            // this reason (its comment says so): a region applied once at the state change keeps the card
            // clipped to the pill's height for as long as the card is open, so the card is composed and
            // then cut off. Harmless to call when nothing moved -- it returns immediately.
            window.ApplyInputRegion();

            // Advance the marquee so the strip demonstrably scrolls.
            scrollDip += step * 26.0f;

            island::FrameState frame;
            frame.pill.box = window.CurrentBox();
            frame.state = window.State();
            frame.clock = (frame.state == island::IslandState::Expanded)
                              ? island::ClockStyle::Card
                              : island::ClockStyle::Compact;
            // The card fades, the bar does not -- see ContentStrengthsFor.
            frame.cardOpacity = window.IsAnimating() ? 0.5f : 1.0f;
            // WHAT THE BAR SHOWS AT REST, which is the field the pill's own line is chosen by: a preview of
            // music has music playing, so its resting state is the lyric strip. The live loop sets this every
            // frame and the preview did not -- and the default is Collapsed, so every "strip" stage here drew
            // the CLOCK while the lyric was only in `frame.lyricLine` (a null line falls through to the clock,
            // see the renderer's pill-line block). That is why a photograph of this stage never showed a lyric.
            frame.restingState = island::IslandState::MusicStrip;
            // The pill's own box, which is NOT the animated one: an outer card's top edge and the bar's
            // own fill are both measured from it (see FrameState::restingPill). The live loop sets it
            // every frame and this preview did not, so a card drawn here had no pill to hang below.
            frame.restingPill = island::BoxForState(island::IslandState::Collapsed,
                                                    window.CurrentAnchor());
            frame.music = (frame.state == island::IslandState::Expanded) ? &sample : nullptr;
            // ... and the card has to be ASKED for, not merely sized.
            //
            // The renderer draws the music card only while `outerCardUp` is set and the frame names it as the
            // owner; the preview set neither, so its "card" stage drew the pill and nothing else. Measured
            // against the stages that do draw: `draw avg 417 us` here against ~1200 us there, and
            // `card layers: total 0 us`. That made a photograph of the card impossible from this tool, which is
            // the one shape whose layout is most often looked at.
            if (stage.state == island::IslandState::Expanded) {
                frame.outerCardUp = true;
                frame.cardOwner = island::CardOwner::Music;
            }
            frame.transportGlyph = ">";
            frame.progressFraction = 0.66f;
            // The strip shows the lyric when there is one, and the track NAME otherwise --
            // which is the case that was reported as rendering only a single character.
            frame.lyricLine = stage.stripShowsFallback ? nullptr : lyric;
            frame.fallbackLine = sample.title;
            frame.lyricScrollDip = scrollDip;

            // The application icons beside the island, in every SHORT stage.
            //
            // Not just the hover stage. The icons are not a hover feature: opening an application
            // adds an icon next to the island and it is there whether or not the pointer is, with
            // hover only widening them. Populating this for Hover alone was part of what made the
            // row look like a hover-only strip.
            if (stage.state == island::IslandState::Collapsed ||
                stage.state == island::IslandState::MusicStrip ||
                stage.state == island::IslandState::Hover) {
                static island::WindowList previewWindows;
                static wchar_t previewTitles[island::kMaxTaskWindows][island::kTaskTitleCapacity];
                static unsigned long long lastPreviewRefresh = 0;
                static int lastPreviewListed = -1;

                // Refreshed on the same timer the live loop uses, not every frame.
                //
                // Refreshing per frame walks 300-odd windows sixty times a second and logs the
                // summary each time: the first version produced four hundred identical lines in one
                // stage and buried everything else in the output.
                if (GetTickCount64() - lastPreviewRefresh >= 500) {
                    lastPreviewRefresh = GetTickCount64();
                    previewWindows.Refresh();
                }

                int listed = previewWindows.Count();
                const int fits = island::TaskIconsThatFit(
                    static_cast<float>(window.Monitor().screen.right -
                                       window.Monitor().screen.left) / window.Scale(),
                    previewWindows.Count());
                listed = fits;

                if (listed != lastPreviewListed) {
                    lastPreviewListed = listed;
                    island::LogAndPrint("      taskbar            %ls",
                                        previewWindows.LastSummary());
                }

                frame.taskButtonCount = listed;
                for (int i = 0; i < listed; ++i) {
                    wcsncpy_s(previewTitles[i], island::kTaskTitleCapacity,
                              previewWindows.At(i).title, _TRUNCATE);
                    frame.taskButtons[i].title = previewTitles[i];
                    frame.taskButtons[i].active =
                        previewWindows.At(i).handle == previewWindows.ForegroundHandle();
                    frame.taskButtons[i].minimised = previewWindows.At(i).iconic;
                    // The renderer resolves the icon from the window; see IconForWindow.
                    frame.taskButtonWindow[i] = previewWindows.At(i).handle;
                    // The island's own colour, so the preview shows what the live bar shows.
                    frame.taskButtons[i].fill = frame.pill.fill;
                }
                // The preview has no pointer, so nothing is pointed at: the row is shown at rest.
                // Forcing an index made the drawing respond to a hover that never happened, which is
                // exactly the kind of preview-versus-live difference that has misled before.
                frame.taskHoveredIndex = -1;
            }

            if (haveIcon && frame.music) {
                frame.iconPixels = iconImage.pixels;
                frame.iconWidth = iconImage.width;
                frame.iconHeight = iconImage.height;
                frame.iconRevision = 1;
            }
            if (frame.music && haveArtwork) {
                frame.artworkPixels = artwork.pixels;
                frame.artworkWidth = artwork.width;
                frame.artworkHeight = artwork.height;
                frame.artworkRevision = 1;
            }
            if (frame.music && haveBackground) {
                frame.backgroundPixels = background.pixels;
                frame.backgroundWidth = background.width;
                frame.backgroundHeight = background.height;
                // The same revision as the cover: one track, one set of images, so the
                // renderer's cache is invalidated once rather than every frame.
                frame.backgroundRevision = 1;
                // The translucent fill the wash needs behind it, as in the live path.
                frame.pill.fill = island::CurrentTheme().pillTop;
            }
            renderer.Render(frame);
            frameStats.Record(/*drew=*/true);

            ::MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }

        // Report what a frame of this shape costs.
        //
        // The per-frame DRAW time is not enough on its own: it says nothing about the
        // INTERVAL between frames, and a stutter is a long interval. Both are reported, plus
        // the worst interval seen, because a hitch is a maximum and not an average.
        island::LogAndPrint("      %-13s %lld frames, draw avg %.0f us, worst %.0f us",
                            stage.name, renderer.FrameCount(),
                            renderer.AverageFrameMicroseconds(),
                            renderer.LastFrameMicroseconds());
        {
            wchar_t summary[256] = {};
            frameStats.Describe(summary, 256);
            island::LogAndPrint("      %-13s interval %ls", stage.name, summary);
        }
        if (stage.state == island::IslandState::Expanded) {
            island::LogAndPrint("      card layers: total %.0f us, background %.0f us, "
                                "cover %.0f us",
                                text.LastCardMicroseconds(),
                                text.LastCardBackgroundMicroseconds(),
                                text.LastCardArtworkMicroseconds());
        }
        std::fflush(stdout);
    }

    renderer.SetTextRenderer(nullptr);
    text.Shutdown();
    window.Destroy();
    return 0;
}

// Fetches lyrics for one track and reports what came back, then exits.
//
// This exists because the fetch cannot be verified from every environment: the
// development sandbox has no outbound TLS at all, so the only place the network path
// can be exercised is a normal shell on the machine, and the live bar would only say
// "none". With this, one command answers whether the service is reachable, whether
// the search matched the right track, and how many lines the parser found.
//
// A leading "[offline]" marker on the output makes it unambiguous which half failed:
// the network or the parsing.
//
// `verbose` also dumps the exact URL requested and the first part of each response, which
// is the only way to tell a 403 apart from an empty result.
int RunLyricsTest(const char* title, const char* artist, bool verbose) {
    island::LogAndPrint("lyrics test: title=\"%s\" artist=\"%s\"",
                        title ? title : "", artist ? artist : "");
    std::fflush(stdout);

    if (!title || title[0] == '\0') {
        island::LogAndPrint("lyrics test: no title given; use --lyrics-test <title> [artist]");
        return 2;
    }

    const DWORD started = GetTickCount();
    static island::Lyrics lyrics;
    static island::LyricsFetchReport report;
    char why[192] = {};
    const bool ok = island::FetchLyricsVerbose(title, artist ? artist : "", 10000, &lyrics,
                                               why, sizeof(why), &report);
    const DWORD elapsed = GetTickCount() - started;

    // The requests and responses first: when something fails, this is the part that says
    // why, and printing it after a success is still useful as confirmation.
    if (verbose) {
        // BOTH services, each with its own URLs, bodies and verdict.
        //
        // The old shape printed a single record under NetEase's name while the reason string named
        // both services, so one service's response could be read against the other's conclusion --
        // which is how "the search found no matching track" came to be believed about a search that
        // had answered 200 with a populated list.
        const auto dump = [](const char* who, const island::LyricsFetchReport::Attempt& attempt) {
            island::LogAndPrint("  %s search: HTTP %d  %s", who, attempt.searchStatus,
                                attempt.searchUrl);
            if (attempt.searchBody[0]) island::LogAndPrint("    <- %.400s", attempt.searchBody);
            island::LogAndPrint("  %s lyric : HTTP %d  %s", who, attempt.lyricStatus,
                                attempt.lyricUrl);
            if (attempt.lyricBody[0]) island::LogAndPrint("    <- %.400s", attempt.lyricBody);
            island::LogAndPrint("  %s verdict: %s", who,
                                attempt.verdict[0] ? attempt.verdict : "(nothing recorded)");
        };
        dump("netease", report.netease);
        dump("qq     ", report.qq);
        std::fflush(stdout);
    }

    if (!ok) {
        island::LogAndPrint("lyrics test: FAILED after %lu ms -- %s", elapsed,
                            why[0] ? why : "no reason recorded");
        return 1;
    }

    island::LogAndPrint("lyrics test: OK in %lu ms, %d lines, [ti:]=\"%s\" [ar:]=\"%s\"",
                        elapsed, lyrics.count, lyrics.title, lyrics.artist);

    // A few lines, with their times, so the parse can be judged rather than trusted.
    const int show = lyrics.count < 6 ? lyrics.count : 6;
    for (int i = 0; i < show; ++i) {
        const island::LyricLine* line = lyrics.At(i);
        if (!line) break;
        island::LogAndPrint("  [%02lld:%02lld.%03lld] %s", line->timeMs / 60000,
                            (line->timeMs / 1000) % 60, line->timeMs % 1000, line->text);
    }
    if (lyrics.count > show) {
        island::LogAndPrint("  ... and %d more", lyrics.count - show);
    }

    // And what the strip would show a third of the way in, which is the value the
    // display actually uses.
    const long long probe = lyrics.lines[lyrics.count - 1].timeMs / 3;
    const island::LyricLine* at = lyrics.At(lyrics.LineAt(probe));
    island::LogAndPrint("  at %lld ms the strip shows: %s", probe, at ? at->text : "(nothing)");
    std::fflush(stdout);
    return 0;
}

} // namespace island
