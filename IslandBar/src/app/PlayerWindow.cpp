// =====================================================================
//  PlayerWindow.cpp
//  See PlayerWindow.h for why this exists and what it deliberately does not do.
// =====================================================================

#include "app/PlayerWindow.h"
#include "platform/ScopeExit.h"

#include <cwchar>
#include <cwctype>
#include <cstring>
#include <string>

namespace island {
namespace {

// The players whose titles are read.
//
// The small list is the point: a browser window's title is ALSO `<page> - <browser>`, and reading
// every window's title would put a web page on the pill as if it were a song. These are the same
// names the session chooser prefers (see MusicSession.cpp), minus their executables' decorations.
const wchar_t* const kKnownPlayers[] = {
    L"cloudmusic.exe", L"qqmusic.exe",  L"kugou.exe",     L"kugoumusic.exe",
    L"kwmusic.exe",    L"kuwo.exe",     L"spotify.exe",   L"musicbee.exe",
    L"foobar2000.exe", L"aimp.exe",     L"zunemusic.exe", L"itunes.exe",
    L"migu.exe",       L"cloudmusic_pc.exe",
};

// What these windows are titled when NO track is up. A lyric lookup for one of these returns
// nothing, and the pill would sit there announcing "网易云音乐" as the song being played.
const wchar_t* const kNotSongs[] = {
    L"网易云音乐", L"网易云音乐 for Windows", L"网易云音乐for Windows", L"QQ音乐",
    L"酷狗音乐",   L"酷我音乐",               L"咪咕音乐",              L"Spotify",
    L"Spotify Free", L"Spotify Premium",      L"MusicBee",              L"foobar2000",
    L"iTunes",      L"CloudMusic",
};

bool EqualsIgnoringCase(const std::wstring& text, const wchar_t* other) {
    if (text.size() != std::wcslen(other)) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (std::towlower(text[i]) != std::towlower(other[i])) return false;
    }
    return true;
}

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
           std::iswspace(static_cast<wint_t>(c)) != 0;
}

std::wstring Trim(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) ++begin;
    while (end > begin && IsSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// UTF-8, because that is what the rest of the music path carries (MusicInfo's char buffers) and
// what the lyric provider expects.
bool ToUtf8(const std::wstring& text, char* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';
    if (text.empty()) return true;
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out,
                                              static_cast<int>(capacity), nullptr, nullptr);
    return written > 0;
}

std::wstring BareExecutableName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// One pass over the top-level windows, keeping the best candidate.
//
// A VISIBLE window beats an invisible one, and the first of equals wins. Invisible windows are
// NOT skipped: NetEase keeps the track on a hidden 1x1 window when it is playing from the tray
// (measured -- with the main window closed, the visible window list held no NetEase window at
// all, while it was playing), and that is exactly the case this source exists for.
struct Search {
    bool found = false;
    bool foundVisible = false;
    char title[kPlayerWindowTitleCapacity] = {};
    char artist[kPlayerWindowArtistCapacity] = {};
    char app[kPlayerWindowAppCapacity] = {};
};

BOOL CALLBACK VisitWindow(HWND hwnd, LPARAM parameter) {
    Search* search = reinterpret_cast<Search*>(parameter);
    if (search->foundVisible) return FALSE;   // nothing can beat a visible one

    wchar_t title[512] = {};
    if (::GetWindowTextW(hwnd, title, 512) <= 0) return TRUE;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) return TRUE;

    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    wchar_t path[MAX_PATH] = {};
    DWORD length = MAX_PATH;
    const bool havePath = ::QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    ::CloseHandle(process);
    if (!havePath) return TRUE;

    const std::wstring exe = BareExecutableName(path);
    if (!IsKnownPlayerExecutable(exe.c_str())) return TRUE;

    if (!ParsePlayerWindowTitle(title, search->title, sizeof(search->title), search->artist,
                                sizeof(search->artist))) {
        return TRUE;
    }

    const bool visible = ::IsWindowVisible(hwnd) != FALSE;
    if (search->found && search->foundVisible) return TRUE;
    if (search->found && !visible) return TRUE;         // keep the first of equals

    ToUtf8(exe, search->app, sizeof(search->app));
    search->found = true;
    search->foundVisible = visible;
    return TRUE;
}

}  // namespace

bool IsKnownPlayerExecutable(const wchar_t* executableName) {
    if (!executableName || executableName[0] == L'\0') return false;
    const std::wstring name = BareExecutableName(executableName);
    for (const wchar_t* known : kKnownPlayers) {
        if (EqualsIgnoringCase(name, known)) return true;
    }
    return false;
}

bool ParsePlayerWindowTitle(const wchar_t* windowTitle, char* titleOut, size_t titleCapacity,
                            char* artistOut, size_t artistCapacity) {
    if (titleOut && titleCapacity > 0) titleOut[0] = '\0';
    if (artistOut && artistCapacity > 0) artistOut[0] = '\0';
    if (!windowTitle || !titleOut || !artistOut || titleCapacity == 0 || artistCapacity == 0) {
        return false;
    }

    const std::wstring text = Trim(windowTitle);
    if (text.empty()) return false;

    // From the RIGHT: `A - B - C` is the song `A - B` by `C`.
    const size_t separator = text.rfind(L" - ");
    if (separator == std::wstring::npos) return false;

    const std::wstring song = Trim(text.substr(0, separator));
    const std::wstring artist = Trim(text.substr(separator + 3));
    if (song.empty() || artist.empty()) return false;

    for (const wchar_t* notSong : kNotSongs) {
        if (EqualsIgnoringCase(song, notSong)) return false;
    }

    return ToUtf8(song, titleOut, titleCapacity) && ToUtf8(artist, artistOut, artistCapacity);
}

PlayerWindowWatch::~PlayerWindowWatch() {
    Stop();
}

bool PlayerWindowWatch::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    if (!worker_.Start(&PlayerWindowWatch::ThreadEntry, weak_from_this().lock())) {
        worker_.Stop();
        return false;
    }
    running_.store(true, std::memory_order_release);
    return true;
}

WorkerThread::StopResult PlayerWindowWatch::Stop() {
    // NO early return when running_ is false, which is what this used to do: the handles belong to the
    // attempt, not to the flag, and a Start() that failed after preparing the event left it open forever.
    running_.store(false, std::memory_order_release);
    return worker_.Stop();
}

unsigned long __stdcall PlayerWindowWatch::ThreadEntry(void* self) {
    static_cast<PlayerWindowWatch*>(self)->Run();
    return 0;
}

void PlayerWindowWatch::Run() {
    const WorkerThread::Signal workerSignal = WorkerThread::CurrentSignal();
    const HANDLE stopEvent = workerSignal.StopEvent();
    auto runExit = MakeScopeExit([this]() noexcept {
        running_.store(false, std::memory_order_release);
    });
    // The FIRST pass runs at once: a bar started while a song is playing should have the words
    // straight away, not one poll interval later.
    while (true) {
        Poll();
        ::InterlockedIncrement(&pollCount_);
        if (::WaitForSingleObject(stopEvent, kPlayerWindowPollMs) != WAIT_TIMEOUT) break;
        if (!running_.load(std::memory_order_acquire)) break;
    }
}

void PlayerWindowWatch::Poll() {
    Search search;
    ::EnumWindows(&VisitWindow, reinterpret_cast<LPARAM>(&search));

    PlayerWindowInfo info;
    info.present = search.found;
    if (search.found) {
        std::strncpy(info.title, search.title, sizeof(info.title) - 1);
        std::strncpy(info.artist, search.artist, sizeof(info.artist) - 1);
        std::strncpy(info.app, search.app, sizeof(info.app) - 1);
    }
    Publish(info);
}

void PlayerWindowWatch::Publish(const PlayerWindowInfo& info) {
    lock_.Enter();
    const bool same = published_.present == info.present &&
                      std::strcmp(published_.title, info.title) == 0 &&
                      std::strcmp(published_.artist, info.artist) == 0 &&
                      std::strcmp(published_.app, info.app) == 0;
    if (!same) {
        published_ = info;
        ::InterlockedIncrement(&revision_);
    }
    lock_.Leave();
}

void PlayerWindowWatch::Snapshot(PlayerWindowInfo* out) const {
    if (!out) return;
    lock_.Enter();
    *out = published_;
    lock_.Leave();
}

unsigned long long PlayerWindowWatch::Revision() const {
    return static_cast<unsigned long long>(::InterlockedCompareExchange(
        const_cast<volatile long*>(&revision_), 0, 0));
}

long long PlayerWindowWatch::PollCount() const {
    return static_cast<long long>(::InterlockedCompareExchange(
        const_cast<volatile long*>(&pollCount_), 0, 0));
}

}  // namespace island
