// =====================================================================
//  MusicSession.cpp
// =====================================================================
#include "media/MusicSession.h"
#include "core/Log.h"

#include <roapi.h>
#include <shellapi.h>   // SHGetFileInfoW, for the source application's icon



#include "media/WicImage.h"
#include "platform/IconRaster.h"   // an HICON to pixels: the same route the renderer uploads with

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
// GetSessions() returns a collection view, whose Size()/GetAt() are only declared in the
// Collections header -- the same "must define this function before using it" trap as the
// Storage.Streams one.
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
// The album-art path reads a stream. Media.Control alone only forward-declares
// these, so calling OpenReadAsync/LoadAsync without them fails to compile with
// "must define this function before using it".
#include <winrt/Windows.Storage.Streams.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>   // towlower, for the app-name comparison

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "version.lib")   // GetFileVersionInfoW, for the app's display name
#pragma comment(lib, "user32.lib")

namespace island {
namespace {

// How often the worker asks the shell what is playing.
//
// The poll interval itself is in the header (`kPollIntervalMs`), together with the shorter one that follows a
// skip -- see NextPollIntervalMs there for why the two exist.

// Copies a WinRT string into a fixed UTF-8 field. Truncates rather than
// overflowing; a long title is not a reason to lose the notification.
void CopyUtf8(const winrt::hstring& source, char* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = '\0';
    if (source.empty()) return;

    const int needed = WideCharToMultiByte(CP_UTF8, 0, source.c_str(),
                                           static_cast<int>(source.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return;

    if (static_cast<size_t>(needed) < capacity) {
        WideCharToMultiByte(CP_UTF8, 0, source.c_str(), static_cast<int>(source.size()),
                            out, needed, nullptr, nullptr);
        out[needed] = '\0';
        return;
    }

    // Too long: convert as much as fits, being careful not to split a multi-byte
    // character. Cutting mid-character would produce invalid UTF-8 that the text
    // renderer would then decline to draw.
    int room = static_cast<int>(capacity) - 1;
    while (room > 0) {
        const int produced = WideCharToMultiByte(CP_UTF8, 0, source.c_str(),
                                                 static_cast<int>(source.size()),
                                                 out, room, nullptr, nullptr);
        if (produced > 0) {
            out[produced] = '\0';
            return;
        }
        --room;   // back off until the last character fits whole
    }
    out[0] = '\0';
}

PlaybackState ConvertStatus(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus status) {
    using Status = winrt::Windows::Media::Control::
        GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    switch (status) {
        case Status::Closed:  return PlaybackState::Closed;
        case Status::Stopped: return PlaybackState::Stopped;
        case Status::Paused:  return PlaybackState::Paused;
        case Status::Playing: return PlaybackState::Playing;
        default:              return PlaybackState::Unknown;
    }
}

// Reads the session's album art and re-encodes it to a small PNG.
//
// Returns the encoded size, or 0 when there is no art or anything went wrong --
// artwork is decoration and must never be able to fail the poll. The stream is
// read to the end through a DataReader rather than sized up front, because the
// thumbnail stream does not always report a length.
size_t FetchArtwork(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties& properties,
    uint8_t* out, size_t capacity) {
    using namespace winrt::Windows::Storage::Streams;

    try {
        auto reference = properties.Thumbnail();
        if (!reference) return 0;

        auto stream = reference.OpenReadAsync().get();
        if (!stream) return 0;

        // THE REPORTED LENGTH IS NOT TRUSTWORTHY, AND THIS FUNCTION USED TO TRUST IT.
        //
        // The comment above has always said the thumbnail stream "does not always report a length" -- and the
        // code below it read `Size()` and returned 0 when it was zero anyway. Measured on this machine: for a
        // track the bar was logging `music: cover NONE (0 bytes)` about, `gsmtc_probe` said
        // `Thumbnail: present`, i.e. the session was carrying the cover and this function refused to read it.
        // That is the whole of "切歌后封面还是没有" and of the telling half of the same report -- "关闭后再打开就
        // 能查到": a session that has been running for a while usually does report a size.
        //
        // So the stream is read in bounded chunks, which is what the comment described: `LoadAsync` returns
        // what is there and stops at the end of the stream, whatever the stream says about its own size. The
        // 8 MB cap still applies, a provider publishing a huge image still cannot make the bar allocate it, and
        // a source that hits the cap is refused rather than re-encoded from a truncated file.
        constexpr uint64_t kMaxSourceBytes = 8u * 1024u * 1024u;
        constexpr uint32_t kChunkBytes = 256u * 1024u;

        DataReader reader(stream);
        std::vector<uint8_t> source;
        bool truncated = false;
        for (;;) {
            const uint64_t room = kMaxSourceBytes - source.size();
            if (room == 0) {
                truncated = true;
                break;
            }
            const uint32_t want =
                static_cast<uint32_t>(room < kChunkBytes ? room : kChunkBytes);
            const uint32_t loaded = reader.LoadAsync(want).get();
            if (loaded == 0) break;                       // the end of the stream
            const size_t before = source.size();
            source.resize(before + loaded);
            reader.ReadBytes(winrt::array_view<uint8_t>(source.data() + before,
                                                        source.data() + source.size()));
            if (loaded < want) break;                     // a short read is the end too
        }

        if (source.empty() || truncated) return 0;

        // Shrink to the size the card actually draws. Doing it here, on the polling
        // thread, keeps the UI thread free of image work and means the struct the UI
        // reads stays small.
        return ReencodeToPng(source.data(), source.size(), kMusicArtworkMaxDimension,
                             out, capacity);
    } catch (const winrt::hresult_error&) {
        return 0;
    } catch (const std::exception&) {
        return 0;
    }
}

// How interesting a session is, when several are publishing one.
//
// A browser playing a video and a music player both publish a media session, and
// `GetCurrentSession` returns whichever the shell considers current -- regularly the browser,
// because a video was started more recently. The bar was therefore showing a browser video
// instead of the music that was playing.
//
// The app id is the only signal available about what a session IS, so the known music players
// are named and the browsers are named as the losers. An unrecognised app is treated as music:
// a dedicated player is far likelier to register one than a browser is to go unrecognised.
int SessionScore(const std::wstring& appId, bool playing) {
    std::wstring lower = appId;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));

    const wchar_t* kMusicApps[] = {L"qqmusic",  L"cloudmusic", L"netease", L"spotify",
                                   L"musicbee", L"foobar",     L"aimp",    L"potplayer",
                                   L"groove",   L"zunemusic",  L"itunes",  L"kugou",
                                   L"kuwo",     L"migu"};
    const wchar_t* kVideoApps[] = {L"chrome",  L"msedge", L"firefox", L"brave",
                                   L"opera",   L"vivaldi", L"iexplore", L"vlc",
                                   L"mpc-hc",  L"mpc-be",  L"bilibili"};

    int score = 0;
    for (const wchar_t* name : kMusicApps) {
        if (lower.find(name) != std::wstring::npos) {
            score += 100;
            break;
        }
    }
    for (const wchar_t* name : kVideoApps) {
        if (lower.find(name) != std::wstring::npos) {
            score -= 100;
            break;
        }
    }
    // A playing session is much likelier to be what the user wants to see than a paused one.
    if (playing) score += 50;
    return score;
}

// Picks the session to show: the best-scoring one, with a bonus for playing.
//
// The bonus is what makes music win over a paused video that was opened afterwards, and the
// app-name score is what makes music win over a PLAYING video -- a browser and a player both
// playing is a genuine tie otherwise, and the browser is the one the shell tends to call current.
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession ChooseSession(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager& manager) {
    using namespace winrt::Windows::Media::Control;

    GlobalSystemMediaTransportControlsSession best = nullptr;
    int bestScore = INT_MIN;

    try {
        auto sessions = manager.GetSessions();
        for (uint32_t i = 0; i < sessions.Size(); ++i) {
            auto candidate = sessions.GetAt(i);
            if (!candidate) continue;

            bool playing = false;
            try {
                auto info = candidate.GetPlaybackInfo();
                if (info) {
                    playing = info.PlaybackStatus() ==
                              GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                }
            } catch (const winrt::hresult_error&) {
            }

            std::wstring appId;
            try {
                appId = candidate.SourceAppUserModelId().c_str();
            } catch (const winrt::hresult_error&) {
            }

            const int score = SessionScore(appId, playing);
            if (score > bestScore) {
                bestScore = score;
                best = candidate;
            }
        }
    } catch (const winrt::hresult_error&) {
        best = nullptr;
    }

    // Enumeration can fail on older builds; fall back to what the shell calls current.
    if (!best) {
        try {
            best = manager.GetCurrentSession();
        } catch (const winrt::hresult_error&) {
            best = nullptr;
        }
    }
    return best;
}

// Finds an executable by its bare file name, through the shell's App Paths registry key.
//
// A session's app id is usually a full path, but not always -- QQ Music registers itself as
// `QQMusic.exe` with no directory at all, so there is nothing to open from the id alone. App
// Paths is where the shell itself looks when you type a program's name, so this finds the same
// executables the shell would.
bool ResolveBareExecutable(const wchar_t* fileName, wchar_t* out, size_t capacity) {
    if (!fileName || !out || capacity == 0) return false;
    out[0] = L'\0';

    // Per-user first, then machine-wide, matching the order the shell searches.
    const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    const wchar_t* subKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";

    for (HKEY root : roots) {
        std::wstring path = subKey;
        path += fileName;

        HKEY key = nullptr;
        if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
            continue;
        }
        DWORD type = 0;
        DWORD bytes = static_cast<DWORD>(capacity * sizeof(wchar_t));
        const LONG result = RegQueryValueExW(key, nullptr, nullptr, &type,
                                             reinterpret_cast<LPBYTE>(out), &bytes);
        RegCloseKey(key);

        if (result == ERROR_SUCCESS && type == REG_SZ && out[0] != L'\0') {
            // App Paths entries are sometimes unquoted or quoted; strip the quotes.
            if (out[0] == L'"') {
                wmemmove(out, out + 1, std::wcslen(out));
                wchar_t* closing = std::wcschr(out, L'"');
                if (closing) *closing = L'\0';
            }
            if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return true;
            out[0] = L'\0';
        }
    }
    return false;
}

// Finds an executable by name under the roots applications actually install into.
//
// App Paths is the shell's own lookup, but plenty of applications never register there: QQ Music
// lives at `C:\SOFTWARE\QQMusic\QQMusic.exe` and registers nothing, so its sessions produced no
// icon and the card fell back to a dot. These are the directories installers use, searched one
// and two levels down, which covers both `C:\SOFTWARE\QQMusic\QQMusic.exe` and
// `C:\Program Files\Tencent\QQMusic\QQMusic.exe`.
bool SearchInstallRoots(const wchar_t* fileName, wchar_t* out, size_t capacity) {
    if (!fileName || !out || capacity == 0) return false;

    const wchar_t* roots[] = {L"C:\\SOFTWARE", L"C:\\Program Files",
                              L"C:\\Program Files (x86)"};

    for (const wchar_t* root : roots) {
        wchar_t pattern[MAX_PATH] = {};
        std::swprintf(pattern, MAX_PATH, L"%s\\*", root);

        WIN32_FIND_DATAW found = {};
        HANDLE search = FindFirstFileW(pattern, &found);
        if (search == INVALID_HANDLE_VALUE) continue;

        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (found.cFileName[0] == L'.') continue;

            wchar_t candidate[MAX_PATH] = {};
            std::swprintf(candidate, MAX_PATH, L"%s\\%s\\%s", root, found.cFileName, fileName);
            if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
                FindClose(search);
                wcsncpy_s(out, capacity, candidate, _TRUNCATE);
                return true;
            }

            // One level deeper still, for `<root>\<vendor>\<product>\<app>.exe`.
            wchar_t deeper[MAX_PATH] = {};
            std::swprintf(deeper, MAX_PATH, L"%s\\%s\\*", root, found.cFileName);
            WIN32_FIND_DATAW inner = {};
            HANDLE innerSearch = FindFirstFileW(deeper, &inner);
            if (innerSearch == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(inner.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (inner.cFileName[0] == L'.') continue;
                std::swprintf(candidate, MAX_PATH, L"%s\\%s\\%s\\%s", root, found.cFileName,
                              inner.cFileName, fileName);
                if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
                    FindClose(innerSearch);
                    FindClose(search);
                    wcsncpy_s(out, capacity, candidate, _TRUNCATE);
                    return true;
                }
            } while (FindNextFileW(innerSearch, &inner));
            FindClose(innerSearch);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return false;
}

// Builds the name shown in the card's top-left corner.
//
// A media session's app id is a MACHINE identifier, not a name: QQ Music publishes
// `C:\SOFTWARE\QQMusic\QQMusic.exe`, which the card was showing verbatim. SMTC does carry a
// `SourceAppDisplayName`, but most desktop applications do not populate it, and that is the only
// channel the API offers -- so the executable's own version information is used next. A player's
// ProductName is the name a user knows it by ("QQ闊充箰"), which is exactly what belongs there.
void FillAppDisplayName(const wchar_t* appId, const wchar_t* executablePath, char* out,
                        size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = '\0';

    // 1. The session's own display name, when the application bothered to set one.
    //    (Handled by the caller, which has the WinRT object.)

    // 1b. The name a user knows it by, for the players that do not state it themselves. BEFORE the
    //     version resource, because for those players the resource is the wrong answer -- see
    //     KnownPlayerDisplayName.
    //     ... asked of the resolved executable AND of the id itself: a session may name a bare
    //     "cloudmusic.exe" with nothing resolvable behind it (the icon path has the same problem, and
    //     the id is what SMTC actually gives us).
    const wchar_t* known = KnownPlayerDisplayName(executablePath);
    if (!known) known = KnownPlayerDisplayName(appId);
    if (known) {
        WideCharToMultiByte(CP_UTF8, 0, known, -1, out, static_cast<int>(capacity), nullptr,
                            nullptr);
        return;
    }

    // 2. The executable's version information.
    if (executablePath && executablePath[0] != L'\0') {
        DWORD ignored = 0;
        const DWORD bytes = GetFileVersionInfoSizeW(executablePath, &ignored);
        if (bytes > 0) {
            std::vector<uint8_t> block(bytes);
            if (GetFileVersionInfoW(executablePath, 0, bytes, block.data())) {
                const wchar_t* keys[] = {L"ProductName", L"FileDescription"};
                for (const wchar_t* key : keys) {
                    struct Translation {
                        WORD language;
                        WORD codePage;
                    };
                    Translation* translations = nullptr;
                    UINT translationBytes = 0;
                    wchar_t query[128] = {};
                    std::swprintf(query, 128, L"\\VarFileInfo\\Translation");

                    std::wstring value;
                    if (VerQueryValueW(block.data(), query,
                                       reinterpret_cast<void**>(&translations),
                                       &translationBytes) &&
                        translationBytes >= sizeof(Translation)) {
                        // The first translation is enough; a player's ProductName does not vary
                        // between the languages it ships.
                        wchar_t path[256] = {};
                        std::swprintf(path, 256,
                                      L"\\StringFileInfo\\%04x%04x\\%s",
                                      translations[0].language, translations[0].codePage, key);
                        void* raw = nullptr;
                        UINT rawBytes = 0;
                        if (VerQueryValueW(block.data(), path, &raw, &rawBytes) && raw &&
                            rawBytes > sizeof(wchar_t)) {
                            value.assign(static_cast<const wchar_t*>(raw));
                        }
                    }

                    // Trim: this text goes in a card and padding is not free.
                    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) {
                        value.pop_back();
                    }
                    if (!value.empty() && value.size() < 120) {
                        // A version string ("1.0.0.1") is not a name.
                        const bool allNumeric =
                            value.find_first_not_of(L"0123456789. ") == std::wstring::npos;
                        if (!allNumeric) {
                            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out,
                                                static_cast<int>(capacity), nullptr, nullptr);
                            return;
                        }
                    }
                }
            }
        }
    }

    // 3. The executable's base name, without the extension: "QQMusic".
    if (executablePath && executablePath[0] != L'\0') {
        const wchar_t* base = std::wcsrchr(executablePath, L'\\');
        base = base ? base + 1 : executablePath;
        std::wstring name(base);
        const size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos) name.erase(dot);
        if (!name.empty()) {
            WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, out,
                                static_cast<int>(capacity), nullptr, nullptr);
            return;
        }
    }

    // 4. Whatever the id was, so the row is never empty.
    if (appId && appId[0] != L'\0') {
        WideCharToMultiByte(CP_UTF8, 0, appId, -1, out, static_cast<int>(capacity), nullptr,
                            nullptr);
    }
}

// Loads an executable's icon and re-encodes it as a small PNG.
//
// The icon is drawn at 20 DIP, so the shell's 32x32 frame is plenty. `SHGetFileInfoW` with
// SHGFI_ICON | SHGFI_SMALLICON gives a ready HICON, which the shared rasterizer draws into its
// top-down BGRA buffer (platform/IconRaster.h -- the same code the renderer uploads its taskbar
// icons with) and which is then fed through the same PNG encoder the album art uses. Reading an
// HICON's pixels directly would need GDI+.
size_t FetchAppIcon(const wchar_t* executablePath, uint8_t* out, size_t capacity) {
    if (!executablePath || executablePath[0] == L'\0' || !out || capacity == 0) return 0;

    SHFILEINFOW info = {};
    // Deliberately WITHOUT SHGFI_USEFILEATTRIBUTES.
    //
    // That flag makes the shell answer from the file NAME alone, never touching the disk, so a
    // path that does not exist still returns a generic executable icon. The self-test caught
    // exactly that: "a missing executable yields no icon" failed, because the shell was happy to
    // hand back a placeholder. Without the flag the call checks the file and fails honestly.
    if (!SHGetFileInfoW(executablePath, 0, &info, sizeof(info),
                        SHGFI_ICON | SHGFI_SMALLICON)) {
        return 0;
    }
    if (!info.hIcon) return 0;

    std::vector<uint8_t> pixels;
    size_t written = 0;
    if (RasterizeIcon32(info.hIcon, pixels)) {
        written = EncodeBgraToPng(pixels.data(), kIconRasterSide, kIconRasterSide, out, capacity);
    }

    // The icon is OURS: SHGetFileInfoW handed it over, and nothing else will release it.
    DestroyIcon(info.hIcon);
    return written;
}

} // namespace

// The name a USER knows the player by, when the player itself does not say it.
//
// The executable's version resource is the first source (see FillAppDisplayName) and it is usually the
// right one: QQ音乐's ProductName IS "QQ音乐", so this table has nothing to correct there. It is not
// always right, and the exception is worth writing down: **NetEase Cloud Music ships the string
// "NetEase Cloud Music" as its ProductName even in its Chinese version resource** -- measured with
// `VerQueryValueW` over `cloudmusic.exe`: it carries exactly ONE translation, 0804/04B0 (Simplified
// Chinese, Unicode), and the name inside it is English. `FileDescription` says the same. So there is no
// Chinese name anywhere in the file to prefer, and the card showed the English one for a Chinese
// application -- reported as "网易云音乐的名字应该改成中文的".
//
// Keyed on the executable the session names (`cloudmusic.exe`), because that is the only identifier
// available, and matched by base name so a full path and any case work. Players whose own answer is
// already the name a user would say are deliberately NOT listed: this table is a correction, not a
// translation of everything.
const wchar_t* KnownPlayerDisplayName(const wchar_t* executableName) {
    if (!executableName || executableName[0] == L'\0') return nullptr;

    const wchar_t* base = std::wcsrchr(executableName, L'\\');
    base = base ? base + 1 : executableName;
    const wchar_t* slash = std::wcsrchr(base, L'/');
    if (slash) base = slash + 1;

    std::wstring lower(base);
    for (wchar_t& c : lower) c = static_cast<wchar_t>(std::towlower(c));

    struct Entry {
        const wchar_t* executable;
        const wchar_t* display;
    };
    static const Entry kEntries[] = {
        {L"cloudmusic.exe", L"网易云音乐"},
        {L"cloudmusic_pc.exe", L"网易云音乐"},
    };
    for (const Entry& entry : kEntries) {
        if (lower == entry.executable) return entry.display;
    }
    return nullptr;
}

// The ranking, exposed so the self-test can check the comparisons that decide which session wins.
// It is pure, so it is worth testing directly rather than by arranging two applications to play at
// once.
int SessionScoreForTest(const wchar_t* appId, bool playing) {
    return SessionScore(appId ? appId : L"", playing);
}

// The icon loader, reachable from outside this file so the preview and the self-test can exercise
// it: the live path only reaches it when some application happens to be publishing a media
// session, which cannot be arranged on demand.
size_t FetchAppIconForTest(const wchar_t* executablePath, uint8_t* out, size_t capacity) {
    return FetchAppIcon(executablePath, out, capacity);
}

// The path the icon lookup would load from, for diagnostics. Empty when nothing resolves.
bool ResolveAppIconPath(const wchar_t* appId, wchar_t* out, size_t capacity) {
    return ExecutableFromAppId(appId, out, capacity);
}

// Turns a session's app id into the executable path its icon can be loaded from.
//
// The id is usually a full path -- `C:\...\QQMusic.exe` -- and sometimes a packaged-app id of
// the form `package!App`, for which there is no file to load. Only the first form is handled,
// and the card falls back to the coloured dot rather than showing a wrong icon.
//
// Public rather than file-local so the self-test can check the id parsing, which is pure and
// therefore worth testing directly.
bool ExecutableFromAppId(const wchar_t* appId, wchar_t* out, size_t capacity) {
    if (!appId || !out || capacity == 0) return false;
    out[0] = L'\0';
    if (std::wcslen(appId) >= capacity) return false;

    if (std::wcschr(appId, L'!') != nullptr) return false;   // a package, not a file

    const wchar_t* dot = std::wcsrchr(appId, L'.');
    if (!dot || _wcsicmp(dot, L".exe") != 0) return false;

    // A full path that exists: use it.
    if (GetFileAttributesW(appId) != INVALID_FILE_ATTRIBUTES) {
        wcsncpy_s(out, capacity, appId, _TRUNCATE);
        return true;
    }

    // A bare file name (`QQMusic.exe`) or a path that no longer exists: ask the shell's App
    // Paths, then look under the directories applications install into. QQ Music registers
    // NOTHING -- it is simply installed at `C:\SOFTWARE\QQMusic\QQMusic.exe` -- so App Paths alone
    // leaves it without an icon.
    const wchar_t* baseName = std::wcsrchr(appId, L'\\');
    baseName = baseName ? baseName + 1 : appId;
    if (ResolveBareExecutable(baseName, out, capacity)) return true;
    return SearchInstallRoots(baseName, out, capacity);
}

const char* PlaybackStateName(PlaybackState state) {
    switch (state) {
        case PlaybackState::Closed:  return "closed";
        case PlaybackState::Stopped: return "stopped";
        case PlaybackState::Paused:  return "paused";
        case PlaybackState::Playing: return "playing";
        default:                     return "unknown";
    }
}

MusicSession::~MusicSession() {
    Stop();
}

bool MusicSession::Start() {
    if (running_) return true;

    toggleEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    // Stop() ends whatever exists rather than unwinding by hand (see platform/WorkerThread.h).
    if (!worker_.Prepare() || !toggleEvent_) {
        Stop();
        return false;
    }

    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, &MusicSession::ThreadEntry, this, 0, &threadId);
    if (!thread) {
        Stop();
        return false;
    }
    worker_.Attach(thread);

    // Wait for the worker's first poll, so Start() returning true means the media
    // API actually answered.
    //
    // The worker cannot set running_ before that -- it only learns whether the API
    // works by trying -- so this waits on pollCount_ and then adopts the worker's
    // verdict. Waiting on running_ would have exited immediately and reported
    // failure for a worker that was doing fine.
    for (int i = 0; i < 400 && pollCount_ == 0; ++i) {
        Sleep(5);
    }

    // Any answer, including "nothing is playing", means the API is present. A
    // worker that never polled at all means it could not enter an apartment.
    running_ = (pollCount_ > 0);
    if (!running_) {
        Stop();
        return false;
    }
    return true;
}

void MusicSession::Stop() {
    worker_.Stop();
    if (toggleEvent_) {
        CloseHandle(toggleEvent_);
        toggleEvent_ = nullptr;
    }
    running_ = false;
}

void MusicSession::Snapshot(MusicInfo* out) const {
    if (!out || !worker_.Prepared()) return;
    lock_.Enter();
    *out = current_;
    lock_.Leave();
}

unsigned long long MusicSession::Revision() const {
    return static_cast<unsigned long long>(revision_);
}

long long MusicSession::PollCount() const {
    return static_cast<long long>(pollCount_);
}

bool MusicSession::RequestTransport(Transport action) {
    if (!running_ || !toggleEvent_) return false;
    // Under the lock so the worker cannot see the pending flag with a stale action.
    lock_.Enter();
    pendingAction_ = action;
    InterlockedExchange(&togglePending_, 1);
    lock_.Leave();
    // Waking the worker makes the action immediate instead of waiting up to two
    // seconds for the next poll, which is the difference between a button that feels
    // connected and one that feels broken.
    SetEvent(toggleEvent_);
    return true;
}

unsigned long __stdcall MusicSession::ThreadEntry(void* self) {
    static_cast<MusicSession*>(self)->Run();
    return 0;
}

void MusicSession::Run() {
    // The worker owns its own apartment. WinRT objects are not usable across
    // apartments unless they are agile, and the session manager is; entering MTA
    // here keeps the UI thread out of it entirely.
    const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    const bool haveApartment = SUCCEEDED(apartment);
    using MediaSessionManager =
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    MediaSessionManager manager{nullptr};
    DWORD nextManagerRefreshAt = 0;
    unsigned managerRefreshAttempts = 0;

    MusicInfo previous;
    // The epoch and the last timeline update that belonged to it. A track
    // change makes the newly read Position unusable until the timeline says it
    // has updated again; the UI counts that new epoch locally in the meantime.
    unsigned long long timelineEpoch = 0;
    long long epochTimelineUpdatedAt = 0;
    long long epochTimelinePosition = -1;
    bool epochHasFreshManager = false;
    DWORD lastPoll = 0;
    // When the user last skipped a track, for the shorter poll interval that follows one. See
    // NextPollIntervalMs.
    DWORD lastSkip = 0;
    // When the cover was last asked for while it was missing: the retry for a thumbnail that the player
    // publishes after the track's own metadata. See the artwork block in the poll below.
    DWORD lastArtworkTry = 0;
    // How many times the CURRENT track has been asked for a cover it has not published, for the bounded
    // retry log. See kArtworkRetryLogLimit.
    int artworkRetries = 0;
    // When the track last changed, for the chase that follows ANY track change -- see NextPollIntervalMs.
    DWORD lastTrackChange = 0;
    // THE ARTWORK REVISION, monotonic across tracks.
    //
    // It used to be `previous.artworkRevision + 1`, which RESETS to 1 every time a track publishes no cover
    // (the published revision goes back to 0). That number is the cache key the renderer rebuilds its GPU
    // bitmap against, so a cover arriving late for the NEXT track was given revision 1 again -- and the
    // renderer matched its cached (revision 1, 128x128) entry and drew the PREVIOUS track's album art.
    //
    // Reported as "歌曲切换了 但是封面图没有换", with a log that looked perfect (`cover dropped`, `cover
    // arrived late`, `cover decoded` -- the BYTES were right), and with a restart that always looked right,
    // because a fresh cache has nothing to match against. A counter that only ever goes up is what makes the
    // renderer's assumption true.
    unsigned long long artworkSerial = 1;

    while (true) {
        // Wake for either a toggle request or the next scheduled poll, whichever
        // comes first. Waiting on both is what makes the button responsive without
        // polling faster for everything else.
        HANDLE waits[2] = { worker_.StopEvent(), toggleEvent_ };
        const DWORD interval =
            static_cast<DWORD>(NextPollIntervalMs(GetTickCount(), lastSkip, lastTrackChange));
        const DWORD elapsed = GetTickCount() - lastPoll;
        const DWORD remaining = (elapsed >= interval) ? 0 : (interval - elapsed);
        const DWORD signalled = WaitForMultipleObjects(2, waits, FALSE, remaining);

        if (signalled == WAIT_OBJECT_0) break;   // stop requested

        if (!haveApartment) break;

        const bool toggleRequested =
            (signalled == WAIT_OBJECT_0 + 1) || InterlockedCompareExchange(&togglePending_, 0, 1) == 1;

        try {
            using namespace winrt::Windows::Media::Control;

            bool managerRebuiltThisPoll = false;
            if (!manager) {
                manager = MediaSessionManager::RequestAsync().get();
                if (!manager) continue;
                LogLine(L"  music: session manager acquired");
            } else if (nextManagerRefreshAt != 0 &&
                       GetTickCount() - nextManagerRefreshAt < 0x80000000u &&
                       managerRefreshAttempts < 3) {
                auto refreshed = MediaSessionManager::RequestAsync().get();
                if (!refreshed) continue;
                manager = refreshed;
                managerRebuiltThisPoll = true;
                ++managerRefreshAttempts;
                nextManagerRefreshAt =
                    managerRefreshAttempts < 3 ? GetTickCount() + 1000 : 0;
            }

            // --- a transport action, if one was asked for ---
            if (toggleRequested) {
                if (auto session = ChooseSession(manager)) {
                    try {
                        // WAITED FOR, on this worker, before the poll below.
                        //
                        // It used to be fire-and-forget -- "the next poll publishes the new state" -- and
                        // the poll below runs IMMEDIATELY after, so it raced the player: the fresh reading
                        // was taken before the toggle had taken effect and still said "playing", and the
                        // state only caught up on the next full interval. Reported as "点击暂停 歌曲停止了
                        // 但是这个差不多2秒左右才变播放按钮" -- two seconds being kPollIntervalMs.
                        //
                        // Blocking here is safe: this is the poll worker, it has no frame to miss, and the
                        // call is bounded by the player acknowledging the request. The bool it returns is
                        // whether the player ACCEPTED it, which is worth having in the log.
                        bool accepted = false;
                        switch (pendingAction_) {
                            case Transport::Previous: accepted = session.TrySkipPreviousAsync().get(); break;
                            case Transport::Next:     accepted = session.TrySkipNextAsync().get(); break;
                            case Transport::PlayPause:
                            default:                  accepted = session.TryTogglePlayPauseAsync().get(); break;
                        }
                        const char* what = (pendingAction_ == Transport::Previous)   ? "previous"
                                           : (pendingAction_ == Transport::Next)     ? "next"
                                                                                     : "play/pause";
                        LogLine(L"  music: transport %hs %ls", what,
                                accepted ? L"accepted" : L"REFUSED by the player");

                        // PUBLISH WHAT THE ACTION IMPLIES, without waiting for the player's own status.
                        //
                        // Waiting for the acknowledgement was not enough: measured on this machine the
                        // button still took about two seconds to become a play button after the music had
                        // audibly stopped ("点击暂停 歌曲停止了 但是这个差不多2秒左右才变播放按钮"), because a
                        // player updates its PlaybackInfo on its own schedule. The acknowledgement says the
                        // action was TAKEN, so the state it implies is published now, and the next poll
                        // corrects it if the player disagrees -- optimistic, and honest about being so.
                        //
                        // `previous` is mutated in place rather than copied: MusicInfo carries a cover image
                        // and is large enough that an extra copy on this stack has bitten this project
                        // before (see HANDOFF: "the self-test overflowed the stack").
                        if (accepted && pendingAction_ == Transport::PlayPause && previous.present) {
                            previous.state = (previous.state == PlaybackState::Playing)
                                                 ? PlaybackState::Paused
                                                 : PlaybackState::Playing;
                            lock_.Enter();
                            current_ = previous;
                            lock_.Leave();
                            InterlockedIncrement(&revision_);
                        }
                    } catch (const winrt::hresult_error&) {
                    }
                }
                InterlockedExchange(&togglePending_, 0);
                // A SKIP STARTS A CHASE. The optimistic publication above is for the play/pause button, whose
                // state is what it toggled; a skip has nothing to publish -- the new track's title, cover and
                // lyrics are all the player's to give, and it needs a moment to load them. Polling every
                // kSkipPollIntervalMs for the next few seconds is what makes the card show them then, instead
                // of at the next full interval (measured: 1.65 s late, reported as "需要等个2-3秒才能切").
                if (pendingAction_ == Transport::Previous || pendingAction_ == Transport::Next) {
                    lastSkip = GetTickCount();
                }
                // NO IMMEDIATE POLL -- it undoes the optimistic publication above.
                //
                // This used to set `lastPoll = 0` "so the button's state catches up with the click", and
                // with a player whose PlaybackInfo lags (QQ音乐, about two seconds) that poll re-read the
                // OLD status and published it straight back over the flipped state: the button changed,
                // changed back, and only caught up on the next scheduled poll -- indistinguishable from
                // never having flipped it. Reported twice, the second time as "还是一样的".
                //
                // The state published above is what the ACCEPTED action means, and the poll that is due
                // within kPollIntervalMs confirms or corrects it. Nothing is gained by asking again in the
                // same instant that the answer is known to be stale.
                //
                // `continue`, and not merely leaving `lastPoll` alone: the gate below lets the poll through
                // whenever a toggle is pending, which is exactly this iteration.
                continue;
            }

            // --- the regular poll ---
            if (GetTickCount() - lastPoll < interval && !toggleRequested) {
                continue;
            }
            lastPoll = GetTickCount();

            MusicInfo fresh;
            auto session = ChooseSession(manager);
            bool trackChanged = false;
            if (session) {
                fresh.present = true;

                try {
                    auto properties = session.TryGetMediaPropertiesAsync().get();
                    if (properties) {
                        CopyUtf8(properties.Title(), fresh.title, kMusicTitleCapacity);
                        CopyUtf8(properties.Artist(), fresh.artist, kMusicArtistCapacity);
                        CopyUtf8(properties.AlbumTitle(), fresh.album, kMusicAlbumCapacity);

                        // Album art: when the track changed, AND WHILE IT IS STILL MISSING.
                        //
                        // Fetching it only on a track change looked like the obvious economy -- decoding and
                        // re-encoding a cover on every poll would be waste -- and it is what made a cover
                        // appear only after the bar was RESTARTED. The player does not always publish the
                        // thumbnail in the same instant as the new track's metadata: measured on this machine,
                        // a track that read `music: cover NONE (0 bytes)` when it started had one the moment
                        // the bar was restarted, which means the session was carrying it and the bar had
                        // simply never asked again. Reported in those words: "切歌就没有了，关闭后再打开就能查到".
                        //
                        // So the question is asked again on every poll while there is nothing for the current
                        // track -- throttled to one try per kArtworkRetryMs, because each ask opens a stream
                        // and re-encodes a PNG. Once a cover arrives the asking stops, so a track that
                        // genuinely has none costs a few tries at the start and then nothing at all.
                        trackChanged =
                            island::MusicTrackChanged(previous, fresh);
                        const DWORD now = GetTickCount();
                        // A NEW TRACK STARTS A CHASE, wherever the skip came from: the chase is what makes the
                        // cover, the lyrics and the position catch up in a fraction of a second instead of
                        // waiting out a poll. See NextPollIntervalMs.
                        if (trackChanged) {
                            lastTrackChange = now;
                        }
                        const bool wantArtwork = ShouldFetchArtwork(
                            trackChanged, previous.artworkSize == 0, now - lastArtworkTry,
                            lastTrackChange != 0 ? now - lastTrackChange : kArtworkRetryWindowMs);
                        if (wantArtwork) {
                            lastArtworkTry = now;
                            fresh.artworkSize = FetchArtwork(properties, fresh.artwork,
                                                             kMusicArtworkCapacity);
                            if (fresh.artworkSize > 0) {
                                fresh.artworkRevision = artworkSerial++;
                                artworkRetries = 0;
                                // Worth its own line: it is the case this retry exists for, and the one the
                                // user could only see by restarting the bar ("切歌就没有了，关闭后再打开就能查到").
                                if (!trackChanged) {
                                    LogLine(L"  music: cover arrived late (%zu bytes)",
                                            fresh.artworkSize);
                                }
                            } else if (!trackChanged && artworkRetries < kArtworkRetryLogLimit) {
                                // "A manual track change does not fetch the cover" is a claim about WHO is not
                                // asking, and until this line existed there was no way to tell the two causes
                                // apart: a bar that stopped asking, and a player that has nothing to give. A
                                // few lines per track is enough to point at one or the other.
                                ++artworkRetries;
                                LogLine(L"  music: cover retry %d -> the player still has none",
                                        artworkRetries);
                            }
                            if (trackChanged) artworkRetries = 0;
                        } else {
                            // Keep what we already had.
                            fresh.artworkSize = previous.artworkSize;
                            fresh.artworkRevision = previous.artworkRevision;
                            if (previous.artworkSize > 0) {
                                std::memcpy(fresh.artwork, previous.artwork, previous.artworkSize);
                            }
                        }
                    }
                } catch (const winrt::hresult_error&) {
                    // Metadata unavailable for this poll; the session itself is still
                    // worth reporting.
                }

                try {
                    auto appId = session.SourceAppUserModelId();
                    CopyUtf8(appId, fresh.app, kMusicAppCapacity);

                    // The icon, once per track change rather than per poll: it only depends on
                    // which application owns the session, and decoding it is not free.
                    const bool appChanged = std::strcmp(fresh.app, previous.app) != 0;
                    if (appChanged) {
                        wchar_t executable[MAX_PATH] = {};
                        const bool resolved =
                            ExecutableFromAppId(appId.c_str(), executable, MAX_PATH);
                        if (resolved) {
                            fresh.iconSize = FetchAppIcon(executable, fresh.icon,
                                                          kMusicArtworkCapacity);
                            if (fresh.iconSize > 0) {
                                fresh.iconRevision = previous.iconRevision + 1;
                            }
                        }
                        if (resolved) {
                            // The card shows a NAME, not the machine id, so it is resolved here
                            // where the executable path is known. SMTC carries no display name for
                            // most desktop applications.
                            FillAppDisplayName(appId.c_str(), executable, fresh.appName,
                                               kMusicAppNameCapacity);
                        }
                        if (fresh.appName[0] == '\0') {
                            CopyUtf8(appId, fresh.appName, kMusicAppNameCapacity);
                        }

                        // Logged because this is the only place the answer is known, and the
                        // symptom of a failure here is a missing picture, which looks the same
                        // whether the lookup failed, the icon export failed, or the draw did.
                        //
                        // WIDE, with the two UTF-8 fields converted first. `%hs` in a wide format reads
                        // a char* through the current locale rather than as UTF-8, so the resolved name
                        // came out as `ç½æäºé³ä¹` -- the same defect the `music: [...]` line had
                        // (HANDOFF 8.72/8.73), in the one line whose whole job is to print that name.
                        {
                            wchar_t wideApp[island::kMusicAppCapacity] = {};
                            wchar_t wideName[island::kMusicAppNameCapacity] = {};
                            MultiByteToWideChar(CP_UTF8, 0, fresh.app, -1, wideApp,
                                                static_cast<int>(island::kMusicAppCapacity));
                            MultiByteToWideChar(CP_UTF8, 0, fresh.appName, -1, wideName,
                                                static_cast<int>(island::kMusicAppNameCapacity));
                            LogLine(L"  icon: app='%ls' name='%ls' -> %ls (%zu bytes)", wideApp,
                                    wideName, resolved ? executable : L"UNRESOLVED", fresh.iconSize);
                        }
                    } else {
                        std::memcpy(fresh.appName, previous.appName, kMusicAppNameCapacity);
                        fresh.iconSize = previous.iconSize;
                        fresh.iconRevision = previous.iconRevision;
                        if (previous.iconSize > 0) {
                            std::memcpy(fresh.icon, previous.icon, previous.iconSize);
                        }
                    }
                } catch (const winrt::hresult_error&) {
                }

                trackChanged = island::MusicTrackChanged(previous, fresh);

                try {
                    auto info = session.GetPlaybackInfo();
                    if (info) {
                        fresh.state = ConvertStatus(info.PlaybackStatus());
                    }
                } catch (const winrt::hresult_error&) {
                    fresh.state = PlaybackState::Unknown;
                }

                long long timelineUpdatedAt = 0;
                bool timelineRangeValid = false;
                try {
                    auto timeline = session.GetTimelineProperties();
                    if (timeline) {
                        // WinRT durations are in 100-nanosecond ticks.
                        fresh.positionMs = timeline.Position().count() / 10000;
                        fresh.durationMs = timeline.EndTime().count() / 10000;
                        timelineUpdatedAt = timeline.LastUpdatedTime().time_since_epoch().count();
                        timelineRangeValid =
                            fresh.durationMs > 0 && fresh.positionMs >= 0 &&
                            fresh.positionMs <= fresh.durationMs;
                    }
                } catch (const winrt::hresult_error&) {
                }

                fresh.timelineTrusted = TimelineTrustedForEpoch(
                    trackChanged, previous.present, timelineRangeValid, previous.timelineTrusted,
                    epochHasFreshManager, timelineUpdatedAt, epochTimelineUpdatedAt,
                    fresh.positionMs, epochTimelinePosition);
                if (trackChanged) {
                    ++timelineEpoch;
                    epochTimelineUpdatedAt = timelineUpdatedAt;
                    epochTimelinePosition = fresh.positionMs;
                    epochHasFreshManager = false;
                    managerRefreshAttempts = 0;
                    nextManagerRefreshAt = previous.present ? GetTickCount() + 1000 : 0;
                    LogLine(L"  music: track epoch=%llu timeline=%ls "
                            L"sessionPosition=%lldms cover=%ls",
                            timelineEpoch, fresh.timelineTrusted ? L"TRUSTED" : L"UNTRUSTED",
                            fresh.positionMs,
                            fresh.artworkSize > 0 ? L"present" : L"NONE");
                } else if (managerRebuiltThisPoll && !fresh.timelineTrusted) {
                    epochTimelineUpdatedAt = timelineUpdatedAt;
                    epochTimelinePosition = fresh.positionMs;
                    epochHasFreshManager = timelineRangeValid;
                    LogLine(L"  music: timeline epoch=%llu rebased after session refresh "
                            L"attempt=%u position=%lldms trusted=0",
                            timelineEpoch, managerRefreshAttempts, fresh.positionMs);
                }
                if (fresh.timelineTrusted) {
                    nextManagerRefreshAt = 0;
                    if (!previous.timelineTrusted) {
                        LogLine(L"  music: timeline epoch=%llu TRUSTED "
                                L"position=%lldms duration=%lldms",
                                timelineEpoch, fresh.positionMs, fresh.durationMs);
                    }
                    epochTimelineUpdatedAt = timelineUpdatedAt;
                }
            }

            fresh.timelineEpoch = timelineEpoch;

            InterlockedIncrement(&pollCount_);

            // Only publish a change. The UI compares revisions to decide whether to
            // redraw, so bumping on every identical poll would keep it awake forever.
            const bool changed =
                fresh.present != previous.present ||
                fresh.state != previous.state ||
                fresh.positionMs / 1000 != previous.positionMs / 1000 ||
                fresh.artworkRevision != previous.artworkRevision ||
                fresh.timelineEpoch != previous.timelineEpoch ||
                fresh.timelineTrusted != previous.timelineTrusted ||
                std::strcmp(fresh.title, previous.title) != 0 ||
                std::strcmp(fresh.artist, previous.artist) != 0 ||
                std::strcmp(fresh.album, previous.album) != 0 ||
                std::strcmp(fresh.app, previous.app) != 0;

            if (changed) {
                lock_.Enter();
                current_ = fresh;
                lock_.Leave();
                InterlockedIncrement(&revision_);
                previous = fresh;
            }
        } catch (const winrt::hresult_error&) {
            // The media API is unavailable, or the shell refused. Report "nothing
            // playing" rather than retrying hot: the next poll is two seconds away.
        } catch (...) {
        }
    }

    if (haveApartment) RoUninitialize();
    running_ = false;
}

} // namespace island
