// =====================================================================
//  WeChatWatch.cpp
// =====================================================================
#include "services/WeChatWatch.h"

#include <windows.h>
#include <tlhelp32.h>   // CreateToolhelp32Snapshot: every process's name, in one call

#include <algorithm>
#include <cmath>     // fabs: how far a window is from the middle of the screen
#include <cstring>   // memcpy: the fixed-capacity fields of a Notification
#include <string>
#include <vector>

#include "core/Log.h"

namespace island {
namespace {

// One announcement per this long, whatever the popup does. A burst of messages re-creates the popup, and
// "有新消息" is the same news three times.
constexpr unsigned long long kAnnounceCooldownMs = 5000;

// Widest and tallest a toast is allowed to be, and the smallest that is still a toast. Measured on this
// machine: WeChat's main window is 400x700 and up, its message-only windows are 0x0, and its call window is
// 376x648 -- all well outside this band, while a notification popup is a few hundred DIP across.
constexpr int kToastMinWidth = 140;
constexpr int kToastMaxWidth = 720;
constexpr int kToastMinHeight = 48;
constexpr int kToastMaxHeight = 340;

struct WindowVisit {
    std::vector<long long> handles;
    std::vector<std::wstring> classes;
    std::vector<std::string> pids;
    std::vector<std::wstring> titles;
    std::vector<unsigned long> processes;
    std::vector<bool> visible;
    std::vector<int> left, top, width, height;
    std::vector<std::wstring> verdicts;
};

// The name test, in one place: the public IsWeChatProcess below and the window scan both use it.
//
// Asked of the executable rather than of the process name, because the new client is "Weixin.exe" while the
// old one is "WeChat.exe" and both are the same thing to a user. The PREFIX is matched, not the whole name:
// this machine runs a dozen WeChatAppEx.exe helpers alongside the client, and a notification raised by one of
// them would be raised by "not WeChat" under an exact match.
// The prefix test on a bare file name: "Weixin.exe" and "WeChat.exe" are the same thing to a user, and
// the helpers (WeChatAppEx.exe) count too.
bool IsWeChatName(const wchar_t* name) {
    if (!name) return false;
    const wchar_t* leaf = std::wcsrchr(name, L'\\');
    leaf = leaf ? leaf + 1 : name;
    return _wcsnicmp(leaf, L"Weixin", 6) == 0 || _wcsnicmp(leaf, L"WeChat", 6) == 0;
}

// EVERY PROCESS'S NAME, IN ONE CALL, kept for a second.
//
// WHY THIS EXISTS, with the measurement (2026-10-18): the walks in this file ask "is this pid WeChat?"
// once per top-level window, and each answer used to be an OpenProcess plus QueryFullProcessImageNameW.
// This machine has ~343 top-level windows, so the sound channel spent ~40 ms per ask every 200 ms and
// the window channel ~40 ms every 500 ms -- ON THE FRAME LOOP, as `slow frame: 40.8 ms -- work:
// wechat-sound`. A Toolhelp snapshot names every process in a single call of about a millisecond, and
// whether a pid belongs to WeChat cannot change faster than a second, so the table is kept that long.
struct ProcessNames {
    std::vector<unsigned long> pids;
    std::vector<bool> wechat;
    ULONGLONG builtAt = 0;
};

const ProcessNames& NamesNow() {
    static ProcessNames table;
    const ULONGLONG now = GetTickCount64();
    if (table.builtAt != 0 && now - table.builtAt < 1000) return table;

    table.pids.clear();
    table.wechat.clear();
    table.builtAt = now;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return table;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            table.pids.push_back(entry.th32ProcessID);
            table.wechat.push_back(IsWeChatName(entry.szExeFile));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return table;
}

// The slow, always-correct answer: ask the process itself. Used for a pid that started after the table
// was built, which is the one case the table cannot answer.
bool AskTheProcess(unsigned long pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[260] = {};
    DWORD length = 260;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != 0;
    CloseHandle(process);
    if (!ok) return false;
    return IsWeChatName(path);
}

bool LooksLikeWeChatProcess(unsigned long pid) {
    const ProcessNames& table = NamesNow();
    for (size_t i = 0; i < table.pids.size(); ++i) {
        if (table.pids[i] == pid) return table.wechat[i];
    }
    return AskTheProcess(pid);
}

// The windows of every WeChat process, top level AND their children.
//
// Children because a self-drawn UI may hang its popup off the main window instead of creating a top-level
// one, and a watch that looked only at the desktop's own list would never see it.
BOOL CALLBACK CollectWindow(HWND window, LPARAM param) {
    auto* visit = reinterpret_cast<WindowVisit*>(param);

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0 || !LooksLikeWeChatProcess(pid)) return TRUE;

    wchar_t className[256] = {};
    GetClassNameW(window, className, 256);
    wchar_t title[256] = {};
    GetWindowTextW(window, title, 256);

    RECT rect = {};
    GetWindowRect(window, &rect);

    visit->handles.push_back(static_cast<long long>(reinterpret_cast<INT_PTR>(window)));
    visit->classes.push_back(className);
    visit->titles.push_back(title);
    visit->processes.push_back(pid);
    visit->visible.push_back(IsWindowVisible(window) != 0);
    visit->left.push_back(rect.left);
    visit->top.push_back(rect.top);
    visit->width.push_back(rect.right - rect.left);
    visit->height.push_back(rect.bottom - rect.top);
    return TRUE;
}

}  // namespace

bool IsWeChatProcess(unsigned long pid) {
    return LooksLikeWeChatProcess(pid);
}

bool AnyWeChatProcessRunning() {
    // ASKED OF THE PROCESS TABLE, not of the window list.
    //
    // It used to walk every top-level window and ask each one's process, which cost ~16 ms here (438
    // windows) and ran five times a second from the sound channel: 80 ms of every second on the frame
    // loop, and a dropped frame each time. The table NamesNow() keeps answers the same question in a
    // few microseconds, and the question is the one that matters for a SOUND: a WeChat that makes noise
    // is a WeChat process, whether or not it has a window on screen. `IsWeChatProcess(session.pid)` on
    // the audio session is still what decides which sound is WeChat's.
    const ProcessNames& table = NamesNow();
    for (size_t i = 0; i < table.wechat.size(); ++i) {
        if (table.wechat[i]) return true;
    }
    return false;
}

bool LooksLikeWeChatToast(const wchar_t* className, const wchar_t* title, bool visible, int width,
                          int height) {
    if (!visible) return false;
    if (width < kToastMinWidth || width > kToastMaxWidth) return false;
    if (height < kToastMinHeight || height > kToastMaxHeight) return false;

    // The classes that are known NOT to be it. The tray icon's message window is nominally full-screen and
    // hidden, so the size and visibility rules already exclude it -- it is named here so that the rule does
    // not silently depend on that.
    if (className) {
        if (std::wcsstr(className, L"TrayIconMessageWindow") != nullptr) return false;
        if (std::wcsstr(className, L"Base_PowerMessageWindow") != nullptr) return false;
        if (std::wcsstr(className, L"Chrome_") != nullptr) return false;
        if (std::wcsstr(className, L"IME") != nullptr) return false;
        if (std::wcsstr(className, L"SystemMessageWindow") != nullptr) return false;
    }

    // NOTHING here judges the TITLE, and that is a correction rather than an omission.
    //
    // The first version excluded the two windows the client is known by -- titles "微信" and "Weixin" -- and
    // the log then showed why that was a mistake: the client owns TWO windows with those two titles, and they
    // are not the same window. The main window is 微信, parked off-screen at (-32000,-32000) and 160x28. The
    // other is WEIXIN, 176x199, hidden, sitting dead centre -- the size and shape of a compact new-message
    // card, and exactly the candidate this watcher exists to notice. A rule that excluded "Weixin" would have
    // excluded the very window it is looking for.
    //
    // `title` is still a parameter because the log prints it: the title is how a person recognises which
    // window the rule was looking at, and it is the first thing to check when this misfires.
    (void)title;
    return true;
}

bool LooksLikeWeChatCall(const wchar_t* title, bool visible) {
    if (!visible) return false;
    if (!title || title[0] == L'\0') return false;
    // The client's own title for this window. Matched as a substring rather than as an equal string, because
    // the trailing space in `微信音视频通话 ` is the client's choice and not a contract.
    return std::wcsstr(title, L"音视频通话") != nullptr;
}

bool ShouldAnnounceWeChatSound(bool wechatRunning, bool wasActive, bool isActive, int wasSessions,
                               int sessions, bool primed, unsigned long long sinceLastAnnounceMs) {
    if (!wechatRunning) return false;
    // The FIRST read is a baseline, not an event. Without this, starting the bar while WeChat already holds a
    // session would announce a message that never arrived -- the classic way an edge detector misfires.
    if (!primed) return false;
    // Two shapes of the same rise, because the client may either keep a silent session open (state goes
    // Inactive -> Active) or create one only when it has something to play (count goes up). Either one is a
    // sound starting.
    const bool becameActive = !wasActive && isActive;
    const bool newSession = sessions > wasSessions;
    if (!becameActive && !newSession) return false;
    // A sound that restarts within the cooldown is the same sound: a notification's "ding" is one event even
    // when the session state flickers while it plays.
    if (sinceLastAnnounceMs < kAnnounceCooldownMs) return false;
    return true;
}

int WeChatWatcher::PollSound(NotificationStore* store, bool wechatRunning, bool sessionActive,
                             int sessionCount, unsigned long long nowMs) {
    if (!store) return 0;
    const bool announce =
        ShouldAnnounceWeChatSound(wechatRunning, wechatAudioWasActive_, sessionActive,
                                  wechatAudioSessions_, sessionCount, wechatAudioPrimed_,
                                  nowMs - lastSoundAnnounceMs_);
    if (!wechatAudioPrimed_ || sessionActive != wechatAudioWasActive_ ||
        sessionCount != wechatAudioSessions_) {
        // Every change is logged, including the baseline: "the session never appeared" and "it appeared and was
        // not counted" are the two ways this can fail, and only this line tells them apart.
        LogLine(L"  wechat: audio %ls -- sessions %d (was %d), active %d (was %d), running %d",
                wechatAudioPrimed_ ? L"changed" : L"baseline", sessionCount, wechatAudioSessions_,
                sessionActive ? 1 : 0, wechatAudioWasActive_ ? 1 : 0, wechatRunning ? 1 : 0);
    }
    wechatAudioWasActive_ = sessionActive;
    wechatAudioSessions_ = sessionCount;
    wechatAudioPrimed_ = true;
    if (!announce) return 0;

    Notification notification;
    const char* app = "微信";
    const char* title = "有新消息";
    std::memcpy(notification.app, app, std::strlen(app) + 1);
    std::memcpy(notification.title, title, std::strlen(title) + 1);
    if (!store->Push(notification)) return 0;
    lastSoundAnnounceMs_ = nowMs;
    ++soundAnnouncements_;
    LogLine(L"  wechat: announced '%ls' from its notification sound (%d in total)", L"有新消息",
            soundAnnouncements_);
    return 1;
}

int WeChatWatcher::Poll(NotificationStore* store, unsigned long long nowMs) {
    if (!store) return 0;

    // NOTHING TO COUNT WHEN THERE IS NO WECHAT. The walk below asks every top-level window whether it
    // belongs to WeChat, and on a machine with 400 windows that is ~16 ms -- 32 ms a second on the frame
    // loop for a client that is not even running. The process table answers "is any WeChat process
    // alive" for free, and a WeChat window cannot exist without one.
    if (!AnyWeChatProcessRunning()) {
        // Cleared rather than kept: the list describes windows that were last seen, and a client that
        // has gone and come back owns different handles. Keeping them would suppress the "appeared"
        // line for the first popup after a restart.
        seen_.clear();
        return 0;
    }

    WindowVisit visit;
    EnumWindows(&CollectWindow, reinterpret_cast<LPARAM>(&visit));
    if (visit.handles.empty()) return 0;

    // ONE line per CHANGE, and a change includes a window becoming VISIBLE or changing size -- not only a
    // window appearing.
    //
    // The first version logged appearances only, and that was the reason a real message produced no evidence
    // anywhere: the notification may well be a window the client already owns and simply SHOWS. "Nothing was
    // logged" and "nothing happened" are different facts, and this is the line that separates them.
    std::vector<Seen> current;
    current.reserve(visit.handles.size());
    bool callUp = false;
    for (size_t i = 0; i < visit.handles.size(); ++i) {
        // The CALL, first: it is a different question from the message popup (a titled window rather than a
        // bare one), and its answer is a state that lasts minutes rather than a six-second announcement.
        if (LooksLikeWeChatCall(visit.titles[i].c_str(), visit.visible[i])) {
            callUp = true;
            callPid_ = visit.processes[i];
            const auto known = std::find_if(seen_.begin(), seen_.end(),
                                            [&](const Seen& s) { return s.handle == visit.handles[i]; });
            if (known == seen_.end() || !known->visible) {
                LogLine(L"  wechat: call window %lld '%ls' appeared", visit.handles[i],
                        visit.titles[i].c_str());
            }
        }
        const bool toast = LooksLikeWeChatToast(visit.classes[i].c_str(), visit.titles[i].c_str(),
                                                visit.visible[i], visit.width[i], visit.height[i]);
        current.push_back(Seen{visit.handles[i], toast, visit.visible[i], visit.width[i],
                               visit.height[i], {}});
        wcsncpy_s(current.back().title, 128, visit.titles[i].c_str(), _TRUNCATE);

        const auto known = std::find_if(seen_.begin(), seen_.end(),
                                        [&](const Seen& s) { return s.handle == visit.handles[i]; });
        const bool appeared = (known == seen_.end());
        const bool changed =
            !appeared && (known->visible != visit.visible[i] || known->width != visit.width[i] ||
                          known->height != visit.height[i] ||
                          std::wcscmp(known->title, visit.titles[i].c_str()) != 0);
        if (appeared || changed) {
            LogLine(L"  wechat: window %ls %lld pid=%lu '%ls' '%ls' (%d,%d) %dx%d visible=%d -> %ls",
                    appeared ? L"appeared" : L"changed ", visit.handles[i], visit.processes[i],
                    visit.classes[i].c_str(), visit.titles[i].c_str(), visit.left[i], visit.top[i],
                    visit.width[i], visit.height[i], visit.visible[i] ? 1 : 0,
                    toast ? L"counted as a notification" : L"NOT a notification");
        }

        if (!toast) continue;

        // One announcement per popup, and not more often than the cooldown.
        if (visit.handles[i] == lastHandle_ &&
            nowMs - lastAnnounceMs_ < kAnnounceCooldownMs) {
            continue;
        }
        if (nowMs - lastAnnounceMs_ < kAnnounceCooldownMs) continue;

        Notification notification;
        // UTF-8, like everything else on this path: the card and the strip take UTF-8 and widen it once.
        const char* app = "微信";
        const char* title = "有新消息";
        std::memcpy(notification.app, app, std::strlen(app) + 1);
        std::memcpy(notification.title, title, std::strlen(title) + 1);
        notification.urgency = Urgency::Normal;

        if (store->Push(notification)) {
            lastHandle_ = visit.handles[i];
            lastAnnounceMs_ = nowMs;
            ++announced_;
            LogLine(L"  wechat: announced '%ls' (window %lld, %d in total)", L"有新消息",
                    visit.handles[i], announced_);
            seen_ = current;
            return 1;
        }
    }
    seen_ = current;

    // --- the call, which is a STATE rather than an announcement --------------------
    //
    // A message is news and fades; a call is something the island should hold while it lasts, which is what
    // "微信通话 也应该在灵动岛 上" asks for. The item is STICKY, so the store keeps it on screen until it is
    // read -- and the end of the call reads exactly that one, rather than marking everything read and
    // swallowing whatever messages happened to be unread at the time.
    if (callUp && callNotificationId_ == 0) {
        Notification call;
        const char* app = "微信";
        const char* title = "通话中";
        std::memcpy(call.app, app, std::strlen(app) + 1);
        std::memcpy(call.title, title, std::strlen(title) + 1);
        call.sticky = true;
        if (const Notification* stored = store->Push(call)) {
            callNotificationId_ = stored->id;
            callStartMs_ = nowMs;
            LogLine(L"  wechat: call announced as '%ls' (notification %lld)", L"通话中",
                    callNotificationId_);
            return 1;
        }
    } else if (!callUp && callNotificationId_ != 0) {
        store->MarkRead(callNotificationId_);
        LogLine(L"  wechat: call ended (notification %lld retired)", callNotificationId_);
        callNotificationId_ = 0;
        callStartMs_ = 0;
        callPid_ = 0;
    }
    return 0;
}

}  // namespace island
