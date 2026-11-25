// =====================================================================
//  MusicSession.h
//  What is playing right now, read from the system media controls.
//
//  Windows exposes this through Windows.Media.Control (the same session the
//  volume flyout shows): SMTC. It works for anything that registers a media
//  session -- Spotify, browsers, the new Media Player, WeChat's player -- without
//  any per-app work, which is why it is the right source here.
//
//  Threading: every WinRT call is made from a worker thread, never the UI thread.
//  The session manager is an async API whose first call waits on the shell; doing
//  that on the frame loop would stutter the animation, and an unresponsive media
//  provider must not be able to freeze the bar. The UI reads a snapshot.
// =====================================================================
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "platform/CriticalSection.h"
#include "platform/WorkerThread.h"

namespace island {

// The position to DRAW for, given the last reading and how long ago it was taken.
//
// The media session reports its position only when something changes -- once a second or two -- so
// anything time-based built on the raw snapshot moves in steps: the card's progress bar jumped, and
// the lyric line changed a couple of seconds after the words were sung ("现在歌词和唱的有点对应不上").
// Projecting the reading forward by the elapsed time is the same correction in both places, so it is
// one function rather than two copies of the arithmetic.
//
// Not projected when it cannot be trusted: paused (the position is not advancing), a live stream with
// no duration, or a session that reports zero (which is what "this application does not report a
// position" looks like). Clamped to the duration, because a projection that runs past the end of the
// track would park the last line for as long as the pause before the next one is noticed.
inline long long ProjectedPositionMs(long long positionMs, long long durationMs, long long elapsedMs,
                                     bool playing) {
    if (!playing || positionMs <= 0 || durationMs <= 0 || elapsedMs <= 0) return positionMs;
    const long long projected = positionMs + elapsedMs;
    return projected > durationMs ? durationMs : projected;
}

// Whether the session can say how far into the track it is.
//
// The timeline is OPTIONAL in SMTC, and a player is free to leave it out entirely. NetEase Cloud
// Music does exactly that: measured with `gsmtc_probe` -- while PLAYING, `Position 00:00`,
// `StartTime 00:00`, `EndTime 00:00` for the whole song. Both of the things built on the position
// then sat still: the lyric line is chosen by it, so the pill stayed on the first line ("歌词不动"),
// and the card printed `00:00 / 00:00` with an empty bar ("进度条也不动").
inline bool SessionReportsTimeline(long long positionMs, long long durationMs) {
    return positionMs > 0 || durationMs > 0;
}

// Where the bar should draw the playhead, and whether that number came from the session.
//
// Two sources, one answer, because three readers depend on it -- the lyric line, the card's printed
// time and its bar, and the once-a-second redraw -- and they must not disagree:
//
//   * the session's own reading, projected forward between its polls, when it publishes a timeline;
//   * otherwise a STOPWATCH the bar runs itself (see TrackPosition::fromSession), which is the same
//     accounting the window-title source needs (PlayerWindow.h).
//
// The stopwatch is advanced by the caller, and only while the song is playing -- so pausing leaves
// the words where they are, which is the rule the projected path already has.
//
// THE HONEST LIMIT: a stopwatch knows how long the track has been ON SCREEN, not how far into the
// song it is. A bar started mid-song therefore begins the words at the first line and drifts until
// the next track; catching a track as it starts is aligned. Said here, in the handover, and in the
// reply to the person who reported it -- not papered over.
struct TrackPosition {
    long long ms = 0;
    bool fromSession = false;   // false: counted by the bar (the session publishes no timeline)
};

inline TrackPosition TrackPositionFor(long long positionMs, long long durationMs,
                                      long long sinceReadingMs, long long stopwatchMs, bool playing,
                                      bool timelineTrusted) {
    TrackPosition out;
    if (timelineTrusted && SessionReportsTimeline(positionMs, durationMs)) {
        out.ms = ProjectedPositionMs(positionMs, durationMs, sinceReadingMs, playing);
        out.fromSession = true;
        return out;
    }
    out.ms = stopwatchMs > 0 ? stopwatchMs : 0;
    out.fromSession = false;
    return out;
}

inline constexpr size_t kMusicTitleCapacity  = 192;
inline constexpr size_t kMusicArtistCapacity = 128;
inline constexpr size_t kMusicAlbumCapacity  = 160;
// The display name can be longer than the id it comes from; a product name with a
// subtitle is still short. 128 is generous and keeps the frame small.
inline constexpr size_t kMusicAppNameCapacity = 128;
inline constexpr size_t kMusicAppCapacity    = 64;

enum class PlaybackState : int {
    Closed    = 0,   // no session at all
    Stopped   = 1,
    Paused    = 2,
    Playing   = 3,
    Unknown   = 4,
};

// Album art, re-encoded to a PNG on the polling thread and bounded by this many
// bytes.
//
// 32 KB, not 192 KB: a 128x128 PNG is a few KB at most, so the larger figure only
// made every MusicInfo copy (and there is one per poll) carry a mostly-empty
// buffer. The bound exists to cap a hostile or broken provider, not to be generous.
inline constexpr size_t kMusicArtworkCapacity = 32 * 1024;

// The longest side the artwork is scaled to. The card draws it at 44 DIP, so 128
// leaves room for a high-DPI display without carrying a full-size cover.
inline constexpr int kMusicArtworkMaxDimension = 128;

struct MusicInfo {
    bool          present = false;    // false means "no media session"
    PlaybackState state = PlaybackState::Closed;

    char title[kMusicTitleCapacity]   = {};
    char artist[kMusicArtistCapacity] = {};
    char album[kMusicAlbumCapacity]   = {};
    char app[kMusicAppCapacity]       = {};   // which app owns the session

    // Playback position and total, in milliseconds. Zero when unknown, which a
    // live stream reports.
    long long positionMs = 0;
    long long durationMs = 0;
    // The player publishes media properties and timeline properties separately,
    // and they are not an atomic pair. On a track change the timeline can still
    // belong to the previous song, so positionMs is usable only while this is
    // true. `timelineEpoch` changes with the track or session identity; the UI
    // uses that epoch's own stopwatch until the media reader can prove that a
    // later timeline reading belongs to it.
    unsigned long long timelineEpoch = 0;
    bool timelineTrusted = false;

    // Album art as a small PNG.
    //
    // `artworkSize` is the number of significant bytes but is reported as the
    // buffer's capacity, not the encoder's exact output: neither Stat nor Seek on a
    // memory-backed WIC stream reports the write position (both return the size the
    // stream was initialised with -- see WicImage.cpp). The decoder stops at the PNG
    // end marker, so a capacity-sized length is safe, and the buffer is small enough
    // that carrying it costs nothing. Treat the figure as an upper bound.
    uint8_t artwork[kMusicArtworkCapacity] = {};
    size_t  artworkSize = 0;

    // Bumped whenever the artwork bytes change, so the renderer knows when its
    // decoded copy is stale without comparing the whole buffer.
    unsigned long long artworkRevision = 0;

    // The source application's own icon, as a small PNG.
    //
    // Worth the trouble because the icon is what identifies the player at a glance: a coloured
    // dot beside the name was reported as looking unfinished, and it did. Extracted from the
    // executable the session's app id names.
    // The name to show in the card, which is NOT the app id: the id is a machine identifier
    // such as "C:\SOFTWARE\QQMusic\QQMusic.exe".
    char appName[kMusicAppNameCapacity] = {};

    uint8_t icon[kMusicArtworkCapacity] = {};
    size_t  iconSize = 0;
    unsigned long long iconRevision = 0;
};

// Whether a fresh reading describes a different track or media session. An
// empty metadata field is not a track change: players can return an empty
// artist or album on one poll without having changed what is playing. This is
// pure so the same rule can be asserted without a real media provider.
inline bool MusicTrackChanged(const MusicInfo& previous, const MusicInfo& fresh) {
    if (previous.present != fresh.present) return true;
    if (!fresh.present) return false;
    const auto changed = [](const char* before, const char* after) {
        return after[0] != '\0' && std::strcmp(before, after) != 0;
    };
    return changed(previous.title, fresh.title) || changed(previous.artist, fresh.artist) ||
           changed(previous.album, fresh.album) || changed(previous.app, fresh.app);
}

// Whether a timeline reading may be used for the current track epoch. A track
// change never trusts the reading that carried the new title: chronology is not
// atomic across the two GSMTC properties. It becomes trustworthy only after the
// timeline reports another update within the same epoch. A first session has no
// older song to contaminate and may use its reading immediately.
inline bool TimelineTrustedForEpoch(bool trackChanged, bool hadPreviousSession,
                                    bool timelineRangeValid, bool previousTrusted,
                                    bool timelineRebased, long long timelineUpdatedAt,
                                    long long epochTimelineUpdatedAt, long long positionMs,
                                    long long epochPositionMs) {
    if (!timelineRangeValid) return false;
    if (trackChanged) return !hadPreviousSession;
    if (previousTrusted) return true;
    if (!timelineRebased) return false;
    const bool timeAdvanced =
        timelineUpdatedAt > 0 && timelineUpdatedAt > epochTimelineUpdatedAt;
    const bool positionAdvanced = epochPositionMs >= 0 && positionMs != epochPositionMs;
    return timeAdvanced || positionAdvanced;
}

class MusicSession : public std::enable_shared_from_this<MusicSession> {
public:
    MusicSession() = default;
    ~MusicSession();

    MusicSession(const MusicSession&) = delete;
    MusicSession& operator=(const MusicSession&) = delete;

    // Starts the worker. Returns false when the media API is unavailable on this
    // build, in which case the rest of the bar still works.
    bool Start();

    WorkerThread::StopResult Stop();

    // Copies the latest snapshot. Cheap and non-blocking; call it from the frame
    // loop.
    void Snapshot(MusicInfo* out) const;

    // True when the snapshot differs from the one the caller last saw. Used to
    // decide whether a redraw is needed without polling the whole struct.
    unsigned long long Revision() const;

    // Number of times the worker has successfully read a session, for reporting.
    long long PollCount() const;

    // What the UI wants the worker to do. The click arrives on the UI thread and is
    // queued: the WinRT calls are async and must never run there.
    enum class Transport { PlayPause, Previous, Next };

    // Queues a transport action. Returns false when there is nothing to control.
    bool RequestTransport(Transport action);

    // Convenience for the play/pause case.
    bool RequestTogglePlayPause() { return RequestTransport(Transport::PlayPause); }

private:
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();

    // The stop event, the thread and the rule for ending both. See platform/WorkerThread.h.
    WorkerThread       worker_;
    std::atomic<bool>  running_{false};

    mutable CriticalSection lock_;
    MusicInfo          current_{};
    // `long`, not `long long`: InterlockedIncrement operates on 32-bit values and
    // has no overload for a 64-bit volatile.
    volatile long      revision_ = 0;
    volatile long      pollCount_ = 0;

    // A toggle requested by the UI, picked up by the worker on its next pass. Using
    // an event rather than a plain flag means the worker acts at once instead of
    // waiting out its two-second poll interval.
    HANDLE             toggleEvent_ = nullptr;
    volatile long      togglePending_ = 0;

    // Which action the pending request is for. Written under the lock alongside the
    // pending flag so the worker cannot read one without the other.
    Transport          pendingAction_ = Transport::PlayPause;
};

const char* PlaybackStateName(PlaybackState state);

// Loads an executable's icon as a small PNG.
//
// Exposed so the preview and the self-test can exercise the icon path directly: the live path
// only reaches it when some application happens to be publishing a media session, which cannot
// be arranged on demand.
size_t FetchAppIconForTest(const wchar_t* executablePath, uint8_t* out, size_t capacity);

// How interesting a session is, for choosing between several. Pure, so it is tested directly
// rather than by arranging two applications to play at once.
int SessionScoreForTest(const wchar_t* appId, bool playing);

// The name a USER knows a player by, for the players that do not state it in their own version
// resource. nullptr when the player is not in the table -- which is the case for every player whose own
// name is already the right one (QQ音乐's ProductName IS "QQ音乐").
//
// The exception that put this here: NetEase Cloud Music carries exactly ONE version translation
// (0804/04B0, Simplified Chinese) and its ProductName in it is the English "NetEase Cloud Music", so
// there is no Chinese name in the file to prefer and the card showed English for a Chinese application.
const wchar_t* KnownPlayerDisplayName(const wchar_t* executableName);

// How long the poll worker waits before reading the session again.
//
// The plain interval is two seconds, which is right for a player that updates its own status on its own
// schedule -- but NOT after the user skips a track. There the metadata is the whole point of the click, and
// the player needs a few hundred milliseconds to load the next track and publish it: waiting out a full
// interval left the card showing the OLD title and cover. Measured from the bar's own log:
//
//   14:11:48.743  music: next requested
//   14:11:48.906  music: transport next accepted      <- the player answered in 163 ms
//   14:11:50.557  music: [QQMusic.exe] ...            <- the bar learned about it 1.65 s after the click
//
// Reported as "切歌的时候点击完 需要等个2-3秒才能切". So for a few seconds after a skip the worker polls every
// kSkipPollIntervalMs instead -- which also brings the new cover and the new lyrics in at the same speed.
//
// ... and a TRACK CHANGE starts the same chase, wherever the skip came from. A skip made in QQ音乐's own
// window never reaches this bar as a request, so the only thing to go on is the new title appearing -- and
// without a chase the bar then waits out a full interval before asking for the new cover, which for a player
// that publishes the thumbnail a moment later is the difference between "it appeared" and "it never came".
//
// Pure, so the chase can be asserted without waiting for a real skip.
// The quiet interval, and it is 1 s rather than the 2 s it was.
//
// Two seconds was chosen when the only question was "has the position moved" -- the metadata is pushed, so
// asking faster does not make it fresher. What that missed is a skip made IN THE PLAYER: this bar never sees
// it as a request, so the new title can only be noticed by asking, and the older interval meant up to two
// seconds of showing the previous track before the chase below could even start (reported as "点击切歌 还是需要
// 等个4 5秒" -- the player's own publish lag on top of it). The read is a local SMTC call, not a network one,
// so halving the interval costs a worker wake-up and buys back a second of the delay.
inline constexpr unsigned long long kPollIntervalMs = 1000;
inline constexpr unsigned long long kSkipPollIntervalMs = 250;
inline constexpr unsigned long long kSkipPollWindowMs = 6000;
// How often the poll may ask a player for a cover it has not published yet.
//
// ASKING IS ALMOST FREE, and that is what the first version of this got wrong: it stopped asking 15 seconds
// after the track changed, on the reasoning that "the player has had its chance". QQ音乐 published the cover
// for 《蚂蚁 (Live)》 more than a minute in -- measured: `music: cover NONE (0 bytes)` at 16:53:56 and
// `gsmtc_probe` reporting `Thumbnail: present` for the same track while the bar had already stopped, so the
// whole track showed the ♪ placeholder ("封面老是查询不到").
//
// The cost of NOT asking is real (a missing cover for the whole song); the cost of asking is a property read:
// `FetchArtwork` returns immediately when the session carries no thumbnail, and the stream open plus PNG
// re-encode happen only once there is something to fetch -- after which the asking stops anyway, because the
// cover is no longer missing. So: every second for the first stretch, then every few seconds for as long as
// the track has none.
inline constexpr unsigned long long kArtworkRetryMs = 1000;
inline constexpr unsigned long long kArtworkRetryWindowMs = 15000;   // the fast stretch, then:
inline constexpr unsigned long long kArtworkRetrySlowMs = 2000;
// How many failed retries for one track are logged before the retrying goes quiet. The line answers "does the
// bar even ask?", which otherwise cannot be told from "the player has nothing"; more than a handful per track
// would be noise in a log that a person reads.
inline constexpr int kArtworkRetryLogLimit = 5;

// Whether this poll should ask the player for the cover.
//
// Two rules have been wrong here, in opposite directions, and the shape below is what is left after both:
// the first asked ONLY when the track changed (a late thumbnail was never picked up at all), the second asked
// for 15 seconds and then gave up (a thumbnail published at 60 seconds was never picked up either). It asks
// while the current track has nothing, and stops only when there IS something.
inline bool ShouldFetchArtwork(bool artworkEnabled, bool trackChanged, bool artworkMissing,
                              unsigned long long sinceLastTryMs,
                              unsigned long long sinceTrackChangeMs) {
    if (!artworkEnabled) return false;
    if (trackChanged) return true;
    if (!artworkMissing) return false;
    const unsigned long long cadence = (sinceTrackChangeMs < kArtworkRetryWindowMs)
                                          ? kArtworkRetryMs
                                          : kArtworkRetrySlowMs;
    return sinceLastTryMs >= cadence;
}

// Reacquiring the manager resets a Windows Runtime object around every two seconds. It is kept as a
// fallback for QQMusic's stale playback-status cache, but a diagnostic needs to switch that periodic
// reset off without discarding the normal PlaybackInfoChanged wake-up.
inline bool ShouldRefreshMediaManager(bool periodicRefreshEnabled, bool playbackChanged,
                                      bool fallbackDue) noexcept {
    return playbackChanged || (periodicRefreshEnabled && fallbackDue);
}

inline unsigned long long NextPollIntervalMs(unsigned long long nowMs, unsigned long long lastSkipMs,
                                            unsigned long long lastTrackChangeMs) {
    // An event that has not happened yet is represented by 0, and the subtraction below would wrap: 0 reads
    // as "long ago", which is what the caller means.
    const auto withinWindow = [&](unsigned long long stamp) {
        return stamp != 0 && nowMs >= stamp && nowMs - stamp < kSkipPollWindowMs;
    };
    return (withinWindow(lastSkipMs) || withinWindow(lastTrackChangeMs)) ? kSkipPollIntervalMs
                                                                        : kPollIntervalMs;
}

// The path the icon lookup would load from, for diagnostics. Empty when nothing resolves.
bool ResolveAppIconPath(const wchar_t* appId, wchar_t* out, size_t capacity);

// Turns a session app id into the executable path its icon can be loaded from. Returns false for
// a packaged-app id, which names a package rather than a file.
bool ExecutableFromAppId(const wchar_t* appId, wchar_t* out, size_t capacity);

} // namespace island
