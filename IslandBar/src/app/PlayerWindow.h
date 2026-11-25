// =====================================================================
//  PlayerWindow.h
//  The player that does not speak to the system: read from its own window title.
//
//  WHY THIS EXISTS
//
//  Everything the bar shows about music comes from the system media session
//  (Windows.Media.Control, SMTC): the title, the artist, the cover, the position and the
//  three transport keys. A player that never registers a session is therefore invisible
//  to the bar -- and there is one on this machine that behaves exactly like that: NetEase
//  Cloud Music 3.1.40 plays with the system's session list EMPTY, and only publishes a
//  session once its own switch is turned on (measured, with the session list at zero
//  while four of its processes were playing; see HANDOFF 8.72).
//
//  What such a player does say out loud is its window title: NetEase's window carries
//  `<song> - <artist>` while a track is up (measured: `长大以后你是否无忧 - 宋扬/不劉`),
//  and `island_bar.exe --lyrics-test "长大以后你是否无忧" "宋扬"` fetched 42 lines from
//  that pair in 406 ms. The words are the one thing the island cannot do without, and the
//  lyrics themselves come from the online lyric APIs by title and artist -- so the title
//  is enough for the line.
//
//  WHAT IT DOES NOT GIVE: the cover, the position, the playing/paused state and the
//  transport keys. So a track read this way drives the PILL'S LINE ONLY; the media card
//  is not offered for it (every field of that card comes from the session, and a card
//  built on a title would be a rectangle of empty rows with three dead buttons). The
//  lyric line is counted from the moment the title appeared, so it is aligned when the
//  bar catches the song as it starts and drifts when the bar is started mid-song. Both
//  of those are written down rather than papered over.
//
//  Read-only: it enumerates windows, reads their titles and the names of the processes
//  that own them. It injects nothing, sends nothing, and touches no player.
// =====================================================================
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <memory>

#include "platform/CriticalSection.h"
#include "platform/WorkerThread.h"

namespace island {

inline constexpr size_t kPlayerWindowTitleCapacity  = 192;
inline constexpr size_t kPlayerWindowArtistCapacity = 128;
// The bare executable name ("cloudmusic.exe"). Long enough for a real one, short because it is.
inline constexpr size_t kPlayerWindowAppCapacity    = 64;

// How often the worker looks. One second: a title changes when a track does, which is a
// human-scale event, and each pass is a handful of window enumerations.
inline constexpr unsigned long kPlayerWindowPollMs = 1000;

struct PlayerWindowInfo {
    bool present = false;
    char title[kPlayerWindowTitleCapacity]   = {};
    char artist[kPlayerWindowArtistCapacity] = {};
    char app[kPlayerWindowAppCapacity]       = {};
};

// Whether this executable is a player whose title is worth reading.
//
// A short list rather than a heuristic: browsers and editors are ALSO windows with titles,
// and a browser tab called "网易云音乐 - 每日推荐" would otherwise be read as a song. Pure, so
// the list is tested rather than trusted.
bool IsKnownPlayerExecutable(const wchar_t* executableName);

// One window title -> (song, artist). Pure and total: anything it cannot read confidently is
// refused, because the pill would otherwise announce a page name as a song.
//
// Refused: no ` - ` separator at all, a side that is empty or only spaces, and a left-hand side
// that is the player's own name ("网易云音乐", "QQ音乐", …) -- which is what these windows are
// titled when no track is up, and would otherwise be looked up as a song called 网易云音乐.
// The separator is taken from the RIGHT: a song title may contain a dash, the artist part does not.
bool ParsePlayerWindowTitle(const wchar_t* windowTitle, char* titleOut, size_t titleCapacity,
                            char* artistOut, size_t artistCapacity);

class PlayerWindowWatch : public std::enable_shared_from_this<PlayerWindowWatch> {
public:
    PlayerWindowWatch() = default;
    ~PlayerWindowWatch();

    PlayerWindowWatch(const PlayerWindowWatch&) = delete;
    PlayerWindowWatch& operator=(const PlayerWindowWatch&) = delete;

    // Starts the worker. False when the thread could not be created; the rest of the bar
    // carries on without this source.
    bool Start();

    WorkerThread::StopResult Stop();

    // Copies the latest reading. Cheap and non-blocking; call it from the frame loop.
    void Snapshot(PlayerWindowInfo* out) const;

    // Bumped whenever the reading changes -- including when the last player window goes away,
    // so the caller sees the pill go back to its clock.
    unsigned long long Revision() const;

    // Passes made, for reporting.
    long long PollCount() const;

private:
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();
    void Poll();
    void Publish(const PlayerWindowInfo& info);

    // The stop event, the thread and the rule for ending both. See platform/WorkerThread.h.
    WorkerThread worker_;
    std::atomic<bool> running_{false};

    mutable CriticalSection lock_;
    PlayerWindowInfo published_{};
    volatile long revision_ = 0;
    volatile long pollCount_ = 0;
};

}  // namespace island
