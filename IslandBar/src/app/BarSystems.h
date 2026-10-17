// =====================================================================
//  BarSystems.h -- the things the bar TALKS TO, in one place: the window, the renderer, and one object per
//  channel the outside world arrives through
//
//  WHY THIS IS SEPARATE FROM BarState.h. The two are different kinds of thing and merging them would hide it:
//  BarState is plain data (numbers, strings, flags) whose whole story is "who reads and writes it"; these own
//  RESOURCES -- a D3D device, a composition surface, COM sessions, worker threads, a file handle -- and their
//  story is "when it is created, when it is started, and when it is let go". A reader looking for "what is the
//  bar's state" should not have to walk past twenty resource owners to answer that, and a reader looking for
//  "how many threads is this process running" should be able to find the six places they come from.
//
//  CONSTRUCTION ORDER IS THE SOURCE ORDER, and it did not change when these moved: the members below are
//  declared in exactly the order the locals were, so they are built in the same order and destroyed in the
//  same reverse order. What did NOT move are the calls that bring each one up -- `window.Create(...)`,
//  `statusReader.Open()`, `music.Start()`, `preview.Create(window.Handle())`, `startApps.RefreshList()` on a worker
//  thread -- because those are a SEQUENCE with logging, early returns and fallbacks between them, and a
//  sequence belongs in RunBar where it can be read from top to bottom. A member is alive from the moment the
//  struct is built; the bar's startup code says when each of them is asked to start.
//
//  TWO SMALL FIELDS RIDE ALONG WITH THEIR OWNER rather than living in BarState: `haveTaskbar` belongs to
//  `taskbar` (it says whether the claim succeeded) and `appBarResult` belongs to `appBar` (it is that call's
//  answer). Keeping them apart from the object they describe is how a reader ends up checking one and not the
//  other.
// =====================================================================
#pragma once

#include <memory>

#include "app/AppBar.h"              // AppBarResult
#include "app/IslandWindow.h"        // IslandWindow, AppBar
#include "app/PlayerWindow.h"        // PlayerWindowWatch
#include "media/AudioVolume.h"
#include "media/Lyrics.h"            // LyricsProvider
#include "media/MusicSession.h"
#include "media/SoundOutput.h"       // SoundOutput, SoundOutputSnapshot
#include "platform/NativeTaskbar.h"
#include "platform/ThumbnailPreview.h"
#include "platform/WindowList.h"
#include "render/Renderer.h"
#include "render/TextRenderer.h"
#include "services/Notification.h"   // NotificationStore
#include "services/NotificationPipe.h"   // NotificationServer
#include "services/ToastStore.h"     // ToastReader
#include "services/WeChatWatch.h"
#include "shell/StartApps.h"
#include "shell/SystemStatus.h"      // SystemStatusReader, SystemStatus
#include "shell/TrayIcons.h"
#include "shell/Weather.h"

namespace island {

struct BarSystems {
    // THE ORDER BELOW IS THE ORDER THE LOCALS WERE IN, one for one, and it is the order they are built in:
    // a struct builds its members top to bottom and destroys them bottom to top, which is what the locals did.
    // So this is not a tidy grouping -- it is the source order, kept deliberately, and any regrouping has to
    // argue that these twenty-odd objects do not care about each other's construction. Grouping is in the
    // comments instead.

    // the machine's own state
    SystemStatusReader statusReader;
    SystemStatus systemStatus;

    // the sound page's own snapshot, on the heap: tens of kilobytes, filled by that page's worker
    std::unique_ptr<SoundOutputSnapshot> soundSnapshot = std::make_unique<SoundOutputSnapshot>();

    // the taskbar it hides -- and whether the hide succeeded, which belongs to the claim and not to the state
    NativeTaskbar taskbar;
    bool haveTaskbar = false;

    // the strip it reserves -- and that call's answer, kept beside it for the same reason
    AppBar appBar;
    AppBarResult appBarResult = AppBarResult::NoWindow;

    // what the bar is made of
    IslandWindow window;
    Renderer renderer;
    TextRenderer text;

    // what the outside world tells it about itself
    NotificationStore notifications;
    NotificationServer notificationServer;
    ToastReader toasts;
    WeChatWatcher wechat;

    // what is playing
    MusicSession music;
    PlayerWindowWatch playerWindow;
    AudioVolume volume;

    // ... and the one object that owns a worker for it
    std::unique_ptr<SoundOutput> soundOutput = std::make_unique<SoundOutput>();

    LyricsProvider lyrics;
    WeatherReader weather;

    // the shell's own things, mirrored
    ThumbnailPreview preview;
    TrayIcons trayIcons;
    StartApps startApps;
    WindowList taskWindows;
};

} // namespace island
