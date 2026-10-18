// =====================================================================
//  Lyrics.h
//  Song lyrics: fetch, parse, and answer "what line is playing now".
//
//  Source: the NetEase Cloud Music public web API, which is the usual choice for
//  this and needs no key. Two steps -- search for the track to get its id, then ask
//  for that id's lyrics, which come back as an LRC document.
//
//  Two things this deliberately does NOT do:
//
//   * touch the network on the UI thread. Fetching runs on a worker and the frame
//     loop only reads a snapshot, for the same reason the media poll does: a slow or
//     unreachable server must not be able to stall the animation.
//   * assume the service is available. A track with no lyrics, a search that finds
//     nothing, a timeout, or a changed response format all end in "no lyrics", and
//     the bar falls back to showing the track name. That is a normal outcome, not an
//     error to surface.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstring>

#include "platform/CriticalSection.h"
#include "platform/WorkerThread.h"

namespace island {

// One timed line. `text` is UTF-8.
inline constexpr size_t kLyricLineCapacity = 256;
inline constexpr int    kMaxLyricLines = 512;

struct LyricLine {
    long long timeMs = 0;
    char      text[kLyricLineCapacity] = {};
};

// A parsed LRC document, kept sorted by time.
struct Lyrics {
    LyricLine lines[kMaxLyricLines] = {};
    int       count = 0;

    // Metadata from the LRC tags, when present.
    char title[kLyricLineCapacity]  = {};
    char artist[kLyricLineCapacity] = {};

    // How long the track is, from the CATALOGUE the lyrics came from (0 when it did not say).
    //
    // Worth carrying because a session is not obliged to publish a timeline and NetEase Cloud Music
    // never does: measured while playing, `Position 00:00 / StartTime 00:00 / EndTime 00:00` for the
    // whole song (see TrackPositionFor). The card had no total to print and its bar could not fill.
    // The search that finds the track already carries the length (`duration`, in ms) -- the same
    // request, no extra round trip -- so this is the track's real length, not a guess from the last
    // lyric line. QQ Music's search carries `interval`, in seconds.
    long long trackDurationMs = 0;

    // Index of the line that should be shown at `positionMs`, or -1 when the
    // position is before the first line WITH WORDS (an intro) or there are no lines.
    //
    // Before the first line the answer is -1 rather than 0 on purpose: showing the
    // first line during an instrumental intro reads as being out of sync, while
    // showing nothing reads as "not started yet".
    //
    // An EMPTY line (a time tag with no text, which is how these files mark the end of a phrase) is
    // never the answer: the last line WITH WORDS stays on screen until the next one arrives. Returning
    // the marker made the current line empty, the pill fell back to the song title, and a slow phrase
    // read as "the lyrics stopped" -- "如果一句唱的慢的话 会显示歌曲名".
    int LineAt(long long positionMs) const;

    // The line at `index`, or nullptr when out of range.
    const LyricLine* At(int index) const;

    bool Empty() const { return count == 0; }
};

// How far a line change has rolled, 0 -> 1, eased so it settles rather than stopping dead.
//
// The change is a ROLL: the new line rises into the pill from below while the old one climbs out through
// the top, which is what "歌词最好是切换的时候是从下面向上滚动" asks for. Both lines move by one line
// height, so they never overlap, and the pill's one-line window is what reveals them -- which is why the
// two half rows are visible for the length of the roll and are BY DESIGN.
//
// THE FADE WAS TRIED AND PUT BACK THE SAME DAY. 8.31 replaced this with a cross-fade in place, on the
// strength of "是歌词不要换行" (a photograph of two half rows of "na na na" mid-roll), and the numbers said
// it did what it claimed: the ink never left one row. The user asked for the roll back before the day was
// out -- "还是改回去吧" -- so the roll is what this is, and the two slivers are the price of it. Any
// transition with a VERTICAL offset has them; only a fade with none can avoid them.
//
// A line that simply APPEARS reads as a jump -- the words change and nothing else does, which is what
// "现在感觉没有动画" described. The roll is short and it ends, so the loop goes back to sleep.
//
// Long enough to be seen: at 0.18 s the roll was over before the eye caught it, which is part of what
// "没有过度动画" meant.
inline constexpr float kLyricLineEnterSeconds = 0.26f;

inline float LyricLineRollFor(float ageSeconds) {
    if (ageSeconds <= 0.0f) return 0.0f;
    if (ageSeconds >= kLyricLineEnterSeconds) return 1.0f;
    const float t = ageSeconds / kLyricLineEnterSeconds;
    const float remaining = 1.0f - t;
    return 1.0f - remaining * remaining * remaining;   // ease-out cubic
}

// Whether a re-evaluated line is a real change -- i.e. whether the roll should run.
//
// The frame loop re-evaluates which line is current for reasons that are NOT a line change: the media
// session bumps its revision for a position update (once every second or two), and the loop resets its
// remembered index so the answer is recomputed. Treating that as a change started a roll whose two
// sides were the SAME text, which is what "歌词每1、2秒就会上下出现一模一样的歌词然后和在一起似的"
// describes. A change in the words is the only thing worth animating (and the only thing a repeated
// lyric line -- a real thing in LRC files -- should not animate).
inline bool LyricLineChanged(const char* arrivingText, const char* shownText) {
    const char* arriving = arrivingText ? arrivingText : "";
    const char* shown = shownText ? shownText : "";
    return std::strcmp(arriving, shown) != 0;
}

// Parses an LRC document. Tolerates the variations real files contain:
//
//   [mm:ss.xx]  [mm:ss.xxx]  [mm:ss]  [mm:ss:xx]
//   [ar:artist] [ti:title] and other metadata tags
//   several time tags on one line, meaning the same text at each time
//   text before the first tag (ignored), blank lines, CRLF or LF, and a BOM
//
// Returns the number of lyric lines parsed. Out-of-range times and unsorted input
// are handled: the result is always sorted.
int ParseLrc(const char* lrc, Lyrics* out);

// Whether a service's search hit is the track that is playing.
//
// A search's FIRST result is not necessarily the song, and a wrong lyric is indistinguishable from a right
// one once parsed -- reported as "歌名，封面，歌手全对，就是歌词不对". Both searches name the track they
// picked, so the name and artist are compared BEFORE the lyric request; a different song falls through to
// the other service. A name that is empty on one side is accepted: the service did not say, and refusing
// then would lose correct lyrics to a field the parser could not read.
bool LyricsLooksLike(const char* requestedTitle, const char* requestedArtist,
                     const char* foundTitle, const char* foundArtist);

// --- response parsing -------------------------------------------------
//
// Exposed rather than kept file-local because the two services' response shapes are the
// part of the fetch that CAN be tested offline: whether they answer at all is a network
// question, and the development sandbox has no outbound TLS. A test that could not call
// these would leave the only testable half of the feature untested.

// Percent-encodes a UTF-8 string for use as a query value.
//
// Takes UTF-8 and emits the UTF-8 percent-encoding. It must NOT be handed ANSI bytes: on
// Windows `argv` gives arguments in the process code page, so a Chinese title arrives as
// GBK, and encoding those bytes produces `%B9%A9%CF%FA...` where a search endpoint expects
// `%E4%BE%9B...`. That is what every Chinese query was sent as, and the service answered
// with nothing -- indistinguishable from "this track has no lyrics". Callers must convert
// first (see MainArgumentAsUtf8 in main.cpp).
void UrlEncode(const char* utf8, wchar_t* out, size_t capacity);

// A field read out of the first object of an array that a PATH of keys reaches.
//
// The two services both put the identifier wanted here inside a list of objects, and both responses
// repeat keys inside those objects -- see the note above FlatFieldInArrayObject in Lyrics.cpp for
// what that cost. `path` names the containers in order:
//
//   NetEase's search:  {"songs"}          -> args[0].id
//   QQ's search:       {"song", "list"}   -> req_0.data.body.song.list[0].mid
//
// The key is read at the object's OWN level, so nesting and key order cannot change the answer.
// `FindFlatStringInArray` refuses a longer key that merely contains `key`.
bool FindFlatStringInArray(const char* document, const char* const* path, size_t pathLength,
                           const char* key, char* out, size_t capacity);
bool FindFlatIntegerInArray(const char* document, const char* const* path, size_t pathLength,
                            const char* key, long long* out);

// Expands the XML entities a QQ lyric response uses. `&#10;` becomes a real newline,
// which is what makes the LRC parseable.
void DecodeXmlEntities(const char* source, char* out, size_t capacity);

// One `"lyric"` field out of a document that names its containers, as the NetEase response does:
//
//   {"lrc":{"version":1,"lyric":"[00:15.24]..."},"klyric":{"lyric":"..."},
//    "tlyric":{"version":2,"lyric":"[00:15.24]..."}}
//
// `container` is "lrc" or "tlyric". Reached by finding the container's key and then the next
// `"lyric"` string after it, which is what the shape guarantees; matching braces would have to
// parse strings containing braces, and a lyric is free to contain those.
bool ExtractJsonLyricField(const char* document, const char* container, char* out, size_t capacity);

// Pulls the LRC out of a response, in this order:
//
//   1. `lrc`      -- the original
//   2. `tlyric`   -- the TRANSLATION, which is timed lyrics too
//   3. the first `"lyric"` string anywhere -- the flat shape QQ answers with
//
// THE TRANSLATION IS NOT A LAST RESORT DECORATION, IT IS OFTEN THE ONLY COPY. Measured on this
// machine: `Maple Road（你的名字）` comes back with `"lrc":{"lyric":""}` -- empty -- while
// `"tlyric"` holds the whole timed text. Reading only `lrc` reported "the response had no lyric
// field", which is TRANSIENT, so the bar retried three times per track and never showed a lyric for
// a song whose lyrics it had already received. Reported as "现在音乐歌词 是怎么获取的" after lyrics
// that never appeared.
//
// The original wins when it is there: the translation is a second-best copy, and for a Chinese song
// the "translation" field is usually empty anyway.
//
// `outSource`, when given, receives which of the three answered ("lrc", "tlyric" or "lyric"), so a
// caller can say in its log that the translation was what it got.
bool ExtractLyricField(const char* document, char* out, size_t capacity,
                       const char** outSource = nullptr);

// Pulls the LRC out of a QQ-style response, where it is an XML attribute.
bool ExtractQqXmlLyric(const char* document, char* out, size_t capacity);

// --- fetching ---------------------------------------------------------

// Everything one fetch attempt did, for diagnosis.
//
// The fetch cannot be verified from the development sandbox -- it has no outbound TLS at
// all -- so the only way to find out what happens on a real machine is to report the exact
// request made and the exact response received. "No lyrics" is otherwise indistinguishable
// from a 403, from a redirect to a login page, and from a genuinely empty result.
struct LyricsFetchReport {
    // ONE RECORD PER SERVICE, not one for the fetch.
    //
    // It used to be a single record, overwritten by each service in turn, so the report showed the
    // LAST service's URLs and bodies while the reason string named BOTH ("netease: the search found
    // no matching track; then qq: ..."). Diagnosing the next failure meant reading NetEase's verdict
    // against QQ's response body -- which is exactly what happened while chasing "mainstream Chinese
    // tracks find nothing": the search that supposedly found no songs had answered 200 with a
    // populated list.
    struct Attempt {
        int  searchStatus = 0;   // HTTP status; 0 when the connection never opened
        int  lyricStatus = 0;
        char searchUrl[1024] = {};
        char lyricUrl[1024] = {};
        // The first part of each response, verbatim.
        char searchBody[2048] = {};
        char lyricBody[2048] = {};
        // What THIS service concluded, in its own words.
        char verdict[160] = {};
    };

    Attempt netease;
    Attempt qq;
};

// Whether a failed fetch is worth retrying.
//
// The two kinds of failure need opposite treatment. "The service answered and this track has
// no lyrics" is permanent: retrying it forever wastes two requests every couple of seconds,
// which is what made the strip fall back to the track name. But "the connection never opened,
// or the service said 403" is TRANSIENT -- a network that was down, a typo in a query, a
// service hiccup -- and caching those permanently is why lyrics that used to appear stopped
// appearing after an unrelated fix: the bad answer from the broken build was still the cached
// one.
struct FetchOutcome {
    bool parsed = false;          // lyrics were fetched and parsed
    bool transientFailure = false; // worth retrying later
};

// Fetches lyrics for a track from the network. Blocking: call it from a worker
// thread, never from the frame loop.
//
// Returns true when lyrics were found and parsed. `out` is only modified on
// success, so a failed fetch leaves the caller's previous lyrics in place.
//
// `diagnostic` receives a short human-readable reason on failure, because "no lyrics"
// has several causes that need different responses: the service is unreachable from
// this session, the search matched nothing, or the response was not what was expected.
// Passing null skips it.
bool FetchLyricsFromNetwork(const char* title, const char* artist, int timeoutMs,
                            Lyrics* out, char* diagnostic = nullptr,
                            size_t diagnosticCapacity = 0);

// As above, but also reports whether the failure is worth retrying.
FetchOutcome FetchLyricsDetailed(const char* title, const char* artist, int timeoutMs,
                                 Lyrics* out, char* diagnostic, size_t diagnosticCapacity);

// As above, and also fills in `report` with the requests made and responses received.
bool FetchLyricsVerbose(const char* title, const char* artist, int timeoutMs, Lyrics* out,
                        char* diagnostic, size_t diagnosticCapacity,
                        LyricsFetchReport* report);

// --- the worker -------------------------------------------------------

// Asks for lyrics for a track and holds the result.
//
// A request for the track already held is ignored, so the frame loop can call this
// every frame without causing a fetch per frame. Requests for a different track
// replace any fetch in flight.
class LyricsProvider {
public:
    LyricsProvider() = default;
    ~LyricsProvider();

    LyricsProvider(const LyricsProvider&) = delete;
    LyricsProvider& operator=(const LyricsProvider&) = delete;

    // Starts the worker. Returns false only when the thread cannot be created.
    bool Start();
    void Stop();

    // Queues a fetch for this track unless it is the one already held or in flight.
    // Safe to call every frame.
    void Request(const char* title, const char* artist);

    // Returns the held lyrics, or nullptr when there are none.
    //
    // A POINTER, not a copy. Lyrics is ~132 KB (512 lines x 260 bytes), and copying
    // it took a megabyte-scale memcpy plus a long hold on the lock -- the same size
    // problem that overflowed the stack in the self-test. The returned pointer stays
    // valid until the next Request() or Stop(); the frame loop only reads it within
    // a frame, which is well inside that.
    const Lyrics* Current() const;

    // Copies the lyrics. Kept for callers that want their own copy; prefer Current().
    // Returns false when none are available.
    bool Snapshot(Lyrics* out) const;

    // Bumped when fetched lyrics become available, so the caller can redraw once.
    unsigned long long Revision() const;

private:
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();

    // The stop event, the thread and the rule for ending both. See platform/WorkerThread.h.
    WorkerThread  worker_;
    HANDLE        requestEvent_ = nullptr;   // auto-reset: a new request is waiting
    volatile bool running_ = false;

    mutable CriticalSection lock_;

    // The request, and what was last answered.
    char requestedTitle[256] = {};
    char requestedArtist[256] = {};

    // Whether the pending request has been taken by the worker.
    volatile long requestPending_ = 0;

    // Whether the current request has been answered, success or not. Only a PERMANENT
    // answer is treated as final: a transient failure is retried up to
    // kMaxAttemptsPerTrack times, because caching a single network hiccup or a malformed
    // request as "this track has no lyrics" is what made lyrics that used to work stop
    // working, with no way to recover short of changing track.
    bool haveAsked_ = false;
    bool lastAnswerWasTransient_ = false;
    int  attemptCount_ = 0;

    // A cap on retries, so a genuinely offline machine does not re-request two services
    // every couple of seconds for as long as the track plays.
    static constexpr int kMaxAttemptsPerTrack = 3;

    Lyrics lyrics_{};
    bool   haveLyrics_ = false;
    volatile long revision_ = 0;
};

} // namespace island
