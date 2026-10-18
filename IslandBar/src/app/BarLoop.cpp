// =====================================================================
//  BarLoop.cpp -- the bar itself: RunBar, the phases of its frame loop, and what they use
//
//  MOVED HERE VERBATIM from main.cpp on 2026-10-16. Nothing was re-typed and nothing was
//  reordered: the phases sit in the order they are called in, RunBar follows them, and the
//  two helpers that only the bar uses (the power list, the CALL icon's decoded pixels) came
//  with it because the compiler would not have let them stay behind unused.
//
//  WHAT DOES NOT BELONG HERE: anything the entry point does before or instead of running the
//  bar -- the console, the per-monitor DPI call, the demo cycle, the argument conversion --
//  and the self-test. Those are main.cpp's, and they are the reason the two files exist.
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
#include "app/BarState.h"
#include "app/BarSystems.h"    // the window, the renderer, and one object per channel     // the bar's own state: slice 1 is the tray card and its menu
#include "shell/StartMenu.h"
#include "domain/TaskStripLayout.h"
#include "platform/WindowList.h"
#include "media/AudioVolume.h"
#include "domain/Geometry.h"
#include "app/IslandWindow.h"
#include "core/FrameStats.h"
#include "core/Log.h"
#include "platform/Monitor.h"
#include <dwmapi.h>
#include <thread>
#include <powrprof.h>   // SetSuspendState, for the Start card's power list
// `security.h` refuses to be included without one of its targets defined (its own #error says so), and
// SECURITY_WIN32 is the one for a user-mode application.
#define SECURITY_WIN32
#include <security.h>   // GetUserNameExW, for the name in that card's footer
#include <sddl.h>       // ConvertSidToStringSidW, to find the account picture
#include "domain/Motion.h"
#include "core/ProcessMetrics.h"   // the memory line: what this process holds, and whether it grows
#include "media/MusicSession.h"
#include "platform/NativeTaskbar.h"
#include "media/Lyrics.h"
#include "services/Notification.h"
#include "services/NotificationIcon.h"
#include "services/NotificationPipe.h"
#include "app/PlayerWindow.h"
#include "services/ToastStore.h"
#include "shell/TrayIcons.h"
#include "shell/TrayLearn.h"
#include "shell/TrayMenu.h"            // what a tray cell does: the press, and the menu the bar draws for it
#include "shell/TrayPress.h"
#include "shell/WallpaperTint.h"
#include "platform/DisplayBrightness.h"   // the brightness row: DDC/CI, the only interface this machine has
#include "shell/SystemToggles.h"       // Bluetooth / night light / live captions: the tiles that switch
#include "render/Theme.h"               // the material: colours and edges, switchable, plus themes\*.txt
#include "services/WeChatWatch.h"
#include "domain/StatePolicy.h"
#include "media/SoundOutput.h"
#include "shell/SystemStatus.h"
#include "media/WicImage.h"
#include "shell/Weather.h"
#include "platform/ThumbnailPreview.h"
#include "render/Renderer.h"

namespace {

// The hover preview's card, from the active theme. platform/ cannot see render/Theme, so the app layer
// hands it three colours: the glass theme gets the island's own refracted average as its body and the
// same PillInk as its title; a painted theme gets its card fill and its ink, which is what those themes are.
island::PreviewStyle PreviewStyleFor(const island::Theme& theme, const D2D1_COLOR_F& glass) {
    const auto rgb = [](const D2D1_COLOR_F& colour) {
        const auto channel = [](float value) {
            const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
            return static_cast<BYTE>(clamped * 255.0f + 0.5f);
        };
        return RGB(channel(colour.r), channel(colour.g), channel(colour.b));
    };
    island::PreviewStyle style;
    style.fill = rgb(theme.lens ? glass : theme.cardFill);
    style.ink = rgb(theme.lens ? island::PillInk(1.0f) : theme.ink);
    style.rim = rgb(theme.lens ? theme.highlight : theme.ink);
    return style;
}

} // namespace

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "secur32.lib")   // GetUserNameExW, for the Start card's footer
#pragma comment(lib, "advapi32.lib")  // ConvertSidToStringSidW, to find the account picture
#pragma comment(lib, "powrprof.lib")  // SetSuspendState, for 睡眠

#include "app/BarLoop.h"

namespace {

// --- the Start card's power list -------------------------------------------------------------------
//
// The five things the shell's own Start offers behind its power button, in the order the card draws them.
// The three that can lose unsaved work are marked as needing a second press: the list appears where the
// user was already reaching for the power button, and signing him out on the way past is not a reasonable
// reading of a mis-click.
enum class PowerAction { Lock = 0, Sleep = 1, SignOut = 2, Restart = 3, ShutDown = 4 };

// WHAT THE ENTRY IS CALLED, and whether it can lose work. They were declared here and defined 5,800 lines
// further down with the tray commands until 2026-10-15, when the move of those commands took the definitions
// with it by accident -- the compiler caught that at link time, which is the reason the definitions live next
// to their declarations now instead of at the other end of the file.
const wchar_t* PowerActionName(PowerAction action) {
    switch (action) {
        case PowerAction::Lock: return L"锁定";
        case PowerAction::Sleep: return L"睡眠";
        case PowerAction::SignOut: return L"注销";
        case PowerAction::Restart: return L"重启";
        case PowerAction::ShutDown: return L"关机";
    }
    return L"?";
}

bool PowerActionLosesWork(PowerAction action) {
    return action == PowerAction::SignOut || action == PowerAction::Restart ||
           action == PowerAction::ShutDown;
}
// Does it. False when Windows refused -- a refusal is reported with the error, because "nothing happened"
// from a power menu is otherwise indistinguishable from a click that never arrived.
bool RunPowerAction(PowerAction action);
// Builds the system panel's tiles.
//
// Ordered the way the shell orders them -- Wi-Fi, Bluetooth, airplane mode -- and assembled here rather
// than in the reader because WHICH tiles exist is a presentation decision, while the reader only reports
// what the machine says.
//
// A tile is shown whenever the machine has the thing, ON or OFF, because the whole point of a tile is
// to be pressed. "Airplane mode" is always shown even though it cannot be toggled from here: on a
// desktop it is permanently off, and hiding it would make the grid's contents depend on which wireless
// hardware happens to be present.
//
// Pure with respect to its inputs, so the tile list can be asserted without a machine to read from.
// `volumeKnown` / `volume` / `muted` are the LIVE reading, not the sampled one.
//
// This is the difference between a slider and a slider that feels broken. The panel's handle used to be
// drawn from `status.volumePercent`, and that reading is taken every two seconds -- so while the handle
// was being dragged it moved in two-second steps, or not at all until the drag was over, which is exactly
// what "声音进度条那个不顺滑" is. `AudioVolume` publishes the new level the instant it is set, on the
// frame that set it, so the handle is under the pointer for the whole of the drag.
// The call strip's avatar: the icon of the process that is IN the call, which on this machine is WeChat itself.
//
// The CALLER's picture is not readable from outside WeChat -- it is inside a self-drawn surface with no
// accessibility tree -- so the strip shows the application's icon in that place. It is the honest stand-in:
// the same icon the notification rows use, and the same one the user already associates with the call.
std::vector<uint8_t> callIconPixels;
int callIconWidth = 0;
int callIconHeight = 0;
unsigned long long callIconRevision = 0;
wchar_t callIconSource[260] = {};

// The executable behind a process id, for the call strip's icon. False when the process has gone or cannot be
// opened, in which case the strip draws a plain dot rather than nothing.
bool ProcessImagePath(unsigned long pid, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = L'\0';
    if (pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD length = static_cast<DWORD>(capacity);
    const bool ok = QueryFullProcessImageNameW(process, 0, out, &length) != 0;
    CloseHandle(process);
    if (!ok) out[0] = L'\0';
    return ok;
}
// The sender's icon, held between frames: the pixels, their size, and the source they came from so the same
// image is not looked up again. File scope because the frame build reads them and the loader fills them, and
// because a notification's icon outlives the message that brought it (the card is drawn while it retracts).
std::vector<uint8_t> notificationIconPixels;

// A CELL WHOSE LEFT PRESS HAD NOTHING TO SEND, waiting for the frame loop to answer it.
//
// The press runs on a worker thread (a delivery waits for the application, and the frame loop must not), and
// the menu belongs to the window on the main thread -- so the worker leaves the cell number here and the next
// frame opens this bar's own menu over it. That menu is the one a right click opens, and it is the deliberate
// answer for a cell nothing is known about: the real click was deleted on 2026-10-15, and a button that does
// nothing would be a worse lie than a menu that says what it can do.
int notificationIconWidth = 0;
int notificationIconHeight = 0;
unsigned long long notificationIconRevision = 0;
wchar_t notificationIconSource[260] = {};
// How long the drawn volume takes to catch up with the reading, in seconds.
//
// Short enough that a click feels immediate and long enough that the eye sees the handle travel: this is the
// "过渡" the volume control was missing. A drag ignores it entirely -- see bar.shownVolume.
constexpr float kVolumeChaseSeconds = 0.12f;
// The slider handle's own size chase: quicker than the volume's, because it answers the POINTER rather
// than a device that has to be asked -- a hover that took 0.12 s to show would feel disconnected.
constexpr float kHandleGrowthSeconds = 0.06f;
int BuildSystemPanel(const island::SystemStatus& status, int installedLayoutCount, bool volumeKnown,
                     float volume, bool muted, island::TextRenderer::SystemPanelText* out,
                     int pageIndex = 0) {
    if (!out) return 0;
    out->tileCount = 0;
    out->volume = volumeKnown ? volume : static_cast<float>(status.volumePercent) / 100.0f;
    out->muted = volumeKnown ? muted : status.muted;

    // The whole set is built here and the requested PAGE is copied out of it, because the shell's flyout pages
    // through its tiles and a page is a window over the set rather than a different set.
    island::TextRenderer::SystemPanelTile all[island::kMaxSystemPanelTiles] = {};
    int allCount = 0;
    const int perPage = island::kMaxSystemTiles;

    // `stateText` is what the caption under the box says when it is not the name -- the shell's own flyout
    // captions its Wi-Fi tile "可用" rather than "Wi-Fi", because the glyph above it already names the control.
    // Chinese, because that is the flyout this panel sits next to and the strings here are user-facing.
    // `chevron` is passed per tile rather than derived from the action, because the two answer different
    // questions: the arrow means "this opens a LIST", while the action means "this does something". The
    // shell's flyout has tiles of both kinds -- Wi-Fi and Bluetooth lead to their own flyouts and carry the
    // arrow, 飞行模式 and 实时字幕 switch in place and carry none -- and it has tiles with an arrow whose
    // list opens INSIDE the panel, which is not a settings page at all.
    const auto addTile = [&](const wchar_t* label, const wchar_t* stateText,
                             island::TextRenderer::IndicatorIcon icon, int iconValue, bool on,
                             bool chevron, island::TextRenderer::PanelAction action,
                             const wchar_t* uri) {
        if (allCount >= island::kMaxSystemPanelTiles) return;
        island::TextRenderer::SystemPanelTile& tile = all[allCount++];
        tile.label = label;
        tile.stateText = stateText;
        tile.icon = icon;
        tile.iconValue = iconValue;
        tile.on = on;
        tile.chevron = chevron;
        tile.action = action;
        tile.uri = uri;
    };

    // Wi-Fi. A TOGGLE, like its neighbours and like the shell's own tile, and the caption says what the
    // RADIO is doing rather than what the connection is doing:
    //
    //     已关闭   the radio is off -- pressing the tile turns Wi-Fi back on
    //     可用     on and associated
    //     未连接   on but not associated (this machine, which reaches the network over Ethernet)
    //
    // The tile lights up when the RADIO is on, which is the state the press changes; it used to light only
    // when ASSOCIATED, so on a machine wired to Ethernet the Wi-Fi tile sat dark next to a lit Bluetooth
    // tile with the same caption under both. Reported as "wifi的显示有问题".
    {
        const bool connected = status.network == island::NetworkKind::Connected;
        const wchar_t* label = status.network == island::NetworkKind::None
                                   ? L"No Wi-Fi"
                                   : (connected ? L"Wi-Fi" : L"Wi-Fi");
        const wchar_t* state = status.network == island::NetworkKind::None
                                   ? L"\u65E0 Wi-Fi"          // 无 Wi-Fi
                                   : (connected ? L"\u53EF\u7528"          // 可用
                                                : L"\u672A\u8FDE\u63A5"); // 未连接
        // A radio that has been switched off is not "not connected": it is off, and saying 未连接 for it
        // would leave the user looking for a network problem that is a switch.
        if (!status.wifiRadioOn) state = L"\u5DF2\u5173\u95ED";   // 已关闭
        addTile(label, state, island::TextRenderer::IndicatorIcon::Network,
                static_cast<int>(status.signal), status.wifiRadioOn, false,
                island::TextRenderer::PanelAction::ToggleWifi, nullptr);
    }

    // Bluetooth. Captioned with the CONNECTION state rather than the radio's, which is what the shell's own
    // flyout prints (its Bluetooth tile reads 未连接 with the radio on and nothing paired). Whether a device
    // is actually attached is not readable from here -- the audio endpoint would answer for headphones only
    // -- so the honest pair is 关 when the radio is off and 未连接 when it is on.
    //
    // IT IS A TOGGLE NOW. The note that used to be here said Bluetooth had "no documented interface a
    // desktop process can use", and that was wrong: Windows.Devices.Radios answers Allowed for this
    // process and the radio went On -> Off -> On (measured, scripts/probe-radios.ps1). Pressing the tile
    // switches the radio, and the ARROW IS GONE with the page it used to open: the arrow means "this opens
    // a LIST" (see SystemPanelTile::chevron), and a tile that switches in place must not promise one. It
    // was kept one round longer than the behaviour it described, which is exactly the drift the note above
    // about arrows was written to prevent.
    addTile(L"Bluetooth", status.bluetoothOn ? L"\u672A\u8FDE\u63A5" : L"\u5173",   // 未连接 / 关
            island::TextRenderer::IndicatorIcon::Bluetooth, status.bluetoothOn ? 1 : 0,
            status.bluetoothOn, false,
            island::TextRenderer::PanelAction::ToggleBluetooth, nullptr);

    // 飞行模式 -- BOTH radios now, which is what airplane mode means in the shell's own flyout.
    //
    // It used to switch Wi-Fi alone, and the log said so, because Bluetooth had "no documented interface".
    // That is no longer true (Windows.Devices.Radios, measured), so switching one radio and calling it
    // airplane mode would be the half-truth this panel was avoiding. The tile lights up when BOTH radios
    // are off, which is the same reading the Wi-Fi and Bluetooth tiles beside it show.
    //
    // Captioned with its NAME and not with 开/关, which is how the shell has this tile: the state is the
    // tile's own highlight.
    addTile(L"\u98DE\u884C\u6A21\u5F0F", nullptr,   // 飞行模式
            island::TextRenderer::IndicatorIcon::Airplane, 0,
            !status.wifiRadioOn && !status.bluetoothOn, false,
            island::TextRenderer::PanelAction::ToggleRadio, nullptr);

    // --- the second row ---------------------------------------------------------------
    //
    // These are the shell's own second row. Two of them have no readable state on this machine and are
    // shown OFF with a page to open, which is honest: the tile still does something useful (it takes the
    // user where the setting lives) without claiming to know what the setting is.
    //
    // Accessibility is a LAUNCHER, not a toggle. The shell opens a flyout of individual switches
    // (magnifier, colour filters, narrator); this opens the page those switches live on, which is the
    // same destination one level up and needs no state of its own.
    // ... and it DOES carry the arrow: the shell opens a list of the individual switches (magnifier, colour
    // filters, narrator) under it. This one opens the page those switches live on, which is the same
    // destination one level up.
    addTile(L"\u8F85\u52A9\u529F\u80FD", nullptr,   // 辅助功能
            island::TextRenderer::IndicatorIcon::Accessibility, 0, false, true,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:easeofaccess");

    // 节能模式 -- battery saver, which is what the shell calls this tile. Its state IS readable
    // (GetSystemPowerStatus reports it in SystemStatusFlag), so the tile reflects the machine; turning it on
    // and off is a power-scheme write that is not implemented, so the page is opened instead of the tile
    // pretending to toggle. Named in Chinese like the tiles around it, which is how the shell's flyout reads.
    addTile(L"\u8282\u80FD\u6A21\u5F0F", status.batterySaverOn ? L"\u5F00" : L"\u5173",   // 节能模式, 开 / 关
            island::TextRenderer::IndicatorIcon::BatterySaver, 0, status.batterySaverOn, false,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:batterysaver");

    // Live captions. A TOGGLE now, driven by the shortcut the feature itself is switched by: measured,
    // Win+Ctrl+L starts LiveCaptions.exe and the same chord stops it again. The tile lights up while the
    // process is running, which is the only state the feature has.
    addTile(L"\u5B9E\u65F6\u5B57\u5E55", status.liveCaptionsOn ? L"\u5F00" : L"\u5173",   // 实时字幕
            island::TextRenderer::IndicatorIcon::Captions, 0, status.liveCaptionsOn, false,
            island::TextRenderer::PanelAction::ToggleLiveCaptions, nullptr);

    // --- the second page -------------------------------------------------------------
    //
    // The shell's own flyout scrolls to a second row of tiles; these are the ones people reach for next, and
    // each is an honest "take me where this lives" tile rather than a toggle this process cannot perform. No
    // state on any of them, because none of these settings can be READ from here -- a tile that guessed would
    // be worse than one that simply opens the page.
    // 夜间模式 -- a TOGGLE, and the most expensive one on the panel: Windows has no API for it, and writing
    // the state it keeps in its own registry blob does NOT switch it (measured: the Settings page still
    // offered "turn it on now" with the on-value written, after reopening the page). What does switch it is
    // the Settings page's own switch, which this presses through UI Automation -- so the cost is a Settings
    // window on screen for a couple of seconds, and the bar says so in the log rather than pretending
    // otherwise. The state is read from that registry blob, which is what the page itself shows.
    addTile(L"\u591C\u95F4\u6A21\u5F0F", status.nightLightOn ? L"\u5F00" : L"\u5173",   // 夜间模式
            island::TextRenderer::IndicatorIcon::Moon, 0, status.nightLightOn, false,
            island::TextRenderer::PanelAction::ToggleNightLight, nullptr);
    addTile(L"\u79FB\u52A8\u70ED\u70B9", nullptr,             // 移动热点
            island::TextRenderer::IndicatorIcon::Hotspot, 0, false, false,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:network-mobilehotspot");
    addTile(L"\u5C31\u8FD1\u5171\u4EAB", nullptr,             // 就近共享
            island::TextRenderer::IndicatorIcon::Share, 0, false, false,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:nearby-sharing");

    // 有线显示器 and 投影, the two tiles that carry the arrow on the shell's own second page: both open a
    // LIST of display modes rather than switching anything in place.
    //
    // 有线显示器 opens the display page -- the list of modes lives there. 投影 sends Win+P, which is the
    // shell's own projection flyout; there is no settings URI for it, and sending the key is exactly what
    // pressing that tile in the shell does.
    addTile(L"\u6709\u7EBF\u663E\u793A\u5668", nullptr,       // 有线显示器
            island::TextRenderer::IndicatorIcon::WiredDisplay, 0, false, true,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:display");
    addTile(L"\u6295\u5F71", nullptr,                         // 投影
            island::TextRenderer::IndicatorIcon::Projection, 0, false, true,
            island::TextRenderer::PanelAction::ProjectionModes, nullptr);

    // --- beyond the shell's own set ---------------------------------------------------
    //
    // Everything the shell's two pages hold is above; these are the tiles this bar has on top of them, and
    // they go after the shell's last one so the two pages the reference pictures show stay exactly as they
    // are. Both are conditional, so on most machines the panel simply ends after 投影.
    //
    // The input method, when there is more than one layout to switch between -- the same rule the indicator
    // row uses, so the two cannot disagree about whether it exists.
    if (island::ShouldDrawInputMethod(status, installedLayoutCount)) {
        // Static, not a local: the tile keeps the pointer until the draw, later in this same frame, and a
        // stack buffer would be gone by then. (The battery percentage below has the same shape.)
        static wchar_t label[8] = {};
        island::InputMethodLabel(status.inputLanguageId, label, 8);
        addTile(label, nullptr, island::TextRenderer::IndicatorIcon::InputMethod, 0, true, false,
                island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:typing");
    }

    // 外观 -- the material, as a list the user can walk (Theme.h). On the panel's second page with the
    // other "how the bar looks" controls rather than among the system switches, and captioned with the
    // theme that is IN FORCE, so the tile answers "which one am I on" without opening anything.
    addTile(L"\u5916\u89C2", island::CurrentTheme().name,   // 外观
            island::TextRenderer::IndicatorIcon::Settings, 0, false, false,
            island::TextRenderer::PanelAction::CycleTheme, nullptr);

    if (island::ShouldDrawBattery(status)) {
        static wchar_t percent[8] = {};
        percent[0] = L'\0';
        std::swprintf(percent, 8, L"%d%%", status.batteryPercent);
        addTile(L"Battery", percent, island::TextRenderer::IndicatorIcon::Battery,
                status.batteryPercent, status.onAcPower || status.batteryPercent > 20, false,
                island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:batterysaver");
    }

    // Copy the requested page out of the set.
    const int pages = (allCount + perPage - 1) / perPage;
    out->pageCount = pages > 0 ? pages : 1;
    out->pageIndex = pageIndex >= 0 && pageIndex < out->pageCount ? pageIndex : 0;
    const int first = out->pageIndex * perPage;
    for (int i = 0; i < perPage && first + i < allCount; ++i) {
        out->tiles[i] = all[first + i];
        out->tileCount = i + 1;
    }
    return out->tileCount;
}

// Fills the 声音输出 page's text from an audio snapshot.
//
// The names in the result are POINTERS into `snapshot`, which the caller keeps in the same scope as the
// frame that is drawn from it -- the page's rows cannot own their strings, because they are rebuilt every
// frame and a copy per row per frame would be work for nothing.
//
// The mixer's FIRST row is the output device itself, which is how the shell's own mixer is built: the
// device's own volume at the top, then one row per application. Its volume comes from the master reading
// rather than from the session list, because the device is not a session.
void BuildSoundPage(const island::SoundOutputSnapshot& snapshot, const island::SystemStatus& status,
                    bool haveMasterLevel, float masterLevel, bool masterMuted, float scroll,
                    island::TextRenderer::SoundPageText* out) {
    if (!out) return;
    *out = island::TextRenderer::SoundPageText{};

    // The page's own words, exactly as the shell's page prints them.
    out->title = L"\u58F0\u97F3\u8F93\u51FA";                       // 声音输出
    out->deviceLabel = L"\u8F93\u51FA\u8BBE\u5907";                 // 输出设备
    out->spatialLabel = L"\u7A7A\u95F4\u97F3\u6548";                // 空间音效
    out->mixerLabel = L"\u97F3\u91CF\u5408\u6210\u5668";            // 音量合成器
    out->footer = L"\u66F4\u591A\u97F3\u91CF\u8BBE\u7F6E";          // 更多音量设置

    // 空间音效. The first row is the one in use: turning spatial sound off is the default on this machine
    // and the setting itself is not readable or writable through any public interface, so the page shows
    // the shell's two choices with the safer one marked and reports honestly when one is pressed.
    out->spatial[0].label = L"\u5173";                              // 关
    out->spatial[0].selected = true;
    out->spatial[1].label = L"\u7528\u4E8E\u8033\u673A\u7684 Windows Sonic";   // 用于耳机的 Windows Sonic
    out->spatial[1].selected = false;

    for (int i = 0; i < snapshot.deviceCount && i < island::kMaxSoundDevices; ++i) {
        out->devices[i].name = snapshot.devices[i].name;
        out->devices[i].formFactor = snapshot.devices[i].formFactor;
        out->devices[i].isDefault = snapshot.devices[i].isDefault;
        out->deviceCount = i + 1;
    }

    int row = 0;
    {
        island::TextRenderer::SoundPageText::MixRow& device = out->mix[row];
        // The short name -- "扬声器" out of "扬声器 (Realtek(R) Audio)" -- which is what the shell prints on
        // this row, because the long name would not fit and every device's long name says the same thing.
        device.label = snapshot.defaultDeviceName[0] ? snapshot.defaultDeviceName : L"Output device";
        device.volume = haveMasterLevel ? masterLevel
                                        : static_cast<float>(status.volumePercent) / 100.0f;
        device.muted = haveMasterLevel ? masterMuted : status.muted;
        ++row;
    }
    for (int i = 0; i < snapshot.sessionCount && row < island::TextRenderer::SoundPageText::kMaxMixRows;
         ++i) {
        const island::SoundSessionInfo& session = snapshot.sessions[i];
        island::TextRenderer::SoundPageText::MixRow& mix = out->mix[row];
        mix.label = session.name;
        mix.volume = session.volume;
        mix.muted = false;
        if (session.hasIcon && session.iconSize > 0) {
            mix.iconPng = session.icon;
            mix.iconSize = session.iconSize;
            mix.iconRevision = session.iconRevision;
        }
        ++row;
    }
    out->mixCount = row;
    out->scroll = scroll;
}

// =====================================================================
//  THE STARTUP, ONE PHASE AT A TIME (2026-10-16)
//
//  WHAT THESE ARE, AND WHAT THEY ARE NOT. Each one is a contiguous block that was ALREADY inside RunBar,
//  moved out unchanged: not one statement reordered, not one value recomputed, not one comment rewritten.
//  The reason is that RunBar is a 5,200-line function -- the last piece of structure debt named in
//  ARCHITECTURE.md section 7 -- and a phase that has a name can be read and tested; the same code as the
//  middle third of a function cannot.
//
//  HOW THEY ARE WRITTEN, and this is the whole safety argument: explicit parameters, so a block that
//  reaches for something it was not handed does not compile. That is the only check that has ever caught
//  this class of mistake here -- "a field set after the thing that reads it" and "the outer variable with
//  the same name" have each cost a round (see the notes in RunBar itself and in app/BarState.h). Nothing is
//  stored in a new struct for the move: state keeps its owner.
//
//  ACCEPTANCE for every one of these: both configurations build with zero warnings, --self-test passes, and
//  scripts\regression\normalize-log.ps1 reports ZERO lines of difference against the baseline. A phase moved
//  twice, or moved to the wrong place in the sequence, changes that log.
// =====================================================================

// --- the shell's taskbar: what happens to it, and the report of what happened ---
//
// Moved out of RunBar verbatim. It says out loud which of the three modes the taskbar is in, and why
// nothing happened when nothing happened: a lower integrity level than the shell's makes every one of these
// calls return 0 with no error, which is the failure that has cost the most time on this machine.
static void ApplyNativeTaskbarMode(bool hideNativeTaskbar, bool keepNativeTray,
                                   island::BarSystems& systems) {
    // Hiding the shell's taskbar is the point of the exercise, but it is also the
    // only thing this program does that the user can see outside its own window,
    // so it is announced and it is always undone on the way out.
    //
    // If this process is killed rather than closed, the restore never runs. The
    // documented recovery then is to restart Explorer, or to run:
    //   ShowWindow(FindWindow("Shell_TrayWnd", NULL), SW_SHOW)
    if (hideNativeTaskbar) {
        systems.haveTaskbar = systems.taskbar.Locate();
        if (systems.haveTaskbar) {
            // WHICH OF THE THREE THINGS HAPPENS TO THE SHELL'S TASKBAR is decided here, before the
            // first Hide(): the object remembers the mode for every later call (see NativeTaskbar.h).
            systems.taskbar.SetKeepTrayVisible(keepNativeTray);
            // Check the integrity level as well as the result. When this process is
            // confined to a lower integrity level than the shell, every method of
            // hiding the taskbar is silently dropped by UIPI: the calls return 0,
            // no error is reported, and the taskbar simply stays. Saying so up
            // front is the difference between a two-minute fix and an afternoon of
            // suspecting the code.
            const bool blocked = systems.taskbar.IsBlockedByIntegrity();
            const bool hidden = systems.taskbar.Hide();

            if (hidden && !blocked && keepNativeTray) {
                // The tray is the shell's own: its icons, its menus, its overflow, its clock. Nothing
                // about it is drawn or simulated here -- see HANDOFF 8.133 for why this is the only
                // route that survives measurement.
                island::LogAndPrint("  native taskbar: clipped to the tray -- the REAL system tray"
                                    " stays on screen");
                island::LogAndPrint("  (the rest of the taskbar is cut away by a window region;"
                                    " --hide-taskbar goes back to hiding it)");
            } else if (hidden && !blocked) {
                island::LogAndPrint("  native taskbar: hidden");
                island::LogAndPrint("  (restored on exit; if this process is killed"
                                    " instead, restart Explorer)");
            } else if (blocked && keepNativeTray) {
                island::LogAndPrint("  native taskbar: CANNOT KEEP THE TRAY VISIBLE from this process");
                island::LogAndPrint("    (see --probe-taskbar: a lower integrity level than the shell's"
                                    " is dropped by UIPI)");
            } else if (blocked) {
                island::LogAndPrint("  native taskbar: CANNOT BE HIDDEN from this process");
                island::LogAndPrint("    island_bar.exe runs at a LOWER integrity level than the");
                island::LogAndPrint("    shell's taskbar, so Windows drops the request silently");
                island::LogAndPrint("    (UIPI: no error, the call just returns 0).");
                island::LogAndPrint("    FIX: start island_bar.exe from your own shell -- Win+R, or a");
                island::LogAndPrint("    PowerShell/cmd window opened from the Start menu -- NOT from");
                island::LogAndPrint("    inside a sandbox, an IDE, or a restricted launcher.");
                island::LogAndPrint("    Confirm with --probe-taskbar there before starting the bar.");
            } else {
                island::LogAndPrint("  native taskbar: could NOT be hidden (reason unknown;"
                                    " try --probe-taskbar)");
            }
        } else {
            island::LogAndPrint("  native taskbar: not found (nothing to hide)");
        }
    } else {
        island::LogAndPrint("  native taskbar: left alone (--keep-taskbar)");
    }
}

// --- the window, and the strip the desktop gives up for it ---
//
// Moved out of RunBar verbatim, with one deliberate exception: creating the window can fail, and what to do
// about that -- say so, put the taskbar back, leave -- stays in RunBar with the startup's other exits, so
// this returns false instead of returning 1 from a nested function. Nothing between the failing Create and
// the GetLastError() in RunBar calls into Windows again, which is what keeps that number meaningful.
static bool CreateBarWindowAndClaimAppBar(bool claimAppBar, const island::MonitorGeometry& monitor,
                                          LONG barThicknessPx, island::BarSystems& systems) {
    // --- the AppBar: claim the strip so the desktop gives up the space -----
    //
    // This is what makes the island a TASKBAR rather than a floating decoration. Without it a
    // maximized window still covers the top of the screen and the island sits on its title bar;
    // with it the work area starts below the island, exactly as it does for the real taskbar.
    //
    // Claimed AFTER the window exists (the shell needs an HWND that is a real top-level window)
    // but the window is NOT repositioned afterwards: the pill is anchored to the window's own
    // top edge and does not read the work area, so a later work-area change cannot move it. That
    // independence is deliberate -- it is why the tests can assert the pill's position without
    // knowing anything about bars.
    // `barThicknessPx` is RunBar's: it is the same measurement the full-screen exit re-claims with, so it
    // stays where both uses can see it and comes in as a parameter (see the call).
    if (!systems.window.Create(monitor, /*show=*/true)) return false;

    systems.window.SetAppBar(&systems.appBar);

    if (!claimAppBar) {
        // Off by default, and that is a deliberate safety decision rather than an unfinished
        // feature.
        //
        // Registering as an AppBar rewrites the desktop's work area, and every maximized window
        // follows it. A first attempt at this moved every window on the machine -- including the
        // island, because the window positioned itself from the work area it was changing -- to
        // the bottom of the screen, and no error was reported anywhere. It cannot be reproduced
        // from the development sandbox, where the request is refused outright as
        // "access denied" before it can do anything.
        //
        // So it is opt-in until it has been seen to work on a real desktop, and the code that
        // performs it verifies the resulting work area and gives the space back if it did not land
        // where it was asked to. --appbar turns it on.
        island::LogAndPrint("  appbar: not claimed (pass --appbar to claim the strip; the space"
                            " is only reserved when asked)");
    } else {
        // This block is only reached when the native bar is being hidden -- the
        // --keep-taskbar case skips the branch above entirely -- so there is no guard here.
        //
        // Claiming the strip while the shell's own taskbar is on screen cannot work: it already
        // owns the top edge. A refusal is reported rather than retried, because retrying cannot
        // change the answer.
        systems.appBarResult = systems.appBar.Claim(systems.window.Handle(), island::DockEdge::Top, barThicknessPx);
        island::LogAndPrint("  appbar: %ls -- %ls", island::AppBarResultName(systems.appBarResult),
                            systems.appBar.LastDetail());

        // Each outcome needs a different response, which is why they are distinguished rather
        // than collapsed into a bool. SpaceNotGranted is the interesting one: the registration
        // stands, so the space may become available once the shell's own bar is gone, and the
        // shell says so with ABN_POSCHANGED.
        if (systems.appBarResult == island::AppBarResult::NotRegistered) {
            island::LogAndPrint("    The shell refused to register another bar on this edge;");
            island::LogAndPrint("    that is expected while the native taskbar still owns it.");
        } else if (systems.appBarResult == island::AppBarResult::SpaceNotGranted) {
            island::LogAndPrint("    Registered, but the desktop did not give up the strip yet.");
            island::LogAndPrint("    It is claimed again automatically on ABN_POSCHANGED.");
        }
        std::fflush(stdout);
    }
    return true;
}

// --- the two notification channels: the local pipe, and the shell's own toast database ---
//
// Moved out of RunBar verbatim, with the two results coming back by reference instead of being declared
// here: the frame loop reads them, so they stay RunBar's locals (see the note on the phase functions).
static void StartNotificationChannels(bool& listening, bool& toastsAvailable, island::BarSystems& systems) {
    // Started before the loop so a sender that connects immediately is served
    // rather than refused. A failure here is reported and the bar keeps working:
    // the clock and the hover states do not depend on notifications.
    listening = systems.notificationServer.Start();
    island::LogAndPrint(listening ? "  notifications: listening on \\\\.\\pipe\\%ls"
                                  : "  notifications: DISABLED (could not create the pipe)",
                        island::kPipeName);

    // The SECOND notification channel: Windows' own toasts, read out of the database the notification centre
    // uses. Polled on the frame loop's slow timer rather than by a thread of its own -- one indexed SELECT
    // every couple of seconds costs less than a thread would -- and a failure to read it leaves the bar exactly
    // as it was, because the push channel does not depend on it.
    toastsAvailable = systems.toasts.Start();
    if (!toastsAvailable) {
        island::LogAndPrint("  notifications: system toasts are NOT being read (see above)");
    }
}

// Declared here because the policy's context asks it long before the call strip's own code below.
static bool IsCallNotification(const island::BarSystems& systems,
                               const island::Notification* active);
// ... and the notification click asks for this one long before its definition near the call strip.
static HWND WindowForNotification(const island::WindowList& windows,
                                  const island::Notification* active);

// --- the two media sources: the system session, and the player that never publishes one ---
//
// Moved out of RunBar verbatim, with the two results coming back by reference (the frame loop reads them).
// The two environment switches are read HERE and stay local: nothing outside this block asks about them.
static void StartMediaReaders(bool& musicAvailable, bool& playerWindowAvailable,
                              island::BarSystems& systems) {
    // --- what is playing ---
    //
    // Polled on a worker thread; the frame loop only reads a snapshot. A failure
    // here is not fatal: the bar is a taskbar replacement, not a music player.
    //
    // ISLANDBAR_NO_MEDIA=1 leaves the reader unstarted, which is how the bar looks on a machine
    // where nothing is playing -- a state that cannot otherwise be photographed on this one, whose
    // owner keeps a player running all day. It is the switch the idle island's own line needs: with
    // no session the pill shows the date and the time, and with one it shows the lyric and the line
    // stands beside the deck (see StatusPlacementFor).
    wchar_t noMediaSetting[8] = {};
    const bool noMedia = GetEnvironmentVariableW(L"ISLANDBAR_NO_MEDIA", noMediaSetting, 8) > 0 &&
                         noMediaSetting[0] == L'1';
    // ISLANDBAR_NO_SESSION=1 leaves the SESSION reader unstarted while the window-title source below
    // stays on. That is the switch for exercising the fallback on a machine whose player does publish
    // a session -- this one, since NetEase's own switch was turned on (HANDOFF 8.72) -- because the
    // fallback only reads while there is no session at all.
    wchar_t noSessionSetting[8] = {};
    const bool noSession = GetEnvironmentVariableW(L"ISLANDBAR_NO_SESSION", noSessionSetting, 8) > 0 &&
                           noSessionSetting[0] == L'1';
    musicAvailable = !noMedia && !noSession && systems.music.Start();
    island::LogAndPrint(musicAvailable
                            ? "  music: reading the system media session"
                            : (noMedia ? "  music: DISABLED BY ISLANDBAR_NO_MEDIA (as if nothing were"
                                         " playing)"
                                       : (noSession ? "  music: the session reader is DISABLED BY"
                                                      " ISLANDBAR_NO_SESSION (the window-title source"
                                                      " below is what is left)"
                                                    : "  music: DISABLED (the media API is"
                                                      " unavailable)")));
    std::fflush(stdout);

    // --- the player that never publishes a session ------------------------------
    //
    // Its own window title is then the only thing it says out loud, and that is enough for the words
    // (the lyrics come from the online lyric APIs by title and artist). Read ONLY while there is no
    // session: a session carries the cover, the position and the transport keys as well, and it wins
    // every time. See PlayerWindow.h for what this source cannot do.
    //
    // Off under ISLANDBAR_NO_MEDIA, which means "as if nothing were playing" -- the idle island's own
    // screenshot depends on that meaning exactly what it says.
    playerWindowAvailable = !noMedia && systems.playerWindow.Start();
    island::LogAndPrint(playerWindowAvailable
                            ? "  music: player window titles are watched too (used only while no"
                              " session exists)"
                            : "  music: player window titles DISABLED");
    std::fflush(stdout);
}

// --- the output side: the volume reader, the sound page's own data, and the lyric fetcher ---
//
// Moved out of RunBar verbatim, with the three results coming back by reference (the frame loop reads them).
static void StartOutputWorkers(bool& volumeAvailable, bool& soundOutputStarted, bool& lyricsAvailable,
                               island::BarSystems& systems) {
    // --- the output volume ---
    //
    // On its own worker, because IMMDeviceEnumerator touches the audio service and can block
    // for tens of milliseconds; blocking the frame loop is exactly what the stutter reports
    // were about.
    volumeAvailable = systems.volume.Start();
    island::LogAndPrint(volumeAvailable ? "  volume: reading the default output endpoint"
                                        : "  volume: DISABLED (no audio endpoint)");
    std::fflush(stdout);

    // --- the 声音输出 page's own data -------------------------------------
    //
    // The output devices and the per-application mixer, on a worker of its own for the same reason the
    // volume is: enumeration touches the audio service, and an application's icon comes off the disk.
    //
    // On the HEAP, because the object carries the mixer's icons and a whole snapshot of the list -- tens of
    // kilobytes -- and this function's own frame is not the place for that.
    //
    // STARTED AT STARTUP, not when the 声音输出 page is first opened, and that changed when the WeChat channel
    // was added: its SOUND signal is read from the per-application session list, and that signal has to work
    // whether or not anybody has ever looked at the mixer. (It did not, silently: the page's lazy start meant
    // the list was never read at all, and the sound channel sat there with nothing to look at.)
    soundOutputStarted = systems.soundOutput->Start();
    island::LogAndPrint(soundOutputStarted
                            ? "  sound: audio sessions are being read (for the mixer and the WeChat channel)"
                            : "  sound: audio sessions are NOT readable; the mixer and the WeChat sound"
                              " channel are unavailable");
    std::fflush(stdout);

    lyricsAvailable = systems.lyrics.Start();
    island::LogAndPrint(lyricsAvailable
                            ? "  lyrics: on demand, NetEase then QQ Music"
                            : "  lyrics: DISABLED (the worker could not start)");
    std::fflush(stdout);
}

// --- the Start card's own state: the application list, the pins, the account picture and name ---
//
// Moved out of RunBar verbatim. It writes into `bar` (the account name and the avatar PNG are BarState's)
// and starts a worker against `systems.startApps`; nothing new is stored here, and the two objects come in
// by reference exactly as RunBar has them.
static void StartStartCard(island::BarState& bar, island::BarSystems& systems) {
    // The shell's application list is read on a WORKER: measured, 121 items cost about 600 ms, and the
    // frame loop cannot spend that. Until it arrives the card has no applications to offer, which is a
    // state the drawing handles (nothing to add yet) rather than something to wait for.
    //
    // The pins are the user's, in a text file beside the bar (see StartApps). The account picture and the
    // name are read once: they are the two things in the card's footer and they change about as often as
    // the user changes his picture.
    // The pins are read on a worker, and it is handed the object BY REFERENCE under its own name: a capture
    // list takes names, not member accesses (see the same trap in the tray learning pass), and naming it
    // `apps` also says what the thread is for.
    std::thread([&apps = systems.startApps]() {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        apps.RefreshList();
        if (SUCCEEDED(comHr)) CoUninitialize();
    }).detach();
    {
        // The account picture: the registry entry the shell itself uses, largest first so a scaled-down
        // draw has something to work with, with the generic picture as the fallback every account has.
        wchar_t user[128] = {};
        DWORD userSize = sizeof(user);
        if (GetUserNameW(user, &userSize)) {
            wcsncpy_s(bar.accountName, 64, user, _TRUNCATE);
        }
        // GetUserNameEx(NameDisplay) is the full name the shell shows in its own footer, which on this
        // machine is not the account name at all; the account name is the fallback.
        wchar_t display[128] = {};
        ULONG displaySize = 128;
        if (GetUserNameExW(NameDisplay, display, &displaySize) && display[0] != L'\0') {
            wcsncpy_s(bar.accountName, 64, display, _TRUNCATE);
        }
        wchar_t sidText[128] = {};
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::vector<uint8_t> buffer(size);
            if (size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
                const TOKEN_USER* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
                LPWSTR sidString = nullptr;
                if (ConvertSidToStringSidW(tokenUser->User.Sid, &sidString) && sidString) {
                    wcsncpy_s(sidText, 128, sidString, _TRUNCATE);
                    LocalFree(sidString);
                }
            }
            CloseHandle(token);
        }
        wchar_t picture[512] = {};
        if (sidText[0] != L'\0') {
            const std::wstring key =
                std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\") +
                sidText;
            for (const wchar_t* value : {L"Image240", L"Image192", L"Image96"}) {
                HKEY handle = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_QUERY_VALUE, &handle) !=
                    ERROR_SUCCESS) {
                    break;
                }
                DWORD type = 0;
                DWORD bytes = sizeof(picture);
                const LONG read = RegQueryValueExW(handle, value, nullptr, &type,
                                                  reinterpret_cast<BYTE*>(picture), &bytes);
                RegCloseKey(handle);
                if (read == ERROR_SUCCESS && type == REG_SZ && picture[0] != L'\0') break;
                picture[0] = L'\0';
            }
        }
        // The generic picture if the account has none: a blank circle would be worse than a silhouette.
        if (picture[0] == L'\0') {
            wcsncpy_s(picture, 512, L"C:\\ProgramData\\Microsoft\\User Account Pictures\\user-192.png",
                      _TRUNCATE);
        }
        // DECODED AS AN IMAGE, not asked for as a file's icon.
        //
        // The first version went through the notification-icon loader, which answers with the SHELL's icon
        // for a path when it cannot read it as a picture -- and the footer then showed a generic
        // image-file icon (a little photograph) where the account picture belongs. Photographed, which is
        // how it was caught. WIC reads the JPEG the account picture actually is.
        std::vector<uint8_t> fileBytes;
        if (FILE* file = nullptr; _wfopen_s(&file, picture, L"rb") == 0 && file) {
            std::fseek(file, 0, SEEK_END);
            const long length = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            if (length > 0 && length < 32 * 1024 * 1024) {
                fileBytes.resize(static_cast<size_t>(length));
                const size_t read = std::fread(fileBytes.data(), 1, fileBytes.size(), file);
                fileBytes.resize(read);
            }
            std::fclose(file);
        }
        island::DecodedImage avatar;
        if (!fileBytes.empty() && island::DecodeImage(fileBytes.data(), fileBytes.size(), &avatar) &&
            avatar.pixels && avatar.width > 0) {
            bar.accountAvatarPng.resize(512 * 1024);
            const size_t size = island::EncodeBgraToPng(avatar.pixels, avatar.width, avatar.height,
                                                       bar.accountAvatarPng.data(),
                                                       bar.accountAvatarPng.size());
            bar.accountAvatarPng.resize(size);
            island::LogAndPrint("  start: account picture %dx%d -> %zu byte PNG from %ls", avatar.width,
                                avatar.height, size, picture);
        } else {
            island::LogAndPrint("  start: no account picture (%ls)", picture);
        }
        island::LogAndPrint("  start: footer name '%ls'", bar.accountName);
        systems.startApps.LoadPins();
        std::fflush(stdout);
    }
}

// --- the strip the shell's taskbar reserved: give it back ---
//
// Moved out of RunBar verbatim (see the note on the phase functions).
static void ReleaseReservedStrip(bool hideNativeTaskbar, bool keepStrip,
                                 const island::MonitorGeometry& monitor, island::BarSystems& systems) {
    // --- the strip the work area reserves: give it back ----------------------
    //
    // A hidden AppBar keeps its claim. Hiding the shell's taskbar left the desktop's WORK AREA inset by
    // its thickness, so a maximized window stopped below a band that holds nothing but our island -- and
    // the user photographed the empty space either side of it: "是我把应用放到最大的时候 直接就顶满屏幕
    // 而不是现在这样 有空白的".
    //
    // The release is deliberately SILENT (no SPIF_SENDCHANGE): measured on this desktop, the announced
    // form of this call reports success and changes nothing, and announcing it ourselves afterwards puts
    // the strip straight back, because the shell re-derives the work area from its own taskbar when it
    // hears about the change. It is re-asserted on the same slow cadence as the re-hide.
    //
    // Skipped when the AppBar above claims the strip: that claim is the same strip, reserved on purpose,
    // and releasing it would put maximized windows on top of the space just asked for.
    if (!hideNativeTaskbar) {
        island::LogAndPrint("  work area: left alone (--keep-taskbar)");
    } else if (keepStrip) {
        island::LogAndPrint("  work area: the strip is NOT released (--keep-strip);");
        island::LogAndPrint("    a maximized window will keep the band the shell's taskbar reserves");
    } else if (systems.appBarResult == island::AppBarResult::Registered) {
        island::LogAndPrint("  work area: the strip stays reserved (the AppBar above claims it)");
    } else {
        const island::NativeTaskbar::WorkAreaRelease release = systems.taskbar.ReleaseWorkArea();
        if (release.changed) {
            island::LogAndPrint("  work area: the strip is released -- %ld,%ld - %ld,%ld  ->  "
                                "%ld,%ld - %ld,%ld",
                                release.before.left, release.before.top, release.before.right,
                                release.before.bottom, release.after.left, release.after.top,
                                release.after.right, release.after.bottom);
            // A window that was ALREADY maximized does not follow the work area -- measured, neither
            // Windows nor the application re-lays it out -- so it keeps the very band this is about.
            const int nudged =
                island::NudgeStaleMaximizedWindows(monitor.screen, release.before, release.after);
            if (nudged) {
                island::LogAndPrint("  work area: %d already-maximized window(s) brought to the full "
                                    "screen", nudged);
            }
        } else if (release.released) {
            island::LogAndPrint("  work area: nothing to release (it is already the whole monitor)");
        } else {
            island::LogAndPrint("  work area: NOT released -- the shell's taskbar is not hidden by "
                                "this process, so its strip is not ours to give back");
        }
        std::fflush(stdout);
    }
}

// --- the composition surface: the renderer and the text renderer ---
//
// Moved out of RunBar verbatim. Returns whether the surface came up, because the frame loop refuses to run
// without it (and says so, at the point where it decides).
static bool InitializeCompositionSurface(island::BarSystems& systems) {
    const RECT rect = systems.window.WindowRect();
    const bool renderOk = systems.renderer.Initialize(
        systems.window.Handle(),
        static_cast<UINT>(rect.right - rect.left),
        static_cast<UINT>(rect.bottom - rect.top),
        systems.window.Scale());

    if (renderOk && systems.text.Initialize()) {
        systems.renderer.SetTextRenderer(&systems.text);
    }
    return renderOk;
}

// --- the weather reader, and the hover preview window ---
//
// Moved out of RunBar verbatim. The weather flag is asked about nowhere else, so it stays local here; the
// preview's is read by the frame loop, so it comes back by reference.
static void StartWeatherAndPreview(bool& previewReady, island::BarSystems& systems) {
    // --- the weather on the status strip ----------------------------------
    //
    // Polled on a worker, like the lyrics and for the same reason: it is a network call, and a
    // slow or unreachable service must not be able to stall the animation.
    //
    // Started unconditionally. A machine with no weather.txt (or no network) is the ordinary
    // case for a first run: the reader logs where the city goes and the strip shows the date and
    // the time without a reading, which is what it is designed to do.
    const bool weatherAvailable = systems.weather.Start();
    island::LogAndPrint(weatherAvailable
                            ? "  weather: position from Windows, forecast from Open-Meteo "
                              "(weather.txt can pin a city instead)"
                            : "  weather: DISABLED (the worker could not start)");
    std::fflush(stdout);

    // --- the hover preview ---------------------------------------------------
    //
    // A live thumbnail of the window under the pointer, hanging below the bar. A window of its own,
    // because a DWM thumbnail is drawn by the compositor into an HWND rather than handed over as a
    // bitmap -- see ThumbnailPreview.h.
    // Owned by the bar's window, so it can never be covered by it (see Create).
    previewReady = systems.preview.Create(systems.window.Handle());
    if (previewReady) systems.preview.SetStyle(PreviewStyleFor(island::CurrentTheme(), systems.renderer.GlassColour()));
    island::LogAndPrint(previewReady
                            ? "  preview: resting on a task icon shows a live thumbnail"
                            : "  preview: DISABLED (the preview window could not be created)");
    std::fflush(stdout);
}

// --- the tray at startup, and the learning pass that follows it ---
//
// Moved out of RunBar verbatim, both blocks together: the first count is the one the second reports against,
// and nothing outside asks about it, so it stays local.
static void ReadTrayAndStartLearning(island::BarSystems& systems) {
    // --- the system tray ------------------------------------------------------
    //
    // Read from the registry rather than from the shell's tray: the classic route through
    // Shell_TrayWnd/ToolbarWindow32 does not exist on Windows 11, and UI Automation only answers while
    // the taskbar is visible -- which this bar hides. See TrayIcons.h.
    //
    // Read here once so the first frame has icons, and refreshed from the frame loop on a timer: the tray
    // changes when applications start and stop, which is a scale of seconds, not frames.

    const int trayAtStart = systems.trayIcons.Refresh();
    // THE ONE LINE THAT SAYS WHICH LIST THIS IS. The card draws what the shell is HOLDING, not what the
    // registry remembers (see shell/TrayIcons.h). At this point in the startup the module has not reported
    // from the shell yet, so what prints here is the fallback clause; the frame loop's tick prints the real
    // one as soon as the ledger fills, which is a few seconds later.
    island::LogAndPrint(L"  tray: %d icon(s) %ls", trayAtStart, systems.trayIcons.SourceClause().c_str());
    std::fflush(stdout);

    // --- learning every tray callback, on a worker, once a session --------------------------------
    //
    // A press is invisible only when it is the application's OWN callback message posted to its own tray
    // window, and that message number used to be learned by hand, one application at a time, with an
    // administrator prompt and a third-party program in the room. It is learned here instead: a small module
    // is offered to every running tray application, every application is asked to register its icon again
    // (the standard TaskbarCreated broadcast), and what comes back is written into the cache the press path
    // reads (see shell/TrayLearn.h -- including what this cannot reach).
    //
    // ON A WORKER because it takes about three seconds, and SKIPPED when every icon is already known: the
    // broadcast makes applications re-add their icons, which is harmless but pointless to repeat once the
    // answers are in the file.
    {
        // THE MODULE GOES IN FIRST, on this thread, before any learning pass: it is offered to the one
        // process that receives tray registrations (see shell/TrayLearn.h), and from then on the card can
        // press an application the moment it registers -- no per-application pass, and no pass at all when a
        // new application starts.
        island::InstallTrayHook();
        const int coveredNow = island::CountCoveredIcons(systems.trayIcons);
        if (coveredNow >= trayAtStart) {
            island::LogAndPrint("  tray learn: %d of %d icon(s) already have a callback entry -- not asking "
                                "again", coveredNow, trayAtStart);
        } else {
            island::LogAndPrint("  tray learn: %d of %d icon(s) known; asking the applications to register "
                                "again (background)", coveredNow, trayAtStart);
            std::thread([]() {
                island::TrayIcons icons;
                icons.Refresh();
                const island::TrayLearnResult learned = island::LearnTrayCallbacks(icons, true);
                const std::wstring observer = island::ObserverClause(learned);
                island::LogAndPrint("  tray learn: the shell (%d process(es) offered) was %s, %d "
                                    "registration(s) captured, %d new entr(ies) -- %d of %d icon(s) have a "
                                    "callback entry (in %lld ms) -- %ls",
                                    learned.appsTried,
                                    learned.appsAlreadyWatched > 0 ? "already carrying the module"
                                                                   : "given the module",
                                    learned.captured, learned.newEntries, learned.covered, learned.icons,
                                    learned.ms, observer.c_str());
            }).detach();
        }
        std::fflush(stdout);
    }
}

// =====================================================================
//  THE FRAME LOOP, ONE PHASE AT A TIME (2026-10-16)
//
//  Same deal as the startup phases above, with one difference worth knowing: the loop's statements sit at
//  twelve spaces of indentation, so the code moved here is re-indented to four -- whitespace only, checked by
//  comparing the trimmed lines before and after. The order of the phases is NOT part of this work: nothing is
//  merged, nothing is moved earlier or later, and the call site replaces the block in place.
// =====================================================================

// --- what has to outlive a single pass through the loop ---
//
// WHY A STRUCT AND NOT PARAMETERS (round 46, the reviewers' (c)): the card's owner and the copy of
// the message it is showing are written in one phase of the pass and read in another, so every phase
// in between would carry three references that say nothing about the phase itself.
//
// WHAT BELONGS IN HERE, and the rule is narrow: a local that genuinely has to survive a pass, because
// something written this frame is read next frame. A local that can live inside a phase stays inside
// it, and nothing that owns policy, a resource or a lifetime moves in here.
struct BarRuntime {
    // Which card owns the shape, held for the whole life of the card rather than for the life of the
    // request that opened it. See NextCardOwner.
    island::CardOwner cardOwner = island::CardOwner::None;
    // The message the card is showing, COPIED, and held for the same reason the owner is.
    //
    // A pointer into the notification store would be reused the moment the store's ring comes round
    // -- and the card is still on screen while it retracts, so a new message arriving during the exit
    // would change what the outgoing card says. The copy keeps the exit honest.
    island::Notification notificationShown;
    bool haveNotificationShown = false;
    // --- the periodic reports, and the counters that say why a frame was drawn ---
    //
    // WHY THE redraw counters ARE HERE and not in the report: they are incremented all over the drawing
    // code, once per reason, and read (and reset) once per interval by the report below. Ten terms
    // because "the bar redrew fifty times a second" is not a fault on its own -- the report has to say
    // which of them did it. The Start card's two are the only terms that are neither a state the loop
    // watches nor the clock, and the defect they were added for ("鼠标移上去卡卡的") was exactly that: the
    // card was drawn on the next clock tick, once a second, and the report said `clock=4` with everything
    // else at zero.
    int whyAnimating = 0;
    int whyClock = 0;
    int whyNotification = 0;
    int whyMusic = 0;
    int whyLyrics = 0;
    int whyProgress = 0;
    int whyFirst = 0;
    int whyStatusStrip = 0;
    int whyStartHover = 0;
    int whyStartWash = 0;
    // The frame times of the interval being measured, described and reset by the same report.
    island::FrameStats liveStats;
    // --- the memory line's own clock ------------------------------------------------------------------
    //
    // The bar is meant to sit on the desktop for days, so "it uses little memory" is a claim about a
    // 24-hour curve rather than one reading. The curve is built from a line every five minutes plus one
    // shortly after an operation suspected of growing the process: a card closing, a track change, a tray
    // refresh. The steady line answers "does it grow at all"; the event line answers "which operation
    // grows it". ProcessMetrics.h carries the rule for reading the two together.
    unsigned long long lastMemorySample = 0;
    // One line two seconds after startup, before the five-minute cadence has had a chance to run: it
    // records the baseline the curve is measured against, and makes the line's presence checkable within
    // seconds of a deploy instead of after five minutes of waiting.
    unsigned long long memorySamplePendingAt = 0;
    bool firstMemorySample = true;
    unsigned long long lastMemoryMusicRevision = 0;
    int lastMemoryTrayIcons = 0;
    bool lastMemoryCardUp = false;
    // --- the throttle line's own clock ----------------------------------------------------------------
    //
    // Frames presented and waits taken, both per second. The pair is the difference between "the loop
    // paced itself on the compositor" and "the loop spun and presented whenever it happened to wake".
    unsigned long long lastFramesPresented = 0;
    unsigned long long frameWaits = 0;
    unsigned long long lastFrameWaits = 0;
    // When the interval report was last written, and how many window regions had been applied then.
    unsigned long long lastIntervalReport = 0;
    unsigned long long lastRegionApplications = 0;
    // When the card last had a reason to be drawn with the cheap material: while it animates, or while the
    // pointer is on it. A TIMESTAMP rather than a flag, because the material must not change on the frames in
    // between (see FrameState::cardSurfaceMoving -- a per-frame flag is what made the card flicker).
    unsigned long long lastCardMotionMs = 0;
    // The 声音输出 page own cache: whether it was up, how far it was scrolled, and the revision of the audio
    // reading it was last drawn from. The mixer rows change on their own -- an application starts or stops
    // playing -- so a change in the reading is a reason to draw, and nothing else on this bar is.
    bool lastSoundPageOpen = false;
    float lastSoundScroll = 0.0f;
    unsigned long long lastSoundRevision = 0;
    // --- the decoded pictures, and the revisions they came from ---
    //
    // The renderer caches its GPU bitmaps against these POINTERS and against the revision fields they are
    // published with (see FrameState::artworkRevision), so the pair has to survive a pass: one decode per
    // revision, and every frame in between draws from whatever the decode left here. What did not survive
    // a pass used to be decoded and uploaded again -- that is what these caches were added for.
    const uint8_t* artworkPixels = nullptr;
    int artworkWidth = 0;
    int artworkHeight = 0;
    unsigned long long lastDecodedArtworkRevision = 0;
    // The wash behind the card, built from the same cover; it belongs to the same revision.
    const uint8_t* backgroundPixels = nullptr;
    int backgroundWidth = 0;
    int backgroundHeight = 0;
    // The source application's icon: the same one-decode-per-revision rule, with a revision of its own,
    // because an icon can change while the artwork does not.
    const uint8_t* iconPixels = nullptr;
    int iconWidth = 0;
    int iconHeight = 0;
    unsigned long long lastDecodedIconRevision = 0;
    // The icon line is written when the SIZE or the fact of a decode changes rather than once per frame,
    // so these two remember what was last reported (see FillDecodedPictures).
    int lastReportedIconWidth = -1;
    bool iconWasDecoded = false;
    // The wave's colour, taken from the cover in the same decode (see AccentColourFromArtwork); the default
    // is what a bar.track with no artwork -- or an unsaturated one -- keeps.
    island::AccentColour waveAccent;
    // --- what is playing, and the lyric sheet that follows it ---
    //
    // The reading itself and the three things that decide when to take a new one (see ReadWhatIsPlaying):
    // the session worker publishes only CHANGES, so a revision that has not moved is not a reason to read,
    // and the first reading is taken whatever the revision says. `musicPositionAt` is the moment the reading
    // was taken, so the position can be projected forward from it; the sheet is a POINTER into the provider
    // rather than a copy (Lyrics is ~132 KB), and the two lyric numbers are where the resting strip's line
    // was last picked out of it.
    island::MusicInfo nowPlaying;
    unsigned long long musicPositionAt = 0;
    bool haveMusicReading = false;
    unsigned long long lastMusicRevision = 0;
    const island::Lyrics* currentLyrics = nullptr;
    unsigned long long lastLyricsRevision = 0;
    int lastLyricIndex = -2;        // -2 forces the first evaluation
    float lyricsScrollDip = 0.0f;
    // --- what the loop last saw, so it can tell a CHANGE from a reading ---
    //
    // Every one of these is a comparison against the previous pass, and every one of them used to be a local
    // in RunBar's scope -- which is exactly the state the phases that read them cannot see (see
    // PollRedrawReasons and DecideNeedDraw). The clock's second, the last notification shown, the bar.track
    // revision that was drawn, the second the card's printed time was drawn for, the volume revision and the
    // pill's meter clock.
    int lastSecond = -1;
    long long lastNotificationId = 0;
    unsigned long long lastDrawnMusicRevision = 0;
    long long lastShownSecond = -1;
    unsigned long long lastVolumeRevision = 0;
    unsigned long long lastMusicMeterMs = 0;
};

// --- the interval and throttle lines ---
//
// Moved out of the frame loop verbatim (indentation aside). Everything it needs that outlives a pass is in
// BarRuntime -- the ten redraw counters, the frame statistics, and the four numbers the throttle line
// subtracts -- which is what that struct was extended for; the pacing path is the pass own.
static void ReportIntervalAndThrottle(BarRuntime& runtime, island::BarSystems& systems,
                                      const char* pacingPath) {
    if (GetTickCount64() - runtime.lastIntervalReport >= 4000) {
        runtime.lastIntervalReport = GetTickCount64();
        wchar_t intervalSummary[256] = {};
        runtime.liveStats.Describe(intervalSummary, 256);
        island::LogAndPrint("  interval [%ls] %ls", island::StateName(systems.window.State()),
                            intervalSummary);
        island::LogAndPrint("  redraws: animating=%d clock=%d notify=%d music=%d "
                            "lyrics=%d progress=%d strip=%d first=%d startHover=%d "
                            "startWash=%d",
                            runtime.whyAnimating, runtime.whyClock, runtime.whyNotification, runtime.whyMusic,
                            runtime.whyLyrics, runtime.whyProgress, runtime.whyStatusStrip, runtime.whyFirst,
                            runtime.whyStartHover, runtime.whyStartWash);
        // Reported beside the frame intervals because the two are the same question: a region
        // re-applied per frame is a window-manager call inside the animation's own loop. A
        // hundred of them in a 4 s window containing one 0.36 s transition means the transition
        // paid for them, and "one per state change" means it did not.
        {
            const unsigned long long applied = systems.window.RegionApplications();
            island::LogAndPrint("  window region: %llu applied in total (%llu in the last 4 s)",
                                applied, applied - runtime.lastRegionApplications);
            runtime.lastRegionApplications = applied;
        }
        {
            const unsigned long long frames = systems.renderer.FramesPresented();
            island::LogAndPrint("  throttle: path=%s frames=%llu/s waits=%llu/s maxLatency=1",
                                pacingPath, frames - runtime.lastFramesPresented,
                                runtime.frameWaits - runtime.lastFrameWaits);
            runtime.lastFramesPresented = frames;
            runtime.lastFrameWaits = runtime.frameWaits;
        }
        std::fflush(stdout);
        runtime.liveStats.Reset();
        runtime.whyAnimating = runtime.whyClock = runtime.whyNotification = runtime.whyMusic = 0;
        runtime.whyLyrics = runtime.whyProgress = runtime.whyFirst = runtime.whyStatusStrip = 0;
        runtime.whyStartHover = runtime.whyStartWash = 0;
    }
}



// --- the volume and brightness sliders: follow the pointer, and let go ---
//
// Moved out of the frame loop verbatim (indentation aside). It follows a drag rather than handling a
// click, because the movement is not an event: the press arrives once and the handle has to keep up for as
// long as the button is held. It needs BarState, the window services, the monitor (a brightness write goes
// to it) and whether the volume reader came up at all.
static void FollowSliderDrags(island::BarState& bar, island::BarSystems& systems,
                              const island::MonitorGeometry& monitor, bool volumeAvailable,
                              bool soundOutputStarted) {
    // The volume slider's DRAG is followed here rather than in the click handler, because the
    // movement is not an event: a click arrives once, and the handle has to keep up with the
    // pointer for as long as the button is held.
    //
    // The value is only pushed when the FRACTION changes. Every frame at 60 Hz would be sixty COM
    // writes a second to the audio endpoint, each of which reopens the device on the worker -- the
    // handle would still follow the pointer, but the level would arrive in a lagging stream behind
    // it, which is half of what "不顺滑" meant.
    if (bar.panelSliderDrag) {
        POINT dragPoint = {};
        GetCursorPos(&dragPoint);
        // From the pointer's x alone, not from the hit test: see PanelSliderFractionForDrag.
        float fraction = 0.0f;
        if (bar.panelSliderDragRow == 1) {
            // The brightness row. DDC/CI writes go over the video cable, so they are pushed on a
            // coarser step than the volume's 0.4% and never more often than the echo deadline
            // allows: a write per frame would queue up in the monitor and arrive behind the
            // handle. The value shown follows the pointer either way (see the frame fill).
            if (bar.brightnessAvailable &&
                systems.window.PanelSliderFractionForDrag(1, dragPoint, &fraction)) {
                const int wanted = static_cast<int>(fraction * 100.0f + 0.5f);
                if (wanted != bar.lastAppliedBrightnessPercent &&
                    (GetTickCount64() >= bar.brightnessWriteNotBefore ||
                     std::abs(wanted - bar.lastAppliedBrightnessPercent) >= 5)) {
                    int landed = wanted;
                    const bool ok = island::SetBrightnessPercent(monitor.monitor, wanted,
                                                                &landed);
                    bar.lastAppliedBrightnessPercent = landed;
                    bar.brightnessWriteNotBefore = GetTickCount64() + 60;
                    if (std::abs(landed - bar.lastLoggedBrightnessPercent) >= 5 || !ok) {
                        bar.lastLoggedBrightnessPercent = landed;
                        island::LogAndPrint(
                            "  brightness: dragging -> %d%% (asked %d, %ls)\n", landed,
                            wanted, ok ? "landed" : "the monitor did not take it");
                        std::fflush(stdout);
                    }
                }
                bar.panelBrightnessEchoUntil = GetTickCount64() + 1200;
            }
        } else if (volumeAvailable &&
                   systems.window.PanelSliderFractionForDrag(0, dragPoint, &fraction)) {
            if (std::fabs(fraction - bar.lastAppliedVolumeFraction) > 0.004f) {
                bar.lastAppliedVolumeFraction = fraction;
                systems.volume.SetLevel(fraction);
                // Reported per real step rather than per frame, so "the handle follows the pointer"
                // and "it only moved twice" can be told apart in the log.
                if (std::fabs(fraction - bar.lastLoggedVolumeFraction) >= 0.03f) {
                    bar.lastLoggedVolumeFraction = fraction;
                    island::LogAndPrint("  volume: dragging -> %.0f%%\n",
                                        fraction * 100.0f);
                    std::fflush(stdout);
                }
            }
            bar.panelVolumeEchoUntil = GetTickCount64() + 1200;
        }
        if (!systems.window.LeftButtonDown()) {
            bar.panelSliderDrag = false;
            if (bar.panelSliderDragRow == 1) {
                int finalLevel = 0;
                if (island::ReadBrightnessPercent(monitor.monitor, &finalLevel)) {
                    island::LogAndPrint("  brightness: drag ended at %d%%\n", finalLevel);
                } else {
                    island::LogAndPrint("  brightness: drag ended (the monitor did not answer)\n");
                }
            } else {
                float finalLevel = 0.0f;
                if (systems.volume.Get(&finalLevel)) {
                    island::LogAndPrint("  volume: drag ended at %.0f%%\n",
                                        finalLevel * 100.0f);
                } else {
                    island::LogAndPrint("  volume: drag ended\n");
                }
            }
            bar.panelSliderDragRow = 0;
            std::fflush(stdout);
        }
    }

    // The mixer's sliders are dragged the same way the panel's volume slider is, and for the same
    // reason: a press sets the level, and HOLDING it has to keep the handle under the pointer.
    if (bar.mixerSliderDragRow >= 0) {
        POINT dragPoint = {};
        GetCursorPos(&dragPoint);
        float fraction = 0.0f;
        if (systems.window.MixerSliderFractionForDrag(bar.mixerSliderDragRow, dragPoint, &fraction) &&
            std::fabs(fraction - bar.lastAppliedMixerFraction) > 0.004f) {
            bar.lastAppliedMixerFraction = fraction;
            if (bar.mixerSliderDragRow == 0) {
                if (volumeAvailable) systems.volume.SetLevel(fraction);
            } else if (soundOutputStarted && systems.soundOutput) {
                systems.soundOutput->SetSessionVolume(bar.mixerSliderDragPid, fraction);
            }
        }
        if (!systems.window.LeftButtonDown()) {
            bar.mixerSliderDragRow = -1;
            bar.mixerSliderDragPid = 0;
            island::LogAndPrint("  mixer: drag ended\n");
            std::fflush(stdout);
    }
        }
}

// --- the tray page own menu, opened for one cell ---
//
// Moved out of the frame loop verbatim (indentation aside): it used to be the body of a lambda that three
// places call. The lambda stays, one line long, so those call sites do not change.
static void OpenBarTrayMenu(island::BarState& bar, island::BarSystems& systems, int cell) {
    bar.trayMenuCell = cell;
    island::TrayMenuFacts facts;
    island::TrayMenuFactsFor(systems.trayIcons, systems.taskWindows, bar.trayMenuCell, &facts);
    bar.trayMenuActionCount = island::TrayMenuEntriesFor(facts, bar.trayMenuActions,
                                                    island::kMaxTrayMenuEntries);
    // Captured with the menu, because the 应用菜单 row's label depends on it and the verdict cannot
    // change while that menu is on screen (a press is what changes it).
    bar.trayMenuOwnMenuUnreachable = facts.ownMenuUnreachable;
    systems.window.OpenTrayMenu(bar.trayMenuCell, bar.trayMenuActionCount);
    bar.trayQuitArmed = false;   // a new menu starts unarmed
    wchar_t flatMenuName[128] = {};
    island::LogAndPrint(L"  tray menu: opened for '%ls' (%d entries: window=%d path=%d callback=%d)\n",
                        island::FlatName(systems.trayIcons.At(bar.trayMenuCell).name, flatMenuName, 128),
                        bar.trayMenuActionCount, facts.hasWindow ? 1 : 0,
                        facts.hasExePath ? 1 : 0, facts.hasCallback ? 1 : 0);
    std::fflush(stdout);
}




// --- a right click: the bar own tray menu, and unpinning a pinned tile ---
//
// Moved out of the frame loop verbatim (indentation aside). It needs BarState and the window services and
// nothing else, which is why it takes exactly those. The worker request is read here too, because it is
// answered on the thread that owns the window -- see island::trayMenuRequestCell.
static void ApplyRightClicks(island::BarState& bar, island::BarSystems& systems) {
    // --- a RIGHT click, which so far means one thing -------------------------------------------
    //
    // A cell of the tray page: right-clicking one of the shell's tray icons opens the
    // APPLICATION'S OWN MENU -- the menu with 退出 in it -- and there is no other way to reach it
    // from here. UI Automation publishes no right-click pattern, so the press is a posted right
    // button message to the shell's own panel (see TrayPress). The request is the same one the left
    // click builds; only the message differs, which is why both go through island::StartTrayPress.
    //
    // Everywhere else on the bar a right click is still nothing: a control that quietly did
    // something different on the other button would be worse than one that does nothing.
    //
    // ... AND A LEFT PRESS THAT HAD NOTHING TO SEND OPENS THE SAME MENU, which is why the opening is
    // one piece of code and not two: since the real click was deleted there is no fallback left for a
    // cell whose callback has never been learned, and the honest answer is this menu. What it holds
    // depends on the cell: the two rows that ARE the icon's own action (打开, 应用自己的菜单…) need a
    // learned callback and are left out without one, because a row that does nothing is worse than a
    // row that is not there (requested by the user on 2026-10-15, see TrayMenuEntriesFor).
    const auto openBarTrayMenu = [&](int cell) { OpenBarTrayMenu(bar, systems, cell); };
    // The worker's request, answered on the thread that owns the window (see island::trayMenuRequestCell).
    const int menuRequestCell = island::trayMenuRequestCell.exchange(-1);
    if (menuRequestCell >= 0 && menuRequestCell < systems.trayIcons.Count()) {
        island::LogAndPrint(L"  tray menu: nothing is known about cell %d yet, so the press opens "
                            L"this bar's own menu instead\n", menuRequestCell);
        openBarTrayMenu(menuRequestCell);
    }
    while (systems.window.TakeRightClick()) {
        POINT rightCursor = {};
        GetCursorPos(&rightCursor);
        const island::IslandWindow::StartHit startHit = systems.window.StartHitHovered();
        if (bar.startPageOpen && bar.systemPanelOpen &&
            startHit.target == island::IslandWindow::StartHitTarget::Pin) {
            // A pinned tile, right-clicked: that is how it comes off again. Unpinning from the
            // CARD rather than from a settings file is the whole point of the card being the
            // user's own -- he said the upper area should be his to fill.
            if (startHit.index >= 0 && startHit.index < bar.startPinCount) {
                const island::StartApp& app = systems.startApps.PinAt(startHit.index);
                island::LogAndPrint("  start: unpinning '%ls'\n", app.name);
                systems.startApps.Unpin(startHit.index);
                bar.startPowerConfirm = -1;
            }
        } else if (island::LevelAnswersInput(bar.panelLevel, island::PanelLevel::Tray) &&
                   systems.window.TrayCellHovered() >= 0 &&
                   systems.window.TrayCellHovered() < systems.trayIcons.Count()) {
            // THE BAR'S OWN MENU, not the shell's (HANDOFF 8.80).
            //
            // Right-clicking a cell used to ask the shell for the application's menu: show the
            // taskbar invisibly, open the overflow panel, move the pointer onto the icon, click, put
            // the pointer back -- three to six hundred milliseconds, the shell's panel briefly on
            // screen (until 8.79), and the menu at the far corner of the screen rather than under
            // the pointer that asked for it. This menu is instant, lands where the pointer is, and
            // is drawn in the card the user is already looking at; the application's own menu is the
            // LAST entry of it, for the things only the application knows ("退出微信").
            openBarTrayMenu(systems.window.TrayCellHovered());
        } else {
            island::LogAndPrint("  right click: no tray cell under the pointer (%d,%d)\n",
                                rightCursor.x, rightCursor.y);
            std::fflush(stdout);
        }
    }
}

// --- who owns the card for this frame, and which message it is showing ---
//
// Moved out of the frame loop verbatim (indentation aside). The two things it carries between frames live in
// BarRuntime, which is what that struct was added for; the rest -- the frame being filled in, the request
// flags, the message that is up now and whether a session exists -- are the pass's own.
static void UpdateCardOwner(BarRuntime& runtime, island::FrameState& frame, island::BarState& bar,
                            island::BarSystems& systems, const island::Notification* active,
                            bool musicPresent) {
    // Who owns the card for this frame. The panel keeps it until the card is gone.
    //
    // `bar.systemPanelOpen` is the request, and the request is withdrawn the moment the control is
    // pressed again; the card that request opened is still on screen for another 0.24 s.
    // Drawing the panel only while the request was set made the retraction invisible -- the
    // panel disappeared in one frame -- and, because the state had been left on the card, left
    // the next press with no transition to animate at all. Both halves of the report are this
    // one latch.
    //
    // `cardWanted` is the TARGET, which is the question "is a card still asked for" -- as
    // opposed to `frame.state`, which is where the shape is now. While the answer is yes the
    // owner is chosen afresh (panel, then a message, then what is playing); while it is no the
    // owner is kept, so the card is drawn all the way out instead of vanishing on the first
    // frame of its own exit.
    const bool cardWanted = (systems.window.State() == island::IslandState::Expanded);
    runtime.cardOwner = island::NextCardOwner(runtime.cardOwner, frame.outerCardUp, cardWanted,
                                              bar.systemPanelOpen, active != nullptr, musicPresent);
    frame.cardOwner = runtime.cardOwner;
    // ... and the WINDOW is told too, because the cursor is decided there (WM_SETCURSOR) and it
    // has to know which card's controls are the ones on screen. See SetCardOwner.
    systems.window.SetCardOwner(runtime.cardOwner);

    // The message the card is showing, held for the whole life of the card.
    if (active) {
        runtime.notificationShown = *active;
        runtime.haveNotificationShown = true;
    }
    if (runtime.cardOwner != island::CardOwner::Notification) runtime.haveNotificationShown = false;
}

// --- the frame's own clock ---
//
// Moved out of the loop. `previous` is carried across frames, so it comes in by reference; `dt` is what every
// later phase in this frame is driven by, so it is returned.
static float AdvanceAnimationClock(LARGE_INTEGER& previous, const LARGE_INTEGER& frequency) {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    const float dt = static_cast<float>(
        static_cast<double>(now.QuadPart - previous.QuadPart) /
        static_cast<double>(frequency.QuadPart));
    previous = now;
    return dt;
}

// --- where the song is ---
//
// Moved out of the loop verbatim (apart from the indentation). It writes bar.track and bar.trackPlayedMs,
// which are BarState's already, and reads the session's own answer.
static void UpdateTrackPosition(island::BarState& bar, const island::MusicInfo& nowPlaying,
                                unsigned long long musicPositionAt, float dt) {
    // Asked ONCE per frame, here, because three readers come after this point and they must not
    // answer it differently (see TrackPositionFor): the lyric line, the card's printed time and
    // its bar, and the redraw cadence.
    //
    // The stopwatch is advanced only while the song is PLAYING, so a pause leaves the words where
    // they are -- the same rule the session's projected reading has.
    {
        const bool sessionTimeline =
            bar.activeFromSession &&
            island::SessionReportsTimeline(nowPlaying.positionMs, nowPlaying.durationMs);
        if (bar.activePresent && !sessionTimeline && bar.activePlaying) {
            bar.trackPlayedMs += static_cast<double>(dt) * 1000.0;
        }
        bar.track = island::TrackPositionFor(
            nowPlaying.positionMs, nowPlaying.durationMs,
            static_cast<long long>(GetTickCount64() - musicPositionAt),
            static_cast<long long>(bar.trackPlayedMs), bar.activePlaying);
        // A session with a timeline but nothing playing yet: keep the caller's other sources out
        // of it -- `TrackPositionFor` already answered, and `bar.activePresent` says whether there is
        // anything on the pill at all.
        if (!bar.activePresent) bar.track = island::TrackPosition();
    }
}


// --- what is playing, and the lyric sheet that follows it ---
//
// Moved out of the frame loop verbatim (indentation aside). Everything it reads that outlives a pass is
// BarRuntime's now -- the reading, the moment it was taken, the two revisions and the lyric sheet with its
// two numbers -- which is what that struct is for; the flags that only say whether the two readers exist are
// RunBar's and come in as arguments.
//
// It is ONE decision per pass, not three: the session wins while it has something to say, the player's own
// window title stands in only when it does not, and the SINGLE bar.track that follows is what the pill, the
// lyric fetch and the stopwatch all key on (see the note on the block itself).
static void ReadWhatIsPlaying(BarRuntime& runtime, island::BarState& bar, island::BarSystems& systems,
                              bool musicAvailable, bool playerWindowAvailable) {
    // --- what is playing ---
    if (musicAvailable) {
        // ... AND ONCE AT STARTUP, whatever the revision says.
        //
        // The worker publishes only CHANGES, and MusicSession::Start() waits for its first poll
        // before returning -- so a session that is already up and STEADY when the bar starts is
        // published exactly once, BEFORE this loop first reads the revision, and never published
        // again (nothing about it changes). A paused player is exactly that case: same title,
        // position 0, no state change. Measured live: NetEase paused while the bar started, and
        // for the whole run there was no `music:` line at all while gsmtc_probe saw the session --
        // the bar believed nothing was playing, so its card could not be opened and its own bar.track
        // was invisible.
        const bool firstMusicReading = !runtime.haveMusicReading;
        if (firstMusicReading || systems.music.Revision() != runtime.lastMusicRevision) {
            runtime.haveMusicReading = true;
            runtime.lastMusicRevision = systems.music.Revision();
            island::MusicInfo latest;
            systems.music.Snapshot(&latest);
            runtime.nowPlaying = latest;
            // The moment this position was read, so it can be projected forward.
            runtime.musicPositionAt = GetTickCount64();

            // Whether this reading is a DIFFERENT TRACK than the one the pill is showing -- which
            // is the question the cover line below asks. The lyric fetch and the roll reset are
            // NOT done here: which SOURCE the bar.track came from is decided once, further down, by
            // the block that also knows about the window-title source (see PlayerWindow.h).
            const bool sessionTrackChanged =
                std::strcmp(bar.drawnTrackTitle, runtime.nowPlaying.title) != 0 ||
                std::strcmp(bar.drawnTrackArtist, runtime.nowPlaying.artist) != 0;

            // Nothing to announce when the reading has no session at all: the FIRST reading is
            // taken unconditionally (see above), so without this guard a machine with nothing
            // playing would get a line of question marks at every startup. The other half of the
            // story -- "the music stopped" -- is logged where the source is decided, below.
            if (runtime.nowPlaying.present) {
                // WIDE, with the arguments converted first. This line used to go through the NARROW
                // LogAndPrint overload, which widens its arguments a byte at a time -- so a UTF-8
                // artist name came out as `è±é¦ç§å¦å®¶ï¼çæ ï¼` in the log while every other line
                // around it read correctly. Found by reading the log of a NetEase bar.track (HANDOFF
                // 8.72). The project's own note on that overload warned about exactly this.
                {
                    wchar_t wideApp[island::kMusicAppCapacity] = {};
                    wchar_t wideArtist[island::kMusicArtistCapacity] = {};
                    wchar_t wideTitle[island::kMusicTitleCapacity] = {};
                    wchar_t wideState[64] = {};
                    MultiByteToWideChar(CP_UTF8, 0, runtime.nowPlaying.app, -1, wideApp,
                                        static_cast<int>(island::kMusicAppCapacity));
                    MultiByteToWideChar(CP_UTF8, 0, runtime.nowPlaying.artist, -1, wideArtist,
                                        static_cast<int>(island::kMusicArtistCapacity));
                    MultiByteToWideChar(CP_UTF8, 0, runtime.nowPlaying.title, -1, wideTitle,
                                        static_cast<int>(island::kMusicTitleCapacity));
                    MultiByteToWideChar(CP_UTF8, 0,
                                        island::PlaybackStateName(runtime.nowPlaying.state), -1,
                                        wideState, 64);
                    // ... AND ONLY WHEN IT IS A DIFFERENT TRACK OR A DIFFERENT STATE.
                    //
                    // This line is above a reader that fires whenever the session's revision moves, and
                    // the revision moves for every POSITION update -- about once a second. So the same
                    // sentence was written every second for as long as the bar ran: measured in the log
                    // before this change, 3,600 lines an hour of "the same song is still playing". What
                    // the line is for is noticing that it changed, which is also what makes it worth
                    // keeping a copy of what was last written (app, artist, title, state).
                    const bool musicIdentityChanged =
                        !bar.haveLoggedMusic ||
                        std::strcmp(bar.lastLoggedMusicApp, runtime.nowPlaying.app) != 0 ||
                        std::strcmp(bar.lastLoggedMusicArtist, runtime.nowPlaying.artist) != 0 ||
                        std::strcmp(bar.lastLoggedMusicTitle, runtime.nowPlaying.title) != 0 ||
                        bar.lastLoggedMusicState != runtime.nowPlaying.state;
                    if (musicIdentityChanged) {
                        island::LogAndPrint(L"  music: [%ls] %ls - %ls (%ls)",
                                            wideApp[0] ? wideApp : L"?",
                                            wideArtist[0] ? wideArtist : L"?",
                                            wideTitle[0] ? wideTitle : L"?", wideState);
                        bar.haveLoggedMusic = true;
                        bar.lastLoggedMusicState = runtime.nowPlaying.state;
                        strncpy_s(bar.lastLoggedMusicApp, runtime.nowPlaying.app, _TRUNCATE);
                        strncpy_s(bar.lastLoggedMusicArtist, runtime.nowPlaying.artist, _TRUNCATE);
                        strncpy_s(bar.lastLoggedMusicTitle, runtime.nowPlaying.title, _TRUNCATE);
                    }
                }
                std::fflush(stdout);

                // ... AND WHETHER THERE IS A COVER, once per bar.track.
                //
                // Asked in as many words -- "现在音乐获取 封面 能获取到不" -- and there was no line
                // anywhere that answered it: the card either had a cover or it did not, and the
                // only way to tell was to photograph it. The bytes below are what the MEDIA SESSION
                // published, so this one line separates the three cases that all look alike on
                // screen: the player published nothing (`gsmtc_probe` says `Thumbnail: none`), it
                // published something this bar could not decode, and it published a cover that is
                // on the card right now.
                if (sessionTrackChanged) {
                    island::LogAndPrint("  music: cover %s (%zu bytes, revision %llu)\n",
                                        runtime.nowPlaying.artworkSize > 0 ? "present" : "NONE",
                                        runtime.nowPlaying.artworkSize, runtime.nowPlaying.artworkRevision);
                    std::fflush(stdout);
                }
            }
        }

    }

    // Lyrics arrive later than the bar.track does, so a fetch completing is its own reason to
    // redraw -- and it is read OUT HERE, not inside the session branch above.
    //
    // It was inside that branch, and the live run caught it: with the session reader off
    // (ISLANDBAR_NO_SESSION=1, the switch for exercising the window-title source) the fetch
    // completed and NOTHING READ IT -- the log showed the bar.track arriving from the window title
    // and then no `lyrics:` line at all, while the words were sitting in the provider. The
    // provider is the same one for both sources, so this belongs to neither of them.
    if (systems.lyrics.Revision() != runtime.lastLyricsRevision) {
        runtime.lastLyricsRevision = systems.lyrics.Revision();
        runtime.currentLyrics = systems.lyrics.Current();
        runtime.lastLyricIndex = -2;
        runtime.lyricsScrollDip = 0.0f;
        // The catalogue's length is on this line because it is what the card prints and the bar
        // fills against when the session publishes no timeline (see Lyrics::trackDurationMs), and
        // a number that reaches the screen without ever reaching the log is a number nobody can
        // check. Zero means the service did not say.
        island::LogAndPrint("  lyrics: %s (%d lines, the catalogue says %lld ms)",
                            runtime.currentLyrics ? "loaded" : "none",
                            runtime.currentLyrics ? runtime.currentLyrics->count : 0,
                            runtime.currentLyrics ? runtime.currentLyrics->trackDurationMs : 0);
        std::fflush(stdout);
    }

    // --- the player that never publishes a session (see PlayerWindow.h) ---------
    //
    // Read from its own window title, and ONLY while there is no session: the session carries
    // the cover, the position and the transport keys as well, and it wins whenever it exists.
    // Measured need: NetEase Cloud Music 3.1.40 plays with the system's session list empty
    // until its own switch is turned on (HANDOFF 8.72), and until then the island had nothing
    // at all to show for the music.
    if (playerWindowAvailable && systems.playerWindow.Revision() != bar.lastPlayerWindowRevision) {
        bar.lastPlayerWindowRevision = systems.playerWindow.Revision();
        // ONLY the reading. It is NOT announced here, because "this player publishes no session"
        // is a statement about which source is IN USE, and that is decided in the block below --
        // the live run produced the opposite: the window arrived a few hundred milliseconds before
        // the session did (the reader's first poll), so the log announced the fallback while a
        // session was about to be published, and the bar.track it named was the same one.
        systems.playerWindow.Snapshot(&bar.playerWindowNow);
    }

    // --- ONE bar.track change, whichever source it came from -------------------------
    //
    // The bar.track the PILL is showing, and the one decision that follows from it. There is
    // exactly one such bar.track: the session's while a session exists, otherwise the player's own
    // window title. Everything that keys on "a different song is up" reads this -- so the two
    // sources cannot each believe they own the transition, which is what a reset and a fetch
    // inside each source's own block would amount to.
    {
        const bool sessionUp = runtime.nowPlaying.present && runtime.nowPlaying.title[0] != '\0';
        const bool windowUp =
            !sessionUp && bar.playerWindowNow.present && bar.playerWindowNow.title[0] != '\0';
        bar.activePresent = sessionUp || windowUp;
        bar.activeFromSession = sessionUp;
        // Whether the line may creep: a paused session stands still (its playhead is not
        // moving), while a window title only exists while the player is playing something.
        bar.activePlaying =
            sessionUp ? runtime.nowPlaying.state == island::PlaybackState::Playing : windowUp;
        bar.activeTitle = sessionUp ? runtime.nowPlaying.title : (windowUp ? bar.playerWindowNow.title : "");
        bar.activeArtist = sessionUp ? runtime.nowPlaying.artist : (windowUp ? bar.playerWindowNow.artist : "");

        const bool trackChanged = std::strcmp(bar.drawnTrackTitle, bar.activeTitle) != 0 ||
                                  std::strcmp(bar.drawnTrackArtist, bar.activeArtist) != 0;
        if (trackChanged) {
            strncpy_s(bar.drawnTrackTitle, sizeof(bar.drawnTrackTitle), bar.activeTitle, _TRUNCATE);
            strncpy_s(bar.drawnTrackArtist, sizeof(bar.drawnTrackArtist), bar.activeArtist, _TRUNCATE);
            // A bar.track change invalidates the lyrics and their scroll position.
            //
            // AND ONLY A TRACK CHANGE. The session bumps its revision for a POSITION update too
            // (once every second or two), so keying this on the revision re-announced the same
            // line as if it had changed, restarted a long line's marquee and replayed the roll
            // with the same words on both sides: "歌词每1、2秒就会上下出现一模一样的歌词然后和
            // 在一起似的". The comparison is on the bar.track, not on the revision.
            runtime.lyricsScrollDip = 0.0f;
            runtime.lastLyricIndex = -2;
            // The stopwatch restarts with the bar.track: it is the playhead for every source that
            // cannot supply one (see TrackPositionFor -- aligned when the bar catches the song as
            // it starts, drifting when it does not).
            bar.trackPlayedMs = 0.0;
            if (bar.activePresent) {
                systems.lyrics.Request(bar.activeTitle, bar.activeArtist);
                if (!bar.activeFromSession) {
                    // WHICH SOURCE, said out loud. Without this line a log cannot tell that the
                    // words on the pill came from a window title instead of a session -- and the
                    // difference matters the moment someone compares it with `gsmtc_probe`.
                    wchar_t wideApp[island::kPlayerWindowAppCapacity] = {};
                    wchar_t wideArtist[island::kPlayerWindowArtistCapacity] = {};
                    wchar_t wideTitle[island::kPlayerWindowTitleCapacity] = {};
                    MultiByteToWideChar(CP_UTF8, 0, bar.playerWindowNow.app, -1, wideApp,
                                        static_cast<int>(island::kPlayerWindowAppCapacity));
                    MultiByteToWideChar(CP_UTF8, 0, bar.playerWindowNow.artist, -1, wideArtist,
                                        static_cast<int>(island::kPlayerWindowArtistCapacity));
                    MultiByteToWideChar(CP_UTF8, 0, bar.playerWindowNow.title, -1, wideTitle,
                                        static_cast<int>(island::kPlayerWindowTitleCapacity));
                    island::LogAndPrint(
                        L"  music: [%ls] %ls - %ls (from its window title -- this player"
                        L" publishes no session)\n",
                        wideApp, wideArtist, wideTitle);
                    std::fflush(stdout);
                }
            } else {
                // Nothing to put on the pill any more: no session AND no player window. Logged
                // because the pill is about to go back to its clock and, with both sources silent,
                // the log is the only witness that it was not a mistake.
                island::LogAndPrint(L"  music: nothing is playing (no session and no player window"
                                    L" title) -- the pill goes back to its clock\n");
                std::fflush(stdout);
            }
        }
    }
}

// --- a full-screen application gets the whole screen ---
//
// Moved out of the frame loop verbatim (apart from the indentation: the loop sits at twelve spaces, this
// body at four). Everything it touches comes in explicitly -- the state it keeps between frames is BarState's,
// the check interval and the two switches are RunBar's, and barThicknessPx is the number the AppBar claim
// uses everywhere else.
static void UpdateFullscreenStandDown(island::BarState& bar, island::BarSystems& systems,
                                      const island::MonitorGeometry& monitor, bool noFullscreen,
                                      unsigned long long fullscreenCheckMs, LONG barThicknessPx) {
    // POLLED, and by us: the shell's announcement only reaches a registered AppBar (see
    // FullScreenAppInFront), so this is the only signal that exists in the shipped
    // configuration.
    if (GetTickCount64() - bar.lastFullscreenCheck >= fullscreenCheckMs) {
        bar.lastFullscreenCheck = GetTickCount64();
        const bool measured =
            !noFullscreen && island::FullScreenAppInFront(monitor.screen);
        if (measured != bar.fullscreenAppInFront) {
            bar.fullscreenAppInFront = measured;
            // ... and the WINDOW is told, because the hit test is answered there: while an
            // application has the screen only the island itself is the bar, so a pointer
            // crossing the top of it cannot land on an icon or a deck button that is not drawn.
            systems.window.SetFullscreenApp(bar.fullscreenAppInFront);
            // `%s` and not `%ls`: this is the NARROW overload of LogAndPrint, whose arguments
            // are widened a byte at a time, so a `%ls` here would read a `const char*` as a wide
            // string -- and the CRT answers an invalid conversion by emptying the whole line,
            // leaving a blank line in the log where the answer should be. (The project's own
            // note on that overload says the same thing; this is what it looks like when it is
            // ignored.)
            island::LogAndPrint(
                "  fullscreen: %s",
                measured ? "an application has the screen -- the bar stands down"
                         : "over -- the bar is back");
            std::fflush(stdout);
        }
    }

    const bool fullscreenNow = bar.fullscreenAppInFront;
    if (fullscreenNow != bar.fullscreenLast) {
        bar.fullscreenLast = fullscreenNow;

        if (fullscreenNow) {
            if (systems.appBar.IsRegistered()) {
                systems.appBar.Release();
                island::LogAndPrint("  fullscreen: space released; the bar fills the band");
            } else {
                island::LogAndPrint("  fullscreen: the bar stands down");
            }
        } else if (systems.appBarResult == island::AppBarResult::Registered) {
            // Reclaimed only if it had been claimed: `--keep-taskbar` never claims, and
            // claiming on the way out of full-screen would reserve space never asked for.
            systems.appBarResult =
                systems.appBar.Claim(systems.window.Handle(), island::DockEdge::Top, barThicknessPx);
            island::LogAndPrint("  fullscreen: over, space re-claimed -- %ls",
                                island::AppBarResultName(systems.appBarResult));
        } else {
            island::LogAndPrint("  fullscreen: over, back to the pill");
        }
        std::fflush(stdout);
    }
}

// --- re-hiding the shell's taskbar, and giving the strip back again, on a slow cadence ---
//
// Moved out of the frame loop verbatim (indentation aside). The countdown and the work flags are RunBar's
// locals and are carried between frames, so they come in by reference; `workBit` is the same
// kWorkTaskbarCheck constant the slow-frame report reads.
static void EnsureTaskbarStillHidden(bool hideNativeTaskbar, bool keepStrip, int& taskbarCheckCountdown,
                                     unsigned long long& frameWork, unsigned long long workBit,
                                     island::BarState& bar, island::BarSystems& systems,
                                     const island::MonitorGeometry& monitor) {
    // ... and NOT gated on `haveTaskbar`. That flag is "the taskbar was there when this process
    // started", and a bar that starts at logon can easily start first -- see the note in
    // NativeTaskbar::EnsureHidden, which is where the second look happens.
    if (hideNativeTaskbar && --taskbarCheckCountdown <= 0) {
        // 50 FRAMES, and the frame is the units of this counter -- not seconds. Measured on this
        // machine (the `interval [music-strip]` line in the log): ~15 ms per frame, so this is
        // about every 0.8 s. The comment here used to claim "roughly every 5 s at an 8 ms tick",
        // and both numbers were wrong: an 8 ms timer from an earlier design that no longer exists.
        // It cost a measurement -- a probe that put the work area back by hand and waited 8 s for
        // "the bar's cycle" was looking at two cycles that had already been and gone (HANDOFF 8.86).
        taskbarCheckCountdown = 50;
        frameWork |= workBit;
        // A full-screen application takes the tray with it, exactly as it takes the rest of the
        // bar: keeping the tray visible is this mode's whole job, so it has to be told when NOT
        // to (see NativeTaskbar::SetSuppressed).
        if (systems.taskbar.KeepTrayVisible()) {
            systems.taskbar.SetSuppressed(bar.fullscreenAppInFront);
        }
        // ... and none of the above needs to know whether a tray press is in flight. Since
        // 2026-10-16 a press is ONE MESSAGE to the application's own window (route A: the module
        // publishes the registration, the bar sends the click itself), so the shell's taskbar is
        // never touched, never shown, never layered -- the borrowing described in the retired
        // TrayPress notes is gone with the route that needed it.
        if (systems.taskbar.EnsureHidden()) {
            if (systems.taskbar.KeepTrayVisible()) {
                island::LogAndPrint("  native taskbar: tray re-clipped (the shell had put the"
                                    " taskbar back)\n");
            } else {
                island::LogAndPrint("  native taskbar: re-hidden (the shell put it back)\n");
            }
            std::fflush(stdout);
        }
        // ... and the same question for the work area the hide used to reserve. The shell
        // re-derives it from its own taskbar whenever the reservation is announced -- by any
        // application, on a display change, or when it re-shows its taskbar -- and every one of
        // those puts the strip back. Silent when nothing happened, which is the usual case.
        if (!keepStrip) {
            const island::NativeTaskbar::WorkAreaRelease again =
                systems.taskbar.EnsureWorkAreaReleased();
            if (again.changed) {
                island::LogAndPrint("  work area: strip released again (it was back) -- "
                                    "%ld,%ld - %ld,%ld  ->  %ld,%ld - %ld,%ld\n",
                                    again.before.left, again.before.top, again.before.right,
                                    again.before.bottom, again.after.left, again.after.top,
                                    again.after.right, again.after.bottom);
                // A window maximized while the strip was back is in the same state as the ones at
                // startup: it will not follow the work area by itself.
                const int nudged = island::NudgeStaleMaximizedWindows(monitor.screen,
                                                                     again.before, again.after);
                if (nudged) {
                    island::LogAndPrint("  work area: %d already-maximized window(s) brought to "
                                        "the full screen\n", nudged);
                }
                std::fflush(stdout);
            }
        }
    }
}

// --- the four channels that bring a message in ---
//
// Moved out of the frame loop verbatim (indentation aside). What they have in common is the answer to "did
// anything arrive": each one pushes into the same store and asks the window for the notice strip. The
// question of WHICH message is showing right now is NOT here -- that pointer is read by the pointer policy
// and by the drawing further down the loop, so it is computed where it is used, from what this filled in.
//
// The three work bits are the loop's own report of which periodic task ran in a pass; they are handed in
// rather than duplicated, exactly like the taskbar check's.
static void PollNotificationChannels(bool listening, bool toastsAvailable, bool soundOutputStarted,
                                     island::BarState& bar, island::BarSystems& systems,
                                     unsigned long long& frameWork, unsigned long long toastPollBit,
                                     unsigned long long wechatScanBit, unsigned long long wechatSoundBit) {
    // --- notifications ---
    //
    // Drain everything the listener thread has received. Each one opens the
    // card, so a burst ends with the last notification on screen rather than
    // flashing through all of them.
    if (listening) {
        island::Notification incoming;
        int drained = 0;
        while (systems.notificationServer.Take(&incoming)) {
            const island::Notification* stored = systems.notifications.Push(incoming);
            ++drained;
            if (stored) {
                // Wide, and converted here: the store is UTF-8 and the narrow overload would print
                // every Chinese character as its bytes ("¹¹½¨Íê³É" for 构建完成), which reads like a
                // corrupt message rather than like a logging bug.
                wchar_t appW[island::kAppNameCapacity] = {};
                wchar_t titleW[island::kTitleCapacity] = {};
                MultiByteToWideChar(CP_UTF8, 0, stored->app, -1, appW,
                                    static_cast<int>(island::kAppNameCapacity));
                MultiByteToWideChar(CP_UTF8, 0,
                                    stored->title[0] ? stored->title : stored->body, -1, titleW,
                                    static_cast<int>(island::kTitleCapacity));
                island::LogAndPrint(L"  notification: [%ls] %ls\n",
                                    stored->app[0] ? appW : L"?", titleW);
                std::fflush(stdout);
            }
        }
        if (drained > 0) {
            // NOT a card any more. A message widens the island and shows itself inside it (see
            // NextShape's notice strip); the card is what the pointer opens when it goes to the island
            // to read the whole thing. Forcing Expanded here -- which is what this did -- would fight
            // that on every arrival: the strip would exist and never be seen.
            systems.window.SetState(island::IslandState::NoticeStrip);
        }
    }

    // ... and the same for the OTHER channel, on a slow timer.
    //
    // Two seconds rather than every frame because it is a database query, and the delay is invisible:
    // the shell's own toast appears at the same moment and the bar's card follows it. Nothing here
    // depends on the push channel and nothing in the push channel depends on this.
    if (toastsAvailable && GetTickCount64() - bar.lastToastPoll >= 2000) {
        bar.lastToastPoll = GetTickCount64();
        frameWork |= toastPollBit;
        const int arrived = systems.toasts.Poll(&systems.notifications);
        if (arrived > 0) {
            island::LogAndPrint("  toasts: %d new system toast(s), %d taken in total", arrived,
                                systems.toasts.Taken());
            std::fflush(stdout);
            // A message from another application deserves the same attention as one pushed here --
            // which is the strip, not the card. See the push channel above.
            systems.window.SetState(island::IslandState::NoticeStrip);
        }
    }

    // ... and WeChat, which has no channel of its own and is recognised by its popup appearing.
    if (GetTickCount64() - bar.lastWeChatPoll >= 500) {
        bar.lastWeChatPoll = GetTickCount64();
        frameWork |= wechatScanBit;
        if (systems.wechat.Poll(&systems.notifications, GetTickCount64()) > 0) {
            island::LogAndPrint("  wechat: %d message announcement(s) so far", systems.wechat.Announced());
            std::fflush(stdout);
            systems.window.SetState(island::IslandState::NoticeStrip);
        }
    }

    // ... and the SOUND channel, which is the one signal WeChat gives when its window does nothing.
    //
    // Its own clock (200 ms) rather than the window scan's: a notification sound is about a second
    // long, and a check every half second could read the session as active twice in a row and never
    // see the edge -- the sound would play with no announcement and no evidence anywhere.
    if (soundOutputStarted && systems.soundOutput && GetTickCount64() - bar.lastWeChatSoundPoll >= 200) {
        bar.lastWeChatSoundPoll = GetTickCount64();
        frameWork |= wechatSoundBit;
        systems.soundOutput->Snapshot(systems.soundSnapshot.get());

        // Is WeChat running, and is one of its audio sessions making sound?
        const bool wechatRunning = island::AnyWeChatProcessRunning();
        bool wechatSessionActive = false;
        int wechatSessions = 0;
        for (int i = 0; i < systems.soundSnapshot->sessionCount; ++i) {
            const island::SoundSessionInfo& session = systems.soundSnapshot->sessions[i];
            if (!island::IsWeChatProcess(session.pid)) continue;
            ++wechatSessions;
            if (session.active) wechatSessionActive = true;
        }
        if (systems.wechat.PollSound(&systems.notifications, wechatRunning, wechatSessionActive,
                                     wechatSessions, GetTickCount64()) > 0) {
            systems.window.SetState(island::IslandState::NoticeStrip);
        }
    }
}

// --- the pointer is polled, and the window's edges are drained ---
//
// Moved out of the frame loop verbatim (indentation aside). It needs nothing but the window: the decision
// about what the pointer MEANS is the policy's, further down the loop (see NextShape), and this phase only
// makes sure the window knows where the pointer is and that its one-shot flags do not pile up.
static void PollPointerState(island::BarSystems& systems) {
    // --- pointer -> state ---
    //
    // The pointer is polled HERE, from the frame loop, rather than only from a window
    // timer.
    //
    // The window ran an 8 ms timer for this and reported it as running, but a trace of the
    // messages actually delivered to the window procedure showed `WM_TIMER=0`: the timer
    // message never arrived, so this poll effectively never happened, `pointerInside_`
    // stayed false for the life of the process, the shape never promoted to a card, and no
    // click was ever accepted. It was silent -- the bar drew perfectly and ignored the
    // pointer.
    //
    // The frame loop runs at the compositor's rate, which is better than 8 ms and cannot
    // be starved, so it is the reliable place to ask. The timer is left in place as a
    // fallback for the paths that do not pump this loop.
    systems.window.UpdatePointerState();

    // The window only reports that the pointer entered or left; what that
    // means is decided here, which keeps the window procedure free of policy.
    //
    // Pointer handling must not fight the notification state: while a
    // notification is showing, leaving the pill returns to the card rather
    // than collapsing it, or an arriving message would be cut off the instant
    // the pointer drifted away.
    bool pointerInside = false;
    if (systems.window.TakePointerInsideChanged(&pointerInside)) {
        // THE EDGE IS CONSUMED AND NOTHING ELSE HAPPENS HERE.
        //
        // The state is the POLICY's answer (NextShape, below). Every state this handler used to
        // pick was a second policy, and the two could disagree -- which is what kept the
        // notification card on screen after it was removed on request:
        //
        //   * with the pointer inside, this frame asked for Expanded (`active` -- a message must
        //     not be cut off when the pointer drifts away) while the policy asked for the notice
        //     strip, so the two alternated every frame;
        //   * measured in the bar's own log: `state: notice-strip` and `state: expanded` swapping
        //     every ~0.35 s while a notification was up, and a screenshot of the card STILL being
        //     drawn after the card had been removed. Reported as "但是现在还是会触发啊".
        //
        // The panel's case below is the policy's too: `bar.systemPanelOpen` returns Expanded whatever
        // the pointer is doing, so nothing was lost by deleting this.
        (void)pointerInside;
    }
    // A layout change (DPI, work-area change) has already been applied by
    // the window; draining the flag keeps it from accumulating.
    systems.window.TakeLayoutChanged();
}

// --- the shape the pointer asks for, once the motion has settled ---
//
// Moved out of the frame loop verbatim (indentation aside). The rule itself is the pure function in
// StatePolicy.h; what this phase does is describe the situation to it and act on the answer. `resting` and
// `active` are computed earlier in the pass and handed in; `musicPresent` is the session's own answer
// (`nowPlaying.present`), passed as a bool so the phase does not have to know about a MusicInfo.
static void ApplyPointerShapePolicy(island::BarState& bar, island::BarSystems& systems,
                                    const island::Notification* active, island::IslandState resting,
                                    bool musicPresent, const wchar_t* forcedState) {
    // --- pointer -> shape, once the motion has settled ---
    //
    // The rule itself lives in StatePolicy.h as a pure function, so the frame loop
    // has no policy of its own to drift from what the tests check. It used to be
    // three inline conditions with the promotion to the card buried inside the
    // branch that lowers an already-open card, so nothing ever moved
    // Hover -> Expanded and the card was unreachable while music played.
    //
    // Only consulted when the motion has stopped, so a shape is never re-targeted
    // mid-flight (that would restart the animation every frame).
    if (!systems.window.IsAnimating()) {
        // The icon row is described to the policy so it can tell "the pointer is on the
        // island" from "the pointer is on one of the icons". Those need different answers:
        // the first widens the island, the second widens one icon and leaves the island
        // exactly as it is.
        island::PointerContext context;
        context.shape = systems.window.State();
        context.resting = resting;
        context.pointerInside = systems.window.IsPointerInside();
        context.notificationActive = active != nullptr;
        context.callActive = IsCallNotification(systems, active);
        // ... and whether there is a player at all: a PAUSED session still opens the card, which
        // is how the play button is reached after a pause (see ShouldPromoteToCard).
        //
        // This is the SESSION, not "the pill has a lyric": a player read from its own window
        // title (see PlayerWindow.h) fills the pill's line but has no cover, no position and no
        // transport keys to put on a card, and the card is exactly those three things.
        context.musicPresent = musicPresent;
        context.fullscreenApp = bar.fullscreenAppInFront;
        // The panel is a request for the CARD, so the policy has to know about it -- otherwise
        // the state stays Hover and the card is never drawn. That was a whole debugging round:
        // the flag was set, the renderer read it, and the shape never changed.
        context.systemPanelOpen = bar.systemPanelOpen;
        context.iconCount = systems.window.IconCount();
        context.hoveredIcon = systems.window.HoveredIcon();
        // Where the pointer is, for the rule that gives the card up: an icon or a deck button is
        // asking about itself, not about the island. The window is the only thing that knows where
        // those buttons are.
        context.pointerOnDeck = systems.window.SystemButtonHovered() || systems.window.StartHovered();
        // ... and whether it is on the PILL rather than elsewhere on the bar. The card is the
        // pill's, so this, not `pointerInside`, is what asks for it.
        context.pointerOnIsland = systems.window.IsPointerOnIsland();
        // ... and whether it is in the CARD's COLUMN -- the card and the gap between it and the bar.
        // That column is the pill's territory as much as the pill is: the pointer has to be able to
        // walk from the pill down onto the card whose buttons it wants, and the gap is part of the walk.
        context.pointerInCardColumn = systems.window.IsPointerInCardColumn();

        const island::IslandState next = island::NextShape(context);

        if (forcedState) {
            // Pinned. The policy above still ran and is still reported; its answer is simply not
            // acted on, so what is on screen is a named state rather than whatever the pointer
            // happened to be doing.
            const island::IslandState pinned = island::StateFromName(forcedState);
            if (systems.window.State() != pinned) systems.window.SetState(pinned);
        } else if (next != systems.window.State()) {
            systems.window.SetState(next);
        }

        // A DRAG UNDER WAY HOLDS THE CARD OPEN, whatever the pointer does.
        //
        // A slider's gesture is "press it, drag anywhere, release" -- the pointer leaves the row,
        // leaves the card, and often leaves the bar itself on its way to the end of the bar.track. With
        // the policy left in charge the card retracted mid-drag: the handle stopped updating at the
        // exact moment the user was watching it, which is the other half of "卡卡的，没有过渡".
        // Same shape as the rule that already holds the card for a notification: something explicit
        // is in progress, so the pointer's position is not the question.
        if (bar.panelSliderDrag || bar.mixerSliderDragRow >= 0) {
            if (systems.window.State() != island::IslandState::Expanded) {
                systems.window.SetState(island::IslandState::Expanded);
            }
        }
    }
}

// --- the wheel, for the two lists that scroll ---
//
// Moved out of the frame loop verbatim (indentation aside). It touches nothing but BarState and the window,
// which is why it needs no other parameters.
static void ApplyWheelScroll(island::BarState& bar, island::BarSystems& systems) {
    // The window counts the notches and hands them over; the OFFSET belongs here, where the page is
    // built and where the same value goes out to the drawing and the hit test. The window clamps it
    // (it is the one that knows how many rows there are) and the clamped value is copied straight
    // back, so there is still only one offset in play.
    {
        int wheelNotches = 0;
        if (bar.soundPageOpen && systems.window.TakeSoundWheel(&wheelNotches)) {
            bar.soundPageScroll -= static_cast<float>(wheelNotches) * 60.0f;
            systems.window.SetSoundPageScroll(bar.soundPageScroll);
            bar.soundPageScroll = systems.window.SoundPageScroll();
            island::LogAndPrint("  sound page: wheel %d -> scroll %.0f\n", wheelNotches,
                                bar.soundPageScroll);
            std::fflush(stdout);
        } else if (bar.startPageOpen && bar.startPageState == 1 &&
                   systems.window.TakeSoundWheel(&wheelNotches)) {
            // The picker scrolls one NOTCH per row, not in pixels: 121 applications at 34 DIP a row
            // is 4,114 DIP of list, and a pixel-per-notch wheel would take a minute to reach the
            // end alphabetically. Clamped so the last screenful is the last thing reachable.
            systems.window.ScrollStartPicker(wheelNotches);
            bar.startPickerScroll = systems.window.StartPickerScroll();
            island::LogAndPrint("  start: picker wheel %d -> row %d of %d\n", wheelNotches,
                                bar.startPickerScroll, bar.startAppCount);
            std::fflush(stdout);
        }
    }
}

// --- what "put the card away" means ---
//
// Moved out of the frame loop verbatim (indentation aside): it used to be the body of a lambda that nine
// places in the loop call, and those call sites are deliberately left alone -- the lambda stays, one line
// long, so the shape of the code that reads as "dismiss the panel" does not change. `active` and `resting`
// are the pass's own answers and are passed in.
static void DismissPanelSurface(island::BarState& bar, island::BarSystems& systems,
                                const island::Notification* active, island::IslandState resting) {
    bar.systemPanelOpen = false;
    bar.soundPageOpen = false;
    // ... and the Start card, which is the fourth level of the same card and has to go with it
    // for the same reason (see the note above about every level being cleared).
    bar.startPageOpen = false;
    // ... AND the tray's page, which is the third level of the same card. It used to be left
    // set, which was harmless while the level was read from the request and is not now: a card
    // put away by a click outside must take every level with it, exactly as a click on the
    // system button does (see the click handler).
    bar.trayPageOpen = false;
    // ... and the tray's own menu, which is a thing in front of that page rather than a level
    // of it, so it cannot outlive the card it was opened over.
    systems.window.CloseTrayMenu();
    bar.panelSliderDrag = false;
    bar.mixerSliderDragRow = -1;
    bar.mixerSliderDragPid = 0;
    // TO THE NOTICE STRIP, NOT TO `resting`, WHILE A CALL OR A MESSAGE IS UP.
    //
    // `resting` is the ordinary shape -- the lyric strip, or the plain pill -- and putting the
    // island there is what "变回普通" was. With a call running: a click on a button opened its
    // card (correct), closing the card ran this line, and the island settled as the ordinary
    // strip for 220 ms before the policy above put the notice back. The log has the pair:
    // `state: music-strip` at 23:00:51.122 and `state: notice-strip` at 23:00:51.345.
    //
    // A notice is not a card, so dismissing a card has no business retiring it. This is the only
    // place a card is put away -- every click-outside branch calls this lambda -- so the shape
    // can now only ever go card -> notice strip while something is being announced.
    systems.window.SetState(active == nullptr ? resting : island::IslandState::NoticeStrip);
}

// --- a press on the Start card ---
//
// Moved out of the frame loop verbatim (indentation aside), with the dismiss calls becoming
// DismissPanelSurface for the same reason the outside-click phase does it: a free function cannot see a
// lambda defined in the loop. Reached before the panel own controls, because the Start card IS the level
// that is up: a press at the position of a tile would otherwise be read as a tile.
static void ApplyStartCardPress(island::BarState& bar, island::BarSystems& systems,
                                const island::Notification* active, island::IslandState resting) {
    // --- A PRESS ON THE START CARD -----------------------------------------------------
    //
    // Reached before the panel's own controls, because the Start card IS the level that is
    // up: a press at the position of a tile would otherwise be read as a tile.
    //
    // Every branch logs what it did: a launcher whose click does nothing is the hardest
    // thing on this bar to tell from a click that never arrived.
    const island::IslandWindow::StartHit startHit = systems.window.StartHitHovered();
    switch (startHit.target) {
        case island::IslandWindow::StartHitTarget::Pin: {
            if (startHit.index >= 0 && startHit.index < bar.startPinCount) {
                const island::StartApp& app = systems.startApps.PinAt(startHit.index);
                island::LogAndPrint("  start: launching pinned '%ls'\n", app.name);
                island::LaunchStartApp(app.target);
            }
            DismissPanelSurface(bar, systems, active, resting);
            break;
        }
        case island::IslandWindow::StartHitTarget::PickerRow: {
            if (startHit.index >= 0 && startHit.index < bar.startAppCount) {
                const island::StartApp& app = systems.startApps.ListAt(startHit.index);
                if (bar.startAllApps) {
                    // THE 全部 VIEW OPENS THE APPLICATION. The same list has one meaning per door: the
                    // 全部 button launches, the empty grid pins, and neither has to explain itself in the
                    // footer. This is the user's "点击应用可以打开".
                    island::LogAndPrint("  start: launching '%ls'\n", app.name);
                    island::LaunchStartApp(app.target);
                    DismissPanelSurface(bar, systems, active, resting);
                    break;
                }
                // The shortcut is NAMED after the application, which is also what the tile
                // will show: renaming the file in Explorer renames the tile.
                const bool added = systems.startApps.Pin(app.target, app.name);
                island::LogAndPrint("  start: %ls '%ls' (%d pinned now)\n",
                                    added ? "pinned" : "already pinned", app.name,
                                    systems.startApps.PinCount());
                // Back to the pins, which is where the new one just appeared: staying in a
                // list of 121 with no sign of what happened is how a click reads as a dud.
                bar.startPageState = 0;
                bar.startPowerConfirm = -1;
            }
            break;
        }
        case island::IslandWindow::StartHitTarget::PowerRow: {
            const int row = startHit.index;
            if (row >= 0 && row <= 4) {
                const PowerAction action = static_cast<PowerAction>(row);
                if (PowerActionLosesWork(action) && bar.startPowerConfirm != row) {
                    // Armed, not done: the list appears above a button the user was already
                    // reaching for, and a mis-click there must not sign him out.
                    bar.startPowerConfirm = row;
                    island::LogAndPrint("  power: '%ls' armed (a second press does it)\n",
                                        PowerActionName(action));
                } else {
                    island::LogAndPrint("  power: '%ls' pressed\n",
                                        PowerActionName(action));
                    bar.startPowerConfirm = -1;
                    DismissPanelSurface(bar, systems, active, resting);
                    RunPowerAction(action);
                }
            }
            break;
        }
        case island::IslandWindow::StartHitTarget::PowerButton: {
            if (bar.startPageState == 1) {
                bar.startPageState = 0;   // the × on the picker: back to the pins
                bar.startAllApps = false;
            } else {
                bar.startPageState = bar.startPageState == 2 ? 0 : 2;
            }
            bar.startPowerConfirm = -1;
            break;
        }
        case island::IslandWindow::StartHitTarget::EmptyGrid: {
            // The empty upper area is the way to fill it, which is what "上面的应该可以让用户
            // 手动添加" asks for: the card itself is the control.
            bar.startPageState = 1;
            bar.startAllApps = false;
            bar.startPickerScroll = 0;
            island::LogAndPrint("  start: picker open (%d application(s))\n",
                                bar.startAppCount);
            break;
        }
        case island::IslandWindow::StartHitTarget::AllApps: {
            // The 全部 button at the card's top-right: the same list, but a row OPENS the application
            // rather than pinning it (see PickerRow). Reset the scroll so the list starts at the top.
            bar.startPageState = 1;
            bar.startAllApps = true;
            bar.startPickerScroll = 0;
            bar.startPowerConfirm = -1;
            island::LogAndPrint("  start: all applications (%d)\n", bar.startAppCount);
            break;
        }
        case island::IslandWindow::StartHitTarget::None:
            break;
    }
    std::fflush(stdout);
}

// --- a press on the 声音输出 page own controls ---
//
// Moved out of the frame loop verbatim (indentation aside), with the dismiss call becoming
// DismissPanelSurface for the same reason the other phases do it. Reached BEFORE the panel own controls,
// because the two are laid out over the same card: while this level is showing, a click at the position of
// a tile is a click on a device row, and asking the panel first would fire the wrong action entirely.
static void ApplySoundPagePress(island::BarState& bar, island::BarSystems& systems, POINT cursor,
                                bool volumeAvailable, const island::Notification* active,
                                island::IslandState resting) {
    // --- the 声音输出 page's own controls ---
    //
    // Reached BEFORE the panel's, because the two are laid out over the same card: while this
    // level is showing, a click at the position of a tile is a click on a device row, and
    // asking the panel first would fire the wrong action entirely.
    const island::SoundPageHit soundHit = systems.window.SoundPageHitAtScreen(cursor);
    switch (soundHit.target) {
        case island::SoundPageTarget::Back:
            bar.soundPageOpen = false;
            island::LogAndPrint("  sound page: back to the panel\n");
            break;

        case island::SoundPageTarget::Device:
            if (soundHit.index >= 0 &&
                soundHit.index < systems.soundSnapshot->deviceCount) {
                const island::SoundDeviceInfo& wanted =
                    systems.soundSnapshot->devices[soundHit.index];
                if (wanted.isDefault) {
                    island::LogAndPrint(
                        L"  sound page: device %d '%ls' is already the default\n",
                        soundHit.index, wanted.name);
                } else {
                    // Handed to the worker: switching the default goes through an interface
                    // that has no public documentation (see SoundOutput), and the worker's
                    // own log line is what says whether it took.
                    systems.soundOutput->SetDefaultDevice(wanted.id);
                    // And a fresh reading asked for at once, rather than up to a second of
                    // showing the old device as the current one.
                    systems.soundOutput->Refresh();
                    island::LogAndPrint(
                        L"  sound page: switching the default device to %d '%ls'\n",
                        soundHit.index, wanted.name);
                }
            }
            break;

        case island::SoundPageTarget::Spatial:
            // Reported rather than pretended: there is no public interface for reading or
            // writing the spatial-sound setting, so the page shows the shell's two choices
            // and says so when one is pressed.
            island::LogAndPrint(
                "  sound page: spatial sound row %d -- no public interface to change it;"
                " open 更多音量设置 for this one\n",
                soundHit.index);
            break;

        case island::SoundPageTarget::MixerSlider: {
            const int row = soundHit.index;
            if (row == 0) {
                // The device's own row is the master volume, which is AudioVolume's.
                if (volumeAvailable) {
                    systems.volume.SetLevel(soundHit.fraction);
                    bar.mixerSliderDragRow = 0;
                    bar.mixerSliderDragPid = 0;
                    island::LogAndPrint("  mixer: master -> %.0f%%\n",
                                        soundHit.fraction * 100.0f);
                }
            } else if (row - 1 < systems.soundSnapshot->sessionCount) {
                const island::SoundSessionInfo& session =
                    systems.soundSnapshot->sessions[row - 1];
                systems.soundOutput->SetSessionVolume(session.pid, soundHit.fraction);
                bar.mixerSliderDragRow = row;
                bar.mixerSliderDragPid = session.pid;
                island::LogAndPrint(L"  mixer: '%ls' (pid %lu) -> %.0f%%\n",
                                    session.name, session.pid,
                                    soundHit.fraction * 100.0f);
            }
            std::fflush(stdout);
            break;
        }

        case island::SoundPageTarget::MixerGear:
        case island::SoundPageTarget::Footer: {
            // 更多音量设置, and the mixer's own gear: both are the settings page, which is
            // where the parts of this page that have no interface of their own live.
            const HINSTANCE opened = ShellExecuteW(nullptr, L"open", L"ms-settings:sound",
                                                   nullptr, nullptr, SW_SHOWNORMAL);
            island::LogAndPrint(
                "  sound page: %ls -> ms-settings:sound (result %lld)\n",
                soundHit.target == island::SoundPageTarget::Footer
                    ? L"more volume settings"
                    : L"mixer gear",
                static_cast<long long>(reinterpret_cast<INT_PTR>(opened)));
            // The settings page is opening, so the card steps aside -- the same rule the
            // panel's tiles follow.
            DismissPanelSurface(bar, systems, active, resting);
            break;
        }

        case island::SoundPageTarget::Scrollbar: {
            // A press on the bar.track moves the page a screenful towards where it was pressed,
            // which is what the shell's own scrollbar does. The window says WHICH WAY -- it is
            // the one that knows where the thumb is, from the same layout the press was
            // hit-tested against -- and the OFFSET stays here with the page it belongs to.
            const int jump = systems.window.SoundScrollJumpFor(cursor);
            if (jump != 0) {
                bar.soundPageScroll += static_cast<float>(jump) * 140.0f;
                systems.window.SetSoundPageScroll(bar.soundPageScroll);
                bar.soundPageScroll = systems.window.SoundPageScroll();
                island::LogAndPrint("  sound page: scroll -> %.0f\n", bar.soundPageScroll);
            }
            break;
        }

        case island::SoundPageTarget::None:
        default:
            // Anything else on the page -- the padding between rows, a section label -- is
            // the page itself, and pressing it means nothing rather than "close".
            break;
    }
    std::fflush(stdout);
}

// --- the press, and the click it turns into ---
//
// Moved out of the frame loop verbatim (indentation aside), both blocks together, because they are one
// gesture: a press on a slider is where a drag BEGINS and sets the value under the pointer at once, and the
// release that follows is only the END of that gesture -- which is why the click chain below opens with the
// dismissal of a drag. Everything it touches comes in explicitly: BarState and the window services, the
// dismissal's four arguments, the monitor the brightness write needs, and the two flags that say whether the
// volume reader and the mixer exist.
//
// A click is routed to whatever it landed on. The icon row is checked FIRST because the icons are not the
// island: a click on one asks to switch to that window, and must not also be read as an acknowledgement of
// the notifications or as a transport control. A while loop, not an if: clicks are counted now, and two can
// arrive inside one frame -- with a flag the second was silently dropped.
static void ApplyPressesAndClicks(island::BarState& bar, island::BarSystems& systems,
                                  const island::Notification* active, island::IslandState resting,
                                  const island::MonitorGeometry& monitor, bool volumeAvailable,
                                  bool soundOutputStarted) {
    {
        POINT pressed = {};
        while (systems.window.TakePress(&pressed)) {
            if (!bar.systemPanelOpen) continue;
            // The level that is SHOWING decides which control the press lands on, exactly as the click
            // chain below does -- see the note at `panelHit`. Without it the mixer's rows answered
            // while the tray page was up, because the two share the card.
            if (island::LevelAnswersInput(bar.panelLevel, island::PanelLevel::Sound)) {
                const island::SoundPageHit hit = systems.window.SoundPageHitAtScreen(pressed);
                if (hit.target == island::SoundPageTarget::MixerSlider) {
                    bar.mixerSliderDragRow = hit.index;
                    bar.lastAppliedMixerFraction = hit.fraction;
                    if (hit.index == 0) {
                        if (volumeAvailable) systems.volume.SetLevel(hit.fraction);
                    } else if (soundOutputStarted && systems.soundOutput &&
                               hit.index - 1 < systems.soundSnapshot->sessionCount) {
                        bar.mixerSliderDragPid = systems.soundSnapshot->sessions[hit.index - 1].pid;
                        systems.soundOutput->SetSessionVolume(bar.mixerSliderDragPid, hit.fraction);
                    }
                    island::LogAndPrint("  mixer: press on row %d -> %.0f%%\n", hit.index,
                                        hit.fraction * 100.0f);
                    std::fflush(stdout);
                }
            } else if (island::LevelAnswersInput(bar.panelLevel, island::PanelLevel::Tiles)) {
                const island::SystemPanelHit hit = systems.window.SystemPanelHitAtScreen(pressed);
                if (hit.target == island::SystemPanelTarget::Slider) {
                    // WHICH slider, from the hit's own index: 0 is the volume row and 1 the
                    // brightness row, and the drag that this press begins has to follow the row it
                    // started on rather than whichever one is looked for first.
                    if (hit.index == 1 && bar.brightnessAvailable) {
                        bar.sliderPressHandled = true;
                        bar.panelSliderDrag = true;
                        bar.panelSliderDragRow = 1;
                        int landed = 0;
                        const int wanted = static_cast<int>(hit.fraction * 100.0f + 0.5f);
                        const bool ok =
                            island::SetBrightnessPercent(monitor.monitor, wanted, &landed);
                        bar.lastAppliedBrightnessPercent = landed;
                        bar.lastLoggedBrightnessPercent = landed;
                        bar.brightnessWriteNotBefore = GetTickCount64() + 60;
                        bar.panelBrightnessEchoUntil = GetTickCount64() + 1200;
                        island::LogAndPrint(
                            "  brightness: press on the slider -> %d%% (asked %d, %ls)\n",
                            landed, wanted, ok ? "landed" : "the monitor did not take it");
                        std::fflush(stdout);
                    } else if (volumeAvailable) {
                        bar.sliderPressHandled = true;
                        bar.panelSliderDrag = true;
                        bar.panelSliderDragRow = 0;
                        bar.lastAppliedVolumeFraction = hit.fraction;
                        systems.volume.SetLevel(hit.fraction);
                        bar.panelVolumeEchoUntil = GetTickCount64() + 1200;
                        island::LogAndPrint("  volume: press on the slider -> %.0f%%\n",
                                            hit.fraction * 100.0f);
                        std::fflush(stdout);
                    }
                }
            }
        }
    }

    while (systems.window.TakeClick()) {
        POINT cursor = {};
        GetCursorPos(&cursor);

        // A release that ENDS a drag is not a click on whatever happens to be under the pointer. The
        // value was already set by the press and by every frame of the drag in between; letting the
        // click run as well would be a second handler for one gesture, and on the mixer it would
        // re-apply the value to whichever row the pointer finished over.
        if (bar.panelSliderDrag || bar.mixerSliderDragRow >= 0) {
            const bool wasMixer = bar.mixerSliderDragRow >= 0;
            bar.panelSliderDrag = false;
            bar.mixerSliderDragRow = -1;
            bar.mixerSliderDragPid = 0;
            island::LogAndPrint("  %ls: drag ended by the release\n",
                                wasMixer ? L"mixer" : L"volume");
            std::fflush(stdout);
            continue;
        }

        // THE TRAY'S OWN MENU, while it is up: a click on one of its rows runs that row, and a
        // click anywhere else puts the menu away and does nothing else. The click that dismisses a
        // menu is CONSUMED -- that is what every context menu does, and the alternative (dismiss and
        // then act on whatever was underneath) presses a tray icon the user was only trying to get
        // out from under the menu.
        if (systems.window.TrayMenuIsOpen()) {
            const int row = systems.window.TrayMenuRowHovered();
            // ARMED, NOT DONE. 结束应用 is the one entry that cannot be taken back -- it ends the
            // application's processes and gives it no chance to save -- so the first press only says
            // what the second will do (the row's own label changes; see TrayMenuArmedLabel) and the
            // menu stays open, so the second press is the same gesture in the same place. The Start
            // card's power list is arranged the same way, for the same reason.
            if (row >= 0 && row < bar.trayMenuActionCount &&
                bar.trayMenuActions[row] == island::TrayMenuAction::QuitApplication &&
                !bar.trayQuitArmed) {
                bar.trayQuitArmed = true;
                // WIDE, because this line is Chinese: the narrow overload widens its argument a byte
                // at a time, which is how `结束应用` came out as `ç»æåºç¨` in the log while the
                // lines around it read correctly (the same defect HANDOFF 8.72/8.73 records).
                island::LogAndPrint(
                    L"  tray menu: 结束应用 armed -- the same row again carries it out\n");
                std::fflush(stdout);
                continue;
            }
            systems.window.CloseTrayMenu();
            bar.trayQuitArmed = false;
            if (row >= 0 && row < bar.trayMenuActionCount) {
                const island::TrayMenuAction action = bar.trayMenuActions[row];
                island::LogAndPrint(L"  tray menu: %ls\n",
                                    island::TrayMenuLabel(action, bar.trayMenuOwnMenuUnreachable));
                std::fflush(stdout);
                island::RunTrayMenuAction(systems.trayIcons, systems.taskWindows, bar.trayMenuCell, action);
                if (action == island::TrayMenuAction::Open ||
                    action == island::TrayMenuAction::CloseWindow ||
                    action == island::TrayMenuAction::ApplicationMenu) {
                    // The card goes with the press, which is what the shell does with its own
                    // flyout: the application's window or menu is what the user is waiting for.
                    DismissPanelSurface(bar, systems, active, resting);
                }
            } else {
                island::LogAndPrint("  tray menu: closed (the click was not on a row)\n");
                std::fflush(stdout);
            }
            continue;
        }

        // What the panel's own controls are under the click, asked ONCE.
        //
        // The chain below asks this same question for every kind of target it knows, and each of
        // those calls re-lays the panel out to answer it. One value, read once, so the answers cannot
        // disagree with each other within a single click either.
        const island::SystemPanelHit panelHit =
            (bar.systemPanelOpen && island::LevelAnswersInput(bar.panelLevel, island::PanelLevel::Tiles))
                ? systems.window.SystemPanelHitAtScreen(cursor)
                : island::SystemPanelHit{};

        // ... AND ONLY WHILE THE TILES ARE THE LEVEL THAT IS SHOWING. All four levels share one card
        // and one rectangle, so each of them has controls where another has empty space: the panel's
        // volume slider lies exactly across the tray page's lower band, and dragging there moved the
        // volume -- reported with a photograph of that band ("在托盘卡片红框的地方拖动 声音也会跟着
        // 变"). Gating the question here gates every branch below that asks `panelHit`, which is why
        // the answer is computed once, at the top, rather than re-asked per branch.

        // What the click was shown to mean, before anything acts on it: which level is up, what the
        // panel's own hit test answered, and what shape the window is in. Without this, "the tile did
        // nothing" and "the click was routed somewhere else entirely" look the same from outside.
        // The WIDE overload, and it matters: the narrow one byte-widens its output, which turns any
        // non-ASCII argument into an invalid conversion -- and an invalid conversion makes the whole
        // line EMPTY while still writing the newline. That is why "the tile did nothing" was the
        // report for three rounds: the tile's own log line was being swallowed, and an empty line in
        // the log looks like nothing happened.
        island::LogAndPrint(L"  click: panel=%d soundPage=%d target=%d shape=%ls\n",
                            bar.systemPanelOpen ? 1 : 0, bar.soundPageOpen ? 1 : 0,
                            static_cast<int>(panelHit.target),
                            island::StateName(systems.window.State()));

        // Nothing about this can be concluded from a screenshot, so it reports what it
        // decided: which icon was hit, which window that was, and whether the activation
        // succeeded. A click that never arrives and a click that arrives and is ignored look
        // identical from outside, and the two need opposite fixes -- that is why this exists.
        // Bounded by the number of icons actually DRAWN, not by the number of windows in the
        // list. `IconAtScreen` answers from the same count the window was given, so it cannot
        // return more than that anyway -- but the list can hold windows the row had no room
        // for, and indexing straight into it would switch to a window with no icon on screen.
        const int icon = systems.window.IconAtScreen(cursor);
        island::LogAndPrint("  click: icon under the pointer = %d\n", icon);

        // The START BUTTON first, because it is on the far side of the island from the icons
        // and a click on it must not be read as anything else.
        //
        // Its outcome is reported rather than assumed: asking for the Start menu can fail in
        // ways that leave no trace afterwards, and "the button did nothing" is the same symptom
        // for every one of them.
        if (systems.window.StartAtScreen(cursor)) {
            // THE BAR'S OWN START CARD, which is what was asked for: "能不能也自己画 而不是使用系统
            // 的". The system menu is not gone -- the Windows key and Ctrl+Esc still open it -- it is
            // simply no longer what THIS button does, and that is one line in the log rather than a
            // silent change of behaviour: "start click: the bar's own card".
            bar.startPageOpen = !bar.startPageOpen;
            bar.systemPanelOpen = bar.startPageOpen;
            bar.startPageState = 0;
            bar.startAllApps = false;
            bar.startPickerScroll = 0;
            bar.startPowerConfirm = -1;
            bar.trayPageOpen = false;
            bar.soundPageOpen = false;
            island::LogAndPrint("  start click: the bar's own card %ls\n",
                                bar.startPageOpen ? L"open" : L"closed");
            std::fflush(stdout);
        } else if (systems.trayIcons.Count() > 0 && systems.window.TrayButtonAtScreen(cursor)) {
            // THE TRAY BUTTON, at the other end of the bar from the system button, and a toggle
            // for the same reason: pressing it again puts away what it opened.
            //
            // It opens the SAME CARD as the system panel, one level down (see TrayPageFor), so the
            // two share a card, a position and an animation -- and the two flags have to be kept in
            // step: opening one closes the other, or the card would be asked to be two pages at
            // once.
            const bool opening = !bar.trayPageOpen;
            bar.trayPageOpen = opening;
            bar.systemPanelOpen = opening;
            bar.systemPanelPage = 0;
            bar.soundPageOpen = false;
            bar.panelSliderDrag = false;
            bar.mixerSliderDragRow = -1;
            bar.mixerSliderDragPid = 0;
            island::LogAndPrint("  tray: %ls\n", opening ? L"card open" : L"card closed");
            std::fflush(stdout);
        } else if (island::LevelAnswersInput(bar.panelLevel, island::PanelLevel::Start) &&
                   systems.window.StartHitHovered().target !=
                       island::IslandWindow::StartHitTarget::None) {
            // The Start card own press handling. The phase lives above RunBar; the dismiss calls
            // inside it are the same four arguments the lambda used.
            ApplyStartCardPress(bar, systems, active, resting);
        } else if (island::LevelAnswersInput(bar.panelLevel, island::PanelLevel::Tray) &&
                   systems.window.TrayCellHovered() >= 0) {
            // A CELL OF THE TRAY PAGE: press that application's tray icon, which is the only
            // thing about the tray this bar cannot do itself -- the menu or the window that
            // follows belongs to the application, and the shell is the one that knows how to ask
            // for it. See TrayPress for the route, and for why it is by POSITION rather than by
            // matching names.
            //
            // On a WORKER THREAD, because showing the shell's tray, opening its overflow and
            // pressing through UI Automation takes a few hundred milliseconds, and the bar draws
            // at the compositor's rate on this thread.
            const int cell = systems.window.TrayCellHovered();
            if (cell < systems.trayIcons.Count()) {
                island::StartTrayPress(systems.trayIcons, cell, false);
            }
            // The card goes with the press, which is what the shell does with its own flyout: the
            // application's window or menu is what the user is waiting for.
            DismissPanelSurface(bar, systems, active, resting);
        } else if (systems.window.SystemButtonAtScreen(cursor)) {
            // The system panel is a TOGGLE, like the shell's own flyout: pressing the control
            // again closes what it opened. Without that the only way out would be to click
            // somewhere unrelated, which is not a way out a user can guess.
            bar.systemPanelOpen = !bar.systemPanelOpen;
            bar.systemPanelPage = 0;   // always opens on the first page, like the shell's flyout
            // ... and the tray's page is a level of the same card, so it goes with it.
            bar.trayPageOpen = false;
            // The page is a LEVEL of the panel, so closing the panel closes it too -- and a drag
            // left half-done is ended, or the next press anywhere would go on dragging a slider
            // that is no longer on screen.
            bar.soundPageOpen = false;
            bar.mixerSliderDragRow = -1;
            bar.mixerSliderDragPid = 0;
            island::LogAndPrint("  system panel: %ls\n",
                                bar.systemPanelOpen ? L"open" : L"closed");
            std::fflush(stdout);
        } else if (island::LevelAnswersInput(bar.panelLevel, island::PanelLevel::Sound)) {
            // The 声音输出 page own controls. The phase lives above RunBar; the dismiss call inside
            // it is the same four arguments the lambda used.
            ApplySoundPagePress(bar, systems, cursor, volumeAvailable, active, resting);
        } else if (bar.systemPanelOpen && panelHit.target ==
                                          island::SystemPanelTarget::PageDown) {
            const int pages = systems.window.SystemPanel().pageCount;
            if (pages > 1) {
                bar.systemPanelPage = (systems.window.SystemPanel().pageIndex + 1) % pages;
                island::LogAndPrint("  system panel: page %d of %d\n", bar.systemPanelPage + 1,
                                    pages);
                std::fflush(stdout);
            }
        } else if (bar.systemPanelOpen && panelHit.target ==
                                          island::SystemPanelTarget::PageUp) {
            const int pages = systems.window.SystemPanel().pageCount;
            if (pages > 1) {
                bar.systemPanelPage = (systems.window.SystemPanel().pageIndex + pages - 1) % pages;
                island::LogAndPrint("  system panel: page %d of %d\n", bar.systemPanelPage + 1,
                                    pages);
                std::fflush(stdout);
            }
        } else if (bar.systemPanelOpen && panelHit.target ==
                                          island::SystemPanelTarget::Slider) {
            // Already handled by the press that this release ends. Without this the same gesture
            // would be applied twice -- once when the button went down and once when it came up --
            // and the log would show a second "drag begins" for a drag that had just finished.
            if (bar.sliderPressHandled) {
                bar.sliderPressHandled = false;
            } else if (panelHit.index == 1 && bar.brightnessAvailable) {
                // Pressed without a press event having been seen -- a click that arrived on its
                // own. The fraction comes from the same layout the bar was drawn from, so the
                // handle goes exactly where the click was.
                int landed = 0;
                const int wanted = static_cast<int>(panelHit.fraction * 100.0f + 0.5f);
                const bool ok =
                    island::SetBrightnessPercent(monitor.monitor, wanted, &landed);
                bar.lastAppliedBrightnessPercent = landed;
                bar.panelSliderDrag = true;
                bar.panelSliderDragRow = 1;
                bar.panelBrightnessEchoUntil = GetTickCount64() + 1200;
                island::LogAndPrint(
                    "  brightness: set to %d%% (drag begins; asked %d, %ls)\n", landed,
                    wanted, ok ? "landed" : "the monitor did not take it");
                std::fflush(stdout);
            } else if (volumeAvailable) {
                // Pressed without a press event having been seen -- a click that arrived on its own,
                // which happens when the panel opened between the two. The fraction comes from the
                // same layout the bar was drawn from, so the handle goes exactly where the click was.
                systems.volume.SetLevel(panelHit.fraction);
                bar.panelSliderDrag = true;   // and keep following the pointer while the button is held
                bar.panelVolumeEchoUntil = GetTickCount64() + 1200;
                island::LogAndPrint("  volume: set to %.0f%% (drag begins)\n",
                                    panelHit.fraction * 100.0f);
                std::fflush(stdout);
            }
        } else if (bar.systemPanelOpen && panelHit.target ==
                                          island::SystemPanelTarget::Speaker) {
            // The speaker glyph is the mute control, as it is in the shell's own flyout.
            if (volumeAvailable) {
                systems.volume.ToggleMute();
                bar.panelVolumeEchoUntil = GetTickCount64() + 1200;
                island::LogAndPrint("  volume: %ls\n",
                                    systems.volume.IsMuted() ? L"muted" : L"unmuted");
                std::fflush(stdout);
            }
        } else if (bar.systemPanelOpen && panelHit.target ==
                                          island::SystemPanelTarget::VolumeDevice) {
            // The output-device button: the shell's own second level, where the devices and the
            // per-application mixer live.
            bar.soundPageOpen = true;
            bar.soundPageScroll = 0.0f;
            island::LogAndPrint("  sound page: open\n");
            std::fflush(stdout);
        } else if (bar.systemPanelOpen && panelHit.target ==
                                          island::SystemPanelTarget::Gear) {
            // The settings button, which has been drawn since the panel was built and has never had an
            // action. It opens the sound settings, which is where this panel's own contents live.
            const HINSTANCE opened = ShellExecuteW(nullptr, L"open", L"ms-settings:sound",
                                                   nullptr, nullptr, SW_SHOWNORMAL);
            island::LogAndPrint("  system panel: gear -> ms-settings:sound (result %lld)\n",
                                static_cast<long long>(reinterpret_cast<INT_PTR>(opened)));
            // Same rule as the tiles that take the user elsewhere: the page they asked for is opening,
            // so the panel gets out of the way.
            DismissPanelSurface(bar, systems, active, resting);
            std::fflush(stdout);
        } else if (bar.systemPanelOpen && panelHit.target ==
                                          island::SystemPanelTarget::Tile) {
            // A tile was pressed. What it does is carried ON the tile, so this does not have to
            // know which tile is which -- and renaming one cannot silently change its action.
            //
            // Read from the WINDOW rather than from a frame, because the click handler runs
            // before this frame's state exists: the frame is filled further down the loop.
            const int tileIndex = panelHit.index;
            const island::TextRenderer::SystemPanelText& panel = systems.window.SystemPanel();
            if (tileIndex >= 0 && tileIndex < panel.tileCount) {
                const island::TextRenderer::SystemPanelTile& tile = panel.tiles[tileIndex];
                island::LogAndPrint(L"  click: tile %d '%ls' action=%d chevron=%d\n", tileIndex,
                                    tile.label ? tile.label : L"?",
                                    static_cast<int>(tile.action), tile.chevron ? 1 : 0);
                switch (tile.action) {
                    case island::TextRenderer::PanelAction::OpenSettings: {
                        // ShellExecute with the ms-settings URI, which is how the shell's own
                        // flyout opens these pages. Reported with its result because "the tile
                        // did nothing" and "the page failed to open" look identical otherwise.
                        // A result above 32 means success; the numeric value is kept because
                        // the failure codes are what would be needed to diagnose it.
                        const HINSTANCE opened =
                            tile.uri ? ShellExecuteW(nullptr, L"open", tile.uri, nullptr,
                                                     nullptr, SW_SHOWNORMAL)
                                     : nullptr;
                        island::LogAndPrint(L"  tile %d '%ls' -> %ls (result %lld)\n",
                                            tileIndex, tile.label ? tile.label : L"?",
                                            tile.uri ? tile.uri : L"no uri",
                                            static_cast<long long>(
                                                reinterpret_cast<INT_PTR>(opened)));
                        // ... and the panel puts itself away.
                        //
                        // This tile's whole job was to take the user somewhere else, and leaving the
                        // card up would leave them looking at a flyout for a page they are no longer
                        // on -- which is what "点击应该…自动收回去" asked for. The tiles that apply
                        // their change HERE (the volume row, and any toggle) stay open instead, so
                        // their new state is there to be seen and several can be flipped in a row.
                        DismissPanelSurface(bar, systems, active, resting);
                        break;
                    }
                    case island::TextRenderer::PanelAction::ToggleMute:
                    case island::TextRenderer::PanelAction::None:
                    default:
                        island::LogAndPrint(L"  tile %d '%ls': no action yet\n", tileIndex,
                                            tile.label ? tile.label : L"?");
                        break;

                    case island::TextRenderer::PanelAction::OpenSoundPage:
                        // A tile that opens the sound page rather than a settings URI. None of them do
                        // today -- the page is reached from the volume row's device button -- but the
                        // action exists, so it is handled rather than falling into "no action yet".
                        bar.soundPageOpen = true;
                        bar.soundPageScroll = 0.0f;
                        island::LogAndPrint(L"  tile %d '%ls' -> sound page\n", tileIndex,
                                            tile.label ? tile.label : L"?");
                        break;

                    case island::TextRenderer::PanelAction::ToggleRadio: {
                        // BOTH radios, which is what this tile has always claimed to be. The state is
                        // read back on the next sample rather than flipped locally, so a switch the
                        // machine refuses shows as what it really is.
                        //
                        // Off when anything is still on, on otherwise -- the same rule the shell's own
                        // airplane mode follows, and the one a user expects from a toggle that is dark
                        // while a radio beside it is lit.
                        const bool wanted = systems.systemStatus.wifiRadioOn || systems.systemStatus.bluetoothOn;
                        const bool started = island::RequestRadios(true, true, wanted);
                        island::LogAndPrint(
                            L"  tile %d '%ls' -> Wi-Fi and Bluetooth %ls (result %d)\n",
                            tileIndex, tile.label ? tile.label : L"?",
                            wanted ? L"off (airplane mode)" : L"on", started ? 1 : 0);
                        // Deliberately NOT closed: a toggle applies its change here, and its new
                        // state is the thing to look at. Several can be flipped in a row.
                        break;
                    }

                    case island::TextRenderer::PanelAction::ToggleWifi: {
                        // The Wi-Fi radio on its own, from the Wi-Fi tile. Its own switch and the
                        // 飞行模式 tile's are the same mechanism at different scopes, which is why
                        // they are one call with different arguments rather than two code paths.
                        const bool wanted = !systems.systemStatus.wifiRadioOn;
                        const bool started = island::RequestRadios(true, false, wanted);
                        island::LogAndPrint(
                            L"  tile %d '%ls' -> Wi-Fi radio %ls (%ls)\n", tileIndex,
                            tile.label ? tile.label : L"?", wanted ? L"on" : L"off",
                            started ? L"asked, the answer follows in the log"
                                    : L"a switch is already running");
                        break;
                    }

                    case island::TextRenderer::PanelAction::ProjectionModes: {
                        // 投影, which the shell answers by sending Win+P. Synthetic input, so it is
                        // dropped by UIPI when this process sits below the shell -- the same caveat
                        // the Start button carries, and the reason the outcome is reported.
                        island::SendWindowsKeyChord('P');
                        island::LogAndPrint(L"  tile %d '%ls' -> Win+P (projection modes)\n",
                                            tileIndex, tile.label ? tile.label : L"?");
                        // The projection list is the shell's own flyout and appears over this one:
                        // staying open would put two flyouts on top of each other.
                        DismissPanelSurface(bar, systems, active, resting);
                        break;
                    }

                    case island::TextRenderer::PanelAction::ToggleBluetooth: {
                        // The radio, switched on a WORKER: the WinRT call blocks for a few hundred
                        // milliseconds and the frame loop must not wait for it. The outcome is
                        // collected in the loop below and logged there, and the caption is read back
                        // from the machine on the next status sample rather than flipped here.
                        const bool wanted = !systems.systemStatus.bluetoothOn;
                        const bool started = island::RequestRadios(false, true, wanted);
                        island::LogAndPrint(
                            L"  tile %d '%ls' -> Bluetooth %ls (%ls)\n", tileIndex,
                            tile.label ? tile.label : L"?", wanted ? L"on" : L"off",
                            started ? L"asked, the answer follows in the log"
                                    : L"a switch is already running");
                        // Like the Wi-Fi tile: a toggle applies its change here, so the new state is
                        // the thing to look at.
                        break;
                    }

                    case island::TextRenderer::PanelAction::ToggleNightLight: {
                        // Pressed on the Settings page's own switch, on a worker, because that page
                        // takes seconds to load. The cost is stated in the log line: a Settings window
                        // appears for the length of the operation.
                        const bool wanted = !systems.systemStatus.nightLightOn;
                        const bool started = island::RequestNightLight(wanted);
                        island::LogAndPrint(
                            L"  tile %d '%ls' -> night light %ls (%ls; this opens the Settings page"
                            L" for a moment -- it has no API)\n",
                            tileIndex, tile.label ? tile.label : L"?",
                            wanted ? L"on" : L"off",
                            started ? L"asked, the answer follows in the log"
                                    : L"a switch is already running");
                        break;
                    }

                    case island::TextRenderer::PanelAction::ToggleLiveCaptions: {
                        // Win+Ctrl+L, the shortcut the feature is switched by. Sent here rather than on
                        // a worker: the chord itself is a few milliseconds, and the state is read back
                        // from the process list on the next status sample.
                        const bool sent = island::ToggleLiveCaptions();
                        island::LogAndPrint(
                            L"  tile %d '%ls' -> Win+Ctrl+L (result %d; the tile's caption comes"
                            L" from whether LiveCaptions.exe is running)\n",
                            tileIndex, tile.label ? tile.label : L"?", sent ? 1 : 0);
                        break;
                    }

                    case island::TextRenderer::PanelAction::CycleTheme: {
                        // The whole material at once, and it takes effect on the NEXT frame
                        // because every drawing call reads CurrentTheme(). The brush cache is
                        // cleared with it: the cached brushes are keyed by colour, so a theme
                        // change would otherwise leave every old colour in memory for the rest
                        // of the session.
                        const island::Theme& chosen = island::CycleTheme();
                        systems.text.ClearBrushCache();
                        island::LogAndPrint(
                            L"  tile %d '%ls' -> theme %ls (%d of %d)\n", tileIndex,
                            tile.label ? tile.label : L"?", chosen.name,
                            island::CurrentThemeIndex() + 1, island::ThemeCount());
                        // The tile's own caption is the theme's name, and it is built from the
                        // frame's panel text -- which was assembled earlier in this same pass.
                        // Redrawing on the next pass is what updates it, and the theme change
                        // already asks for one.
                        break;
                    }
                }
                std::fflush(stdout);
            }
        } else if (bar.systemPanelOpen) {
            // Anything else while the panel is open is treated as "put it away".
            //
            // A flyout closes when you click outside it, and on this bar the island IS outside
            // it. Without this the island's own click was still handled as a request for the
            // card, so pressing it opened a SECOND card-style surface while the panel was up --
            // reported as "clicking this one opened it too".
            //
            // Closing on any other click is also the behaviour a user expects, and it is the only
            // way out that does not require finding the chevron again.
            DismissPanelSurface(bar, systems, active, resting);
            island::LogAndPrint("  system panel: closed (click elsewhere)\n");
            std::fflush(stdout);
        } else if (icon >= 0 && icon < bar.taskWindowsShown) {
            // The handle that was DRAWN at this position, not whatever the live list holds now.
            const HWND target = bar.taskHandles[icon];
            island::LogAndPrint("  icon click: index %d -> %p", icon,
                                reinterpret_cast<void*>(target));

            // The taskbar button is a TOGGLE: the window already in front is put away, any
            // other comes forward. Without this the bar could only ever raise things, which is
            // half the gesture -- reported as "clicking an application on the desktop does not
            // minimise it".
            //
            // Asked LIVE rather than from the list's cached foreground, because the cache is
            // refreshed twice a second and the whole question is which window is in front at
            // the instant of the click.
            const bool alreadyInFront = (GetForegroundWindow() == target);

            if (alreadyInFront) {
                if (systems.taskWindows.Minimize(target)) {
                    island::LogAndPrint("  icon click: minimised %p (it was already in front)",
                                        reinterpret_cast<void*>(target));
                } else {
                    island::LogAndPrint("  icon click: minimise FAILED for %p",
                                        reinterpret_cast<void*>(target));
                }
            } else if (systems.taskWindows.Activate(target)) {
                island::LogAndPrint("  icon click: activated %p",
                                    reinterpret_cast<void*>(target));
            } else {
                // Not reported as success. A minimised window that stayed minimised, or a
                // refused SetForegroundWindow, both land here -- and reporting "activated"
                // regardless would make the next investigation start from a false fact.
                island::LogAndPrint("  icon click: activation FAILED for %p",
                                    reinterpret_cast<void*>(target));
            }
            std::fflush(stdout);
        } else {
            // A click that is not on an icon, reported with the numbers that decided it.
            //
            // "The click was never routed" and "it was routed and the hit test missed" are
            // different faults and the log otherwise shows neither. This is the line that
            // separates them.
            island::LogAndPrint("  icon click: no icon (index %d, %d shown)", icon,
                                bar.taskWindowsShown);
            std::fflush(stdout);

        const island::CardControl control = systems.window.MusicControlAtScreen(cursor);
        // Every click reports what it was taken to mean.
        //
        // "The card does not respond" and "the click never reached the process" look the
        // same from outside, and the second one cannot even be logged from in here -- so
        // this line's presence or absence is itself the answer.
        {
            const char* what = "dismiss";
            if (control == island::CardControl::Previous) {
                what = "previous";
            } else if (control == island::CardControl::PlayPause) {
                what = "play/pause";
            } else if (control == island::CardControl::Next) {
                what = "next";
            }
            island::LogAndPrint("  click routed: state=%ls, meaning=%s",
                                island::StateName(systems.window.State()), what);
            std::fflush(stdout);
        }
        if (control != island::CardControl::None) {
            island::MusicSession::Transport action =
                island::MusicSession::Transport::PlayPause;
            const char* name = "play/pause";
            if (control == island::CardControl::Previous) {
                action = island::MusicSession::Transport::Previous;
                name = "previous";
            } else if (control == island::CardControl::Next) {
                action = island::MusicSession::Transport::Next;
                name = "next";
            }
            if (systems.music.RequestTransport(action)) {
                island::LogAndPrint("  music: %s requested", name);
                std::fflush(stdout);
            }
        } else if (systems.window.State() == island::IslandState::NoticeStrip) {
            // A CLICK ON A MESSAGE OPENS THE WINDOW THAT SENT IT -- "点击通知能打开当前来通知的窗口".
            //
            // This branch used to KEEP the strip and mark nothing read, after a press on the plain pill
            // was found to dismiss a message that was still wanted. A click that asks for the sender's
            // window is a different gesture, and it is the one the notice is for: the message is then
            // marked read -- which is what retracts the strip, since ActiveDisplay answers null for a
            // read item -- and the window comes forward. A call never reaches this branch (it is the bar
            // cluster; see NextShape) and is never marked read.
            const HWND sender = WindowForNotification(systems.taskWindows, active);
            if (sender && systems.taskWindows.Activate(sender)) {
                island::LogAndPrint("  notice click: opened the sender's window %p",
                                    reinterpret_cast<void*>(sender));
            } else if (sender) {
                island::LogAndPrint("  notice click: the sender's window %p refused activation",
                                    reinterpret_cast<void*>(sender));
            } else {
                island::LogAndPrint("  notice click: no window found for the sender");
            }
            const int marked =
                systems.notifications.MarkAllReadExcept(systems.wechat.CallNotificationId());
            if (marked > 0) {
                island::LogAndPrint("  notifications: %d marked read (notice click)", marked);
            }
            std::fflush(stdout);
        } else {            // A CLICK DISMISSES MESSAGES, NOT A CALL.
            //
            // Every other item on the strip is news: showing it for a few seconds and marking it read
            // when it is clicked is what "read" means. A CALL is a STATE, and the user asked for it to
            // stay on the island for as long as it lasts ("微信通话 也应该在灵动岛 上") -- so a click
            // must leave it alone. It did not, and that is one of the two reasons the call strip
            // disappeared for good: `notifications: 1 marked read (click)` at 13:43:19 with the call
            // still up, after which the island never showed it again. The call is retired by the call
            // ENDING, which the watcher detects and reports.
            // A CALL IS A STATE AND ITS CLICK OPENS THE CALLER'S WINDOW; it is not marked read, so the
            // call strip stays up while the window comes forward.
            if (IsCallNotification(systems, active)) {
                const HWND caller = WindowForNotification(systems.taskWindows, active);
                if (caller && systems.taskWindows.Activate(caller)) {
                    island::LogAndPrint("  call click: opened the caller's window %p",
                                        reinterpret_cast<void*>(caller));
                }
            }
            const int marked = systems.notifications.MarkAllReadExcept(systems.wechat.CallNotificationId());
            if (marked > 0) {
                island::LogAndPrint("  notifications: %d marked read (click)", marked);
                std::fflush(stdout);
                // Recompute immediately so the same frame reflects the click
                // instead of waiting for the next tick to notice.
                systems.window.SetState(systems.window.IsPointerInside()
                                    ? island::IslandState::Hover
                                    : resting);
            }
        }
        }
    }
}

// --- does anything ask for a frame this pass? ---
//
// Moved out of the frame loop verbatim (indentation aside), with one named deviation: the four answers the
// loop's own counters read come back by reference, so the statements that were `bool x = false;` /
// `const bool x = ...` are assignments to those parameters, and the last statement returns rather than
// declaring `needDraw` for a reader that is one line below it. Everything the phase has to remember between
// passes is BarRuntime's.
//
// What it answers is the only reason the loop draws at all: the shape is moving (the caller's `animating`),
// the clock's second or the shown notification or a new bar.track changed (PollRedrawReasons), the lyric
// asked for it, the card's printed time moved on, a reading behind the strip arrived (volume, weather, the
// mixed-audio rows), one of the two meters is due, or the Start card's pointer moved. Nothing else can
// alter a frame.
static bool DecideNeedDraw(BarRuntime& runtime, island::BarState& bar, island::BarSystems& systems,
                           bool animating, bool clockChanged, bool notificationChanged,
                           bool musicChanged, bool lyricsScrolling, bool lyricLineChanged,
                           bool lyricLineAnimating, bool musicResting, bool volumeAvailable,
                           bool soundOutputStarted, bool haveDrawn, bool& progressChanged,
                           bool& weatherChanged, bool& musicMeterDue, bool& startHoverChanged) {
    // Does the card's printed position need to change?
    //
    // The bar and the elapsed time advance locally, so without this they would only be
    // redrawn when the media session happened to report a new position -- every couple
    // of seconds, which is the jumping the user saw. One redraw per second is enough
    // for a text clock and a bar that moves about 1% a second, and it only applies
    // while the card is actually open.
    // ... and it is the SAME playhead the pill's line uses (see the top of this iteration), so
    // the time printed on the card and the words on the island cannot disagree -- including for
    // a session that sends no timeline, where both are the bar's own count.
    const long long shownPosition =
        (systems.window.State() == island::IslandState::Expanded) ? bar.track.ms : 0;
    // The volume can be changed by the keyboard or the shell flyout, not only here, so
    // it is a redraw reason too.
    bool volumeChanged = false;
    if (volumeAvailable && systems.volume.Revision() != runtime.lastVolumeRevision) {
        runtime.lastVolumeRevision = systems.volume.Revision();
        volumeChanged = true;
    }

    // A new weather reading is the same kind of reason: it arrives on its own schedule, and
    // nothing else about the frame has to move for the strip to have something new to say.
    if (systems.weather.Revision() != bar.lastWeatherRevision) {
        bar.lastWeatherRevision = systems.weather.Revision();
        weatherChanged = true;
    }

    progressChanged = (shownPosition / 1000) != runtime.lastShownSecond;
    runtime.lastShownSecond = shownPosition / 1000;

    // The page changes for three reasons of its own: it was opened or closed, it was scrolled, or
    // the audio reading behind it moved on. Read BEFORE the frame is built, because the build is
    // what records the new values.
    const bool soundPageChanged =
        bar.soundPageOpen != runtime.lastSoundPageOpen ||
        (bar.soundPageOpen && systems.window.SoundPageScroll() != runtime.lastSoundScroll) ||
        (bar.soundPageOpen && soundOutputStarted && systems.soundOutput &&
         systems.soundOutput->Revision() != runtime.lastSoundRevision);

    // A call's level meter is the one thing on this bar that changes without an event, so it asks for
    // frames of its own. 20 Hz: twice a second looks broken, and sixty times a second is a meter nobody
    // can see paid for with a loop that never idles during a call that may last an hour.
    const bool callMeterDue = (systems.wechat.CallNotificationId() != 0) &&
                              (GetTickCount64() - bar.lastCallMeterMs >= 50);
    if (callMeterDue) bar.lastCallMeterMs = GetTickCount64();

    // The pill's background meter is the other thing here that moves without an event, and it
    // asks for frames at 30 Hz: the rate the reading behind it is refreshed at (see
    // AudioVolume::Peak), and what a drifting shape needs to look continuous rather than stepped.
    // Only while music is what the bar is showing: a wave nobody can see is a redraw nobody
    // asked for.
    musicMeterDue = musicResting && bar.activePlaying && volumeAvailable &&
                    (GetTickCount64() - runtime.lastMusicMeterMs >= 33);
    if (musicMeterDue) runtime.lastMusicMeterMs = GetTickCount64();

    // The Start card's pointer and its scroll, as CHANGE rather than as state: the window answers
    // where the pointer is, and only a different answer (or a different scroll offset) needs a
    // frame. See the note in the condition below for what this cost.
    {
        const island::IslandWindow::StartHit hit = systems.window.StartHitHovered();
        const int target = static_cast<int>(hit.target);
        startHoverChanged = island::StartCardAsksForFrame(target, hit.index, bar.startPickerScroll,
                                                         bar.lastStartHitTarget,
                                                         bar.lastStartHitIndex,
                                                         bar.lastStartScrollDip);
        bar.lastStartHitTarget = target;
        bar.lastStartHitIndex = hit.index;
        bar.lastStartScrollDip = bar.startPickerScroll;
    }

    return animating || clockChanged || notificationChanged ||
           musicChanged || lyricsScrolling || lyricLineChanged ||
           lyricLineAnimating || progressChanged || musicMeterDue ||
           bar.iconAnimating || bar.rowAppearAnimating || soundPageChanged ||
           callMeterDue || bar.statusStripAnimating || weatherChanged ||
           bar.controlWashAnimating ||
           // ... and the Start card's own pointer and scroll. Neither is a state
           // this loop watches anywhere else, and without them the highlight and the
           // list moved only on the next clock tick -- measured, that is ONE redraw
           // per second while idle, which is what "鼠标移上去卡卡的" was.
           startHoverChanged || bar.startWashAnimating ||
           systems.renderer.TakeLensFrame() ||
           !haveDrawn;
}

// --- the tray and the panel, told to the frame ---
//
// Moved out of the frame loop verbatim (indentation aside): the tray icons and their count, whether the
// card surface is still moving (the latch that keeps the full material for as long as the pointer is on the
// card, which is what stopped "鼠标在上面移动卡片会闪"), the tray menu rows and their labels, the three
// control washes, and the panel open flags. It reads the pass own animating flag, delta and tray-page
// answer, and carries one timestamp between frames through BarRuntime.


// --- the three reasons that come from a revision or a second ---
//
// Moved out of the frame loop verbatim (indentation aside), apart from the three answers: what the loop
// hands back are the reasons the counters and the decision below read, so the three declarations became out
// parameters and the statements that were `const bool x = ...` are assignments to them. Everything the
// phase has to REMEMBER between passes is BarRuntime's now -- the clock's last second, the last
// notification id and the last bar.track revision -- which is exactly what the struct is for, and what the
// reference's `lastSecond`-style locals were doing in RunBar's scope.
static void PollRedrawReasons(BarRuntime& runtime, island::BarSystems& systems,
                              const island::Notification* active, bool& clockChanged,
                              bool& notificationChanged, bool& musicChanged) {
    // --- draw ---
    //
    // Three reasons to draw: the shape moved, the clock's second changed, or
    // the notification being shown changed. Nothing else can alter a frame,
    // so when none holds the loop does no drawing at all.
    int second = -1;
    systems.text.CurrentSecond(&second);
    clockChanged = (second != runtime.lastSecond);
    runtime.lastSecond = second;

    const long long activeId = active ? active->id : 0;
    notificationChanged = (activeId != runtime.lastNotificationId);
    runtime.lastNotificationId = activeId;

    // A bar.track change is a reason to redraw even when nothing moved.
    musicChanged = (systems.music.Revision() != runtime.lastDrawnMusicRevision);
    runtime.lastDrawnMusicRevision = systems.music.Revision();
}

// --- the lyric line the resting strip shows ---
//
// Moved out of the frame loop verbatim (indentation aside). It decides what the pill says this pass: which
// line of the sheet the projected playhead is on, whether those words are NEW (a roll happens only then --
// see the note in the block), how far the marquee has crept, and, when the sheet has nothing for this track,
// the title as the honest fallback.
//
// The line, that fallback and the three answers the draw reads come back by reference; the sheet and the two
// numbers that say which line was last picked out are BarRuntime's; the wave's PHASE comes in by reference
// because it is the loop's own clock rather than state the drawing reads, and it is advanced here because
// this is where the strip's frames are decided. `musicResting`, and not the state, is the question -- see the
// note at the call site.
// --- WHAT A NOTIFICATION IS, where the shape depends on it ---------------------
//
// A CALL is a WeChat call announcement: the notice strip is the wrong shape for it (see
// PointerContext::callActive), and the two decisions that used to be "any notification" -- the notice
// strip being up and the resting pill being the wide one -- have to ask the same question the policy
// does, or the drawing, the window region and the state disagree about one box.
static bool IsCallNotification(const island::BarSystems& systems,
                               const island::Notification* active) {
    return active != nullptr && systems.wechat.CallNotificationId() != 0 &&
           active->id == systems.wechat.CallNotificationId();
}

// THE WINDOW A NOTIFICATION CAME FROM -- "点击通知能打开当前来通知的窗口".
//
// The sender's own process first: the pipe knows who connected, and a tool's own window is the answer.
// A toast names its app rather than its process, so for those the window titles are the only bridge; the
// app name is matched inside a title because a title is rarely just the app's name ("微信" beside a
// conversation). Nothing found returns null, and the caller says so rather than pretending it opened.
static HWND WindowForNotification(const island::WindowList& windows,
                                  const island::Notification* active) {
    if (!active) return nullptr;
    if (active->senderPid != 0) {
        for (int i = 0; i < windows.Count(); ++i) {
            if (windows.At(i).processId == active->senderPid) return windows.At(i).handle;
        }
    }
    if (active->app[0] != '\0') {
        wchar_t name[island::kAppNameCapacity] = {};
        if (MultiByteToWideChar(CP_UTF8, 0, active->app, -1, name,
                                static_cast<int>(island::kAppNameCapacity)) > 0) {
            for (int i = 0; i < windows.Count(); ++i) {
                if (std::wcsstr(windows.At(i).title, name) != nullptr) {
                    return windows.At(i).handle;
                }
            }
        }
    }
    return nullptr;
}

static void PickRestingLyricLine(BarRuntime& runtime, island::BarState& bar, island::BarSystems& systems,
                                  bool musicResting, float lyricScrollDipPerSec, float dt,
                                  const char*& lyricLine, const char*& fallbackLine, bool& lyricsScrolling,
                                  bool& lyricLineAnimating, bool& lyricLineChanged, float& musicWavePhase) {
    if (musicResting) {
        bar.lyricLineAge += static_cast<float>(dt > 0.1 ? 0.1 : dt);
        // The wave drifts even while the level sits still, which is what keeps it looking
        // alive between the beats. Wrapped, so a session that runs for hours cannot lose
        // precision in the sine's argument.
        musicWavePhase += static_cast<float>(dt > 0.1 ? 0.1 : dt) * 1.15f;
        if (musicWavePhase > 6.2831853f * 64.0f) musicWavePhase -= 6.2831853f * 64.0f;
        if (runtime.currentLyrics && !runtime.currentLyrics->Empty()) {
            // The position is what selects the line, so it advances within a bar.track without the
            // bar.track itself changing -- and it has to be the PROJECTED one, not the raw reading:
            // the session reports once every second or two, so the line changed a couple of
            // seconds after the words were sung ("现在歌词和唱的有点对应不上"). The same numbers
            // the card's time and bar use are taken from the one playhead decided above, which
            // falls back to the bar's own stopwatch for a session that sends no timeline at all
            // -- NetEase Cloud Music does that, and the line sat on the first one for the whole
            // song before this ("歌词不动"). See TrackPositionFor.
            const long long sungMs = bar.track.ms;
            const int index = runtime.currentLyrics->LineAt(sungMs);
            const island::LyricLine* line = runtime.currentLyrics->At(index);
            const char* nextText = (line && line->text[0] != '\0') ? line->text : "";
            if (index != runtime.lastLyricIndex) {
                runtime.lastLyricIndex = index;
                // A ROLL ONLY WHEN THE WORDS CHANGE.
                //
                // The index is re-evaluated for a reason that is not a line change: the media
                // session bumps its revision for a position update (once every second or two)
                // and this loop then re-asks which line is current. Rolling on that put the SAME
                // text on both sides of the window -- "歌词每1、2秒就会上下出现一模一样的歌词
                // 然后和在一起似的" -- and it restarted a long line's marquee from the top.
                if (island::LyricLineChanged(nextText, bar.shownLyricText)) {
                    // The line being replaced is kept -- with the marquee offset it had reached --
                    // because the cross-fade draws it in the same place for the whole change.
                    strncpy_s(bar.replacedLyricText, sizeof(bar.replacedLyricText), bar.shownLyricText,
                              _TRUNCATE);
                    bar.replacedLyricScrollDip = runtime.lyricsScrollDip;
                    runtime.lyricsScrollDip = 0.0f;   // a new line starts at its beginning
                    bar.lyricLineAge = 0.0f;      // ... and fades in rather than appearing
                    lyricLineChanged = true;
                    // The WORDS are on the line too, not just the index: "which line is it" cannot
                    // be checked from a number, and the whole point of this record is that the pill
                    // and the log can be compared. Measured need: an empty LRC line used to blank
                    // the pill and it showed the song title instead ("如果一句唱的慢的话 会显示歌
                    // 曲名") -- with the text in the log that is visible in one grep.
                    wchar_t wideLine[island::kLyricLineCapacity] = {};
                    MultiByteToWideChar(CP_UTF8, 0, nextText, -1, wideLine,
                                        static_cast<int>(island::kLyricLineCapacity));
                    if (bar.track.fromSession) {
                        island::LogAndPrint(
                            L"  lyrics: line %d at %lld ms (the session last reported %lld ms): "
                            L"%ls\n",
                            index, sungMs, runtime.nowPlaying.positionMs, wideLine);
                    } else if (bar.activeFromSession) {
                        // A session that sends no timeline: the number is the bar's own stopwatch,
                        // which is exactly what has to be checked when the words look out of step
                        // with the singing. Measured on NetEase Cloud Music, which reports
                        // `Position 00:00` for the whole song.
                        island::LogAndPrint(
                            L"  lyrics: line %d at %lld ms (counted by the bar -- this session"
                            L" reports no position): %ls\n",
                            index, sungMs, wideLine);
                    } else {
                        // No session, so no reading to compare against: the number is this
                        // source's own count from the moment the window title appeared, which is
                        // exactly what has to be checked when the words look out of step.
                        island::LogAndPrint(
                            L"  lyrics: line %d at %lld ms (counted from the player's window"
                            L" title -- there is no session): %ls\n",
                            index, sungMs, wideLine);
                    }
                }
            }
            lyricLineAnimating = bar.lyricLineAge < island::kLyricLineEnterSeconds;
            // What is on screen, kept in step whether or not the words changed -- the
            // comparison above is against this.
            strncpy_s(bar.shownLyricText, sizeof(bar.shownLyricText), nextText, _TRUNCATE);

            if (line && line->text[0] != '\0') {
                lyricLine = line->text;

                // Advance the marquee only when there is something to scroll: a
                // line that fits is centred and stays put, so the loop can
                // sleep instead of redrawing 60 times a second.
                // The PILL's width, which is what the line is drawn in -- not `CurrentBox`,
                // which is card-sized for the whole of a card transition and would tell the
                // marquee the line fits while the renderer clips it to 220 DIP.
                const island::RoundedBox lineBox =
                    island::BoxForState(island::IslandState::Collapsed, systems.window.CurrentAnchor());
                const float stripWidth = lineBox.rect.right - lineBox.rect.left;
                wchar_t wide[512] = {};
                if (MultiByteToWideChar(CP_UTF8, 0, line->text, -1, wide, 512) > 0) {
                    // The same predicate the renderer uses to place the line.
                    // They used to work it out separately from slightly different
                    // numbers, so the renderer could centre a line while the loop
                    // scrolled it -- the text would jump sideways for no reason.
                    // ... and the same WIDTH measurement: `TrackLineWidth` is the width in the format
                    // that draws the line. With `TextWidth` (the narrow, non-bold face) a line just
                    // over the threshold was judged to fit, centred, and cut at both ends.
                    if (island::LyricNeedsScroll(systems.text.TrackLineWidth(wide),
                                                 stripWidth)) {
                        // A PAUSED SESSION'S LINE STANDS STILL. The words stay on the pill -- they
                        // are what it is for -- but a line that kept creeping across a paused player
                        // would be reading a playhead that is not moving, and asking for frames
                        // forever to do it.
                        if (bar.activePlaying) {
                            lyricsScrolling = true;
                            runtime.lyricsScrollDip +=
                                static_cast<float>(dt) * lyricScrollDipPerSec;
                        }
                    }
                }
            }
        }
        // No lyrics for this bar.track: the title is the honest fallback. The
        // clock is only shown when there is no music at all.
        if (!lyricLine) fallbackLine = bar.activeTitle;
    }
}

static void FillTrayAndPanelFrame(BarRuntime& runtime, island::FrameState& frame, island::BarState& bar,
                                 island::BarSystems& systems, bool animating, float dt,
                                 bool forcedStartHover, bool trayPageShown) {
    frame.trayIcons = systems.trayIcons.Count() > 0 ? &systems.trayIcons.At(0) : nullptr;
    frame.trayIconCount =
        systems.trayIcons.Count() < island::kMaxTrayIcons ? systems.trayIcons.Count()
                                                  : island::kMaxTrayIcons;
    frame.trayRevision = systems.trayIcons.Revision();
    frame.trayHoveredIndex = systems.window.TrayCellHovered();
    // ... AND WHETHER THE CARD'S SURFACE IS MOVING, which decides how much of the material is
    // drawn this frame: the shadow and the grain are skipped while the card animates or while a
    // moving pointer keeps changing which cell is hovered, and come back on the first frame that
    // neither is true (see FrameState::cardSurfaceMoving and FillCardSurface).
    //
    // A cell CHANGE is the test rather than "the pointer is over the card", because the cost is
    // paid per redraw: the pointer moving WITHIN one cell does not redraw anything, and using the
    // hovered cell keeps the full material on screen for as long as the pointer is still.
    //
    // STICKY, NOT PER-FRAME, and that distinction is the whole of a report that came back the
    // same evening: "鼠标在上面移动卡片会闪". Toggling the material on the frame the hovered cell
    // changed and off again on the frame it did not meant a moving pointer produced
    // shadow-frames and no-shadow-frames alternately -- the card visibly flickered between two
    // materials. The state now lasts as long as the pointer is on the card (and for a further
    // 200 ms, so it does not change in the gaps between cells), and the full material returns
    // once, after the interaction is over.
    const bool overTrayCell = frame.trayHoveredIndex >= 0;
    const unsigned long long nowMs = GetTickCount64();
    if (animating || overTrayCell) runtime.lastCardMotionMs = nowMs;
    frame.cardSurfaceMoving = (nowMs - runtime.lastCardMotionMs) < 200;
    frame.trayButtonHovered = systems.window.TrayButtonHovered();
    // ... and the tray's own menu, when one is up: its rows come from the window (it owns the
    // card and answers the hover test), its labels from here (user-facing text is not the
    // renderer's to invent -- the same split as the page's title above).
    frame.trayMenuOpen = systems.window.TrayMenuIsOpen();
    frame.trayMenuHovered = systems.window.TrayMenuRowHovered();
    if (frame.trayMenuOpen) {
        const island::TrayMenuLayout& menu = systems.window.TrayMenu();
        const int rows = menu.count < bar.trayMenuActionCount ? menu.count : bar.trayMenuActionCount;
        frame.trayMenuRowCount = rows;
        for (int i = 0; i < rows; ++i) {
            frame.trayMenuRows[i] = menu.rows[i];
            frame.trayMenuLabels[i] =
                bar.trayQuitArmed ? island::TrayMenuArmedLabel(bar.trayMenuActions[i])
                              : island::TrayMenuLabel(bar.trayMenuActions[i],
                                                      bar.trayMenuOwnMenuUnreachable);
        }
    }
    // ... and the three washes, each a spring toward "the pointer is on it" (see
    // FrameState::startWash for why a wash cannot be a switch). Stepped here, before the
    // frame is drawn, and reported in the frame as AMOUNTS rather than as the booleans.
    {
        const bool want[3] = {systems.window.StartHovered() || forcedStartHover,
                              systems.window.SystemButtonHovered(),
                              systems.window.TrayButtonHovered()};
        bar.controlWashAnimating = false;
        for (int i = 0; i < 3; ++i) {
            if (island::ControlWashStep(&bar.controlWash[i], &bar.controlWashVelocity[i],
                                        want[i] ? 1.0f : 0.0f, dt)) {
                bar.controlWashAnimating = true;
            }
        }
        frame.startWash = bar.controlWash[0];
        frame.systemButtonWash = bar.controlWash[1];
        frame.trayButtonWash = bar.controlWash[2];
    }
    // The level the card is SHOWING, not the page that was asked for: the two differ for the
    // whole of the exit, and drawing the request is what put the tiles under a shrinking tray
    // page. See NextPanelLevel.
    frame.trayPageOpen = trayPageShown;
    frame.trayPageTitle = trayPageShown ? L"\u6258\u76d8" : nullptr;   // 托盘
    systems.window.SetTrayIconCount(frame.trayIconCount);

    frame.systemPanelOpen = bar.systemPanelOpen;
    // ... and WHICH CONTROL SHOULD LOOK OPEN, which is a question about the level and not about
    // whether a card is up: every level sets `bar.systemPanelOpen` (that is what makes the card
    // exist), so asking that flag lit the SYSTEM button's chevron for the tray page, for the
    // Start card and for everything else. Reported as "点击 ⊞ 系统按钮也跟着变".
    {
        const island::ControlLit lit =
            island::ControlLitFor(bar.panelLevel, frame.outerCardUp);
        frame.systemButtonOpen = lit.system;
        frame.startButtonOpen = lit.start;
    }
}

// --- the Start card, told to the frame and to the window ---
//
// Moved out of the frame loop verbatim (indentation aside). One place fills this in, so the drawing and the
// hit tests cannot be given different answers; the window is told the counts and the page rather than the
// lists, because it owns no application list and the hit tests are pure functions of the same geometry the
// drawing uses.
static void FillStartCardFrame(island::FrameState& frame, island::BarState& bar,
                              island::BarSystems& systems, bool startPageShown, float dt) {
    // --- the Start card, told to the frame and to the window --------------------------
    //
    // One place fills this in, so the drawing and the hit tests cannot be given different
    // answers -- the arrangement the tray page already uses. The window is told the counts and
    // the page rather than the lists: it owns no application list, and the hit tests are pure
    // functions of the same geometry the drawing uses.
    frame.startPageOpen = startPageShown;
    {
        // THE HIGHLIGHTS, spring-stepped, one per item -- see FrameState::pinWashes for why
        // they are per item and why they are springs. Sized to the lists, which change when a
        // pin is added or an application arrives.
        {
            // The module's counts are read here rather than cached at startup: the list arrives
            // from a worker, and the pins change whenever the user adds one. Read BEFORE the
            // highlights are sized, so a pin that was just added has a spring of its own in the
            // frame it appears in -- sizing them from the previous frame's counts left the
            // newest tile the one tile with no highlight.
            bar.startPinCount = systems.startApps.PinCount();
            bar.startAppCount = systems.startApps.ListCount();
            const island::IslandWindow::StartHit hover = systems.window.StartHitHovered();
            const int pinHover = hover.target == island::IslandWindow::StartHitTarget::Pin
                                     ? hover.index
                                     : -1;
            const int rowHover =
                hover.target == island::IslandWindow::StartHitTarget::PickerRow ? hover.index
                                                                                : -1;
            const int powerHover =
                hover.target == island::IslandWindow::StartHitTarget::PowerRow ? hover.index
                                                                               : -1;
            const auto resize = [](std::vector<island::WashSlot>& slots,
                                   std::vector<float>& values, int count) {
                if (static_cast<int>(slots.size()) != count) {
                    slots.assign(static_cast<size_t>(count), island::WashSlot{});
                    values.assign(static_cast<size_t>(count), 0.0f);
                }
            };
            resize(bar.startPinWashes, bar.startPinWashValues, bar.startPinCount);
            resize(bar.startRowWashes, bar.startRowWashValues, bar.startAppCount);
            resize(bar.startPowerWashes, bar.startPowerWashValues, 5);
            bar.startWashAnimating = false;
            const auto step = [&](std::vector<island::WashSlot>& slots,
                                  std::vector<float>& values, int hovered) {
                for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
                    if (island::ControlWashStep(&slots[static_cast<size_t>(i)].value,
                                                &slots[static_cast<size_t>(i)].velocity,
                                                i == hovered ? 1.0f : 0.0f, dt)) {
                        bar.startWashAnimating = true;
                    }
                    values[static_cast<size_t>(i)] = slots[static_cast<size_t>(i)].value;
                }
            };
            step(bar.startPinWashes, bar.startPinWashValues, pinHover);
            step(bar.startRowWashes, bar.startRowWashValues, rowHover);
            step(bar.startPowerWashes, bar.startPowerWashValues, powerHover);
        }
        const island::IslandWindow::StartHit startHit = systems.window.StartHitHovered();
        island::TextRenderer::StartCardText& card = frame.startCard;
        card.pins = bar.startPinCount > 0 ? &systems.startApps.PinAt(0) : nullptr;
        card.pinCount = bar.startPinCount;
        card.list = bar.startAppCount > 0 ? &systems.startApps.ListAt(0) : nullptr;
        card.listCount = bar.startAppCount;
        card.pickerScrollRow = bar.startPickerScroll;
        card.page = bar.startPageState;
        card.hoveredPin =
            startHit.target == island::IslandWindow::StartHitTarget::Pin ? startHit.index : -1;
        card.hoveredPickerRow =
            startHit.target == island::IslandWindow::StartHitTarget::PickerRow
                ? startHit.index
                : -1;
        card.hoveredPowerRow =
            startHit.target == island::IslandWindow::StartHitTarget::PowerRow
                ? startHit.index
                : -1;
        card.powerButtonHovered =
            startHit.target == island::IslandWindow::StartHitTarget::PowerButton;
        card.allAppsHovered = startHit.target == island::IslandWindow::StartHitTarget::AllApps;
        card.allAppsMode = bar.startAllApps;
        card.powerConfirmRow = bar.startPowerConfirm;
        card.pinWashes = bar.startPinWashValues.empty() ? nullptr : bar.startPinWashValues.data();
        card.rowWashes = bar.startRowWashValues.empty() ? nullptr : bar.startRowWashValues.data();
        card.powerWashes =
            bar.startPowerWashValues.empty() ? nullptr : bar.startPowerWashValues.data();
        card.accountName = bar.accountName;
        card.avatarPng = bar.accountAvatarPng.empty() ? nullptr : bar.accountAvatarPng.data();
        card.avatarPngSize = bar.accountAvatarPng.size();

        // One array of icon pointers, rebuilt here because the frame carries pointers and must
        // not carry a vector of them: the renderer walks `pinIcons[i]` against `pins[i]`, and
        // the two have to be the same length in the same order.
        bar.startIconPointers.clear();
        bar.startIconSizes.clear();
        for (int i = 0; i < bar.startPinCount; ++i) {
            size_t size = 0;
            const uint8_t* png = systems.startApps.PinIconPng(i, &size);
            bar.startIconPointers.push_back(png);
            bar.startIconSizes.push_back(size);
        }
        card.pinIcons = bar.startIconPointers.empty() ? nullptr : bar.startIconPointers.data();
        card.pinIconSizes = bar.startIconSizes.empty() ? nullptr : bar.startIconSizes.data();

        // ... and the same for the all-applications grid, which draws every entry of the list as a tile.
        // Rebuilt per frame for the same reason: the frame carries pointers, and the storage is here.
        bar.startListIconPointers.clear();
        bar.startListIconSizes.clear();
        for (int i = 0; i < bar.startAppCount; ++i) {
            size_t size = 0;
            const uint8_t* png = systems.startApps.ListIconPng(i, &size);
            bar.startListIconPointers.push_back(png);
            bar.startListIconSizes.push_back(size);
        }
        card.listIcons =
            bar.startListIconPointers.empty() ? nullptr : bar.startListIconPointers.data();
        card.listIconSizes =
            bar.startListIconSizes.empty() ? nullptr : bar.startListIconSizes.data();
    }
    // ... and the window, which answers "what is the pointer on" with the same geometry.
    systems.window.SetStartState(startPageShown, bar.startPageState, bar.startPinCount, bar.startAppCount,
                         bar.startPickerScroll, bar.startAllApps);
}

// --- the panel content, level by level ---
//
// Moved out of the frame loop verbatim (indentation aside). The panel CONTENT is built whenever a card is up
// OR ON ITS WAY, not only while the panel flag is set: building it only while the flag was set made the tiles
// and the slider vanish the moment the panel was asked to close, and the exit then played on an empty card --
// reported as the panel "appearing and disappearing" with the pointer. The SHOWN level builds the content,
// not the request, for the same reason.
static void FillPanelFrame(BarRuntime& runtime, island::FrameState& frame, island::BarState& bar,
                           island::BarSystems& systems, bool soundPageShown, bool trayPageShown,
                           bool startPageShown, float dt) {
    // `cardUp` is computed here rather than handed in: it is one expression off the frame, and the block this
    // phase was moved out of had it in a local right above itself.
    const bool cardUp = frame.outerCardUp;
    if (cardUp) {
        // The SHOWN level builds the content, not the request: the 声音输出 page has to keep its
        // rows while it leaves the screen, exactly as the tray page keeps its grid, or the exit
        // plays on an empty card (which is the other half of the defect above -- reported once
        // already, as the panel "appearing and disappearing").
        if (soundPageShown) {
            // --- the panel's second level: the 声音输出 page ---
            //
            // The worker is already running -- it starts with the bar, because the WeChat sound
            // channel reads the same session list. This used to start it here, on the first open of
            // the page, which quietly meant "nothing that needs the session list works until
            // somebody opens the mixer".
            //
            // A snapshot per frame while the page is up. Cheap enough -- it is a copy of a list
            // that changes about once a second -- and the alternative, holding the live object,
            // would put the worker's own state on screen mid-update.
            systems.soundOutput->Snapshot(systems.soundSnapshot.get());
            float masterLevel = 0.0f;
            const bool haveMaster = systems.volume.Get(&masterLevel);
            // The device's own row is drawn at the chased value as well, and at the raw reading
            // while that row is the one being dragged -- the same rule as the other two bars.
            const float drawnMaster =
                (bar.mixerSliderDragRow == 0 || bar.shownVolume < 0.0f) ? masterLevel : bar.shownVolume;
            BuildSoundPage(*systems.soundSnapshot, systems.systemStatus, haveMaster, drawnMaster,
                           systems.volume.IsMuted(), bar.soundPageScroll, &frame.soundPage);
            frame.soundPageOpen = true;
            runtime.lastSoundRevision = systems.soundOutput->Revision();
            systems.window.SetSoundPage(frame.soundPage, bar.soundPageScroll);
        } else {
            // Rebuilding every frame is what the tiles need: their on/off state comes from the
            // status readings, and the readings are sampled on a timer. The text is copied into
            // the frame's own buffers, so nothing here points at a shorter-lived string.
            float liveLevel = 0.0f;
            const bool liveKnown = systems.volume.Get(&liveLevel);
            BuildSystemPanel(systems.systemStatus, systems.statusReader.InstalledLayoutCount(), liveKnown,
                             liveLevel, systems.volume.IsMuted(), &frame.systemPanel,
                             bar.systemPanelPage);

            // The volume's own percentage, shown while the slider is the control in hand: being
            // dragged, used within the last second, or simply hovered. The handle's position is
            // the only reading the slider carries by itself, and "about three quarters along" is
            // not an answer to "how loud is it".
            {
                POINT echoPoint = {};
                GetCursorPos(&echoPoint);
                // WHICH ROW the pointer is on, for the handle's own size: the hit carries the
                // row index (0 the volume, 1 the brightness), and a drag pins it to the row the
                // gesture started on -- a handle that shrank because the pointer wandered off
                // the bar.track mid-drag would be the slider losing the control being held.
                const island::SystemPanelHit echoHit =
                    systems.window.SystemPanelHitAtScreen(echoPoint);
                const bool onSlider =
                    echoHit.target == island::SystemPanelTarget::Slider;
                const bool onVolumeRow =
                    echoHit.target == island::SystemPanelTarget::Slider &&
                    echoHit.index == 0;
                const bool onBrightnessRow =
                    echoHit.target == island::SystemPanelTarget::Slider &&
                    echoHit.index == 1;
                const float wanted[2] = {
                    (onVolumeRow || (bar.panelSliderDrag && bar.panelSliderDragRow == 0)) ? 1.0f
                                                                                 : 0.0f,
                    (onBrightnessRow || (bar.panelSliderDrag && bar.panelSliderDragRow == 1))
                        ? 1.0f
                        : 0.0f,
                };
                for (int row = 0; row < 2; ++row) {
                    // The same chase the volume uses, with its own (quicker) constant: the size
                    // answers the pointer, so it should arrive about as fast as a hover.
                    bar.sliderHandleGrowth[row] +=
                        (wanted[row] - bar.sliderHandleGrowth[row]) *
                        (1.0f - std::exp(-dt / kHandleGrowthSeconds));
                    if (std::fabs(wanted[row] - bar.sliderHandleGrowth[row]) < 0.004f) {
                        bar.sliderHandleGrowth[row] = wanted[row];
                    }
                    frame.systemPanel.sliderEmphasis[row] = bar.sliderHandleGrowth[row];
                }
                frame.systemPanel.showVolumePercent =
                    bar.panelSliderDrag || onSlider ||
                    GetTickCount64() < bar.panelVolumeEchoUntil;
                // The panel's handle is drawn at the chased value too, and at the pointer's own
                // value while it is being dragged -- the same rule as the card's bar.
                frame.systemPanel.volume =
                    bar.panelSliderDrag
                        ? (liveKnown ? liveLevel : frame.systemPanel.volume)
                        : (bar.shownVolume >= 0.0f ? bar.shownVolume : frame.systemPanel.volume);
            }

            // The brightness row, and its own percentage on the same terms.
            //
            // WHILE IT IS BEING DRAGGED the handle is drawn at the POINTER's position, not at the
            // last value the monitor confirmed: a DDC/CI write is a round trip over the video
            // cable, and drawing the confirmed value would make the handle lag behind the pointer
            // by a visible fraction of a second -- the complaint "不顺滑" that the volume row
            // already answered the same way.
            frame.systemPanel.hasBrightness = bar.brightnessAvailable;
            if (bar.brightnessAvailable) {
                float dragged = 0.0f;
                POINT echoPoint = {};
                GetCursorPos(&echoPoint);
                const island::SystemPanelHit echoHit =
                    systems.window.SystemPanelHitAtScreen(echoPoint);
                const bool onBrightnessRow =
                    echoHit.target == island::SystemPanelTarget::Slider &&
                    echoHit.index == 1;
                const bool draggingBrightness =
                    bar.panelSliderDrag && bar.panelSliderDragRow == 1;

                if (draggingBrightness || onBrightnessRow) {
                    dragged = echoHit.fraction;
                }
                frame.systemPanel.brightness =
                    draggingBrightness ? dragged
                                       : static_cast<float>(bar.brightnessPercent) / 100.0f;
                frame.systemPanel.showBrightnessPercent =
                    draggingBrightness || onBrightnessRow ||
                    GetTickCount64() < bar.panelBrightnessEchoUntil;
            }
            // The page's hit test is closed the moment the page is not showing: leaving it open
            // would let a click on the panel's tiles be read as a device row.
            systems.window.CloseSoundPage();
        }

        // The card is CENTRED on the button that opened it -- the system button for the
        // panel, the tray button for the tray page (see DeckCardAnchorCentre, and the latch
        // just above this block).
        //
        // Chosen by the user after being shown the numbers: centring the card on the button
        // puts its right half under the ISLAND, and the alternative -- keeping the card
        // wholly to the island's left -- leaves its centre about 100 DIP left of the
        // button's, which does not read as "under the button".
        //
        // The overlap is not a collision. The card hangs BELOW the bar (top at 50 while the
        // island ends at 42), so the card's right half simply continues down the screen
        // underneath the island; nothing of the bar's is covered, and the application icon
        // row -- which is to the RIGHT of the island -- is never reached at all, so it stays
        // exactly where it is.
        //
        // Left-aligning to the button instead -- tried first -- sent the card 520 DIP to the
        // RIGHT, over the icon row, which then had to be pushed aside. The user reported
        // exactly that: "the icons beside the island were all squeezed out".
        //
        // The anchor comes from the RESTING island, so the position does not move while the
        // card's own width is animating.
        const island::RoundedBox anchorIsland = systems.window.LayoutIslandFor();
        // The centre of the control that opened the card: the system button for the panel, the
        // tray button for the tray page, the START button for the Start card. See
        // DeckCardAnchorCentre -- and its note about how the Start card came to be drawn under
        // the system button, 200 DIP from the control that opened it, until the card was
        // photographed.
        const island::CardHang hang = trayPageShown  ? island::CardHang::TrayButton
                                      : startPageShown ? island::CardHang::StartButton
                                                       : island::CardHang::SystemButton;
        const float buttonCentre = island::DeckCardAnchorCentre(anchorIsland, hang);
        // WHICH THING THE CARD HANGS FROM DEPENDS ON WHICH CARD IT IS.
        //
        // The system panel hangs from the control that opened it: the user chose that
        // ("面板居中于按钮", 7.76) and it is what a flyout does -- press a button, get a panel
        // under that button.
        //
        // The other two belong to the ISLAND. The media card is the pill's own detail view, and a
        // message is about the bar as a whole; both were originally centred under the pill (the
        // screenshots in docs/ from that phase show it), and 7.76 moved all three together
        // because the question was asked about the panel. Reported as "this should be under the
        // island, not under this [the button]".
        const float islandCentre = (anchorIsland.rect.left + anchorIsland.rect.right) * 0.5f;
        const float anchorCentre = (runtime.cardOwner == island::CardOwner::SystemPanel)
                                       ? buttonCentre
                                       : islandCentre;
        const float anchorLeft = anchorCentre - island::kExpandedWidthDip * 0.5f;
        const float windowWidthDipNow =
            static_cast<float>(systems.window.Monitor().screen.right -
                               systems.window.Monitor().screen.left) /
            systems.window.Scale();
        frame.cardLeftDip = island::CardLeftFor(anchorLeft, island::kExpandedWidthDip,
                                                windowWidthDipNow);
    }
}

// --- pace ---
//
// Moved out of the frame loop verbatim (indentation aside), and it is the last thing a pass does: while
// animating, wait for the next compositor tick (or a short message wait as a fallback); when idle, block on
// input instead of spinning, because the clock only needs a wake-up once a second. It needs the window
// services, whether the compositor clock answered, and the wait counter BarRuntime carries.
static void PaceTheLoop(island::BarSystems& systems, bool clockDriven, BarRuntime& runtime) {
    // Kept in the signature because it still records what the compositor clock answered, but the pacing
    // is now the vsync present: see below.
    (void)clockDriven;
    // --- pace ---
    //
    // While animating, wait for the next compositor tick (or a short
    // message wait as a fallback). When idle, block on input instead of
    // spinning: the clock only needs a wake-up once a second, so a 100 ms
    // cap is plenty and costs nothing on an idle desktop.
    if (systems.window.IsAnimating()) {
        // THE PRESENT IS THE PACING NOW. It waits for vsync (Renderer::RenderFrame), so blocking here
        // again on the swap chain's frame-latency waitable waits TWICE for one frame -- which is the
        // "感觉有点不跟鼠标" report, and it also spaces the animation frames unevenly. Only drain input
        // without blocking; the present blocks for the next refresh.
        ::MsgWaitForMultipleObjectsEx(0, nullptr, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        runtime.frameWaits += 1;
    } else {
        ::MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT,
                                      MWMO_INPUTAVAILABLE);
    }
}

// --- the CALL strip, and the date / time / weather strip ---
//
// Moved out of the frame loop verbatim (indentation aside, plus one line that a past edit had merged
// with the brace above it and is split again here). The CALL strip is what WeChat call looks like in the
// reference: an avatar, a green running timer counted from when the WATCHER saw the call start -- not from
// this frame -- and a level meter from the output peak. The strip below it decides WHERE the pill own line
// is drawn, asked once in StatusPlacementFor, because the one thing that must not happen is the same words
// in both places at once.
static void FillCallAndStatusStrip(island::FrameState& frame, island::BarState& bar,
                                   island::BarSystems& systems, const island::Notification* active,
                                   float lyricsScrollDip, float dt) {
    // --- the CALL strip ------------------------------------------------------
    //
    // When the notification on show is WeChat's CALL one, the strip says it the way the reference
    // does: an avatar, a green running timer, and a level meter from the output's peak. The timer
    // counts from when the WATCHER saw the call begin, not from when this frame was drawn, so
    // restarting the bar in the middle of a call does not restart the count.
    if (active && systems.wechat.CallNotificationId() != 0 && active->id == systems.wechat.CallNotificationId()) {
        frame.callUp = true;
        // The avatar, looked up once per call: the icon of the process that is in it.
        {
            wchar_t source[260] = {};
            if (ProcessImagePath(systems.wechat.CallPid(), source, 260) &&
                std::wcscmp(source, callIconSource) != 0) {
                wcscpy_s(callIconSource, 260, source);
                callIconPixels.clear();
                callIconWidth = 0;
                callIconHeight = 0;
                if (island::LoadNotificationIconPixels(source, &callIconPixels, &callIconWidth,
                                                       &callIconHeight)) {
                    ++callIconRevision;
                    island::LogAndPrint(
                        L"  wechat: call icon from '%ls' -> %dx%d\n", source, callIconWidth,
                        callIconHeight);
                    std::fflush(stdout);
                }
            }
                    }
                    const unsigned long long startedMs = systems.wechat.CallStartMs();
        frame.callStrip.seconds =
            startedMs != 0 ? static_cast<int>((GetTickCount64() - startedMs) / 1000) : 0;
        float peak = 0.0f;
        frame.callStrip.levelKnown = systems.volume.Peak(&peak);
        frame.callStrip.level = peak;
        frame.callStrip.iconPixels =
            callIconPixels.empty() ? nullptr : callIconPixels.data();
        frame.callStrip.iconWidth = callIconWidth;
        frame.callStrip.iconHeight = callIconHeight;
        frame.callStrip.iconRevision = callIconRevision;
    }
    frame.lyricScrollDip = lyricsScrollDip;

    // --- the date, the weekday, the time and the weather --------------------------
    //
    // IS THE PILL'S OWN LINE TAKEN? That is the whole question, and it is exactly "the
    // island has something else to say": a call, a message, or a lyric. The answer decides
    // WHICH OF THE TWO HOMES the line is drawn in -- inside the pill, or beside the deck --
    // and it is asked ONCE, in StatusPlacementFor, because the one thing that must not
    // happen is the same words in both places at once.
    //
    // The three values are the ones the renderer chooses the pill's own content from, so
    // the two draw sites cannot disagree: a pill showing the lyric always has the line
    // beside it, and a pill showing the date and the time never has.
    const bool islandLineTaken =
        (frame.restingState == island::IslandState::MusicStrip &&
         (frame.lyricLine || frame.fallbackLine)) ||
        (frame.state == island::IslandState::NoticeStrip &&
         (frame.callUp || (frame.noticeLine && frame.noticeLine[0] != '\0')));
    const island::StatusPlacement placement =
        island::StatusPlacementFor(islandLineTaken);

    bar.statusStripOpacity =
        island::StatusStripFadeStep(bar.statusStripOpacity, placement.besideDeck, dt);
    frame.statusStripOpacity = bar.statusStripOpacity;

    // The reading is COPIED into this scope's buffers rather than pointed at where the
    // worker keeps it: the worker can publish between this line and the draw, and a frame
    // that read half of one reading and half of the next would print a temperature that
    // never happened.
    //
    // Two pieces, not one string: the line sets the word and the temperature apart (see
    // FrameState::statusSky).
    systems.weather.Pieces(bar.statusSkyText, 32, bar.statusTemperatureText, 32);
    frame.statusSky = bar.statusSkyText[0] != L'\0' ? bar.statusSkyText : nullptr;
    frame.statusTemperature =
        bar.statusTemperatureText[0] != L'\0' ? bar.statusTemperatureText : nullptr;
    // ... and the CODE the two pieces came from, because the mark before the word is drawn
    // from it: the line used to draw one cloud-with-rain whatever the sky was.
    {
        double statusTemperatureC = 0.0;
        int statusWeatherCode = -1;
        int statusSunrise = frame.sunriseMinutes;
        int statusSunset = frame.sunsetMinutes;
        frame.statusCode = systems.weather.Get(&statusTemperatureC, &statusWeatherCode,
                                               &statusSunrise, &statusSunset)
                               ? statusWeatherCode
                               : -1;
        frame.sunriseMinutes = statusSunrise;
        frame.sunsetMinutes = statusSunset;
    }

    // Read by the next pass's redraw decision, like the icon row's own arrival flags.
    bar.statusStripAnimating = bar.statusStripOpacity > 0.001f && bar.statusStripOpacity < 0.999f;
}

// --- the hover preview: a live thumbnail under the icon the pointer rests on ---
//
// Moved out of the frame loop verbatim (indentation aside; the last line of the block is the notice line,
// which belongs to the same stretch of frame filling). AFTER A DWELL, not on arrival (see bar.previewTarget),
// and not while a card is up: the media card and the system panel hang BELOW the bar, which is exactly where a
// preview goes, so one would be drawn under the other. The box comes from the pure ThumbnailBoxFor and the SAME
// icon rectangle the row was laid out with, so the preview cannot point at a different icon from the open one.
static void FillHoverPreview(BarRuntime& runtime, island::FrameState& frame, island::BarState& bar,
                             island::BarSystems& systems, const island::MusicInfo& nowPlaying,
                             bool previewReady, int drawn, int listed, float dt,
                             float musicWavePhase, island::AccentColour waveAccent,
                             const char* lyricLine, const char* fallbackLine,
                             const std::string& noticeLine, unsigned long long& frameWork,
                             unsigned long long thumbnailBit, unsigned long long dwellMs,
                             bool traceCardFrames) {
    // `cardUp` is computed here rather than handed in, the same way FillPanelFrame does it: it is one
    // expression off the frame.
    const bool cardUp = frame.outerCardUp;
    // --- the hover preview: a live thumbnail under the icon the pointer rests on -------
    //
    // AFTER A DWELL, not on arrival (see bar.previewTarget), and not while a card is up: the
    // media card and the system panel hang BELOW the bar, which is exactly where a preview
    // goes, so one would be drawn under the other.
    //
    // The box is computed by the pure ThumbnailBoxFor, from the SAME icon rectangle the row
    // was laid out with -- so the preview cannot point at a different icon from the one
    // that is open.
    if (previewReady) {
        const unsigned long long previewNow = GetTickCount64();
        const int hoveredIcon = frame.taskHoveredIndex;
        const HWND wanted = (hoveredIcon >= 0 && hoveredIcon < drawn)
                                ? bar.taskHandles[hoveredIcon]
                                : nullptr;
        if (wanted != bar.previewTarget) {
            bar.previewTarget = wanted;
            bar.previewSinceMs = previewNow;
        }

        const bool rested = wanted && (previewNow - bar.previewSinceMs) >= dwellMs;
        // ONCE IT IS UP, IT FOLLOWS THE POINTER ALONG THE ROW AT ONCE.
        //
        // The dwell exists to stop a preview flashing up for every icon the pointer passes on
        // its way somewhere. Once one is already on screen the user has decided where to look,
        // and making them wait again for the neighbour is the flicker rather than the cure:
        // the preview would vanish and come back 350 ms later. It still goes away the moment
        // the pointer leaves the row.
        const bool follow = wanted != nullptr && systems.preview.Visible();
        bool shown = false;
        // The preview path registers a DWM thumbnail on the window and can redraw the live
        // content every frame, so it is one of the things a slow frame may turn out to be.
        frameWork |= thumbnailBit;
        // ALREADY PLACED: the compositor keeps drawing the live picture itself, so a frame in which
        // neither the target nor the box changed has nothing to tell it. Doing the SetStyle/Begin/Place
        // dance anyway was the 120-180 ms per frame that made every animation stutter.
        const bool alreadyPlaced =
            wanted != nullptr && wanted == bar.previewPlacedTarget && bar.previewShown;
        if ((rested || follow) && !frame.outerCardUp && IsWindow(wanted)) {
            if (alreadyPlaced) {
                shown = true;
            } else {            const RECT monitorPx = systems.window.Monitor().screen;
            const float scale = systems.window.Scale();
            // THE ROW THE PREVIEW HANGS OFF IS THE ROW THAT IS DRAWN, tray button and all.
            //
            // The application icons are painted from a box the tray button has moved along
            // (see RowAnchorWithTray), and the preview is placed under "the icon the
            // pointer is on". Measuring the button from the un-shifted box put the card
            // one button to the LEFT of the icon it belongs to -- the same off-by-one
            // button that made the FIRST icon open the SECOND, seen from the drawing side.
            const island::RoundedBox islandBox = island::RowAnchorWithTray(
                systems.window.LayoutIslandFor(), frame.trayIconCount > 0);
            const island::RectF button = island::TaskIconButtonAtAmounts(
                islandBox, hoveredIcon, drawn, bar.taskHoverAmount);
            RECT iconPx = {};
            iconPx.left = monitorPx.left +
                          static_cast<LONG>(std::lround(button.left * scale));
            iconPx.right = monitorPx.left +
                           static_cast<LONG>(std::lround(button.right * scale));
            iconPx.top = monitorPx.top + static_cast<LONG>(std::lround(button.top * scale));
            iconPx.bottom =
                monitorPx.top + static_cast<LONG>(std::lround(button.bottom * scale));

            // THE COMPOSITOR'S OWN SIZE FIRST, then the geometry: it stretches its picture to
            // fill whatever rectangle it is given, so the card has to be built around the shape
            // IT reports. GetWindowRect is not that shape -- using it drew one window squashed
            // into a flat strip (see ThumbnailPreview.h).
            // The card takes the active theme's colours each time it comes up; the acrylic is applied
            // once (SetStyle only re-applies it when it was off).
            systems.preview.SetStyle(PreviewStyleFor(island::CurrentTheme(), systems.renderer.GlassColour()));
            const SIZE sourceSize = systems.preview.Begin(wanted);
            const island::ThumbnailPlacement placement = island::ThumbnailBoxFor(
                sourceSize.cx, sourceSize.cy, iconPx, monitorPx, scale);
            shown = placement.valid && systems.preview.Place(placement);

            if (!bar.previewReported) {
                bar.previewReported = true;
                island::LogAndPrint(
                    "  preview: hwnd %p -> card %ld,%ld %ldx%ld, picture %ldx%ld "
                    "(compositor says %ldx%ld), shown=%d, hr=0x%08llX",
                    reinterpret_cast<void*>(wanted), placement.card.left,
                    placement.card.top, placement.card.right - placement.card.left,
                    placement.card.bottom - placement.card.top,
                    placement.picture.right - placement.picture.left,
                    placement.picture.bottom - placement.picture.top, sourceSize.cx,
                    sourceSize.cy, shown ? 1 : 0, systems.preview.LastResult());
                std::fflush(stdout);
            }
            }
        }
        if (shown) {
            bar.previewPlacedTarget = wanted;
            bar.previewShown = true;
        } else if (bar.previewShown) {
            systems.preview.Hide();
            bar.previewPlacedTarget = nullptr;
            bar.previewShown = false;
        }

        // A refusal is reported once per count rather than once per frame: a window the
        // compositor will not preview is a fact about that window, and repeating it 60 times
        // a second would drown the log.
        if (systems.preview.Refusals() != bar.previewLastRefusals) {
            bar.previewLastRefusals = systems.preview.Refusals();
            island::LogAndPrint("  preview: the compositor refused a thumbnail of hwnd %p "
                                "(0x%08llX)",
                                reinterpret_cast<void*>(wanted), systems.preview.LastResult());
            std::fflush(stdout);
        }
    }

    // Each icon gets a background in the island's own colour, so the row reads as part
    // of the island rather than as icons floating on the wallpaper -- a bright
    // wallpaper made both the icons and their titles unreadable.
    //
    // Only the COLOUR is set here; the rectangle is derived inside the draw from the
    // same layout call that positions the button. Computing it here as well would
    // mean the layout was evaluated twice, and the two would part company the moment
    // one of them used a different hovered index.
    for (int i = 0; i < drawn; ++i) {
        frame.taskButtons[i].fill = frame.pill.fill;
        // Handed over from the animation, which runs before this draw. An icon on its way OUT
        // is never widened: the amount belongs to the slot it used to occupy.
        frame.taskButtons[i].hoverAmount = (i < listed) ? bar.taskHoverAmount[i] : 0.0f;
    }
    // What the loop worked out, before the draw: the pointer's icon, and the two
    // animated amounts for the first button.
    //
    // Both halves of the row are now reported -- this says what the loop DECIDED, and
    // the renderer's own `row:` line says what the layout PRODUCED. A row that does not
    // move is either a decision that never changed or a layout that ignored it, and only
    // having both makes the difference visible.
    if (systems.window.HoveredIcon() != bar.lastReportedHoveredIcon ||
        bar.taskHoverAmount[0] != bar.lastReportedHoverAmount) {
        bar.lastReportedHoveredIcon = systems.window.HoveredIcon();
        bar.lastReportedHoverAmount = bar.taskHoverAmount[0];
        island::LogAndPrint("  loop: hoveredIcon=%d, button0 hover=%.2f, %d icons",
                            systems.window.HoveredIcon(), bar.taskHoverAmount[0],
                            systems.window.IconCount());
        std::fflush(stdout);
    }

    frame.clock = (frame.state == island::IslandState::Expanded)
                      ? island::ClockStyle::Card
                      : island::ClockStyle::Compact;

    // What the taskbar actually got, when it changed.
    //
    // "No buttons appear" cannot be diagnosed from the development machine: the sandbox
    // has its own cursor, so a real hover can never be performed here, and the preview
    // forces the state directly -- which proves the drawing works and says nothing about
    // whether the state is ever reached. This line separates the two: it reports the
    // shape the loop is drawing, how many windows were listed, and how many buttons the
    // frame was given. If the shape is not Hover, the problem is the pointer; if it is
    // Hover with zero windows, the problem is the enumeration; if the count is right and
    // nothing is on screen, the problem is the drawing.
    if (frame.taskButtonCount != bar.lastReportedButtons ||
        frame.state != bar.lastReportedState) {
        bar.lastReportedButtons = frame.taskButtonCount;
        bar.lastReportedState = frame.state;
        island::LogAndPrint("  taskbar: state=%ls, %d buttons from %d windows listed "
                            "(%ls)",
                            island::StateName(frame.state), frame.taskButtonCount,
                            systems.taskWindows.Count(), systems.taskWindows.LastSummary());
        std::fflush(stdout);
    }
    // The system panel is a request for the CARD, so the frame has to be told about it in
    // EVERY state -- including the card itself.
    //
    // This lived inside the `shortShape` block, which by definition EXCLUDES Expanded. The
    // result was a flag that was true at the top of the loop and never reached the frame:
    // the policy promoted the bar to the card, the renderer then checked a flag that had
    // silently stayed false, and the panel was never composed. Nothing errored, and the only
    // symptom was an empty card-shaped outline -- which is what made it expensive to find.
    // The window region LAST, once the state and the flag for this frame are both settled.
    //
    // `UpdatePointerState` applies it too, but that runs near the TOP of the loop and knows
    // only what the previous frame concluded. The region follows the animated shape, so a
    // region applied one frame late keeps the CARD clipped to the pill's height for as long
    // as the panel is open -- which is what "the panel is open but I only see its top edge"
    // was. Harmless to call again: it returns immediately when the rectangle is unchanged.
    // The strip's own telling, before the region is applied: the band's width while the line
    // is beside the deck, zero while it is inside the pill. The window needs it because the
    // region DECIDES which pixels belong to this window, so a strip outside it is clipped --
    // see SetStatusStripWidth. Keyed on the FADE rather than on the placement flag, because
    // the strip is still on screen for the length of its exit.
    systems.window.SetStatusStripWidth(bar.statusStripOpacity > 0.001f
                                   ? island::kStatusStripWidthDip
                                   : 0.0f);

    systems.window.ApplyInputRegion();

    // HOW STRONG, and for WHOM. The card's own content follows the reveal; the BAR's does
    // not fade at all.
    //
    // FADED with the animation, not switched at the end of it. This was
    // `IsAnimating() ? 0.35f : 1.0f`, which is a step: the panel's own fill is 0.97 * this
    // number, so for the whole 0.36 s of the slide the card was drawn at 34% alpha -- the
    // desktop showing straight through it -- and then went to 97% in a single frame. A
    // one-frame change of that size at the end of a movement is what reads as a stutter, and
    // it was the one part of this animation that no amount of easing or duration could fix.
    //
    // The eased progress and not the linear one, because the reveal is masked by the shape
    // drawn from that same eased value: driven by the linear clock the fade would run ahead
    // of the edge and then wait for it.
    //
    // AND ONLY THE CARD'S. One number used to serve both, so entering the island with music
    // playing (which opens the music card) dimmed the deck, the icon row and the lyric line
    // to 35% and ramped them back -- the bar itself blinking for a card that grows BELOW it
    // and never touches it. Reported as "点击系统按钮 或者 播放音乐的时候 鼠标移入灵动岛时候
    // 图片中红框里的会闪一下". See ContentStrengthsFor.
    const island::ContentStrengths strengths =
        island::ContentStrengthsFor(cardUp, systems.window.EasedProgress(),
                                    systems.window.State() == island::IslandState::Expanded);
    frame.contentOpacity = strengths.bar;
    frame.cardOpacity = strengths.card;

    // One line per frame of a card transition, when asked for.
    //
    // Everything that decides how this movement LOOKS, on one line: how long the frame took,
    // how far the curve has pushed the shape, where the card is, how solid its content is,
    // and how much of the window the region admits. "It looks janky" becomes checkable --
    // uneven intervals, a rectangle that jumps, an opacity that steps.
    if (traceCardFrames && frame.outerCardUp) {
        const island::RoundedBox animated = frame.pill.box;
        const float animatedHeight = animated.rect.bottom - animated.rect.top;
        const float cardTopDip = frame.restingPill.rect.bottom + island::kCardTopGapDip;
        const RECT applied = systems.window.AppliedRegion();
        island::LogAndPrint("  card dt=%5.1fms eased=%.3f h=%5.1f left=%.0f "
                            "rect=[%.0f,%.0f %.0f,%.0f] cardOpacity=%.2f barOpacity=%.2f "
                            "region=[%ld,%ld %ld,%ld] panel=%d state=%ls",
                            dt * 1000.0f, systems.window.EasedProgress(), animatedHeight,
                            frame.cardLeftDip, frame.cardLeftDip, cardTopDip,
                            frame.cardLeftDip + island::kExpandedWidthDip,
                            cardTopDip + animatedHeight, frame.cardOpacity,
                            frame.contentOpacity,
                            applied.left, applied.top, applied.right, applied.bottom,
                            static_cast<int>(frame.cardOwner),
                            island::StateName(frame.state));
        std::fflush(stdout);
    }
    // What the card shows, decided by its OWNER rather than by the state.
    //
    // `state == Expanded` was false for the whole of a retraction, so the content vanished on
    // the first frame of its own exit and the exit was invisible. The owner is held until the
    // shape is back, so the content is there to be seen leaving.
    frame.notification =
        (frame.cardOwner == island::CardOwner::Notification && runtime.haveNotificationShown)
            ? &runtime.notificationShown
            : nullptr;
    // ... and the sender's icon, which is what makes the card say WHO as well as what.
    //
    // Resolved once per change rather than per frame: getting an executable's icon is a trip to
    // the shell and the disk, and a notification is on screen for six seconds -- about 360
    // frames of the same image.
    if (frame.notification) {
        wchar_t source[260] = {};
        if (NotificationIconSource(*frame.notification, source, 260)) {
            if (std::wcscmp(source, notificationIconSource) != 0) {
                wcscpy_s(notificationIconSource, 260, source);
                notificationIconPixels.clear();
                notificationIconWidth = 0;
                notificationIconHeight = 0;
                if (island::LoadNotificationIconPixels(source, &notificationIconPixels,
                                                 &notificationIconWidth,
                                                 &notificationIconHeight)) {
                    // nothing more to do: the pixels are the frame's
                }
                ++notificationIconRevision;
                island::LogAndPrint(
                    L"  notification: icon from '%ls' -> %dx%d, %zu byte(s)\n", source,
                    notificationIconWidth, notificationIconHeight,
                    notificationIconPixels.size());
                std::fflush(stdout);
            }
            frame.notificationIconPixels =
                notificationIconPixels.empty() ? nullptr : notificationIconPixels.data();
            frame.notificationIconWidth = notificationIconWidth;
            frame.notificationIconHeight = notificationIconHeight;
            frame.notificationIconRevision = notificationIconRevision;
        }
    }
    // Music is offered only when the card is the media card, matching the renderer's
    // one-message-per-card rule.
    frame.music = (frame.cardOwner == island::CardOwner::Music && nowPlaying.present)
                      ? &nowPlaying
                      : nullptr;
    // The output's peak, for the pill's background meter. This is the reader's CACHED
    // sample -- its worker refreshes it about thirty times a second (see
    // AudioVolume::Peak) -- so this is a load rather than a trip to the audio engine.
    frame.musicLevelKnown = systems.volume.Peak(&frame.musicLevel);
    frame.musicWavePhase = musicWavePhase;
    frame.musicAccent = D2D1::ColorF(waveAccent.r, waveAccent.g, waveAccent.b, 1.0f);
    frame.transportGlyph =
        (nowPlaying.state == island::PlaybackState::Playing) ? ">" : "||";
    frame.lyricLine = lyricLine;
    frame.fallbackLine = fallbackLine;

    // THE CARD'S OWN LYRICS, beside the cover: the line after the current one and how far through the
    // current line the song is -- the karaoke fill. The same sheet the pill's line is read from, so the
    // two cannot disagree; the current line itself is `lyricLine`, already set above.
    frame.lyricLineNext = nullptr;
    frame.lyricProgress = 0.0f;
    frame.lyricIndex = -1;
    frame.lyricCount = 0;
    if (runtime.currentLyrics && !runtime.currentLyrics->Empty() &&
        runtime.lastLyricIndex >= 0) {
        frame.lyricCount = runtime.currentLyrics->count;
        const island::LyricLine* current = runtime.currentLyrics->At(runtime.lastLyricIndex);
        const island::LyricLine* next = runtime.currentLyrics->At(runtime.lastLyricIndex + 1);
        if (current) {
            frame.lyricIndex = runtime.lastLyricIndex;
            frame.lyricLineNext = next ? next->text : nullptr;
            const long long start = current->timeMs;
            const long long end =
                next ? next->timeMs
                     : (runtime.currentLyrics->trackDurationMs > start
                            ? runtime.currentLyrics->trackDurationMs
                            : start + 4000);
            float progress = (end > start)
                                 ? static_cast<float>(bar.track.ms - start) /
                                       static_cast<float>(end - start)
                                 : 1.0f;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            frame.lyricProgress = progress;
        }
    }
    // How far through its arrival the pill's line is, and what it is replacing: 1 and
    // nothing once it has settled.
    frame.lyricLineRoll = island::LyricLineRollFor(bar.lyricLineAge);
    frame.lyricLinePrev = bar.replacedLyricText[0] != '\0' ? bar.replacedLyricText : nullptr;
    frame.lyricLinePrevScrollDip = bar.replacedLyricScrollDip;
    frame.noticeLine = noticeLine.empty() ? nullptr : noticeLine.c_str();
}

// --- the album art, the wash behind it, and the source application's icon ---
//
// Moved out of the frame loop verbatim (indentation aside; the moved text is otherwise untouched,
// down to the identifiers, which now read and write BarRuntime). Nothing about WHAT it does changed:
// one decode per revision, the wash built from the same bytes, and the cover dropped when the track
// has none. What changed is WHERE the results live, and that is not tidiness: the renderer caches its
// GPU bitmaps against exactly these pointers and revisions, so a picture that did not survive a pass
// would be decoded and uploaded again on the next one -- which is what the caches were added for.
static void FillDecodedPictures(BarRuntime& runtime, island::FrameState& frame,
                                const island::MusicInfo& nowPlaying) {
    // Album art is decoded once per artwork revision, not per frame: the
    // decode is the expensive part and the bytes only change when the
    // bar.track does. The blurred background is built here too -- it is a
    // second encode/decode pair, and doing it per frame would be wasteful
    // for a wash that only changes with the bar.track.
    //
    // ... AND IT IS DECODED WHETHER OR NOT THE CARD IS UP.
    //
    // This used to be gated on `frame.music` -- "nothing is displaying it, so do not decode it" --
    // and that gate had two costs, which the user found one after the other:
    //
    //   * the pill's audio wave takes its COLOUR from this same decode, so with the card closed the
    //     strip kept the previous bar.track's accent for the whole song. Reported as "歌词后面的 随音乐
    //     跳动的的颜色 如果鼠标不移入 就一直一个颜色" -- the colour only changed when the pointer
    //     went onto the island, because that is what opened the card, and opening the card is what
    //     ran the decode.
    //   * a cover that arrived while the card was closed had not been decoded by the time the card
    //     opened, so the card showed the placeholder for a moment and then caught up.
    //
    // One decode per bar.track either way: the same work the card would have done a moment later.
    if (nowPlaying.artworkSize > 0) {
        if (nowPlaying.artworkRevision != runtime.lastDecodedArtworkRevision) {
            runtime.lastDecodedArtworkRevision = nowPlaying.artworkRevision;
            island::DecodedImage decoded;
            // Its own slot: the blurred background below is decoded in the same frame, and
            // sharing one buffer is what drew the cover out of the blur's bytes (see
            // DecodeSlot).
            if (island::DecodeImage(nowPlaying.artwork, nowPlaying.artworkSize, &decoded,
                                    island::DecodeSlot::Artwork)) {
                runtime.artworkPixels = decoded.pixels;
                runtime.artworkWidth = decoded.width;
                runtime.artworkHeight = decoded.height;
                // The wave's colour, taken from the same decode: once per cover, not per
                // frame. See AccentColourFromArtwork.
                runtime.waveAccent = island::AccentColourFromArtwork(decoded.pixels, decoded.width,
                                                             decoded.height);
            } else {
                runtime.artworkPixels = nullptr;
                runtime.artworkWidth = 0;
                runtime.artworkHeight = 0;
                runtime.waveAccent = island::AccentColour{};
            }

            // THE ONE CASE THE LINE ABOVE CANNOT TELL: bytes arrived and would not decode. A cover
            // that never appears looks exactly like a cover that was never sent (`music: cover
            // NONE`), and this is the line that tells the two apart.
            island::LogAndPrint("  music: cover %ls %dx%d\n",
                                runtime.artworkPixels ? L"decoded" : L"COULD NOT DECODE",
                                runtime.artworkWidth, runtime.artworkHeight);
            std::fflush(stdout);

            // The card is 420x236 DIP; a wash at a quarter of that is
            // plenty, since it is out of focus by construction.
            static uint8_t backgroundPng[island::kMusicArtworkCapacity] = {};
            const size_t backgroundSize = island::MakeBlurredBackgroundPng(
                nowPlaying.artwork, nowPlaying.artworkSize, 105, 59,
                backgroundPng, sizeof(backgroundPng));
            if (backgroundSize > 0) {
                island::DecodedImage blurred;
                if (island::DecodeImage(backgroundPng, backgroundSize, &blurred,
                                        island::DecodeSlot::Background)) {
                    runtime.backgroundPixels = blurred.pixels;
                    runtime.backgroundWidth = blurred.width;
                    runtime.backgroundHeight = blurred.height;
                } else {
                    runtime.backgroundPixels = nullptr;
                }
            } else {
                runtime.backgroundPixels = nullptr;
            }
        }
        if (runtime.artworkPixels) {
            frame.artworkPixels = runtime.artworkPixels;
            frame.artworkWidth = runtime.artworkWidth;
            frame.artworkHeight = runtime.artworkHeight;
            // The renderer caches the GPU bitmap against this, so it is
            // uploaded once per bar.track rather than once per frame.
            frame.artworkRevision = nowPlaying.artworkRevision;
        }
        if (runtime.backgroundPixels) {
            frame.backgroundPixels = runtime.backgroundPixels;
            frame.backgroundWidth = runtime.backgroundWidth;
            frame.backgroundHeight = runtime.backgroundHeight;
            // Derived from the artwork revision: the wash is a function of the
            // same cover, so it changes exactly when the cover does.
            frame.backgroundRevision = nowPlaying.artworkRevision;
            // The card's own fill has to become translucent or the wash
            // would be hidden behind it -- and only the CARD's: with no card up this would change
            // the island's own fill, which is the one thing the wash has nothing to do with.
            if (frame.music) {
                frame.pill.fill = island::CurrentTheme().pillTop;
            }
        }
    } else if (nowPlaying.artworkRevision == 0 && runtime.lastDecodedArtworkRevision != 0) {
        // THE COVER BELONGS TO THE TRACK, SO DROP IT WHEN THE TRACK HAS NONE.
        //
        // Everything the block above produces lives on in `runtime.artworkPixels` and `runtime.backgroundPixels`
        // -- file-scope pointers the renderer caches its GPU copies against -- and that block only
        // runs while the session HAS artwork. So a bar.track that starts without any kept drawing the
        // PREVIOUS bar.track's cover: reported as "歌曲切换了 但是封面图没有换", which is the opposite
        // of the complaint before it (an empty slot) and the same root cause seen from the other
        // side.
        //
        // A revision of 0 is the session saying "this bar.track has no cover yet", and the retry in
        // MusicSession keeps asking for one -- when it arrives the revision goes to 1 and the
        // decode above takes over. Until then the card shows the PLACEHOLDER (see DrawMusicCard),
        // not someone else's album art.
        runtime.lastDecodedArtworkRevision = 0;
        runtime.artworkPixels = nullptr;
        runtime.artworkWidth = 0;
        runtime.artworkHeight = 0;
        runtime.backgroundPixels = nullptr;
        runtime.backgroundWidth = 0;
        runtime.backgroundHeight = 0;
        runtime.waveAccent = island::AccentColour{};
        island::LogAndPrint("  music: cover dropped (the new bar.track has none yet)\n");
        std::fflush(stdout);
    }

    // The source application's icon, decoded once per icon revision. It is not
    // inside the artwork block: it does not depend on the album art at all, and it
    // must still show for a session that publishes no cover.
    if (nowPlaying.iconSize > 0 &&
        nowPlaying.iconRevision != runtime.lastDecodedIconRevision) {
        runtime.lastDecodedIconRevision = nowPlaying.iconRevision;
        island::DecodedImage decodedIcon;
        if (island::DecodeImage(nowPlaying.icon, nowPlaying.iconSize, &decodedIcon,
                                island::DecodeSlot::Icon)) {
            runtime.iconPixels = decodedIcon.pixels;
            runtime.iconWidth = decodedIcon.width;
            runtime.iconHeight = decodedIcon.height;
        } else {
            runtime.iconPixels = nullptr;
        }
    }
    if (runtime.iconPixels) {
        frame.iconPixels = runtime.iconPixels;
        frame.iconWidth = runtime.iconWidth;
        frame.iconHeight = runtime.iconHeight;
        frame.iconRevision = nowPlaying.iconRevision;
    }
    // Reported whenever the answer changes, so "the icon is missing" splits into: no
    // bytes, bytes that will not decode, or bytes that decode and are never drawn.
    // Those have three different fixes, and from the screen they look identical.
    if (runtime.iconWidth != runtime.lastReportedIconWidth ||
        (runtime.iconPixels != nullptr) != runtime.iconWasDecoded) {
        runtime.lastReportedIconWidth = runtime.iconWidth;
        runtime.iconWasDecoded = runtime.iconPixels != nullptr;
        island::LogAndPrint("  icon decode: %s, %dx%d, rev %llu",
                            runtime.iconPixels ? "ok" : "NO PIXELS", runtime.iconWidth, runtime.iconHeight,
                            nowPlaying.iconRevision);
    }
}

// --- a click outside the bar puts the panel away ---
//
// Moved out of the frame loop verbatim (indentation aside), with one deliberate change: the call to the
// dismiss lambda became a call to DismissPanelSurface with the same four arguments the lambda would have
// used, because a free function cannot see a lambda defined in the loop.
static void ApplyOutsideClickDismissal(island::BarState& bar, island::BarSystems& systems,
                                       const island::Notification* active, island::IslandState resting) {
    // --- a click OUTSIDE the bar puts the panel away -------------------------------
    //
    // The card is open until the user says otherwise, and "otherwise" is a click that the bar never
    // hears about: the window is CLICK-THROUGH everywhere except the pill and the card, so a click on
    // the desktop, on another application, or in the shell's own tray is delivered to whatever was
    // clicked and not to us. The panel therefore looked stuck -- reported as exactly that, with the
    // card sitting over a window the user had already clicked into.
    //
    // So the button is LOOKED AT rather than waited on. This is what the shell's own flyout does as
    // well: its outside click dismisses the flyout and also acts on whatever was clicked.
    //
    // Polled, not hooked. A WH_MOUSE_LL hook would see the click first, and it would also mean this
    // process watching every click on the machine for days at a time for the sake of one flyout --
    // a disproportionate thing for a bar to do, and the kind of thing that gets a program flagged.
    if (bar.injectClickAtMs_ != 0 && GetTickCount64() >= bar.injectClickAtMs_) {
        bar.injectClickAtMs_ = 0;
        island::LogAndPrint("  diagnostic: clicking at (%ld,%ld) from the bar itself\n",
                            bar.injectClick_, bar.injectClickY_);
        std::fflush(stdout);
        SetCursorPos(bar.injectClick_, bar.injectClickY_);
        Sleep(80);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        Sleep(80);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
    if (bar.systemPanelOpen) {
        POINT where = {};
        GetCursorPos(&where);
        const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (leftDown && !bar.leftWasDown) {
            // Reported even when the press was on the bar: "the panel did not close" and "the press
            // was never seen" are different faults, and only this line tells them apart.
            const bool onBar = systems.window.HitTestScreen(where);
            island::LogAndPrint(
                "  system panel: a press at (%ld,%ld) %ls the bar\n", where.x, where.y,
                onBar ? L"on" : L"outside");
            if (onBar) {
                // A press on the bar has its own meaning, decided by the click handler; this poll is
                // only about the presses the bar never hears about.
            } else {
                DismissPanelSurface(bar, systems, active, resting);
                island::LogAndPrint("  system panel: closed (a click outside the bar)\n");
                std::fflush(stdout);
            }
        }
        bar.leftWasDown = leftDown;
    } else {
        // The state is tracked even while the panel is shut, so that a panel OPENED by pressing the
        // deck button -- which is itself a click -- is not dismissed by the button being down at the
        // moment it opened.
        bar.leftWasDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }
}

// --- the panel's own timer, when the diagnostic is on ---
//
// Moved out of the frame loop verbatim (indentation aside), with the dismiss call becoming
// DismissPanelSurface for the same reason the outside-click phase does it: a free function cannot see a
// lambda defined in the loop.
static void RunPanelTimerDiagnostic(DWORD autoPanelMs, DWORD autoPanelCloseMs, DWORD autoPanelCycleMs,
                                    DWORD diagnosticStart, const wchar_t* forcedState,
                                    bool& autoPanelOpened, bool& autoPanelClosed,
                                    island::BarState& bar, island::BarSystems& systems,
                                    const island::Notification* active, island::IslandState resting) {
    // Placed where a click would have left it -- `bar.systemPanelOpen` is the same flag the chevron
    // sets -- so what the frame loop does next is what it does for a real press, with no
    // pointer involved.
    if (autoPanelMs > 0 && !forcedState) {
        const DWORD diagnosticElapsed = GetTickCount() - diagnosticStart;
        if (autoPanelCycleMs > 0 && autoPanelOpened && autoPanelClosed) {
            // Open, close, open, close ... on a fixed period, which is what a user pressing the
            // control repeatedly does.
            const DWORD phase =
                (diagnosticElapsed - autoPanelMs) / autoPanelCycleMs;
            const bool wantOpen = (phase % 2) == 0;
            if (wantOpen != bar.systemPanelOpen) {
                bar.systemPanelOpen = wantOpen;
                island::LogAndPrint("  diagnostic: panel %ls at %lu ms (cycle %lu)\n",
                                    wantOpen ? L"opened" : L"closed",
                                    static_cast<unsigned long>(diagnosticElapsed),
                                    static_cast<unsigned long>(phase));
                std::fflush(stdout);
            }
        } else if (!autoPanelOpened && diagnosticElapsed >= autoPanelMs) {
            autoPanelOpened = true;
            bar.systemPanelOpen = true;
            island::LogAndPrint("  diagnostic: panel opened at %lu ms\n",
                                static_cast<unsigned long>(diagnosticElapsed));
            std::fflush(stdout);
        } else if (autoPanelOpened && !autoPanelClosed &&
                   diagnosticElapsed >= autoPanelCloseMs) {
            autoPanelClosed = true;
            // Through the same door a press uses, so the diagnostic reproduces the real thing rather
            // than a version of it that leaves the shape where it was.
            DismissPanelSurface(bar, systems, active, resting);
            island::LogAndPrint("  diagnostic: panel closed at %lu ms\n",
                                static_cast<unsigned long>(diagnosticElapsed));
            std::fflush(stdout);
        }
    }
}

// --- the Start button's own timer, when the diagnostic is on ---
//
// Moved out of the frame loop verbatim (indentation aside).
static void RunStartTimerDiagnostic(DWORD autoStartMs, DWORD autoStartCycleMs, DWORD diagnosticStart,
                                    const wchar_t* forcedState, DWORD& lastStartTogglePhase) {
    // This presses the control that has to inject input into the SHELL, on a period, so the one
    // path that needs a hand on the mouse can be exercised from a script. It is a toggle, like the
    // key: the first call opens the menu, the next closes it.
    //
    // The integrity level matters here and nowhere else in this program: UIPI discards synthetic
    // input aimed at a process of a HIGHER integrity level, and the shell that answers Ctrl+Esc is
    // Explorer. A bar started from a restricted launcher is below it, and the keystroke then goes
    // nowhere with no error anywhere -- which is exactly what the report "the Start button no
    // longer works" looked like. Whether that is the case is stated at startup ("native taskbar:
    // CANNOT BE HIDDEN"), and repeated here with the result of the attempt.
    if (autoStartMs > 0 && !forcedState) {
        const DWORD diagnosticElapsed = GetTickCount() - diagnosticStart;
        if (diagnosticElapsed >= autoStartMs) {
            const DWORD period = (autoStartCycleMs > 0) ? autoStartCycleMs : 2000;
            const DWORD phase = (diagnosticElapsed - autoStartMs) / period;
            if (phase != lastStartTogglePhase) {
                lastStartTogglePhase = phase;
                const island::StartMenuResult startResult = island::ToggleStartMenu();
                island::LogAndPrint("  diagnostic: start menu toggle %lu -> %ls at %lu ms\n",
                                    static_cast<unsigned long>(phase),
                                    island::StartMenuResultName(startResult),
                                    static_cast<unsigned long>(diagnosticElapsed));
                std::fflush(stdout);
            }
        }
    }
}


// --- the tray, on its own cadence ---
//
// Moved out of the frame loop verbatim (indentation aside). Everything it keeps between ticks is BarState's
// already; the two numbers it is driven by -- how often to look, and which work bit to report -- come in as
// parameters rather than being duplicated, the same way the taskbar check does it.
static void TickTray(island::BarState& bar, island::BarSystems& systems,
                     unsigned long long& frameWork, unsigned long long refreshBit,
                     unsigned long long refreshMs) {
    // --- the tray, on its own cadence --------------------------------------------
    //
    // Refreshed here and not at the top of the loop so the count the frame draws, the count the
    // row's sizing spent room on and the count the window hit-tests are all the same number.
    if (GetTickCount64() - bar.lastTrayRefresh >= refreshMs) {
        bar.lastTrayRefresh = GetTickCount64();
        frameWork |= refreshBit;
        // The other half of "is the shell holding this one": a registration whose window has
        // gone belongs to an application that exited without saying so, which the registry
        // cannot see either (see shell/TrayLearn.h). Swept BEFORE the refresh, so the list the
        // card is about to draw has already lost it.
        island::ForgetDeadTrayWindows();
        const int trayNow = systems.trayIcons.Refresh();
        if (trayNow != bar.lastTrayCount) {
            bar.lastTrayCount = trayNow;
            island::LogAndPrint(L"  tray: %d icon(s) %ls", trayNow,
                                systems.trayIcons.SourceClause().c_str());
            std::fflush(stdout);
        }

        // Whatever the resident module has written since the last tick. No injection, no
        // broadcast, no waiting: an application that has already been reached keeps reporting
        // every registration for the rest of the session, and this is where those become
        // pressable (see shell/TrayLearn.h).
        //
        // ... EXCEPT WHILE A LEARNING PASS IS RUNNING, and that exception is a fix, not a
        // precaution. The tick and a pass drain the SAME ring through the same cursor
        // (shell/TrayLearn.cpp), and the pass DELETES the report file before it starts: records
        // this tick consumed had their lines written into a file the pass then removed.
        // MEASURED 2026-10-16: the first pass after Explorer restarted logged "0 registration(s)
        // captured" while the ring held 63 registrations, because this tick had drained them a
        // hundred milliseconds earlier. Nothing was lost -- the cache above is where they land --
        // but the count a pass prints is the count the next person believes, so the two must not
        // race over it.
        const int harvested =
            bar.learnInFlight->load() ? 0 : island::HarvestLearnedCallbacks();
        if (harvested > 0) {
            island::LogAndPrint("  tray learn: %d registration(s) arrived from applications "
                                "already reached by the module -- %d of %d icon(s) have a "
                                "callback entry now",
                                harvested, island::CountCoveredIcons(systems.trayIcons), trayNow);
            std::fflush(stdout);
        }

        // ... AND ONE MORE PASS ONLY WHEN THE SHELL ITSELF HAS TO BE TAUGHT AGAIN.
        //
        // The module lives in the process that receives tray registrations (the owner of
        // Shell_TrayWnd), and it is pinned there for the life of that process. So the one event
        // that costs the tray its learning is Explorer being replaced: a new process has none of
        // the module, and every registration after that goes unread. This watches for exactly
        // that, and for nothing else -- an application starting later needs no pass at all, which
        // is the whole point of reading registrations where they arrive (shell/TrayLearn.h).
        const DWORD shellNow = island::TrayShellProcessId();
        if (shellNow != 0 && shellNow != bar.lastShellPid && !bar.learnInFlight->load() &&
            GetTickCount64() - bar.lastLearnAttempt >= 15000) {
            bar.lastShellPid = shellNow;
            bar.lastLearnAttempt = GetTickCount64();
            // THE NEW SHELL HOLDS NONE OF THEM. Every registration the old one was holding died
            // with it, so the ledger is emptied before the module is offered again: until it
            // reports back the card falls back to the registry list (see shell/TrayIcons.h)
            // instead of drawing registrations no shell is holding any more.
            island::ResetLiveTray();
            bar.learnInFlight->store(true);
            island::LogAndPrint("  tray learn: the shell is a new process (pid %lu) -- offering "
                                "the module to it again (background)\n",
                                static_cast<unsigned long>(shellNow));
            std::fflush(stdout);
            // The flag is captured INTO A LOCAL NAME: a capture list takes names, not members, and
            // the worker outlives this scope -- it is detached, which is exactly why the flag is a
            // shared_ptr on the heap (see app/BarState.h).
            const std::shared_ptr<std::atomic<bool>> learnFlight = bar.learnInFlight;
            std::thread([learnFlight]() {
                island::TrayIcons icons;
                icons.Refresh();
                const island::TrayLearnResult learned = island::LearnTrayCallbacks(icons, true);
                const std::wstring observer = island::ObserverClause(learned);
                island::LogAndPrint("  tray learn: the shell (%d process(es) offered) was "
                                    "%s, %d registration(s) captured, %d new entr(ies) -- %d of "
                                    "%d icon(s) have a callback entry (in %lld ms) -- %ls",
                                    learned.appsTried,
                                    learned.appsAlreadyWatched > 0 ? "already carrying the module"
                                                                   : "given the module",
                                    learned.captured, learned.newEntries, learned.covered,
                                    learned.icons, learned.ms, observer.c_str());
                learnFlight->store(false);
            }).detach();
        }
    }
}


// --- the memory line ---
//
// Moved out of the frame loop verbatim (indentation aside). Emitted on a five-minute cadence and again
// shortly after a suspected grower; the counts ride on the same line as the bytes on purpose, because
// "a cache filled up" and "something is leaking" are told apart by whether they rise together (see
// ProcessMetrics.h). The six things it carries between frames are RunBar's locals and come in by
// reference; the two values it reads -- what is playing and the lyric sheet -- are the pass's own.
static void ReportMemoryLine(BarRuntime& runtime, island::BarSystems& systems, unsigned long long sampleMs,
                             const island::MusicInfo& nowPlaying, const island::Lyrics* currentLyrics) {
    // --- the memory line --------------------------------------------------------------------
    //
    // Emitted on a five-minute cadence and again shortly after a suspected grower. See the note
    // in ProcessMetrics.h: the counts ride on the same line as the bytes on purpose, because
    // "a cache filled up" and "something is leaking" are told apart by whether they rise together.
    {
        const unsigned long long nowTicks = GetTickCount64();
        const unsigned long long musicRevision = systems.music.Revision();
        const int trayCount = systems.trayIcons.Count();
        // The same expression the frame's own `outerCardUp` is built from (`frame` is declared in
        // the inner block that draws, and is not visible here). Kept as one expression rather
        // than two spellings of the same idea wherever it can be helped: this one only decides
        // when to take a sample, so a disagreement would cost a sample rather than a drawing.
        const bool cardUp = systems.window.State() == island::IslandState::Expanded ||
                            systems.window.FromState() == island::IslandState::Expanded;

        if (musicRevision != runtime.lastMemoryMusicRevision) {
            runtime.lastMemoryMusicRevision = musicRevision;
            runtime.memorySamplePendingAt = nowTicks + 3000;   // once the new cover and lyric sheet land
        }
        if (trayCount != runtime.lastMemoryTrayIcons) {
            runtime.lastMemoryTrayIcons = trayCount;
            runtime.memorySamplePendingAt = nowTicks + 3000;   // once the icons are decoded
        }
        if (runtime.lastMemoryCardUp && !cardUp) {
            // Delayed rather than immediate: a card's bitmaps and text layouts are released as it
            // closes, and what the sample is for is what is STILL held after that.
            runtime.memorySamplePendingAt = nowTicks + 30000;
        }
        runtime.lastMemoryCardUp = cardUp;

        const bool steady = (nowTicks - runtime.lastMemorySample) >= sampleMs;
        const bool afterEvent = runtime.memorySamplePendingAt != 0 && nowTicks >= runtime.memorySamplePendingAt;
        if (steady || afterEvent) {
            island::ProcessMemorySample sample;
            island::SampleProcessMemory(&sample);
            sample.vramValid = systems.renderer.VideoMemory(&sample.vramLocalBytes,
                                                    &sample.vramNonLocalBytes);

            island::ProcessCacheCounts counts;
            counts.coverBytes    = nowPlaying.artworkSize;
            counts.lyricLines    = currentLyrics ? currentLyrics->count : 0;
            counts.trayIcons     = trayCount;
            counts.taskButtons   = systems.taskWindows.Count();
            counts.notifications = systems.notifications.Count();
            systems.renderer.CacheCounts(&counts.bitmaps, &counts.windowIcons);

            const char* reason = runtime.firstMemorySample ? "start"
                                                  : (steady ? "5min" : "after-event");
            island::LogProcessMemory(reason, sample, counts);
            runtime.firstMemorySample = false;
            runtime.lastMemorySample = nowTicks;
            runtime.memorySamplePendingAt = 0;
        }
    }
}

bool RunPowerAction(PowerAction action) {
    // SHUTTING DOWN AND RESTARTING NEED A PRIVILEGE THAT IS HELD BUT DISABLED.
    //
    // `ExitWindowsEx` fails with ERROR_PRIVILEGE_NOT_HELD unless the process's token has
    // SE_SHUTDOWN_NAME ENABLED -- a normal user session holds it, so this is an enable and not an
    // elevation, and without it the menu item does nothing at all with no visible reason.
    const auto enableShutdownPrivilege = []() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
            return false;
        }
        LUID id = {};
        if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &id)) {
            CloseHandle(token);
            return false;
        }
        TOKEN_PRIVILEGES privileges = {};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = id;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        const BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
        CloseHandle(token);
        return ok != FALSE;
    };

    switch (action) {
        case PowerAction::Lock: {
            const BOOL locked = LockWorkStation();
            island::LogAndPrint("  power: lock -> %s\n", locked ? "ok" : "FAILED");
            return locked != FALSE;
        }
        case PowerAction::Sleep: {
            // Asynchronous: this returns as the machine is going to sleep.
            const BOOLEAN slept = SetSuspendState(FALSE, TRUE, FALSE);
            island::LogAndPrint("  power: sleep -> %s\n", slept ? "ok" : "FAILED");
            return slept != FALSE;
        }
        case PowerAction::SignOut: {
            const BOOL done = ExitWindowsEx(EWX_LOGOFF | EWX_FORCEIFHUNG, 0);
            island::LogAndPrint("  power: sign out -> %s (error %lu)\n", done ? "ok" : "FAILED",
                                done ? 0UL : GetLastError());
            return done != FALSE;
        }
        case PowerAction::Restart: {
            enableShutdownPrivilege();
            const BOOL done = ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, 0);
            island::LogAndPrint("  power: restart -> %s (error %lu)\n", done ? "ok" : "FAILED",
                                done ? 0UL : GetLastError());
            return done != FALSE;
        }
        case PowerAction::ShutDown: {
            enableShutdownPrivilege();
            const BOOL done = ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG, 0);
            island::LogAndPrint("  power: shut down -> %s (error %lu)\n", done ? "ok" : "FAILED",
                                done ? 0UL : GetLastError());
            return done != FALSE;
        }
    }
    return false;
}
}  // namespace

// `forcedState` pins the shape so it can be looked at, and `forcedStartHover` pins the Start button's
// hover. Debug only, and they exist because a shape can satisfy every assertion and still be wrong to
// look at: a screenshot of a named state settles in seconds what reasoning about geometry does not.
int RunBar(bool hideNativeTaskbar, bool claimAppBar, bool keepStrip, const wchar_t* forcedState,
           bool forcedStartHover, bool forcedSystemPanel, bool keepNativeTray) {
    island::MonitorGeometry monitor = island::QueryMonitor(nullptr);

    // --- the material ---------------------------------------------------------------------------
    //
    // Read before the first frame, because every surface, edge and word on the bar is drawn from it:
    // the built-in themes, plus any `themes\*.txt` beside the program, with the choice remembered in
    // `theme.txt` (see Theme.h). A missing file is the ordinary case.
    island::ApplyRememberedTheme();

    // ... and the colour it stands on, read ONCE from the wallpaper file (see shell/WallpaperTint.h). A
    // wallpaper that cannot be read logs one line and tints nothing, so this cannot stop the bar from
    // starting.
    {
        const island::WallpaperTintResult wallpaper = island::ReadWallpaperTint();
        island::SetWallpaperTint(wallpaper.colour, wallpaper.fromWallpaper);
    }

    // The machine's own state, sampled on a timer rather than per frame.

    // THE THINGS THE BAR TALKS TO, in one named place: the window and the renderer, and one object per
    // channel the outside world arrives through. See app/BarSystems.h for why this is separate from
    // app/BarState.h (resources, and a construction order) -- and note that the calls that START each
    // one stay below, in the order they always were: this line only says they exist.
    island::BarSystems systems;
    systems.statusReader.Open();

    // How long between samples. Most of these values come from APIs that open a COM object or walk
    // a device tree, and they do not change quickly enough to justify paying that every frame.
    // Two seconds is short enough that plugging in a network cable looks immediate.
    constexpr unsigned long long kStatusSampleMs = 2000;
    unsigned long long lastStatusSampleMs = 0;

    // How long the pointer must REST on an icon before its live thumbnail appears.
    //
    // Short enough that the preview is there by the time the user has decided to look at it, and long
    // enough that crossing the row on the way to another icon does not flash one up for every icon
    // passed. The shell's own taskbar uses about this figure.
    constexpr unsigned long long kThumbnailDwellMs = 350;

    // How often the foreground window is asked whether it has the whole screen. Four times a second:
    // GetForegroundWindow, GetWindowRect and a class name, which is nothing next to a frame.
    constexpr unsigned long long kFullscreenCheckMs = 250;

    // ISLANDBAR_NO_FULLSCREEN=1 makes the bar ignore a full-screen application in front.
    //
    // A DIAGNOSTIC, and one this project kept needing: while a game has the screen the bar stands down by
    // design, so there is no way to photograph the bar itself -- its tray, its row, its status line -- on a
    // machine where a game is running. That is exactly the machine this bar is developed on. Read once, at
    // startup.
    wchar_t noFullscreenSetting[8] = {};
    const bool noFullscreen =
        GetEnvironmentVariableW(L"ISLANDBAR_NO_FULLSCREEN", noFullscreenSetting, 8) > 0 &&
        noFullscreenSetting[0] == L'1';

    // How often the system tray is re-read. The registry plus a process snapshot, a few milliseconds; the
    // thing it describes -- which applications have a tray icon -- changes on the scale of starting and
    // stopping applications.
    constexpr unsigned long long kTrayRefreshMs = 5000;

    // THE BAR'S OWN STATE, SLICE 1: the tray card, its menu, and the clocks that re-read the tray and
    // learn what it holds. See app/BarState.h for why it is a struct now and what moves in next.
    //
    // bar.lastShellPid is seeded HERE rather than in the struct, because the shell that exists at startup is
    // what the first tick must compare against.
    island::BarState bar;
    bar.lastShellPid = island::TrayShellProcessId();
    // The system panel can be forced open at startup by a diagnostic; that is the only piece of the state
    // above whose initial value comes from a parameter instead of a constant (see app/BarState.h).
    bar.systemPanelOpen = forcedSystemPanel;
    // The last audio reading, kept for the CLICK handler as well as the draw: a click on a device row has to
    // name the endpoint it wants, and the row the user is looking at is the one in this snapshot. On the
    // heap, like the worker that fills it: it is tens of kilobytes and this function's frame is not the
    // place for that.

    island::LogAndPrint("IslandBar\n");
    island::LogAndPrint("  monitor %ldx%ld, work %ldx%ld, %u dpi, taskbar inset %ld px\n",
                island::ScreenWidth(monitor), island::ScreenHeight(monitor),
                island::WorkWidth(monitor), island::WorkHeight(monitor),
                monitor.dpi, island::TaskbarThickness(monitor));
    island::LogAndPrint("  move the pointer onto the pill to spread it; click to open the card\n");
    island::LogAndPrint("  close the window to exit\n");

    // What happens to the shell's taskbar, and the report of it. The phase lives above RunBar; the order it
    // runs in is unchanged (see the note on the phase functions).
    ApplyNativeTaskbarMode(hideNativeTaskbar, keepNativeTray, systems);
    // Flushed explicitly rather than left to the buffer: this process runs for
    // hours and is then killed, and a killed process discards whatever the
    // runtime still holds. Redirecting stdout makes _IOLBF block-buffered
    // anyway, so the mode alone cannot be relied on here.
    std::fflush(stdout);

    // The window, and the AppBar: the strip the desktop gives up for it. The phase lives above RunBar and
    // the order is unchanged; the failure path stays here, where the startup's other exits are.
    //
    // kReservedBarHeightDip, not margin + collapsed height: the reservation has to cover the whole
    // chrome, and the two differ by the pill's own height. See the constant. Kept here because the
    // full-screen exit re-claims with the same number (search for barThicknessPx below).
    const LONG barThicknessPx =
        island::DipToPixels(island::kReservedBarHeightDip, monitor.dpi);
    if (!CreateBarWindowAndClaimAppBar(claimAppBar, monitor, barThicknessPx, systems)) {
        island::LogAndPrint("FATAL: could not create the window (error %lu)\n", GetLastError());
        std::fflush(stdout);
        // The taskbar was already hidden by this point, so it must not be left
        // that way because startup failed.
        systems.taskbar.Restore();
        return 1;
    }

    // The strip the shell's taskbar reserved, and the composition surface. Both phases live above RunBar;
    // the order is unchanged.
    ReleaseReservedStrip(hideNativeTaskbar, keepStrip, monitor, systems);
    const bool renderOk = InitializeCompositionSurface(systems);

    // The two notification channels. The phase lives above RunBar; the flags stay here because the frame
    // loop reads them (see the note on the phase functions).
    bool listening = false;
    bool toastsAvailable = false;
    StartNotificationChannels(listening, toastsAvailable, systems);

    // The THIRD channel: WeChat, which tells Windows nothing at all and shows its own popup instead.
    //
    // Polled faster than the toast database because a WeChat popup is on screen for a few seconds: two
    // seconds of polling would miss it about as often as not. The scan is a handful of windows.
    std::fflush(stdout);

    // The two media sources. The phase lives above RunBar; the flags stay here because the frame loop
    // reads them.
    bool musicAvailable = false;
    bool playerWindowAvailable = false;
    StartMediaReaders(musicAvailable, playerWindowAvailable, systems);

    // --- lyrics ---
    //
    // Also on a worker thread, because fetching them is a network round trip. A
    // failure here is not fatal either: the strip falls back to the bar.track name.
    // The output side: the volume, the sound page's own data, and the lyric fetcher. The phase lives above
    // RunBar; the flags stay here because the frame loop reads them.
    bool volumeAvailable = false;
    bool soundOutputStarted = false;
    bool lyricsAvailable = false;
    StartOutputWorkers(volumeAvailable, soundOutputStarted, lyricsAvailable, systems);

    // The weather reader, the hover preview, and the tray's own startup (its first read and the learning
    // pass). The phases live above RunBar; the order is unchanged. Only the preview's flag is read later.
    bool previewReady = false;
    StartWeatherAndPreview(previewReady, systems);
    ReadTrayAndStartLearning(systems);

    // The Start card's own state. The phase lives above RunBar; the order is unchanged (see the note on the
    // phase functions).
    StartStartCard(bar, systems);

    // --- the taskbar's window list ---
    //
    // Refreshed from the frame loop rather than polled here, because it is only needed while the
    // strip is on screen. The titles are copied into this array for the frame to point at: the list
    // can refresh between frames, and a frame that pointed into the list would read freed entries.

    if (!renderOk) {
        island::LogAndPrint("WARNING: composition surface unavailable; the window stays empty\n");
        std::fflush(stdout);
    } else {
        // Frame pacing. The compositor clock drives this when available, so an
        // idle desktop costs nothing; otherwise MsgWaitForMultipleObjects blocks
        // on input, which is also event-driven rather than a spin.
    const bool clockDriven = systems.renderer.WaitForCompositorClock(1);
    // Which wait the loop will use, named once here and repeated on every throttle line below. The
    // order is the order of preference: the swap chain's own frame-latency object, then the compositor
    // clock, then a short message wait.
    const char* pacingPath = systems.renderer.PacingIsWaitable() ? "waitable"
                                                        : (clockDriven ? "compositor" : "timer");
    island::LogAndPrint("  frame pacing: %s\n", pacingPath);
        std::fflush(stdout);

        LARGE_INTEGER frequency = {};
        LARGE_INTEGER previous = {};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&previous);

        island::IslandState lastLogged = systems.window.State();
        bool haveDrawn = false;
        // The decoded pictures and the wave's colour are BarRuntime's now: they outlive a pass (see
        // FillDecodedPictures). The wave PHASE stays here -- this is the loop's own clock, not state the
        // drawing reads.
        float musicWavePhase = 0.0f;

        // --- now-playing display policy ---
        //
        // The card opens when playback starts and closes a few seconds after it
        // stops. It deliberately does NOT open for a paused session: the bar is
        // always on top of everything, and a card that unfolds every time a browser
        // tab pauses a video would be worse than no feature at all.
        //
        // The reading itself, and everything that decides when to take a new one, is BarRuntime's now (see
        // ReadWhatIsPlaying), and so is the last seen value of what the drawing watches (see PollRedrawReasons
        // and DecideNeedDraw, and the note on those fields).
        // ISLANDBAR_MEM_MS=N puts the memory line on a faster cadence, and it is the switch that makes a
        // leak measurable at all. The line carries the handle, GDI, user-object, thread and bitmap counts
        // beside the bytes, so a run that drives one operation in a loop and samples every few seconds
        // shows a leak as a TREND rather than as one number -- which is the difference between "it leaked"
        // and "it was 1000 handles when I looked". On the default five-minute cadence that test needs an
        // hour of driving to produce two samples.
        //
        // The event line cannot stand in for it (measured 2026-10-16): a card closing re-arms that line's
        // own 30 s delay every time, so a loop that opens and closes a card every 2.5 s never produces one
        // -- a 200 s run with 40 cycles wrote exactly zero samples.
        //
        // Values below one second are refused rather than clamped: a sample per frame would flood the log
        // and change the very thing being measured.
        const unsigned long long kDefaultMemorySampleMs = 5ull * 60ull * 1000ull;
        unsigned long long kMemorySampleMs = kDefaultMemorySampleMs;
        {
            wchar_t memorySetting[32] = {};
            if (GetEnvironmentVariableW(L"ISLANDBAR_MEM_MS", memorySetting, 32) > 0) {
                const unsigned long long wanted = wcstoull(memorySetting, nullptr, 10);
                if (wanted >= 1000ull) kMemorySampleMs = wanted;
            }
        }
        // The card's owner and the copy of the message it is showing live in BarRuntime: the latch that
        // writes them and the drawing that reads them are in different phases of the same pass, which is
        // what the struct is for (see BarRuntime).
        BarRuntime runtime;
        DWORD lastStartTogglePhase = 0xFFFFFFFFul;   // no toggle has happened yet
        // The revisions start WHERE THEY ARE, not at zero: the workers publish only CHANGES, and a session
        // already up and steady when the bar starts would otherwise read as "nothing changed" forever. One
        // assignment each, here, because a default member initialiser cannot see `systems`.
        runtime.lastMusicRevision = systems.music.Revision();
        runtime.lastLyricsRevision = systems.lyrics.Revision();
        runtime.lastDrawnMusicRevision = systems.music.Revision();
        runtime.lastVolumeRevision = volumeAvailable ? systems.volume.Revision() : 0;
        runtime.musicPositionAt = GetTickCount64();

        // --- diagnostic hooks, all off unless the environment asks for them ---
        //
        // The panel's opening animation has been argued about from still captures three times, because
        // this machine cannot move the pointer (so no transition can be performed on demand) and the
        // interesting part of a 0.36 s movement is invisible in a photograph of either end of it.
        //
        //   ISLANDBAR_PANEL_MS    open the system panel after N ms, close it again at 2N -- a real
        //                         transition, with no input needed
        //   ISLANDBAR_SLOWMO      stretch every transition's duration by this factor, so a capture can
        //                         land inside one
        //   ISLANDBAR_TRACE_CARD  one line per frame of a card transition: frame interval, eased
        //                         progress, the card's rectangle, the content's opacity and the window
        //                         region
        //
        // Read once, here. Unset, they cost a few microseconds and change nothing.
        DWORD autoPanelMs = 0;
        DWORD autoPanelCloseMs = 0;
        DWORD autoPanelCycleMs = 0;
        DWORD autoStartMs = 0;
        DWORD autoStartCycleMs = 0;
        int forcedHoverIcon = -2;   // -2 none, -1 the last icon, 0..n that icon
        DWORD autoDeckHoverMs = 0;  // >0: pretend the pointer reaches a deck button after this long
        bool deckHoverForced = false;
        // >0: flick the pointer on and off the island with this period, which is what a mouse crossing
        // the bar quickly does. Reproduces an INTERRUPTED card retraction -- see the pill's line rule in
        // Renderer.cpp -- which is otherwise only reachable by hand.
        DWORD autoInsideFlickerMs = 0;
        bool flickerInside = true;
        DWORD autoCardHoverMs = 0;  // >0: pretend the pointer reaches the card after this long
        bool cardHoverForced = false;
        int  autoCardMode = 3;      // 3 = on the card, 4 = in the gap above it
        float slowMotion = 1.0f;
        bool traceCardFrames = false;
        {
            wchar_t setting[32] = {};
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_MS", setting, 32) > 0) {
                autoPanelMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // Which page of tiles to open on, so the second page can be photographed without a pointer.
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_PAGE", setting, 32) > 0) {
                bar.startPanelPage = static_cast<int>(wcstol(setting, nullptr, 10));
                bar.systemPanelPage = bar.startPanelPage;
            }
            // Open the panel's second level -- the 声音输出 page -- straight away, so it can be photographed
            // without a pointer, and at a chosen scroll offset so its own second half (the mixer) can be too.
            if (GetEnvironmentVariableW(L"ISLANDBAR_SOUND_PAGE", setting, 32) > 0) {
                bar.soundPageOpen = wcstol(setting, nullptr, 10) != 0;
                bar.systemPanelOpen = true;
                island::LogAndPrint("  diagnostic: the sound page is opened at startup\n");
                std::fflush(stdout);
            }
            // ... and the tray's page, for the same reason: it is behind a click on a control whose clicks
            // cannot be synthesised from a script on this machine (see ISLANDBAR_CLICK_AT), so without this
            // there is no way to photograph it.
            if (GetEnvironmentVariableW(L"ISLANDBAR_TRAY_PAGE", setting, 32) > 0) {
                bar.trayPageOpen = wcstol(setting, nullptr, 10) != 0;
                bar.systemPanelOpen = bar.trayPageOpen;
                island::LogAndPrint("  diagnostic: the tray page is opened at startup\n");
                std::fflush(stdout);
            }
            // ... and the Start card's own page, for the same reason: the card is behind a click on a
            // control this machine's scripting cannot press, so without this there is no way to photograph
            // it. The value IS the page: 0 the pins, 1 the picker, 2 the power list.
            if (GetEnvironmentVariableW(L"ISLANDBAR_START_PAGE", setting, 32) > 0) {
                bar.startPageState = static_cast<int>(wcstol(setting, nullptr, 10));
                bar.startPageOpen = true;
                bar.systemPanelOpen = true;
                island::LogAndPrint("  diagnostic: the Start card is opened on page %d\n", bar.startPageState);
                std::fflush(stdout);
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_SOUND_SCROLL", setting, 32) > 0) {
                bar.soundPageScroll = wcstof(setting, nullptr);
            }
            // Synthesize ONE click at a screen point, after the bar has settled.
            //
            // This exists because the outside-click dismissal CANNOT be tested from a script on a machine
            // like this one: the bar runs at the shell's integrity level and a script's synthetic input is
            // sent from lower down, so Windows drops it before the bar's own GetAsyncKeyState can see it --
            // the test looks exactly like a broken feature. A click the BAR sends to itself is not filtered
            // that way, so this is the only way to check "a press outside the card puts it away" end to end.
            //
            // ISLANDBAR_CLICK_AT="x,y" (screen pixels), one shot, reported before and after.
            if (GetEnvironmentVariableW(L"ISLANDBAR_CLICK_AT", setting, 32) > 0) {
                wchar_t point[32] = {};
                GetEnvironmentVariableW(L"ISLANDBAR_CLICK_AT", point, 32);
                if (swscanf_s(point, L"%ld,%ld", &bar.injectClick_, &bar.injectClickY_) == 2) {
                    bar.injectClickAtMs_ = GetTickCount64() + 3000;
                    island::LogAndPrint("  diagnostic: will click at (%ld,%ld) in 3 s\n", bar.injectClick_,
                                        bar.injectClickY_);
                    std::fflush(stdout);
                }
            }
            // Hold the panel -- and, if it is up, the sound page -- open whatever happens, so a photograph of
            // it cannot be undone by a stray click elsewhere on the machine. Debug only, and it says so where
            // it takes effect.
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_LOCK", setting, 32) > 0) {
                bar.panelLocked = wcstol(setting, nullptr, 10) != 0;
                bar.lockedSoundPage = bar.soundPageOpen;
                island::LogAndPrint("  diagnostic: the panel is held open (lock %d)\n",
                                    bar.panelLocked ? 1 : 0);
                std::fflush(stdout);
            }
            // Separate from the opening time on purpose: with the transition stretched, closing at
            // twice the opening delay would cut the movement off in the middle of itself, and the
            // capture would show a reversal rather than the thing being measured.
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_CLOSE_MS", setting, 32) > 0) {
                autoPanelCloseMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // Toggles the panel over and over, which is the only way to exercise the SECOND, THIRD and
            // fourth transitions: pressing the control again is what the request flag does, and the
            // defects that survived the first round lived entirely in the repeats.
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_CYCLE_MS", setting, 32) > 0) {
                autoPanelCycleMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // Presses the Start button on a timer. The one part of this program that has to inject input
            // into the SHELL, and therefore the one part that cannot be tried by hand from a script --
            // it needs a click on the bar. A timer gives it one, and the result is reported: "the
            // channel is blocked" and "the keystroke went and nothing happened" are the same picture.
            if (GetEnvironmentVariableW(L"ISLANDBAR_START_MS", setting, 32) > 0) {
                autoStartMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_TRANSPORT_MS", setting, 32) > 0) {
                bar.autoTransportMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_START_CYCLE_MS", setting, 32) > 0) {
                autoStartCycleMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // 1..5 chooses how the Start menu is asked for; see StartMenuRoute.
            if (GetEnvironmentVariableW(L"ISLANDBAR_START_ROUTE", setting, 32) > 0) {
                const long route = wcstol(setting, nullptr, 10);
                if (route >= 1 && route <= 5) {
                    island::SetStartMenuRoute(static_cast<island::StartMenuRoute>(route - 1));
                }
            }
            // Which icon counts as hovered. The icon row only exists for the pointer, and a script cannot
            // move the pointer here, so this is how a row with its last button opened can be photographed.
            if (GetEnvironmentVariableW(L"ISLANDBAR_HOVER_ICON", setting, 32) > 0) {
                forcedHoverIcon = static_cast<int>(wcstol(setting, nullptr, 10));
                systems.window.SetForcedHoverIcon(forcedHoverIcon);
                island::LogAndPrint("  diagnostic: icon %d is treated as hovered\n", forcedHoverIcon);
                std::fflush(stdout);
            }
            // Treat the pointer as being on the bar. The hover is what opens the media card, and the
            // pointer cannot be moved from a script here, so this is how that card gets photographed.
            if (GetEnvironmentVariableW(L"ISLANDBAR_FORCE_INSIDE", setting, 32) > 0) {
                const int mode = static_cast<int>(wcstol(setting, nullptr, 10));
                systems.window.SetForcedPointerInside(mode);
                island::LogAndPrint("  diagnostic: the pointer is treated as %ls\n",
                                    mode == 4   ? L"in the GAP between the bar and the card"
                                    : mode == 3 ? L"on the CARD"
                                    : mode == 2 ? L"on the BAR but off the island"
                                    : mode == 1 ? L"on the pill"
                                                : L"off");
                std::fflush(stdout);
            }
            // ... and as resting on a deck button AFTER N ms, for the rule that gives the card up when the
            // pointer moves off the pill. Timed rather than immediate because the case worth checking is the
            // TRANSITION -- a card that is already open being put away -- and at startup nothing is open yet.
            if (GetEnvironmentVariableW(L"ISLANDBAR_FORCE_DECK_MS", setting, 32) > 0) {
                autoDeckHoverMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // The pointer flicking in and out, which is the gesture that interrupts a card's exit. A
            // script cannot do it here (its input is dropped before it reaches the bar), and the
            // interruption is the only way to get a frame where the shape is card-sized with no card up.
            if (GetEnvironmentVariableW(L"ISLANDBAR_INSIDE_FLICKER_MS", setting, 32) > 0) {
                autoInsideFlickerMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
                island::LogAndPrint("  diagnostic: the pointer flicks in and out every %lu ms\n",
                                    static_cast<unsigned long>(autoInsideFlickerMs));
                std::fflush(stdout);
            }
            // ... or onto the CARD, which is the same kind of check for the other direction: the pointer
            // walks from the pill down onto the card, and the card must stay for its buttons to be usable.
            if (GetEnvironmentVariableW(L"ISLANDBAR_FORCE_CARD_MS", setting, 32) > 0) {
                autoCardHoverMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
                // Which half of the column to move to: 3 = the card itself (the default), 4 = the gap above
                // it. The gap is the half that matters for the walk from the pill, and it is the one the
                // first version of this missed.
                if (GetEnvironmentVariableW(L"ISLANDBAR_FORCE_CARD_MODE", setting, 32) > 0) {
                    autoCardMode = static_cast<int>(wcstol(setting, nullptr, 10));
                }
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_SLOWMO", setting, 32) > 0) {
                slowMotion = wcstof(setting, nullptr);
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_TRACE_CARD", setting, 32) > 0) {
                traceCardFrames = (wcstol(setting, nullptr, 10) != 0);
            }
        }
        if (slowMotion > 1.0f) {
            systems.window.ScaleTransitionDurations(slowMotion);
            island::LogAndPrint("  diagnostic: transitions stretched %.0fx\n", slowMotion);
            std::fflush(stdout);
        }
        if (autoPanelMs > 0) {
            if (autoPanelCloseMs == 0) autoPanelCloseMs = autoPanelMs * 2;
            island::LogAndPrint("  diagnostic: the system panel opens at %lu ms, closes at %lu ms\n",
                                static_cast<unsigned long>(autoPanelMs),
                                static_cast<unsigned long>(autoPanelCloseMs));
            std::fflush(stdout);
        }
        if (autoStartMs > 0) {
            island::LogAndPrint("  diagnostic: the Start button is pressed at %lu ms and every %lu ms "
                                "after that, route = %ls\n",
                                static_cast<unsigned long>(autoStartMs),
                                static_cast<unsigned long>(autoStartCycleMs > 0 ? autoStartCycleMs
                                                                                : 2000),
                                island::StartMenuRouteName(island::CurrentStartMenuRoute()));
            std::fflush(stdout);
        }
        const DWORD diagnosticStart = GetTickCount();
        bool autoPanelOpened = false;
        bool autoPanelClosed = false;

        // Marquee speed. Slow enough to read a Chinese lyric line.
        constexpr float kLyricScrollDipPerSec = 26.0f;

        // Re-hiding the taskbar is checked on a slow cadence. The shell puts it
        // back on its own -- an Explorer restart, a display change, the Windows
        // key -- but nothing about that is urgent enough to poll at frame rate,
        // and each check is a handful of window queries.
        int taskbarCheckCountdown = 0;

        // WHICH PERIODIC TASK RAN IN A PASS, so a slow frame can name its own cause. Set by each block
        // below, moved into `lastWork` and cleared at the bottom of the pass -- the interval being
        // measured is the gap since the previous pass, so the flags that describe it are the previous
        // pass's. The names travel in the log line rather than in a debugger, because the interesting
        // hitches are one or two per twenty seconds and only happen on the machine being used.
        constexpr unsigned long long kWorkStatusSample = 1ull << 0;
        constexpr unsigned long long kWorkTrayRefresh = 1ull << 1;
        constexpr unsigned long long kWorkToastPoll = 1ull << 2;
        constexpr unsigned long long kWorkWeChatScan = 1ull << 3;
        constexpr unsigned long long kWorkWeChatSound = 1ull << 4;
        constexpr unsigned long long kWorkTaskbarCheck = 1ull << 5;
        constexpr unsigned long long kWorkThumbnail = 1ull << 6;
        constexpr unsigned long long kWorkDraw = 1ull << 7;
        unsigned long long frameWork = 0;
        unsigned long long lastWork = 0;

        while (true) {
            if (!systems.window.PumpMessages()) break;

            // The shell's taskbar, re-hidden on a slow cadence, and the strip given back if the shell
            // took it again. The phase lives above RunBar; its countdown and the work flags are carried
            // across frames, which is why they are handed in by reference.
            EnsureTaskbarStillHidden(hideNativeTaskbar, keepStrip, taskbarCheckCountdown, frameWork,
                                     kWorkTaskbarCheck, bar, systems, monitor);

            // The four channels that bring a message in. The phase lives above RunBar; which message is
            // showing right now is worked out below, where the policy and the drawing read it.
            PollNotificationChannels(listening, toastsAvailable, soundOutputStarted, bar, systems,
                                     frameWork, kWorkToastPoll, kWorkWeChatScan, kWorkWeChatSound);

            // Is something asking for attention right now? Computed before the
            // pointer logic so both branches agree on the same answer.
            const island::Notification* active =
                systems.notifications.ActiveDisplay(GetTickCount64());

            // ... AND A CALL HOLDS THE ISLAND, even when it is no longer the newest thing in the store.
            //
            // A call is pushed as a STICKY item so the island keeps it for as long as the call lasts -- but
            // `ActiveDisplay` answers with the NEWEST item only, so a WeChat message arriving during the call
            // took the island, expired six seconds later, and the call was never shown again for the rest of
            // the call. Reported as "微信通话 不显示了", and the log says exactly that:
            //
            //   13:37:22.307  wechat: call announced as '通话中' (notification 9)
            //   13:37:23.232  wechat: announced '有新消息' from its notification sound
            //   13:37:29.394  state: music-strip            <- the call is still up; the island is not
            //
            // The rule is the one the reference uses: a FRESH message wins (a call is not a reason to hide
            // what someone just sent), and the call comes back as soon as the message is done. Reading it --
            // a click on the island -- retires it for good, which is what "read" means everywhere else here.
            if (active == nullptr && systems.wechat.CallNotificationId() != 0) {
                const island::Notification* call = systems.notifications.Find(systems.wechat.CallNotificationId());
                if (call != nullptr && !call->read) active = call;
            }

            // ... AND THE WINDOW IS TOLD THE SAME ANSWER, because it cannot work it out for itself: while a
            // card is open its state is Expanded, and a notice is not a state -- it is something the bar is
            // saying. Everything that lays out the bar's own controls asks the window for this (see
            // SetNoticeStripUp), so the drawing, the hit test and the window region agree about one box.
            systems.window.SetNoticeStripUp(active != nullptr &&
                                            !IsCallNotification(systems, active));

            // What is playing, from whichever source has it: the media session, that player's own window
            // title, or the lyric provider's answer. The phase lives above RunBar; the reading it keeps
            // between frames is BarRuntime's.
            ReadWhatIsPlaying(runtime, bar, systems, musicAvailable, playerWindowAvailable);

            // --- a full-screen application gets the whole screen ----------------
            //
            // The shell tells us when a full-screen window comes to the front. It has done so since the
            // AppBar was written and NOTHING EVER LISTENED: the accessor existed with no callers.
            //
            // What can be done about it is limited by the desktop, not by this program. A screen edge
            // holds ONE AppBar, and the shell's own taskbar is a registered AppBar even while hidden --
            // it keeps its claim on whatever edge it is docked to. So the desktop work area stays inset
            // by its thickness, a maximized window keeps an unusable band above it, and claiming that
            // edge for ourselves is refused for the same reason.
            //
            // The bar therefore FILLS the band rather than fighting for it: while an application is
            // full-screen the shape becomes the full-width strip (see IslandState::FullBar), which is
            // exactly as tall as the reservation, so the band is covered and nothing is wasted.
            //
            // The reservation is released meanwhile, because holding it would be reserving space for a
            // bar that is not in its usual place.
            UpdateFullscreenStandDown(bar, systems, monitor, noFullscreen, kFullscreenCheckMs,
                                      barThicknessPx);

            // --- the resting state ---
            //
            // A MEDIA SESSION -- playing or PAUSED -- means the lyric strip; no session at all means the
            // plain pill. Named once here so the click handling, the pointer handling and the renderer
            // cannot disagree about where "rest" is.
            //
            // It used to require PLAYING, so pausing swapped the lyric for the clock. Asked for in these
            // words: "暂停音乐后灵动岛不显示歌词了 应该显示" -- the words are what the strip is for, and a
            // paused player still has them.
            //
            // EITHER source counts as something to say, because the question is what the PILL is for, not
            // where the words came from: the session's bar.track, or the player's own window title while it
            // publishes no session (see PlayerWindow.h). The CARD is a different question and stays with
            // the session -- see ShouldPromoteToCard.
            const bool musicResting = bar.activePresent;
            const island::IslandState resting =
                musicResting ? island::IslandState::MusicStrip : island::IslandState::Collapsed;

            // Puts the panel away -- and puts the SHAPE away with it.
            //
            // Two things, and leaving the second out is what the user saw as "音乐卡片会闪一下". Closing the
            // panel withdraws the REQUEST, but the shape was still on the card: the pointer is over the bar
            // area and music is playing, so nothing in the policy pulled it back down. The card's owner is
            // then chosen afresh (a card is still wanted, the panel is no longer asking) and the answer is
            // the MEDIA card -- which appeared in the panel's place and stayed there for as long as the
            // pointer did.
            //
            // With the shape told to go back to rest, cardWanted is false from that moment on, so the owner
            // is KEPT and the panel is drawn all the way out (see NextCardOwner): the card retracts, and
            // nothing takes its place.
            // The panel is put away in nine places down this loop, and what that MEANS lives above RunBar
            // (see DismissPanelSurface). Those nine places are inside the phases now -- the two presses,
            // the outside-click dismissal, the panel's own controls -- so the local name they used to go
            // through (`dismissPanel`, a lambda over the same four arguments) has nothing left to serve.
            // The sliders follow the pointer while the button is held, and let go with it. The phase lives
            // above RunBar; its place in the sequence is unchanged.
            FollowSliderDrags(bar, systems, monitor, volumeAvailable, soundOutputStarted);

            // The wheel, for the two lists that scroll (the sound page and the Start picker). The phase
            // lives above RunBar; it touches nothing but BarState and the window.
            ApplyWheelScroll(bar, systems);

            // --- a press on a slider, which is where a drag BEGINS ---
            //
            // Handled before the clicks, because a press is the earlier event of the two and the value has to
            // be under the pointer from the moment the button goes down. The release that follows is then
            // only the END of the gesture: see the top of the click loop below.
            // A press AND a click, resolved together: the press is where a slider drag begins, the release
            // that follows is only the end of that gesture, and the click chain decides which control was
            // under the pointer. The phase lives above RunBar; the panel dismissal inside it is the same
            // four arguments the lambda over there used to pass.
            ApplyPressesAndClicks(bar, systems, active, resting, monitor, volumeAvailable, soundOutputStarted);

            // The right-click handling: the bar own tray menu, and unpinning from the Start card. The
            // phase lives above RunBar.
            ApplyRightClicks(bar, systems);

            // Held open for a photograph, if the diagnostic asked for it. Re-asserted AFTER the clicks above,
            // which is the only place either flag is cleared -- so the next frame draws the same level
            // whatever happened to the mouse in between.
            if (bar.panelLocked) {
                bar.systemPanelOpen = true;
                // The page only rides along when the diagnostic OPENED it at startup: a lock that also pinned
                // the page shut would make the page's own controls impossible to exercise by hand or by a
                // script, which is the opposite of what a diagnostic is for.
                if (bar.lockedSoundPage) bar.soundPageOpen = true;
            }

            // A press outside the bar puts the card away, and the diagnostic click-injection lands here too.
            // The phase lives above RunBar; the dismiss call inside it is the same four arguments the lambda
            // used.
            ApplyOutsideClickDismissal(bar, systems, active, resting);

            // The two diagnostic timers: the panel opening and closing on a schedule, and the Start button
            // being pressed on one. Both phases live above RunBar and both are inert unless the environment
            // asked for them.
            RunPanelTimerDiagnostic(autoPanelMs, autoPanelCloseMs, autoPanelCycleMs, diagnosticStart,
                                    forcedState, autoPanelOpened, autoPanelClosed, bar, systems, active,
                                    resting);
            RunStartTimerDiagnostic(autoStartMs, autoStartCycleMs, diagnosticStart, forcedState,
                                    lastStartTogglePhase);

            // --- the pointer flicking in and out of the island, when the environment asks for it ---
            //
            // ... and one play/pause on the media session, which is otherwise reachable only by clicking
            // the card: "点击暂停 歌曲停止了 但是这个差不多2秒左右才变播放按钮" could not be checked here at all
            // until this existed (a script's synthetic click goes to the REAL cursor's position).
            if (bar.autoTransportMs > 0 && !forcedState && !bar.transportForced) {
                const DWORD elapsed = static_cast<DWORD>(GetTickCount() - diagnosticStart);
                if (elapsed >= bar.autoTransportMs) {
                    bar.transportForced = true;
                    const bool asked = systems.music.RequestTogglePlayPause();
                    island::LogAndPrint("  diagnostic: play/pause %ls at %lu ms\n",
                                        asked ? L"requested" : L"REFUSED (no session to control)",
                                        static_cast<unsigned long>(elapsed));
                    std::fflush(stdout);
                }
            }
            //
            // A mouse crossing the island quickly leaves it and comes back within a few milliseconds, and
            // THAT is what interrupts a card's exit: the retarget is a pill state while the shape is still
            // card-sized, which is the one frame range where the animated box is card-sized with no card
            // up. See the pill's line rule in Renderer.cpp. The gesture cannot be performed from a script
            // here (its input is dropped before it reaches the bar), so this does it on a period.
            if (autoInsideFlickerMs > 0 && !forcedState) {
                const DWORD since = static_cast<DWORD>(GetTickCount() - diagnosticStart);
                const bool insideNow = ((since / autoInsideFlickerMs) % 2) == 0;
                if (insideNow != flickerInside) {
                    flickerInside = insideNow;
                    systems.window.SetForcedPointerInside(insideNow ? 1 : 0);
                    island::LogAndPrint("  diagnostic: the pointer flicks %ls at %lu ms\n",
                                        insideNow ? L"ONTO the island" : L"OFF the island", since);
                    std::fflush(stdout);
                }
            }

            // --- the deck-hover diagnostic, when the environment asks for it ---
            //
            // Turns on at a given moment rather than at startup: what it is for is the TRANSITION -- a card
            // that is already open being given up because the pointer moved off the pill -- and nothing is
            // open yet at startup.
            if (autoDeckHoverMs > 0 && !forcedState && !deckHoverForced &&
                GetTickCount() - diagnosticStart >= autoDeckHoverMs) {
                deckHoverForced = true;
                systems.window.SetForcedDeckHover(true);
                island::LogAndPrint("  diagnostic: the pointer is treated as ON a deck button at %lu ms\n",
                                    static_cast<unsigned long>(GetTickCount() - diagnosticStart));
                std::fflush(stdout);
            }

            // ... and the same for the CARD, which is the pill's territory: the card must survive the pointer
            // arriving on it, or its buttons cannot be pressed.
            if (autoCardHoverMs > 0 && !forcedState && !cardHoverForced &&
                GetTickCount() - diagnosticStart >= autoCardHoverMs) {
                cardHoverForced = true;
                systems.window.SetForcedPointerInside(autoCardMode);
                island::LogAndPrint("  diagnostic: the pointer is treated as %ls at %lu ms\n",
                                    autoCardMode == 4 ? L"in the GAP above the card" : L"ON the card",
                                    static_cast<unsigned long>(GetTickCount() - diagnosticStart));
                std::fflush(stdout);
            }

            // The pointer is polled, and the window's one-shot flags are drained. The phase lives above
            // RunBar; it needs nothing but the window, and what the pointer MEANS is decided below.
            PollPointerState(systems);

            // The shape the pointer asks for. The phase lives above RunBar; `resting` and `active` were
            // worked out earlier in this pass and are handed in with the session's own answer.
            ApplyPointerShapePolicy(bar, systems, active, resting, runtime.nowPlaying.present, forcedState);

            // --- advance the animation ---
            // The frame's own clock and the track position: both phases live above RunBar, in this order.
            const float dt = AdvanceAnimationClock(previous, frequency);
            UpdateTrackPosition(bar, runtime.nowPlaying, runtime.musicPositionAt, dt);

            // --- open the pointed-at icon button, over a few frames ----------------
            //
            // The width used to change in a single frame, and a width that jumps reads as a stutter
            // even when nothing is slow -- which is exactly how it was reported. Driven from the
            // frame's own delta, so it is smooth rather than tied to the frame rate.
            //
            // TWO amounts are animated, not one. `hover` is how far the pointed-at button has grown;
            // `shift` is how far the buttons before it have pushed it along. Animating both is what
            // makes the row open as ONE movement: growing in place leaves no room for the title and
            // is drawn over the neighbours, while shifting in a single frame shuffles the row.
            {
                // The island's own expansion already opens the whole row, so the pointed-at button is
                // left alone in that state: growing it on top would be two animations where one is
                // wanted.
                const bool islandOpen = (systems.window.State() == island::IslandState::Hover);
                const int pointed = islandOpen ? -1 : systems.window.HoveredIcon();

                bar.iconAnimating = false;
                for (int i = 0; i < island::kMaxTaskWindows; ++i) {
                    const float target = (i == pointed) ? 1.0f : 0.0f;
                    // A spring per button, not a linear ramp: see RowOpenStep for why, and for the
                    // sub-stepping that keeps it stable at this machine's worst frame times.
                    if (island::RowOpenStep(&bar.taskHoverAmount[i], &bar.taskHoverVelocity[i], target, dt)) {
                        bar.iconAnimating = true;
                    }
                }

                // Reported whenever the pointer's icon changes, and once when the animation starts or
                // stops.
                //
                // NOT on every change of `step`: the step is derived from the real frame time, so it
                // differs slightly every frame, and including it in the condition produced a line per
                // frame -- hundreds of lines in five seconds, burying everything else. A trace
                // condition has to be a value that is actually stable between the events being traced.
                if (pointed != bar.lastReportedPointed || bar.iconAnimating != bar.lastReportedAnimating) {
                    bar.lastReportedPointed = pointed;
                    bar.lastReportedAnimating = bar.iconAnimating;
                    // The step is gone (the row opens on a spring now, which has a velocity rather than a
                    // rate), so the trace reports the velocity of the first button instead -- the number
                    // that says whether the movement is settling or still swinging.
                    island::LogAndPrint("  anim: pointed=%d animating=%d velocity=%.3f hover0=%.2f",
                                        pointed, bar.iconAnimating ? 1 : 0, bar.taskHoverVelocity[0],
                                        bar.taskHoverAmount[0]);
                    std::fflush(stdout);
                }
            }

            // Clamp: a long stall (a debugger break, a sleep) must not teleport
            // the animation to its end, which would read as a jump cut.
            const bool animating = systems.window.Tick(dt > 0.1f ? 0.1f : dt);

            // The three reasons that come from a revision or a second rather than from a shape. The phase lives
            // above RunBar; what it last saw is BarRuntime's, and the three answers come back by reference.
            bool clockChanged = false;
            bool notificationChanged = false;
            bool musicChanged = false;
            PollRedrawReasons(runtime, systems, active, clockChanged, notificationChanged, musicChanged);

            // --- the lyric line for the resting strip ---
            //
            // The line scrolls, so the strip needs frames even when nothing else
            // changed -- but only while the strip is actually on screen. Scrolling a
            // shape nobody can see would keep the loop awake for nothing.
            const char* lyricLine = nullptr;
            const char* fallbackLine = nullptr;

            // ... and the line the NOTICE STRIP shows: who it is from, and what it says.
            //
            // Built here rather than in the renderer because it is a decision about the message (which field,
            // in which order, what to do when one of them is empty), and the renderer is handed finished
            // words. Held in a member of this function's scope so the pointer in the frame stays valid for
            // the frame's whole life.
            std::string noticeLine;
            if (active) {
                if (active->app[0] != '\0') {
                    noticeLine = active->app;
                    noticeLine += " \xC2\xB7 ";   // " · "
                }
                noticeLine += (active->title[0] != '\0') ? active->title : active->body;
            }
            bool lyricsScrolling = false;
            // How long the line on screen has been there, for its entrance. Kept past the end of the
            // entrance so a frame that changes nothing does not animate anything.
            bool lyricLineAnimating = false;
            bool lyricLineChanged = false;
            // While MUSIC RESTS -- not "while the strip is the state".
            //
            // The pill keeps showing the lyric when a card is up (see the pill's text rule in
            // Renderer.cpp), so the line has to be kept up to date then too. Keyed on the state, the
            // lyric vanished the moment the system button opened a panel, with the music still playing:
            // "我现在正在播放音乐 但是点击系统按钮的时候 灵动岛就不显示歌词了".
            PickRestingLyricLine(runtime, bar, systems, musicResting, kLyricScrollDipPerSec, dt, lyricLine,
                                 fallbackLine, lyricsScrolling, lyricLineAnimating, lyricLineChanged,
                                 musicWavePhase);

            // Does the card's printed position need to change, did a reading behind the strip move, is one of
            // the meters due -- and with all of that together, does ANYTHING ask for a frame this pass? The
            // phase lives above RunBar; the four answers the counters below read come back by reference, and
            // the ones only the decision itself needs stay inside it (the sound page's and the call meter's).
            bool progressChanged = false;
            bool weatherChanged = false;
            bool musicMeterDue = false;
            bool startHoverChanged = false;
            const bool needDraw = DecideNeedDraw(runtime, bar, systems, animating, clockChanged,
                                                 notificationChanged, musicChanged, lyricsScrolling,
                                                 lyricLineChanged, lyricLineAnimating, musicResting,
                                                 volumeAvailable, soundOutputStarted, haveDrawn,
                                                 progressChanged, weatherChanged, musicMeterDue,
                                                 startHoverChanged);
            if (needDraw) {
                // Which condition forced this redraw, counted per report interval.
                //
                // The frame INTERVAL only says the loop is running at 62 fps; that is a
                // symptom, not a cause. Without knowing which term keeps the condition true,
                // the only options are to guess or to keep optimising the draw -- and the draw
                // is already only 1.3 ms. This names the cause.
                if (animating) ++runtime.whyAnimating;
                if (clockChanged) ++runtime.whyClock;
                if (notificationChanged) ++runtime.whyNotification;
                if (musicChanged) ++runtime.whyMusic;
                // The scrolling, the arrival of a new line and the entrance itself are all "the
                // lyric asked for this frame" -- one counter, because that is the question it answers.
                if (lyricsScrolling || lyricLineChanged || lyricLineAnimating) ++runtime.whyLyrics;
                if (musicMeterDue) ++runtime.whyMusic;
                if (progressChanged) ++runtime.whyProgress;
                if (bar.statusStripAnimating || weatherChanged) ++runtime.whyStatusStrip;
                if (startHoverChanged) ++runtime.whyStartHover;
                if (bar.startWashAnimating) ++runtime.whyStartWash;
                if (!haveDrawn) ++runtime.whyFirst;

                haveDrawn = true;

                island::FrameState frame;

                // The system panel's flag and the card's position, set FIRST -- before the shape is
                // captured and before anything is composed.
                //
                // These were set at the BOTTOM of the loop, after `frame.pill.box` had already been
                // taken and after the card's box had been derived from it. The result was a card placed
                // from the OLD position while a second copy was placed from the new one: two panels, one
                // hanging from the button and one in the middle of the screen. Setting a field late is
                // the same defect as not setting it at all, and it looks like a rendering fault.
                // Whether an outer card is up OR ON ITS WAY, taken from the motion rather than from the
                // panel's flag. MUST BE COMPUTED BEFORE ANYTHING USES IT.
                //
                // It was first written beside the other frame fields, well AFTER the panel's content and
                // position had been derived from it -- so for the whole opening transition the flag was a
                // frame stale, `cardLeftDip` stayed zero, and the card was drawn as a 220-wide pill
                // growing in place instead of as a 520-wide panel sliding out from under its button.
                // Measured: `w=220` on every frame of the transition, with the tile count at zero.
                //
                // Setting a field late is the same defect as not setting it at all; this project has now
                // paid for that lesson four times, which is why this comment is this emphatic.
                frame.outerCardUp = systems.window.State() == island::IslandState::Expanded ||
                                    systems.window.FromState() == island::IslandState::Expanded;
                // WHICH LEVEL THE CARD SHOWS, decided here -- before anything reads it -- and held for the
                // card's whole life. It has to be this early in the loop for the same reason
                // `outerCardUp` is: a field set after the thing that reads it is a field one frame stale
                // (see the note below, and the four times this project has paid for that lesson).
                bar.panelLevel = island::NextPanelLevel(bar.panelLevel, frame.outerCardUp, bar.systemPanelOpen,
                                                    bar.trayPageOpen, bar.soundPageOpen, bar.startPageOpen);
                const bool trayPageShown = bar.panelLevel == island::PanelLevel::Tray;
                const bool soundPageShown = bar.panelLevel == island::PanelLevel::Sound;
                const bool startPageShown = bar.panelLevel == island::PanelLevel::Start;
                // ... and whether the bar is in the picture at all: with a full-screen application in front
                // only the island is drawn (see FrameState::fullscreenApp).
                frame.fullscreenApp = bar.fullscreenAppInFront;

                // The tray's own cadence: re-read what the shell is holding, harvest what the module saw, and
                // start a learning pass only when the shell itself was replaced. The phase lives above RunBar.
                TickTray(bar, systems, frameWork, kWorkTrayRefresh, kTrayRefreshMs);
                // The tray and the panel as the frame sees them. The phase lives above RunBar; the order
                // against the Start card below is unchanged.
                FillTrayAndPanelFrame(runtime, frame, bar, systems, animating, dt, forcedStartHover,
                                      trayPageShown);

                // The Start card as the frame and the window see it. The phase lives above RunBar; the order
                // against the card-owner latch below is unchanged.
                FillStartCardFrame(frame, bar, systems, startPageShown, dt);

                // Who owns the card, and which message it is showing. The phase lives above RunBar; the two
                // things it carries between frames are BarRuntime's.
                UpdateCardOwner(runtime, frame, bar, systems, active, runtime.nowPlaying.present);
                frame.systemButtonHovered = systems.window.SystemButtonHovered();
                frame.systemPanel.tileCount = 0;
                frame.cardLeftDip = 0.0f;

                // The panel's CONTENT is built whenever a card is up OR ON ITS WAY -- not only while the
                // panel flag is set.
                //
                // Building it only while the flag was set meant that the moment the panel was asked to
                // close, the tiles and the slider vanished and the close animation played on an EMPTY
                // card: the panel's contents disappeared first and a blank rounded rectangle shrank
                // afterwards. Reported as the panel "appearing and disappearing" with the pointer.
                // The panel own content, level by level. The phase lives above RunBar; its place in the
                // sequence is unchanged.
                FillPanelFrame(runtime, frame, bar, systems, soundPageShown, trayPageShown, startPageShown, dt);

                // Given to the window here as well, because the click handler reads it to work out which
                // tile was pressed -- and the click handler runs near the TOP of the loop, before the
                // renderer's own bookkeeping.
                systems.window.SetSystemPanel(frame.systemPanel, frame.cardLeftDip);

                frame.pill.box = systems.window.CurrentBox();
                frame.state = systems.window.State();

                // The pill's own box, which is NOT the animated one while a card is opening or closing.
                //
                // `CurrentBox` is the shape in flight: it is card-sized for the whole of a transition to
                // or from the card, and the pill's fill drew that rectangle in the pill's near-opaque
                // black -- a dark band under the island for 200 ms per transition. The resting box is
                // derived from the anchor, so it is the pill regardless of what is animating.
                // ... AND IT IS THE NOTICE STRIP WHILE A CALL OR A MESSAGE IS BEING ANNOUNCED -- in every
                // state, including while a card is open.
                //
                // The note above ("the resting box is the pill regardless of what is animating") was true, and
                // it is exactly what made a call strip collapse to the pill's width the moment a card was
                // opened: everything drawn from this box -- the bar's own capsule, the deck, the icon row, and
                // the line inside it -- was measured from a 220 DIP pill while the bar was announcing. The same
                // answer the window is given below (SetNoticeStripUp), from the same frame.
                frame.restingPill =
                    island::BoxForState(active != nullptr &&
                                                !IsCallNotification(systems, active)
                                            ? island::IslandState::NoticeStrip
                                            : island::IslandState::Collapsed,
                                        systems.window.CurrentAnchor());
                // ... and WHAT the bar shows at rest, which is what the pill's own line is chosen by
                // (see FrameState::restingState). A card does not change this, which is the point.
                frame.restingState = resting;

                // ... and WHERE THE POINTER IS ACROSS IT, which the liquid-glass sweep follows. Taken from
                // the pointer the window last saw, in window-local DIP, and expressed as a fraction of the
                // resting pill's width; the renderer clamps it (SpecularCentreX), so a pointer off the pill
                // leaves the highlight on the glass rather than pushing it away.
                {
                    float pointerX = 0.0f;
                    float pointerY = 0.0f;
                    systems.window.ToLocalDip(systems.window.LastPointer(), &pointerX, &pointerY);
                    const float left = frame.restingPill.rect.left;
                    const float right = frame.restingPill.rect.right;
                    frame.pillPointerFraction = (right > left) ? (pointerX - left) / (right - left) : 0.5f;
                    frame.pointerOnIsland = systems.window.IsPointerInside();
                }

                if (GetTickCount64() - bar.lastTaskRefresh >= 500) {
                    bar.lastTaskRefresh = GetTickCount64();
                    systems.taskWindows.Refresh();
                }

                // Trimmed to what fits before the row would leave the monitor, so the count the
                // renderer draws is the count the click handler will hit-test.
                // Trimmed to what fits before the row would leave the monitor, so the count the
                // renderer draws is the count the click handler will hit-test.
                //
                // The indicators are NOT subtracted here any more: they live on the LEFT deck,
                // which is bounded by the island's left edge and cannot be reached by the icon row
                // whatever its length. Subtracting them would cost icons for space that the icons
                // cannot use.
                const float monitorWidthDip =
                    static_cast<float>(systems.window.Monitor().screen.right -
                                       systems.window.Monitor().screen.left) / systems.window.Scale();
                // ... AND THE TRAY BUTTON'S ROOM IS SPENT FIRST, WHEN THERE IS ONE.
                //
                // The button stands between the island and the row, so the application row is what has to
                // give way: the row is sized for the room that is left once the button has taken its
                // 26 DIP. Without this the two overlap on a machine with many windows open -- and an
                // overlap of two live controls is not a cosmetic problem, it is two things claiming the
                // same pixels and the same clicks.
                //
                // ... AND ONLY WHEN THERE IS ONE, which is what the button's own geometry says
                // (RowAnchorWithTray): with no tray icons running the row is drawn from the island
                // itself, so reserving room for a button that is not on screen is an icon the user
                // does not get. This read the count into `trayShown` and then subtracted the room
                // regardless -- the compiler had been pointing at the unused variable for a while.
                const int trayShown =
                    systems.trayIcons.Count() < island::kMaxTrayIcons ? systems.trayIcons.Count()
                                                              : island::kMaxTrayIcons;
                const float rowWidthDip =
                    trayShown > 0 ? monitorWidthDip - island::TrayButtonReservedDip()
                                  : monitorWidthDip;
                int listed = island::TaskIconsThatFit(rowWidthDip, systems.taskWindows.Count());

                // EACH ICON'S ARRIVAL, and the row as the UNION of what is listed and what is leaving.
                //
                // The membership used to change in a single frame: a window opened and its icon was
                // simply THERE, a window closed and it was simply gone. Reported as "灵动岛右侧 图标出现和
                // 消失有点太突兀了没有动画". So every window the row has shown keeps an amount that eases
                // towards 1 while it is listed and towards 0 while it is not (TaskIconAppearStep), and the
                // frame is given the listed windows first and the departing ones after them -- a departing
                // icon is drawn, fading and shrinking, for the length of its exit.
                //
                // The order of the LISTED ones is untouched: the departing ones are appended, so nothing
                // that is staying moves because something else left.
                const float rowDt = static_cast<float>(dt > 0.1 ? 0.1 : dt);
                for (int i = 0; i < bar.rowSlotCount;) {
                    bool listedNow = false;
                    for (int j = 0; j < listed; ++j) {
                        if (systems.taskWindows.At(j).handle == bar.rowSlots[i].handle) {
                            listedNow = true;
                            break;
                        }
                    }
                    bar.rowSlots[i].appear =
                        island::TaskIconAppearStep(bar.rowSlots[i].appear, listedNow, rowDt);
                    if (!listedNow && bar.rowSlots[i].appear <= 0.0f) {
                        bar.rowSlots[i] = bar.rowSlots[bar.rowSlotCount - 1];   // swap the last one in
                        --bar.rowSlotCount;
                        continue;
                    }
                    ++i;
                }
                for (int j = 0; j < listed && bar.rowSlotCount < island::FrameState::kMaxTaskButtons; ++j) {
                    const HWND handle = systems.taskWindows.At(j).handle;
                    bool tracked = false;
                    for (int i = 0; i < bar.rowSlotCount; ++i) {
                        if (bar.rowSlots[i].handle == handle) {
                            tracked = true;
                            break;
                        }
                    }
                    if (!tracked) {
                        bar.rowSlots[bar.rowSlotCount].handle = handle;
                        bar.rowSlots[bar.rowSlotCount].appear = 0.0f;   // arrives from nothing
                        ++bar.rowSlotCount;
                    }
                }

                int drawn = 0;
                const auto appearanceOf = [&](HWND handle) {
                    for (int i = 0; i < bar.rowSlotCount; ++i) {
                        if (bar.rowSlots[i].handle == handle) return bar.rowSlots[i].appear;
                    }
                    return 1.0f;
                };
                for (int j = 0; j < listed && drawn < island::FrameState::kMaxTaskButtons; ++j) {
                    const island::TaskEntry& entry = systems.taskWindows.At(j);
                    // Copied into a persistent buffer: the frame is consumed after this loop
                    // body returns, and the list can refresh in between, so pointing at the
                    // entry's own title would be a dangling read.
                    wcsncpy_s(bar.taskTitles[drawn], island::kTaskTitleCapacity, entry.title,
                              _TRUNCATE);
                    frame.taskButtons[drawn].title = bar.taskTitles[drawn];
                    frame.taskButtons[drawn].active =
                        (entry.handle == systems.taskWindows.ForegroundHandle());
                    frame.taskButtons[drawn].minimised = entry.iconic;
                    // The icon is resolved by the RENDERER, which owns the GPU bitmaps and
                    // caches one per window. The caller only names the window.
                    frame.taskButtonWindow[drawn] = entry.handle;
                    bar.taskHandles[drawn] = entry.handle;
                    frame.taskButtonAppear[drawn] = appearanceOf(entry.handle);
                    ++drawn;
                }
                // ... and the ones on their way out, which have no title to show any more (their window is
                // usually gone) -- only the icon, which the renderer still has cached.
                for (int i = 0; i < bar.rowSlotCount && drawn < island::FrameState::kMaxTaskButtons; ++i) {
                    if (bar.rowSlots[i].appear <= 0.0f) continue;
                    bool stillListed = false;
                    for (int j = 0; j < listed; ++j) {
                        if (systems.taskWindows.At(j).handle == bar.rowSlots[i].handle) {
                            stillListed = true;
                            break;
                        }
                    }
                    if (stillListed) continue;
                    frame.taskButtons[drawn].title = nullptr;
                    frame.taskButtons[drawn].active = false;
                    frame.taskButtons[drawn].minimised = false;
                    frame.taskButtonWindow[drawn] = bar.rowSlots[i].handle;
                    bar.taskHandles[drawn] = bar.rowSlots[i].handle;
                    frame.taskButtonAppear[drawn] = bar.rowSlots[i].appear;
                    ++drawn;
                }

                frame.taskButtonCount = drawn;
                frame.taskButtonAppearValid = true;
                // Whether any icon is still on its way in or out, which is what asks for the next frame.
                // Read by needDraw on the NEXT pass, because the row is updated inside the draw.
                bar.rowAppearAnimating = false;
                for (int i = 0; i < drawn; ++i) {
                    if (frame.taskButtonAppear[i] < 0.999f) bar.rowAppearAnimating = true;
                }
                // The count the WINDOW is told is the count that is DRAWN, because the row's geometry is
                // laid out from it: a departing icon occupies a slot for the length of its exit, and a hit
                // test against a different count would point at the wrong slot for 0.2 s.
                bar.taskWindowsShown = drawn;

                // Told to the window so it can work out whether the pointer is on an icon, and
                // which one. The window owns the hit test; the frame loop owns the list, and this
                // is the handover between them.
                systems.window.SetTaskIconCount(drawn);

                // The system READINGS, sampled on a timer.
                //
                // Most of them come from APIs that open a COM object or walk a device tree, and the bar
                // draws at the compositor's rate, so they are sampled on a clock rather than per frame. A
                // value that cannot be read keeps its previous meaning, which is why the reader is handed
                // the struct instead of filling a fresh one.
                //
                // They used to be turned into a row of indicators on the left deck as well. That row is
                // gone -- the deck holds the Start and system buttons now -- but the readings themselves
                // are what the system panel is built from, so they stay.
                {
                    // Named apart from the frame loop's own `nowMs`: the two are the same instant read twice,
                    // and shadowing the outer one was a warning, not a bug -- but a reader should not have to
                    // work out which clock a line is on.
                    const unsigned long long statusNowMs = GetTickCount64();
                    if (statusNowMs - lastStatusSampleMs >= kStatusSampleMs) {
                        lastStatusSampleMs = statusNowMs;
                        frameWork |= kWorkStatusSample;
                        systems.statusReader.Sample(&systems.systemStatus);

                        // The brightness, on the same clock. It is a DDC/CI read -- an I2C transaction over
                        // the video cable -- which is why it is not read per frame, and why the panel's row is
                        // only offered when the display actually answers: a monitor with DDC/CI switched off
                        // reports nothing, and a row drawn at a made-up level would be a lie about the screen.
                        {
                            int percent = 0;
                            const bool read = island::ReadBrightnessPercent(monitor.monitor, &percent);
                            if (read) {
                                bar.brightnessPercent = percent;
                                if (!bar.brightnessAvailable) {
                                    bar.brightnessAvailable = true;
                                    island::LogAndPrint(
                                        "  brightness: the panel gets a brightness row (now %d%%)\n",
                                        percent);
                                }
                                if (bar.lastAppliedBrightnessPercent < 0) {
                                    bar.lastAppliedBrightnessPercent = percent;
                                    bar.lastLoggedBrightnessPercent = percent;
                                }
                            } else if (bar.brightnessAvailable) {
                                // It answered before and does not now: the monitor was switched off, moved to
                                // another input, or DDC/CI was turned off in its menu. The row goes away
                                // rather than staying at the last number it saw.
                                bar.brightnessAvailable = false;
                                island::LogAndPrint(
                                    "  brightness: the display stopped answering; the row is gone\n");
                            }
                        }
                    }
                }

                // The two worker switches report here, once each: the work is done on a thread and this is
                // the only place the answer surfaces. A finished Bluetooth switch also re-samples the status,
                // because the tile's caption is the machine's reading and waiting two seconds for it would
                // leave the tile showing the state the press just changed.
                {
                    bool wifiNow = false;
                    bool bluetoothNow = false;
                    bool wifiAsked = false;
                    bool bluetoothAsked = false;
                    bool succeeded = false;
                    long hresult = 0;
                    if (island::TakeRadioOutcome(&wifiNow, &bluetoothNow, &wifiAsked, &bluetoothAsked,
                                                 &succeeded, &hresult)) {
                        // Only the radios the request NAMED are reported: a Wi-Fi-only press that printed
                        // Bluetooth's state as well read as if the job had touched it.
                        const char* verdict = succeeded ? "as asked" : "NOT as asked";
                        const char* refused = hresult ? " (the radio refused the call)" : "";
                        if (wifiAsked && bluetoothAsked) {
                            island::LogAndPrint("  radios: Wi-Fi %s, Bluetooth %s -- %s%s\n",
                                                wifiNow ? "on" : "off", bluetoothNow ? "on" : "off",
                                                verdict, refused);
                        } else if (wifiAsked) {
                            island::LogAndPrint("  radios: Wi-Fi %s -- %s%s\n", wifiNow ? "on" : "off",
                                                verdict, refused);
                        } else {
                            island::LogAndPrint("  radios: Bluetooth %s -- %s%s\n",
                                                bluetoothNow ? "on" : "off", verdict, refused);
                        }
                        systems.statusReader.Sample(&systems.systemStatus);
                        lastStatusSampleMs = GetTickCount64();
                        std::fflush(stdout);
                    }
                    bool on = false;
                    bool stateAfter = false;
                    if (island::TakeNightLightOutcome(&on, &stateAfter, &succeeded)) {
                        island::LogAndPrint(
                            "  night light: asked for %s; it is now %s (%s)\n",
                            on ? "on" : "off", stateAfter ? "on" : "off",
                            stateAfter == on ? "it took" : "IT DID NOT CHANGE");
                        std::fflush(stdout);
                    }
                }

                // The ONE button to widen. Read back from the window, which computed it from the
                // pointer's position using the geometry that is actually on screen.
                frame.taskHoveredIndex = systems.window.HoveredIcon();
                frame.startHovered = systems.window.StartHovered() || forcedStartHover;

                // The hover preview (and the notice line that shares this stretch). The phase lives above
                // RunBar; the drawn count it needs is the one the row was laid out with, above.
                FillHoverPreview(runtime, frame, bar, systems, runtime.nowPlaying, previewReady, drawn, listed, dt,
                                 musicWavePhase, runtime.waveAccent, lyricLine, fallbackLine, noticeLine, frameWork,
                                 kWorkThumbnail, kThumbnailDwellMs, traceCardFrames);

                // The CALL strip and the date/weather strip. The phase lives above RunBar; its place in the
                // sequence is unchanged.
                FillCallAndStatusStrip(frame, bar, systems, active, runtime.lyricsScrollDip, dt);

                // The card's printed time and the fill of its bar, from the ONE playhead at the top of
                // this iteration -- the same number the pill's lyric line is chosen by.
                //
                // The bar needs a TOTAL, and a player that sends no timeline has none: NetEase Cloud
                // Music reports `EndTime 00:00`, so the bar.track is drawn (the renderer always draws it) but
                // NEVER filled, and the total reads `--:--` rather than a `00:00` the player never said.
                // The elapsed time still moves, because the bar is counting it -- before this, both ends
                // showed `00:00` and nothing moved at all ("进度条也不动").
                if (bar.activePresent) {
                    // The TOTAL: the session's, or -- when it publishes none, which is NetEase Cloud
                    // Music's case -- the catalogue's, which the lyric provider fetched together with the
                    // words (see Lyrics::trackDurationMs). Now the bar has a scale to fill against and the
                    // card prints a real length instead of `--:--`.
                    const long long totalMs =
                        runtime.nowPlaying.durationMs > 0
                            ? runtime.nowPlaying.durationMs
                            : (runtime.currentLyrics ? runtime.currentLyrics->trackDurationMs : 0);
                    frame.musicPositionMs = bar.track.ms;
                    frame.musicDurationMs = totalMs;
                    if (totalMs > 0) {
                        const double fraction = static_cast<double>(bar.track.ms) /
                                                static_cast<double>(totalMs);
                        frame.progressFraction = static_cast<float>(fraction > 1.0 ? 1.0 : fraction);
                    }
                }

                // The chased master volume, which the SYSTEM PANEL's slider is drawn at (see bar.shownVolume).
                //
                // It used to feed the music card's own bar as well. The card has no volume control any more --
                // the speaker in its top-right corner was removed on request -- but the chase stays, because it
                // is what makes the panel's handle MOVE to a new level instead of appearing there, and that is
                // the fix for "调节声音的还是卡卡的，没有动画过渡".
                if (volumeAvailable) {
                    float level = 0.0f;
                    if (systems.volume.Get(&level)) {
                        // A drag is drawn at the pointer's own value (see the panel's volume block), so the
                        // chase never lags the finger.
                        if (bar.shownVolume < 0.0f) {
                            bar.shownVolume = level;
                        } else {
                            bar.shownVolume += (level - bar.shownVolume) *
                                           (1.0f - std::exp(-dt / kVolumeChaseSeconds));
                            if (std::fabs(level - bar.shownVolume) < 0.002f) bar.shownVolume = level;
                        }
                    }
                }

                // The album art, the wash built from it, and the source application's icon. The phase
                // lives above RunBar; its place in the sequence is unchanged.
                FillDecodedPictures(runtime, frame, runtime.nowPlaying);

                systems.renderer.Render(frame);
                frameWork |= kWorkDraw;

                // What the page looked like, recorded AFTER the draw: these are the values the next
                // iteration's redraw decision compares against, so recording them before the draw would
                // swallow the very change that asked for it.
                runtime.lastSoundPageOpen = bar.soundPageOpen;
                runtime.lastSoundScroll = systems.window.SoundPageScroll();
            }

            // Log the settled shape once per change. Without this, "the card never
            // appears" is indistinguishable from "the pointer was never detected", and
            // the two need completely different fixes -- it took adding exactly this line
            // to find that a settled hover never promoted to the card.
            if (systems.window.State() != lastLogged && !systems.window.IsAnimating()) {
                lastLogged = systems.window.State();
                island::LogAndPrint("  state: %ls (pointer inside=%d, resting=%ls)\n",
                                    island::StateName(lastLogged),
                                    systems.window.IsPointerInside() ? 1 : 0,
                                    island::StateName(resting));
                std::fflush(stdout);
            }

            // Report the frame-interval tail and what forced each redraw, LIVE, every 4 s.
            //
            // Kept in the shipping path on purpose. It says three things nothing else does:
            // whether the loop hiccups (the worst interval and the >50 ms bucket), which term
            // keeps the draw condition true, and how many frames were drawn. An earlier round
            // optimised the DRAW cost for two attempts while this kind of data showed the draw
            // was 1.3 ms and the loop was not hitching at all -- measuring the wrong quantity
            // is how a fix gets reported that changes nothing. A stutter is a maximum, so the
            // worst value matters more than the average.
            // A SLOW FRAME SAYS WHAT IT WAS DOING, and that is the point of the bitfield.
            //
            // The interval this measures is the gap since the previous pass, so the work it describes is
            // the work of the pass that just ENDED -- `frameWork` is copied into `lastWork` and cleared at
            // the bottom of the loop for exactly that reason. Without the flags the line would only say
            // "something took 90 ms", and the last three rounds of guessing which periodic task it was
            // would have been three rounds of guessing.
            // ... and whether that pass drew, which is what separates a slow frame from a sleeping loop.
            const double intervalMs = runtime.liveStats.Record((lastWork & kWorkDraw) != 0);
            if (intervalMs >= island::FrameStats::kSlowFrameMs) {
                char work[192] = {};
                auto add = [&work](const char* name) {
                    if (work[0]) strcat_s(work, sizeof(work), " ");
                    strcat_s(work, sizeof(work), name);
                };
                if (lastWork & kWorkStatusSample) add("status-sample");
                if (lastWork & kWorkTrayRefresh) add("tray-refresh");
                if (lastWork & kWorkToastPoll) add("toast-poll");
                if (lastWork & kWorkWeChatScan) add("wechat-scan");
                if (lastWork & kWorkWeChatSound) add("wechat-sound");
                if (lastWork & kWorkTaskbarCheck) add("taskbar-check");
                if (lastWork & kWorkThumbnail) add("thumbnail");
                if (lastWork & kWorkDraw) add("draw");
                island::LogAndPrint("  slow frame: %.1f ms -- work: %s%s\n", intervalMs,
                                    work[0] ? work : "none logged",
                                    (lastWork & kWorkDraw) ? "" : " (no draw: the loop was waiting)");
            }
            // The memory line: the five-minute cadence plus one shortly after a suspected grower. The phase
            // lives above RunBar; the cadence state it keeps is carried across frames through references.
            ReportMemoryLine(runtime, systems, kMemorySampleMs, runtime.nowPlaying, runtime.currentLyrics);

            // The two periodic reports: what the frames did, and whether the loop paced itself. The phase
            // lives above RunBar; its cross-frame state is BarRuntime's.
            ReportIntervalAndThrottle(runtime, systems, pacingPath);

            // The wait that paces the loop. The phase lives above RunBar and it is the last thing a pass
            // does; the work flags below are the loop own bookkeeping and stay here.
            PaceTheLoop(systems, clockDriven, runtime);

            // What this pass did, for the slow-frame line above: the interval it will be measured against
            // is the gap from the END of this pass to the next, so these flags describe it.
            lastWork = frameWork;
            frameWork = 0;
        }
    }

    systems.renderer.SetTextRenderer(nullptr);
    systems.text.Shutdown();
    // Stop the listener before tearing the window down: a client mid-send should be
    // finished with, not left holding a pipe that is about to disappear.
    systems.notificationServer.Stop();
    systems.lyrics.Stop();
    systems.weather.Stop();
    systems.volume.Stop();
    systems.playerWindow.Stop();
    systems.music.Stop();

    // Give the reserved strip back while the window still exists.
    //
    // The destructor would do this, but releasing here makes the order explicit and puts the
    // result in the log: a work area left inset after the program exits is a permanent gap on
    // the desktop that looks like a broken system rather than a program that failed to clean up.
    // Doing it before window.Destroy() means the shell still has a live HWND to talk to.
    if (systems.appBar.IsRegistered()) {
        systems.appBar.Release();
    }

    systems.window.Destroy();

    // Put the shell's taskbar back before exiting. This runs on every normal exit
    // path -- the window closing, WM_QUIT, or the loop breaking for any other
    // reason. Only a hard kill skips it, which is why the recovery command is
    // documented in the startup message.
    // Un-gated for the same reason the check above is: a session whose taskbar appeared late is a session
    // where this object owns a hide that the flag never saw. `Restore()` is a no-op when nothing was
    // hidden, so the wider condition costs nothing.
    if (hideNativeTaskbar) {
        systems.taskbar.Restore();
        island::LogAndPrint("  native taskbar: restored\n");
        // The work area went back with it, inside Restore() and BEFORE the taskbar was shown: showing
        // the taskbar makes the shell re-derive the work area from it, so this order leaves one answer,
        // not two. Its own line is written there.
        std::fflush(stdout);
    }

    // Its handles, on the same path. Closing the WLAN client here rather than leaving it to process
    // teardown matters because the shell's wireless service keeps a per-client context, and a bar
    // that is started and stopped repeatedly would accumulate them.
    systems.statusReader.Close();
    return 0;
}

