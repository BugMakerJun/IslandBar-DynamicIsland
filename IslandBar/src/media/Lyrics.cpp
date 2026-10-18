// =====================================================================
//  Lyrics.cpp
// =====================================================================
#include "media/Lyrics.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "domain/MiniJson.h"
#include "core/Log.h"
#include "platform/Http.h"

namespace island {
namespace {

// --- LRC parsing ------------------------------------------------------

// Reads a decimal run and advances the cursor. Returns 0 when there are no digits.
int ReadNumber(const char** cursor, int maxDigits) {
    int value = 0;
    int digits = 0;
    while (digits < maxDigits && **cursor >= '0' && **cursor <= '9') {
        value = value * 10 + (**cursor - '0');
        ++(*cursor);
        ++digits;
    }
    return digits > 0 ? value : -1;
}

// Tries to read one `[mm:ss]`, `[mm:ss.xx]`, `[mm:ss:xx]` or `[mm:ss.xxx]` tag at
// `cursor`. On success sets `timeMs` and advances `cursor` past the closing bracket.
bool TryReadTimeTag(const char** cursor, long long* timeMs) {
    const char* p = *cursor;
    if (*p != '[') return false;
    ++p;

    const int minutes = ReadNumber(&p, 3);
    if (minutes < 0 || *p != ':') return false;
    ++p;

    const int seconds = ReadNumber(&p, 2);
    if (seconds < 0) return false;

    int fraction = 0;
    if (*p == '.' || *p == ':') {
        ++p;
        // One to three digits. Two digits are centiseconds, three are milliseconds;
        // a single digit is tenths. Real files contain all three.
        int digits = 0;
        int raw = 0;
        while (digits < 3 && *p >= '0' && *p <= '9') {
            raw = raw * 10 + (*p - '0');
            ++p;
            ++digits;
        }
        if (digits == 0) return false;
        if (digits == 1) fraction = raw * 100;
        else if (digits == 2) fraction = raw * 10;
        else fraction = raw;
    }

    if (*p != ']') return false;
    ++p;

    *timeMs = static_cast<long long>(minutes) * 60000 +
              static_cast<long long>(seconds) * 1000 + fraction;
    *cursor = p;
    return true;
}

// Copies `source` (not necessarily terminated) into a fixed field.
void CopyLine(char* destination, size_t capacity, const char* source, size_t length) {
    if (!destination || capacity == 0) return;
    const size_t take = (length < capacity - 1) ? length : capacity - 1;
    if (source && take > 0) std::memcpy(destination, source, take);
    destination[take] = '\0';

    // Trailing spaces and CR are common in real files and only widen the text.
    size_t end = std::strlen(destination);
    while (end > 0 && (destination[end - 1] == ' ' || destination[end - 1] == '\t' ||
                       destination[end - 1] == '\r')) {
        destination[--end] = '\0';
    }
}

// Inserts a line keeping `lines` sorted by time. `count` is updated.
void InsertSorted(Lyrics* lyrics, long long timeMs, const char* text, size_t textLength) {
    if (!lyrics || lyrics->count >= kMaxLyricLines) return;

    // Walk back from the end: the input is nearly always already sorted, so this is
    // one comparison per line in the common case.
    int at = lyrics->count;
    while (at > 0 && lyrics->lines[at - 1].timeMs > timeMs) {
        lyrics->lines[at] = lyrics->lines[at - 1];
        --at;
    }

    lyrics->lines[at].timeMs = timeMs;
    CopyLine(lyrics->lines[at].text, kLyricLineCapacity, text, textLength);
    ++lyrics->count;
}

// --- HTTP -------------------------------------------------------------

// Percent-encodes a UTF-8 string for use as a query value.
void Utf8ToWide(const char* text, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (!text) return;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, static_cast<int>(capacity));
}


} // namespace

void UrlEncode(const char* text, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (!text) return;

    static const wchar_t* kHex = L"0123456789ABCDEF";
    size_t written = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
         *p != '\0'; ++p) {
        const bool unreserved = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                                *p == '.' || *p == '~';
        if (unreserved) {
            if (written + 2 > capacity) break;
            out[written++] = static_cast<wchar_t>(*p);
        } else {
            if (written + 4 > capacity) break;
            out[written++] = L'%';
            out[written++] = kHex[(*p) >> 4];
            out[written++] = kHex[(*p) & 0x0F];
        }
    }
    out[written < capacity ? written : capacity - 1] = L'\0';
}


// Expands the entities a QQ lyric response uses.
//
// QQ serves the lyrics as XML with the LRC inside an attribute, so its line breaks arrive
// as `&#10;` and the text cannot be parsed until they become real newlines.
void DecodeXmlEntities(const char* source, char* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = '\0';
    if (!source) return;

    struct Entity {
        const char* text;
        char value;
    };
    const Entity entities[] = {
        {"&#10;", '\n'}, {"&#13;", '\r'}, {"&lt;", '<'},   {"&gt;", '>'},
        {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'},
    };

    size_t written = 0;
    for (const char* p = source; *p != '\0' && written + 1 < capacity;) {
        bool matched = false;
        for (const Entity& entity : entities) {
            const size_t length = std::strlen(entity.text);
            if (std::strncmp(p, entity.text, length) == 0) {
                out[written++] = entity.value;
                p += length;
                matched = true;
                break;
            }
        }
        if (!matched) out[written++] = *p++;
    }
    out[written] = '\0';
}

namespace {

bool IsJsonSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The three container names these responses use, in the order they appear.
const char* const kLyricContainers[] = {"\"lrc\"", "\"klyric\"", "\"tlyric\""};
constexpr size_t kLyricContainerCount = 3;

// Reads one `"name": "value"` field whose value is a JSON STRING, scanning from `from` up to (not
// including) `until`. An occurrence whose value is an object is skipped, which is what tells the
// container key `"lyric":{...}` apart from the text itself.
//
// True only for a NON-EMPTY value, and the scan continues past empty ones. That distinction is the
// whole point: "the field is there and empty" is what sends the reader on to the translation, while
// "there is no such field" is a different answer.
bool ScanStringField(const char* from, const char* until, const char* name, char* out,
                     size_t capacity) {
    if (!from || !until || !name || !out || capacity == 0) return false;
    out[0] = '\0';

    char quoted[64] = {};
    std::snprintf(quoted, sizeof(quoted), "\"%s\"", name);
    const size_t quotedLength = std::strlen(quoted);

    for (const char* p = from; p < until && (p = std::strstr(p, quoted)) != nullptr && p < until;
         ++p) {
        const char* cursor = p + quotedLength;
        while (IsJsonSpace(*cursor)) ++cursor;
        if (*cursor != ':') continue;
        ++cursor;
        while (IsJsonSpace(*cursor)) ++cursor;
        if (*cursor != '"') continue;   // an object: the container's own key, not the text

        ++cursor;
        // The closing quote, honouring backslash escapes.
        const char* end = cursor;
        while (*end != '\0') {
            if (*end == '\\' && end[1] != '\0') {
                end += 2;
                continue;
            }
            if (*end == '"') break;
            ++end;
        }
        if (*end != '"') return false;   // unterminated: a truncated response

        island::json::Unescape(cursor, static_cast<size_t>(end - cursor), out, capacity);
        if (out[0] != '\0') return true;
    }
    return false;
}

// The first container key in the document, or the end of it.
const char* FirstLyricContainer(const char* document) {
    const char* first = nullptr;
    for (size_t i = 0; i < kLyricContainerCount; ++i) {
        const char* found = std::strstr(document, kLyricContainers[i]);
        if (found && (!first || found < first)) first = found;
    }
    return first ? first : document + std::strlen(document);
}

// The end of a container's scope: the next container key after it, or the end of the document.
//
// Scanned, not brace-matched. The services lay the containers out one after another with no other
// container name between them, while a brace-matched walk would have to step over the braces a lyric
// is free to contain -- that is a JSON parser's job, in a file that deliberately reads these
// responses with a scanner (see the note in Lyrics.h about `json::GetString`).
const char* LyricContainerEnd(const char* containerKey, const char* document) {
    const char* next = nullptr;
    for (size_t i = 0; i < kLyricContainerCount; ++i) {
        const char* found = std::strstr(containerKey + 1, kLyricContainers[i]);
        if (found && (!next || found < next)) next = found;
    }
    return next ? next : document + std::strlen(document);
}

} // namespace

namespace {

// The value of `key` at the FIRST LEVEL of the first object inside the array at `path`, or nullptr.
//
// WHY THE LEVEL MATTERS. These responses are full of repeated keys, and the response's KEY ORDER is
// not fixed. Measured on this machine: NetEase's song object nests an album and an artist that each
// carry an `id`, and `晴天` came back with `"artist":{...,"id":0,...}` BEFORE the song's own id -- so
// "the first `id` after `songs`" returned 0, the caller rejected it, and the bar reported "the search
// found no matching track" for a response that had the song sitting in it. The same track's `mid` was
// taken from the wrong nesting by QQ's reader and refused by the service with `retcode 1101`.
// Another track (Maple Road) happened to have the song's own id FIRST, which is why the old reader
// worked for one track and failed the next.
//
// `path` names the containers in order -- {"songs"} for NetEase, {"song","list"} for QQ, whose
// response has three `list` arrays and the first two are EMPTY, so "the first list" is the wrong
// array as well as the wrong level. Depth is counted in braces with quoted strings skipped, so a
// value that happens to contain `"id":` cannot be mistaken for a key.
const char* FlatFieldInArrayObject(const char* document, const char* const* path, size_t pathLength,
                                   const char* key) {
    if (!document || !path || pathLength == 0 || !key) return nullptr;

    const char* cursor = document;
    for (size_t i = 0; i < pathLength; ++i) {
        char needle[64] = {};
        std::snprintf(needle, sizeof(needle), "\"%s\"", path[i]);
        const char* at = std::strstr(cursor, needle);
        if (!at) return nullptr;
        // Enter whatever this key opens: an object for an intermediate step, the array for the last.
        const char* open = at + std::strlen(needle);
        while (*open && *open != '{' && *open != '[') ++open;
        if (!*open) return nullptr;
        cursor = open + 1;
    }

    // The first object of the array, then its own level only.
    const char* p = cursor;
    while (*p && *p != '{' && *p != ']') ++p;
    if (*p != '{') return nullptr;

    char want[64] = {};
    std::snprintf(want, sizeof(want), "\"%s\"", key);
    const size_t wantLength = std::strlen(want);

    int depth = 0;
    while (*p) {
        if (*p == '"') {
            const char* q = p + 1;
            while (*q && *q != '"') {
                if (*q == '\\' && q[1] != '\0') {
                    q += 2;
                } else {
                    ++q;
                }
            }
            if (*q != '"') return nullptr;
            // `q - p + 1`, because p is the OPENING quote and q the closing one: the token is one
            // character longer than their difference. Compared as `q - p` first, this matched nothing
            // at all -- every lookup returned "not found", including on the simplest possible
            // document -- which the tests below now catch in their easiest case, not just the hard one.
            if (depth == 1 && static_cast<size_t>(q - p + 1) == wantLength &&
                std::strncmp(p, want, wantLength) == 0) {
                const char* value = q + 1;
                while (IsJsonSpace(*value)) ++value;
                if (*value != ':') return nullptr;
                ++value;
                while (IsJsonSpace(*value)) ++value;
                return value;
            }
            p = q + 1;
            continue;
        }
        if (*p == '{' || *p == '[') {
            ++depth;
            ++p;
            continue;
        }
        if (*p == '}' || *p == ']') {
            --depth;
            if (depth <= 0) return nullptr;   // left the object without finding the key
            ++p;
            continue;
        }
        ++p;
    }
    return nullptr;
}

} // namespace

bool FindFlatStringInArray(const char* document, const char* const* path, size_t pathLength,
                           const char* key, char* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';

    const char* value = FlatFieldInArrayObject(document, path, pathLength, key);
    if (!value || *value != '"') return false;
    ++value;
    const char* end = value;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1] != '\0') {
            end += 2;
        } else {
            ++end;
        }
    }
    if (*end != '"') return false;
    island::json::Unescape(value, static_cast<size_t>(end - value), out, capacity);
    return out[0] != '\0';
}

bool FindFlatIntegerInArray(const char* document, const char* const* path, size_t pathLength,
                            const char* key, long long* out) {
    if (!out) return false;
    const char* value = FlatFieldInArrayObject(document, path, pathLength, key);
    if (!value) return false;

    if (*value == '"') ++value;   // a quoted number is still a number here
    const bool negative = (*value == '-');
    if (negative) ++value;
    if (*value < '0' || *value > '9') return false;

    long long parsed = 0;
    while (*value >= '0' && *value <= '9') {
        parsed = parsed * 10 + (*value - '0');
        ++value;
    }
    *out = negative ? -parsed : parsed;
    return true;
}

bool ExtractJsonLyricField(const char* document, const char* container, char* out, size_t capacity) {
    if (!document || !container || !out || capacity == 0) return false;
    out[0] = '\0';

    char key[64] = {};
    std::snprintf(key, sizeof(key), "\"%s\"", container);
    const char* at = std::strstr(document, key);
    if (!at) return false;
    return ScanStringField(at, LyricContainerEnd(at, document), "lyric", out, capacity);
}

bool ExtractLyricField(const char* document, char* out, size_t capacity, const char** outSource) {
    if (!document || !out || capacity == 0) return false;
    out[0] = '\0';
    if (outSource) *outSource = nullptr;

    // 1. the original, 2. the translation, 3. the flat shape (QQ).
    //
    // The karaoke field (`klyric`) is deliberately NOT read: it carries the words with per-character
    // timing inside the text ("ka<00:01.5>ra"), which our LRC reader would show verbatim. It is only
    // reached by step 3 if a document has no containers at all, which is not a shape either service
    // sends -- step 3 is bounded to the text BEFORE the first container for exactly that reason.
    if (ExtractJsonLyricField(document, "lrc", out, capacity)) {
        if (outSource) *outSource = "lrc";
        return true;
    }
    if (ExtractJsonLyricField(document, "tlyric", out, capacity)) {
        if (outSource) *outSource = "tlyric";
        return true;
    }
    if (ScanStringField(document, FirstLyricContainer(document), "lyric", out, capacity)) {
        if (outSource) *outSource = "lyric";
        return true;
    }
    out[0] = '\0';
    return false;
}

// Pulls the LRC out of a QQ lyric response, which is XML with the text in an attribute.
bool ExtractQqXmlLyric(const char* document, char* out, size_t capacity) {
    if (!document || !out || capacity == 0) return false;
    out[0] = '\0';

    const char* marker = std::strstr(document, "lyric=\"");
    if (!marker) return false;
    const char* start = marker + 7;

    const char* end = start;
    while (*end != '\0' && *end != '"') ++end;
    if (*end != '"' || end == start) return false;

    static char raw[512 * 1024];
    const size_t length = static_cast<size_t>(end - start);
    const size_t take = (length < sizeof(raw) - 1) ? length : sizeof(raw) - 1;
    std::memcpy(raw, start, take);
    raw[take] = '\0';

    DecodeXmlEntities(raw, out, capacity);
    return out[0] != '\0';
}


// --- Lyrics -----------------------------------------------------------

int Lyrics::LineAt(long long positionMs) const {
    if (count == 0) return -1;

    // Before the first line there is nothing to show: an instrumental intro would
    // otherwise display the first lyric for its whole duration.
    if (positionMs < lines[0].timeMs) return -1;

    // THE LAST LINE WITH WORDS, and an empty line is SKIPPED rather than shown.
    //
    // NetEase's LRCs mark the end of a phrase with a time tag and no text, and they are everywhere:
    // measured on 满江《归来》, 37 of its 88 lines are such markers, one of them 1.4 s after "我的心"
    // began. Returning the marker made the current line EMPTY, and the frame loop reads an empty line as
    // "no lyric" and falls back to the SONG TITLE -- reported as "如果一句唱的慢的话 会显示歌曲名 然后唱
    // 下一句的时候 歌词才能对上". Skipping them keeps the words that were just sung on the pill until
    // the next ones arrive, which is what every player does and what the pill is for.
    //
    // The line is still KEPT in the array (its emptiness is the marker's whole meaning, and the parser's
    // test says so); it is only never the answer to "what is being sung now".
    //
    // Linear scan from the end. A song has at most a few hundred lines and the position moves forward, so
    // this is cheaper than a binary search in practice and has no branch-heavy setup.
    int best = -1;
    for (int i = 0; i < count; ++i) {
        if (lines[i].timeMs > positionMs) {
            break;
        }
        if (lines[i].text[0] != '\0') {
            best = i;
        }
    }
    return best;
}

const LyricLine* Lyrics::At(int index) const {
    if (index < 0 || index >= count) return nullptr;
    return &lines[index];
}



int ParseLrc(const char* lrc, Lyrics* out) {
    if (!out) return 0;
    *out = Lyrics{};
    if (!lrc) return 0;

    const char* cursor = lrc;
    // `[offset:N]`, applied at the end (see the tag's handler).
    long long offsetMs = 0;

    // A UTF-8 BOM is common on files exported by Windows tools.
    if (static_cast<unsigned char>(cursor[0]) == 0xEF &&
        static_cast<unsigned char>(cursor[1]) == 0xBB &&
        static_cast<unsigned char>(cursor[2]) == 0xBF) {
        cursor += 3;
    }

    while (*cursor != '\0') {
        // One physical line.
        const char* lineEnd = cursor;
        while (*lineEnd != '\0' && *lineEnd != '\n') ++lineEnd;

        const char* p = cursor;

        // Collect every time tag at the head of the line: `[00:01.00][00:05.00]text`
        // means the same text at both times, which real files use for repeats.
        long long times[16] = {};
        int timeCount = 0;
        while (TryReadTimeTag(&p, &times[timeCount])) {
            ++timeCount;
            if (timeCount >= 16) break;
        }

        if (timeCount > 0) {
            // Everything after the last tag is the text.
            const char* text = p;
            size_t textLength = static_cast<size_t>(lineEnd - text);
            for (int i = 0; i < timeCount; ++i) {
                InsertSorted(out, times[i], text, textLength);
            }
        } else if (*p == '[') {
            // A metadata tag such as [ti:...] or [ar:...]. Only two are kept; the
            // rest (album, offset, by) are ignored rather than stored unused.
            const char* tag = p + 1;
            const char* colon = tag;
            while (*colon != '\0' && *colon != ':' && *colon != ']' && colon < lineEnd) ++colon;
            if (*colon == ':') {
                const size_t keyLength = static_cast<size_t>(colon - tag);
                const char* value = colon + 1;
                const char* valueEnd = value;
                while (valueEnd < lineEnd && *valueEnd != ']') ++valueEnd;
                const size_t valueLength = static_cast<size_t>(valueEnd - value);

                if (keyLength == 2 && (tag[0] == 't' || tag[0] == 'T') &&
                    (tag[1] == 'i' || tag[1] == 'I')) {
                    CopyLine(out->title, kLyricLineCapacity, value, valueLength);
                } else if (keyLength == 2 && (tag[0] == 'a' || tag[0] == 'A') &&
                           (tag[1] == 'r' || tag[1] == 'R')) {
                    CopyLine(out->artist, kLyricLineCapacity, value, valueLength);
                } else if (keyLength == 6 && (tag[0] == 'o' || tag[0] == 'O')) {
                    // `[offset:N]` -- the whole timeline shifted by N milliseconds, which the comment
                    // here used to write off ("the rest ... are ignored rather than stored unused").
                    // It is not decoration: an LRC carrying an offset is off by that much for the WHOLE
                    // song, which is exactly "歌词应该和当前唱的一样才对" when the offset is a second or
                    // more. Applied to every line at the end, because the tag may come after the lines.
                    long long parsed = 0;
                    bool negative = false;
                    const char* digit = value;
                    if (digit < valueEnd && (*digit == '-' || *digit == '+')) {
                        negative = (*digit == '-');
                        ++digit;
                    }
                    bool any = false;
                    for (; digit < valueEnd && *digit >= '0' && *digit <= '9'; ++digit) {
                        parsed = parsed * 10 + (*digit - '0');
                        any = true;
                    }
                    if (any) offsetMs = negative ? -parsed : parsed;
                }
            }
        }
        // A line with neither a time tag nor metadata is ignored: text before the
        // first tag has no position to attach to.

        cursor = (*lineEnd == '\n') ? lineEnd + 1 : lineEnd;
    }

    // The offset last, because `[offset:..]` can sit anywhere in the file. A uniform shift cannot
    // disturb the order, so nothing has to be re-sorted; times that would go negative clamp to zero.
    if (offsetMs != 0) {
        for (int i = 0; i < out->count; ++i) {
            const long long shifted = out->lines[i].timeMs + offsetMs;
            out->lines[i].timeMs = shifted > 0 ? shifted : 0;
        }
    }

    return out->count;
}

// Whether a service's search hit is the track that is playing. See the header for why this exists.
bool LyricsLooksLike(const char* requestedTitle, const char* requestedArtist,
                     const char* foundTitle, const char* foundArtist) {
    const auto normalised = [](const char* text) {
        std::string out;
        if (!text) return out;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
            if (std::isspace(*p)) continue;
            out.push_back(static_cast<char>(std::tolower(*p)));
        }
        return out;
    };
    // Either side containing the other counts: services append "(Live)", "(电视剧...)" and the like, and
    // a session's title is sometimes the bare name. An empty side is "did not say" rather than a mismatch.
    const auto agree = [](const std::string& a, const std::string& b) {
        if (a.empty() || b.empty()) return true;
        return a == b || a.find(b) != std::string::npos || b.find(a) != std::string::npos;
    };
    const std::string requestedName = normalised(requestedTitle);
    const std::string foundName = normalised(foundTitle);
    if (!requestedName.empty() && !foundName.empty() && !agree(requestedName, foundName)) {
        return false;
    }
    // The artist only when both sides named one: a soundtrack can credit "群星" while the session reports
    // the performer, and that is not a different track.
    const std::string requestedSinger = normalised(requestedArtist);
    const std::string foundSinger = normalised(foundArtist);
    if (!requestedSinger.empty() && !foundSinger.empty() &&
        !agree(requestedSinger, foundSinger)) {
        return false;
    }
    return true;
}

// --- the two services -------------------------------------------------
//
// Two, tried in order, because they fail independently: a network that blocks or cannot
// reach one will often reach the other, and the first was reported as producing no lyrics
// at all. Each reports why it failed, so "no lyrics" is never the only thing the log says.

bool FetchFromNetEase(const char* title, const char* artist, int timeoutMs, Lyrics* out,
                      char* diagnostic, size_t diagnosticCapacity,
                      LyricsFetchReport::Attempt* report) {
    const auto explain = [&](const char* reason) {
        if (diagnostic && diagnosticCapacity > 0) {
            strncpy_s(diagnostic, diagnosticCapacity, reason, _TRUNCATE);
        }
    };

    char query[512] = {};
    if (artist && artist[0] != '\0') {
        std::snprintf(query, sizeof(query), "%s %s", title, artist);
    } else {
        std::snprintf(query, sizeof(query), "%s", title);
    }

    wchar_t encodedQuery[2048] = {};
    UrlEncode(query, encodedQuery, 2048);

    // Step 1: find the song id.
    //
    // The plain search endpoint is used rather than the newer one: it needs no
    // authentication, returns JSON with a flat `songs` array, and has been stable for
    // years. The Referer is required -- without it the service answers 403.
    wchar_t searchPath[3072] = {};
    swprintf(searchPath, 3072, L"/api/search/get/?s=%ls&type=1&limit=1&offset=0",
             encodedQuery);

    static char searchBody[256 * 1024];
    size_t searchLength = 0;
    DWORD searchStatus = 0;
    const bool searchOk =
        HttpGet(L"music.163.com", searchPath, L"https://music.163.com/",
                timeoutMs, searchBody, sizeof(searchBody), &searchLength,
                &searchStatus);
    if (report) {
        report->searchStatus = searchStatus;
        std::snprintf(report->searchUrl, sizeof(report->searchUrl), "%ls",
                      searchPath);
        const size_t take = (searchLength < sizeof(report->searchBody) - 1)
                                ? searchLength
                                : sizeof(report->searchBody) - 1;
        if (searchLength > 0) std::memcpy(report->searchBody, searchBody, take);
        report->searchBody[take] = '\0';
    }
    if (!searchOk) {
        if (searchStatus == 0) {
            explain("netease: TRANSIENT: search could not connect (offline, blocked, or TLS refused)");
        } else {
            static char detail[96];
            std::snprintf(detail, sizeof(detail), "netease: TRANSIENT: search returned HTTP %lu",
                          searchStatus);
            explain(detail);
        }
        return false;
    }
    if (searchLength == 0) {
        explain("netease: TRANSIENT: search returned an empty body");
        return false;
    }

    long long songId = 0;
    long long songDurationMs = 0;
    {
        // The song's OWN id -- not the album's and not the artist's. See FlatFieldInArrayObject:
        // the previous reader took the first `id` after `songs`, which for `晴天` was an artist's
        // `"id":0`, and the bar then reported "no matching track" over a response with the song in it.
        static const char* const kSongsPath[] = {"songs"};
        if (!FindFlatIntegerInArray(searchBody, kSongsPath, 1, "id", &songId) || songId <= 0) {
            // TRANSIENT and not "no matching track": the search may well have answered with a track,
            // and "the id could not be read" and "there is no such song" need opposite fixes.
            explain("netease: TRANSIENT: the search response had no usable song id");
            return false;
        }
        // ... and how long the catalogue says that track is, out of the SAME response.
        //
        // This is the number the card prints and the bar fills against, and it exists only because the
        // session does not always supply one: NetEase Cloud Music reports `EndTime 00:00` for the whole
        // song (see TrackPositionFor), so the card had no total and its bar could not fill. Measured
        // response for `偷心 糯米Nomi`: `"duration":243714` (4:03) right beside the `"id"` this already
        // reads -- no extra request, and the track's real length rather than a guess from the LRC.
        if (!FindFlatIntegerInArray(searchBody, kSongsPath, 1, "duration", &songDurationMs) ||
            songDurationMs <= 0) {
            songDurationMs = 0;   // it did not say; the card then prints `--:--` instead of a number
        }

        // THE FIRST HIT IS NOT ALWAYS THIS SONG -- see LyricsLooksLike. Both names are already in this
        // response, so the check costs nothing and saves the lyric request for the wrong track.
        char foundName[192] = {};
        char foundSinger[192] = {};
        static const char* const kArtistsPath[] = {"songs", "artists"};
        FindFlatStringInArray(searchBody, kSongsPath, 1, "name", foundName, sizeof(foundName));
        FindFlatStringInArray(searchBody, kArtistsPath, 2, "name", foundSinger, sizeof(foundSinger));
        if (!LyricsLooksLike(title, artist, foundName, foundSinger)) {
            explain("netease: the search found a different track");
            return false;
        }
    }

    // Step 2: ask for that id's lyrics.
    wchar_t lyricPath[512] = {};
    swprintf(lyricPath, 512, L"/api/song/lyric/?id=%lld&lv=1&kv=1&tv=-1", songId);

    static char lyricBody[512 * 1024];
    size_t lyricLength = 0;
    DWORD lyricStatus = 0;
    const bool lyricOk =
        HttpGet(L"music.163.com", lyricPath, L"https://music.163.com/",
                timeoutMs, lyricBody, sizeof(lyricBody), &lyricLength,
                &lyricStatus);
    if (report) {
        report->lyricStatus = lyricStatus;
        std::snprintf(report->lyricUrl, sizeof(report->lyricUrl), "%ls", lyricPath);
        const size_t take = (lyricLength < sizeof(report->lyricBody) - 1)
                                ? lyricLength
                                : sizeof(report->lyricBody) - 1;
        if (lyricLength > 0) std::memcpy(report->lyricBody, lyricBody, take);
        report->lyricBody[take] = '\0';
    }
    if (!lyricOk) {
        static char detail[96];
        std::snprintf(detail, sizeof(detail), "netease: TRANSIENT: lyric returned HTTP %lu",
                      lyricStatus);
        explain(detail);
        return false;
    }

    static char lrc[512 * 1024];
    const char* fromField = nullptr;
    if (!ExtractLyricField(lyricBody, lrc, sizeof(lrc), &fromField)) {
        explain("netease: TRANSIENT: the response had no lyric field");
        return false;
    }
    // Said out loud, because it changes what the strip shows: the words on screen are a TRANSLATION.
    // This is the case that never worked -- `lrc` empty, everything under `tlyric` -- and the reason
    // the bar reported "no lyrics" for a song whose lyrics it had already downloaded.
    if (fromField && std::strcmp(fromField, "tlyric") == 0) {
        LogLine(L"  lyrics: netease sent no original -- using its TRANSLATION");
    }
    if (lrc[0] == '\0') {
        explain("netease: that track has no lyrics (an instrumental)");
        return false;
    }
    if (ParseLrc(lrc, out) == 0) {
        explain("netease: TRANSIENT: the lyric text held no timed lines");
        return false;
    }
    explain("ok (netease)");
    out->trackDurationMs = songDurationMs;
    return true;
}

// QQ Music.
//
// Two steps again, and both need a Referer from a QQ Music page or the service answers
// 403. The lyric response is JSON *or* XML with the LRC inside an attribute, and its line
// breaks arrive as `&#10;`, so that form needs entity decoding rather than JSON unescaping.
bool FetchFromQqMusic(const char* title, const char* artist, int timeoutMs, Lyrics* out,
                      char* diagnostic, size_t diagnosticCapacity,
                      LyricsFetchReport::Attempt* report) {
    const auto explain = [&](const char* reason) {
        if (diagnostic && diagnosticCapacity > 0) {
            strncpy_s(diagnostic, diagnosticCapacity, reason, _TRUNCATE);
        }
    };

    char query[512] = {};
    if (artist && artist[0] != '\0') {
        std::snprintf(query, sizeof(query), "%s %s", title, artist);
    } else {
        std::snprintf(query, sizeof(query), "%s", title);
    }

    // Step 1: find the song's `mid`, QQ's stable identifier.
    //
    // The `data` payload is JSON that has to be percent-encoded as a query value; sending
    // it raw makes the service reject the request, which was one of the things wrong with
    // the first attempt at this.
    char searchJson[1024] = {};
    std::snprintf(searchJson, sizeof(searchJson),
                  "{\"req_0\":{\"module\":\"music.search.SearchCgiService\",\"method\":"
                  "\"DoSearchForQQMusicDesktop\",\"param\":{\"query\":\"%s\","
                  "\"num_per_page\":1,\"page_num\":1}}}",
                  query);

    wchar_t encodedSearch[4096] = {};
    UrlEncode(searchJson, encodedSearch, 4096);

    wchar_t searchPath[6144] = {};
    swprintf(searchPath, 6144, L"/cgi-bin/musicu.fcg?format=json&data=%ls", encodedSearch);

    static char searchBody[256 * 1024];
    size_t searchLength = 0;
    DWORD searchStatus = 0;
    const bool searchOk =
        HttpGet(L"u.y.qq.com", searchPath, L"https://y.qq.com/", timeoutMs,
                searchBody, sizeof(searchBody), &searchLength, &searchStatus);
    if (report) {
        report->searchStatus = searchStatus;
        std::snprintf(report->searchUrl, sizeof(report->searchUrl), "%ls", searchPath);
        const size_t take = (searchLength < sizeof(report->searchBody) - 1)
                                ? searchLength
                                : sizeof(report->searchBody) - 1;
        if (searchLength > 0) std::memcpy(report->searchBody, searchBody, take);
        report->searchBody[take] = '\0';
    }
    if (!searchOk) {
        if (searchStatus == 0) {
            explain("qq: TRANSIENT: search could not connect (offline, blocked, or TLS refused)");
        } else {
            static char detail[96];
            std::snprintf(detail, sizeof(detail), "qq: TRANSIENT: search returned HTTP %lu",
                          searchStatus);
            explain(detail);
        }
        return false;
    }

    char songMid[128] = {};
    long long songDurationMs = 0;
    // `song.list` and not `list`: this response carries three `list` arrays and the first two are
    // EMPTY (the album's and the singer's), so "the first list" is the wrong array as well as the
    // wrong level. Measured: the old reader took a mid that the service then refused with
    // `retcode 1101`.
    static const char* const kSongListPath[] = {"song", "list"};
    if (!FindFlatStringInArray(searchBody, kSongListPath, 2, "mid", songMid, sizeof(songMid))) {
        explain("qq: TRANSIENT: the search response had no usable song mid");
        return false;
    }
    // The track's length, from the SAME response, for the same reason NetEase's is read: the card needs
    // a total to print and a bar to fill against, and a session that sends no timeline has none. QQ
    // states it in SECONDS (`interval`), so it is converted here rather than at every reader.
    {
        long long seconds = 0;
        if (FindFlatIntegerInArray(searchBody, kSongListPath, 2, "interval", &seconds) && seconds > 0) {
            songDurationMs = seconds * 1000;
        }
    }

    // ... and the same check on QQ's own response, before its lyric request (see LyricsLooksLike).
    {
        char foundName[192] = {};
        char foundSinger[192] = {};
        static const char* const kSingerPath[] = {"song", "list", "singer"};
        FindFlatStringInArray(searchBody, kSongListPath, 2, "name", foundName, sizeof(foundName));
        FindFlatStringInArray(searchBody, kSingerPath, 3, "name", foundSinger, sizeof(foundSinger));
        if (!LyricsLooksLike(title, artist, foundName, foundSinger)) {
            explain("qq: the search found a different track");
            return false;
        }
    }

    wchar_t wideMid[128] = {};
    Utf8ToWide(songMid, wideMid, 128);

    // Step 2: ask for that mid's lyrics.
    wchar_t lyricPath[1024] = {};
    swprintf(lyricPath, 1024,
             L"/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=%ls&format=json&nobase64=1",
             wideMid);

    static char lyricBody[512 * 1024];
    size_t lyricLength = 0;
    DWORD lyricStatus = 0;
    const bool lyricOk =
        HttpGet(L"c.y.qq.com", lyricPath, L"https://y.qq.com/", timeoutMs,
                lyricBody, sizeof(lyricBody), &lyricLength, &lyricStatus);
    if (report) {
        report->lyricStatus = lyricStatus;
        std::snprintf(report->lyricUrl, sizeof(report->lyricUrl), "%ls", lyricPath);
        const size_t take = (lyricLength < sizeof(report->lyricBody) - 1)
                                ? lyricLength
                                : sizeof(report->lyricBody) - 1;
        if (lyricLength > 0) std::memcpy(report->lyricBody, lyricBody, take);
        report->lyricBody[take] = '\0';
    }
    if (!lyricOk) {
        static char detail[96];
        std::snprintf(detail, sizeof(detail), "qq: TRANSIENT: lyric returned HTTP %lu", lyricStatus);
        explain(detail);
        return false;
    }

    static char lrc[512 * 1024];
    const char* fromField = nullptr;
    if (!ExtractLyricField(lyricBody, lrc, sizeof(lrc), &fromField)) {
        if (!ExtractQqXmlLyric(lyricBody, lrc, sizeof(lrc))) {
            explain("qq: TRANSIENT: the response had no lyric field");
            return false;
        }
    }
    if (lrc[0] == '\0') {
        explain("qq: that track has no lyrics (an instrumental)");
        return false;
    }
    if (ParseLrc(lrc, out) == 0) {
        explain("qq: TRANSIENT: the lyric text held no timed lines");
        return false;
    }
    explain("ok (qq)");
    out->trackDurationMs = songDurationMs;
    return true;
}

bool FetchLyricsFromNetwork(const char* title, const char* artist, int timeoutMs,
                            Lyrics* out, char* diagnostic, size_t diagnosticCapacity) {
    return FetchLyricsVerbose(title, artist, timeoutMs, out, diagnostic,
                              diagnosticCapacity, nullptr);
}

// Reads the TRANSIENT marker the providers put in their reason.
//
// A marker in the text rather than a second return value: the providers already report a
// reason string, and threading a parallel out-parameter through both of them for one bit
// would be more places to get wrong.
bool ReasonIsTransient(const char* reason) {
    return reason && std::strstr(reason, "TRANSIENT") != nullptr;
}

FetchOutcome FetchLyricsDetailed(const char* title, const char* artist, int timeoutMs,
                                 Lyrics* out, char* diagnostic, size_t diagnosticCapacity) {
    FetchOutcome outcome;
    char reason[384] = {};
    outcome.parsed = FetchLyricsVerbose(title, artist, timeoutMs, out, reason, sizeof(reason),
                                        nullptr);
    outcome.transientFailure = !outcome.parsed && ReasonIsTransient(reason);
    if (diagnostic && diagnosticCapacity > 0) {
        strncpy_s(diagnostic, diagnosticCapacity, reason, _TRUNCATE);
    }
    return outcome;
}

bool FetchLyricsVerbose(const char* title, const char* artist, int timeoutMs, Lyrics* out,
                        char* diagnostic, size_t diagnosticCapacity,
                        LyricsFetchReport* report) {
    if (!out || !title || title[0] == '\0') {
        if (diagnostic && diagnosticCapacity > 0) {
            strncpy_s(diagnostic, diagnosticCapacity, "no track title", _TRUNCATE);
        }
        return false;
    }
    if (report) *report = LyricsFetchReport{};

    // Try both services, in order. Each writes its own reason, so the caller ends up with a
    // real explanation rather than a generic failure.

    if (FetchFromNetEase(title, artist, timeoutMs, out, diagnostic, diagnosticCapacity,
                         report ? &report->netease : nullptr)) {
        if (report) {
            strncpy_s(report->netease.verdict, sizeof(report->netease.verdict), "ok", _TRUNCATE);
        }
        return true;
    }
    char neteaseReason[192] = {};
    if (diagnostic && diagnosticCapacity > 0) {
        strncpy_s(neteaseReason, sizeof(neteaseReason), diagnostic, _TRUNCATE);
    }
    if (report) {
        strncpy_s(report->netease.verdict, sizeof(report->netease.verdict),
                  diagnostic && diagnostic[0] ? diagnostic : "netease: failed", _TRUNCATE);
    }

    // Each service writes its OWN record, so nothing is overwritten and nothing has to be merged.
    //
    // There used to be a single record plus a "keep whichever service produced more evidence" rule,
    // and the rule is what made the next diagnosis wrong: the report showed QQ's URLs and bodies
    // while the reason named NetEase first, so a 200-with-results was read as "the search found no
    // matching track".
    if (FetchFromQqMusic(title, artist, timeoutMs, out, diagnostic, diagnosticCapacity,
                         report ? &report->qq : nullptr)) {
        if (report) {
            strncpy_s(report->qq.verdict, sizeof(report->qq.verdict), "ok", _TRUNCATE);
        }
        return true;
    }
    if (report) {
        strncpy_s(report->qq.verdict, sizeof(report->qq.verdict),
                  diagnostic && diagnostic[0] ? diagnostic : "qq: failed", _TRUNCATE);
    }

    // Both failed: report both, because one being blocked and the other having no lyrics
    // are different problems and the log should not hide either.
    if (diagnostic && diagnosticCapacity > 0) {
        char combined[384] = {};
        std::snprintf(combined, sizeof(combined), "%s; then %s",
                      neteaseReason[0] ? neteaseReason : "netease: failed",
                      diagnostic[0] ? diagnostic : "qq: failed");
        strncpy_s(diagnostic, diagnosticCapacity, combined, _TRUNCATE);
    }
    return false;
}

// --- provider ---------------------------------------------------------

LyricsProvider::~LyricsProvider() {
    Stop();
}

bool LyricsProvider::Start() {
    if (running_) return true;

    requestEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Stop() ends whatever exists rather than unwinding by hand (see platform/WorkerThread.h).
    if (!worker_.Prepare() || !requestEvent_) {
        Stop();
        return false;
    }

    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, &LyricsProvider::ThreadEntry, this, 0, &threadId);
    if (!thread) {
        Stop();
        return false;
    }
    worker_.Attach(thread);

    running_ = true;
    return true;
}

void LyricsProvider::Stop() {
    // The worker may be inside a network call. It checks the stop event between requests, and its HTTP
    // timeouts bound how long a single one can take, so this wait is finite rather than a hang -- which is
    // why the timeout here is 5000 ms and not the 3000 ms the local readers use.
    worker_.Stop(5000);
    if (requestEvent_) {
        CloseHandle(requestEvent_);
        requestEvent_ = nullptr;
    }
    running_ = false;
}

void LyricsProvider::Request(const char* title, const char* artist) {
    if (!running_ || !title || title[0] == '\0') return;

    lock_.Enter();
    // Already asked about this track -- but only cache a PERMANENT answer.
    //
    // Caching everything was a bug with a nasty shape: a network hiccup, or a build whose
    // queries were malformed, produced "no lyrics" once and that answer was then remembered
    // for the whole life of the track. Lyrics that had worked stopped working and no amount
    // of restarting the music helped. A transient failure is retried, with a cap so a
    // genuinely offline machine does not spin.
    const bool sameTrack =
        haveAsked_ && std::strcmp(title, requestedTitle) == 0 &&
        std::strcmp(artist ? artist : "", requestedArtist) == 0;
    const bool permanentAnswer = sameTrack && !lastAnswerWasTransient_;
    const bool retryBudgetLeft = sameTrack ? (attemptCount_ < kMaxAttemptsPerTrack) : true;
    if (permanentAnswer || !retryBudgetLeft) {
        lock_.Leave();
        return;
    }
    // A different track starts its own budget.
    if (!sameTrack) attemptCount_ = 0;

    strncpy_s(requestedTitle, sizeof(requestedTitle), title, _TRUNCATE);
    strncpy_s(requestedArtist, sizeof(requestedArtist), artist ? artist : "",
                   _TRUNCATE);
    haveAsked_ = true;
    ++attemptCount_;
    InterlockedExchange(&requestPending_, 1);
    lock_.Leave();

    SetEvent(requestEvent_);
}

const Lyrics* LyricsProvider::Current() const {
    if (!worker_.Prepared() || !haveLyrics_) return nullptr;
    // Deliberately no lock. The pointer refers to a member the worker replaces only
    // when a fetch completes, and the frame loop reads it within a single frame, so
    // the worst a race can do is show the previous track's line for one frame --
    // against taking the lock on every frame forever.
    return &lyrics_;
}

bool LyricsProvider::Snapshot(Lyrics* out) const {
    if (!out || !worker_.Prepared()) return false;
    bool have = false;
    lock_.Enter();
    if (haveLyrics_) {
        *out = lyrics_;
        have = true;
    }
    lock_.Leave();
    return have;
}

unsigned long long LyricsProvider::Revision() const {
    return static_cast<unsigned long long>(revision_);
}

unsigned long __stdcall LyricsProvider::ThreadEntry(void* self) {
    static_cast<LyricsProvider*>(self)->Run();
    return 0;
}

void LyricsProvider::Run() {
    char title[256] = {};
    char artist[256] = {};

    while (true) {
        HANDLE waits[2] = { worker_.StopEvent(), requestEvent_ };
        const DWORD signalled = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (signalled == WAIT_OBJECT_0) break;   // stop
        if (signalled != WAIT_OBJECT_0 + 1) break;

        // Take the request, and only the fields needed for it, so a new request
        // arriving mid-fetch cannot change what this one is fetching.
        lock_.Enter();
        haveAsked_ = true;
        strncpy_s(title, sizeof(title), requestedTitle, _TRUNCATE);
        strncpy_s(artist, sizeof(artist), requestedArtist, _TRUNCATE);
        InterlockedExchange(&requestPending_, 0);
        lock_.Leave();

        if (title[0] == '\0') continue;

        Lyrics fetched;
        // Ten seconds: long enough for a slow connection, short enough that the
        // worker cannot be stuck in one request for the rest of the session.
        char reason[384] = {};
        const FetchOutcome outcome =
            FetchLyricsDetailed(title, artist, 10000, &fetched, reason, sizeof(reason));

        if (WaitForSingleObject(worker_.StopEvent(), 0) == WAIT_OBJECT_0) break;

        lock_.Enter();
        // Recorded so Request() knows whether this answer may be cached. A transient
        // failure -- a hiccup, an offline moment, a malformed request -- must not be
        // remembered as "this track has no lyrics" for the rest of its life.
        lastAnswerWasTransient_ = outcome.transientFailure;
        if (outcome.parsed) {
            lyrics_ = fetched;
            haveLyrics_ = true;
            InterlockedIncrement(&revision_);
        } else {
            // No lyrics is a normal outcome. Clear the previous track's so the card
            // does not show the wrong song's words.
            lyrics_ = Lyrics{};
            haveLyrics_ = false;
            InterlockedIncrement(&revision_);
        }
        lock_.Leave();
    }

    running_ = false;
}

} // namespace island
