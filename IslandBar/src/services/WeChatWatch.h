// =====================================================================
//  WeChatWatch.h
//  "微信 · 有新消息" -- the third notification channel, and the one that had
//  to be built by watching a WINDOW rather than by reading a message.
//
//  WHY IT IS DONE THIS WAY, all of it measured on this machine (2026-10-11):
//
//   * WeChat 4.x ("Weixin", Qt + a self-drawn MMUI surface) is NOT registered as a
//     Windows toast sender: it is absent from HKCU\...\Notifications\Settings, it has
//     never written a row to wpndatabase.db, and it does not raise a tray balloon. The
//     bar's other two notification channels can therefore never see it.
//   * Its window exposes UI Automation DOWN TO A SINGLE SELF-DRAWN PANE
//     (`MMUIRenderSubWindow`): there is no chat name and no message text to read.
//   * What it does do is SHOW A POPUP of its own when a message arrives -- and that is
//     the signal this file uses. The user's decision was to keep it simple: the bar says
//     "微信 · 有新消息", not who said what. (Reading the text would mean photographing the
//     popup and running Windows' OCR over it; the engine and a Chinese recogniser are
//     both present, so that remains an option, but it is not what was asked for.)
//
//  NOTHING HERE TOUCHES WECHAT'S DATA OR ITS PROCESS. It enumerates windows, which is
//  public, and it never reads, injects into, or subclasses anything of theirs.
// =====================================================================
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "platform/CriticalSection.h"
#include "platform/WorkerThread.h"
#include "services/Notification.h"

namespace island {

// Whether a window is WeChat announcing a message, judged from what a window can be asked.
//
// PURE, and that is the point: the whole feature hangs off this decision, and a wrong answer is
// invisible -- a window that was not recognised produces exactly the same screen as a message that
// never arrived. So it is a function of values, asserted in the self-test against the windows this
// machine actually has (the main window, the call window, the tray window, the 0x0 message windows)
// as well as against a synthetic corner popup.
//
// The rules, and why each one is there:
//   * VISIBLE -- a hidden window is not a notification.
//   * A TOAST-SIZED RECTANGLE. WeChat's own windows are either the 400x700+ main window or 0x0
//     message-only windows; a notification is a few hundred DIP across and about a hundred tall.
//   * NOT A WINDOW THAT IS ALWAYS THERE. The rule used to require the popup to be near a screen edge, on
//     the theory that a toast lives in a corner. That was WRONG, and the log proved it: when a message
//     arrived, no window appeared at all -- and the client does own a hidden 176x199 window sitting dead
//     centre, which is exactly the shape a compact "new message" card has. A hidden window's rectangle is
//     whatever its layout last computed, not where it will be shown, so judging position was judging a
//     number that means nothing until the window is on screen.
//   * NOT ONE OF THE CLASSES THAT ARE KNOWN NOT TO BE IT -- the tray icon's message window is
//     full-screen sized and hidden in practice, but it is excluded by name so the rule does not
//     depend on that staying true.
bool LooksLikeWeChatToast(const wchar_t* className, const wchar_t* title, bool visible, int width,
                          int height);

// Whether a window is WeChat's AUDIO/VIDEO CALL window -- the other thing the user asked the island to
// show ("微信通话 也应该在灵动岛 上").
//
// Judged by the title, because this window HAS one: `微信音视频通话` (with a trailing space, which is what
// the client actually sets). The notification popup has no title at all, which is the other half of why
// these are two separate rules rather than one "is this WeChat doing something" test.
//
// Pure, for the same reason as the toast rule: a call that is not recognised is indistinguishable from no
// call at all.
bool LooksLikeWeChatCall(const wchar_t* title, bool visible);

// Whether a WeChat SOUND starting means "a message just arrived".
//
// This is the last signal WeChat gives and the one the user asked for in as many words ("不需要读微信信息里有什么
// 就是来微信的时候有个通知就行"). Measured facts behind the rule:
//
//   * WeChat keeps an audio session open from the moment it starts, so the session EXISTING says nothing;
//   * `AudioSessionStateActive` means audio is flowing, so the moment it BECOMES active a sound has started;
//   * a notification plays a sound because "新消息通知声音" is on, so a sound with nobody talking is a message.
//
// The state is a level, not an event, so this is an EDGE detector and is pure: the failure it guards against is
// announcing on every frame of a sound that is still playing, and announcing again the instant the cooldown
// expires while the same sound continues.
bool ShouldAnnounceWeChatSound(bool wechatRunning, bool wasActive, bool isActive, int wasSessions,
                               int sessions, bool primed, unsigned long long sinceLastAnnounceMs);

// Watches the WeChat process's windows and reports a message arriving.
class WeChatWatcher {
public:
    WeChatWatcher() = default;
    ~WeChatWatcher();

    WeChatWatcher(const WeChatWatcher&) = delete;
    WeChatWatcher& operator=(const WeChatWatcher&) = delete;

    // Keeps the process-name index fresh on a worker. The worker owns a shared reference for its
    // whole lifetime, so Stop() timing out cannot leave a pointer into a destroyed watcher.
    bool Start(const std::shared_ptr<WeChatWatcher>& owner);
    WorkerThread::StopResult Stop();

    // Main-thread queries. They copy a shared immutable index and do no process or window I/O.
    bool AnyProcessRunning() const;
    bool IsProcessWeChat(unsigned long pid) const;

    // Enumerates the Weixin/WeChat processes' windows and pushes ONE notification for a new
    // toast-like window. Returns how many it pushed (0 or 1).
    //
    // `nowMs` is GetTickCount64: the debounce is time-based rather than count-based, because a message
    // arriving is one event even when the popup is repainted or re-created while it is on screen.
    int Poll(NotificationStore* store, unsigned long long nowMs);

    // The SOUND channel: told whether a WeChat audio session is active, this announces a message when one
    // starts. Separate from Poll because the audio state comes from the sound worker's snapshot rather than
    // from a window scan, and the two are polled on different clocks.
    int PollSound(NotificationStore* store, bool wechatRunning, bool sessionActive,
                  int sessionCount, unsigned long long nowMs);

    // Total ever announced, for the log.
    int Announced() const { return announced_; }

    // The call's own readings, for the strip: when it started, and which process is in it.
    //
    // The START is kept here rather than derived by the caller, because "since when" is a fact about the call
    // and the watcher is what saw it begin -- a caller that started counting when it first drew the strip
    // would print a timer that begins at zero every time the bar was restarted mid-call.
    unsigned long long CallStartMs() const { return callStartMs_; }
    unsigned long CallPid() const { return callPid_; }
    long long CallNotificationId() const { return callNotificationId_; }

    // The id of the sticky "通话中" notification while a call is up, or 0. Held so that the END of the call
    // can retract exactly that one item rather than marking everything read -- which would swallow whatever
    // messages happened to be unread at the time.
    long long callNotificationId_ = 0;
    unsigned long long callStartMs_ = 0;
    unsigned long callPid_ = 0;

    // The WeChat audio session's last state, for the edge above.
    bool wechatAudioWasActive_ = false;
    int wechatAudioSessions_ = 0;
    bool wechatAudioPrimed_ = false;
    unsigned long long lastSoundAnnounceMs_ = 0;
    int soundAnnouncements_ = 0;

private:
    struct ProcessIndex;
    static unsigned long __stdcall ProcessThreadEntry(void* self);
    void RunProcessIndex();
    std::shared_ptr<const ProcessIndex> ProcessSnapshot() const;

    WorkerThread processWorker_;
    mutable CriticalSection processLock_;
    std::shared_ptr<const ProcessIndex> processIndex_;

    // The handle last announced, so one popup is not reported twice, and when it was announced.
    long long lastHandle_ = 0;
    unsigned long long lastAnnounceMs_ = 0;
    int announced_ = 0;
    // The windows seen on the last pass, so a CHANGE can be logged rather than the whole set every half
    // second. This is the diagnostic that makes the feature debuggable: when a message does not produce a
    // card, the log has to say which window appeared and why it was not taken as one.
    struct Seen {
        long long handle = 0;
        bool toast = false;
        bool visible = false;
        int width = 0;
        int height = 0;
        // The TITLE is tracked as well, and that is the newest thing this watcher learns.
        //
        // When a message arrived, no window appeared and no window changed shape -- so the client's own popup
        // is not a window at all. What many clients DO have is the unread count in the window title
        // ("微信(1)", "(3) 微信"), and the title is a change this watcher can see: the main window is parked
        // off-screen with a title, and that title is where an unread count would go first.
        wchar_t title[128] = {};
    };
    std::vector<Seen> seen_;
};

} // namespace island
