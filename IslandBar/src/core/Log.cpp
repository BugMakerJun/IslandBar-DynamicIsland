// =====================================================================
//  Log.cpp
// =====================================================================
#include "core/Log.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace island {
namespace {

// A local element count: _countof lives in <cstdlib> and is MSVC-specific, and
// this is three lines.
template <typename T, size_t N>
constexpr size_t CountOf(const T (&)[N]) { return N; }

FILE* g_file = nullptr;
wchar_t g_path[MAX_PATH] = {};
bool g_tried = false;

void Timestamp(wchar_t* out, size_t capacity) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    std::swprintf(out, capacity, L"%02u:%02u:%02u.%03u",
                  static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
                  static_cast<unsigned>(now.wSecond), static_cast<unsigned>(now.wMilliseconds));
}

// Writes `text` to the console with exactly one trailing newline, whatever the
// caller supplied.
//
// Call sites are written with whatever newline they wanted -- many end their format
// string with \n by habit. That newline plus the one added here is what produced a
// blank line after every line of the probe report on the console.
void PrintOneLineImpl(wchar_t* text) {
    size_t length = std::wcslen(text);
    while (length > 0 && (text[length - 1] == L'\n' || text[length - 1] == L'\r')) {
        text[--length] = L'\0';
    }
    // fputws does not translate the newline, so the console gets exactly one.
    std::fputws(text, stdout);
    std::fputwc(L'\n', stdout);
    std::fflush(stdout);
}

} // namespace

bool StdoutIsUsable() {
    // A console-less GUI process has stdout bound to descriptor -1. Writing there
    // trips the CRT's own assertion rather than returning an error, so the check
    // has to happen before the write.
    const int descriptor = _fileno(stdout);
    return descriptor >= 0;
}

void PrintLine(const wchar_t* text) {
    if (!StdoutIsUsable()) return;

    // Copy: PrintOneLineImpl trims in place, and callers pass string literals.
    wchar_t buffer[2048] = {};
    wcsncpy_s(buffer, CountOf(buffer), text, _TRUNCATE);
    PrintOneLineImpl(buffer);
}

const wchar_t* LogPath() {
    if (g_path[0] != L'\0') return g_path;

    // The log goes BESIDE THE EXECUTABLE first, not under %TEMP%.
    //
    // %TEMP% is per-shell. A program started from one shell writes there, and anyone looking from
    // another shell -- including the tooling used to develop it -- searches a directory the file was
    // never written to. That cost several rounds: the log existed the whole time and was invisible
    // from where it was being looked for.
    //
    // Beside the executable the path is fixed and known to everybody: it depends on where the program
    // is, not on who started it. %TEMP% is kept as the fallback for the case where the program sits
    // somewhere it cannot write, which is the case the temp directory was originally chosen for.
    wchar_t exePath[MAX_PATH] = {};
    const DWORD exeLength = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (exeLength > 0 && exeLength < MAX_PATH) {
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
            std::swprintf(g_path, MAX_PATH, L"%sisland_bar.log", exePath);

            // Probed rather than assumed: a build directory is usually writable, but "usually" is
            // how a logger ends up silently writing nowhere.
            FILE* probe = nullptr;
            if (_wfopen_s(&probe, g_path, L"a") == 0) {
                if (probe) fclose(probe);
                return g_path;
            }
        }
    }

    // Fallback: the temp directory. Always writable, and needs no directory created first -- an
    // earlier version used %LOCALAPPDATA%\IslandBar, which failed silently wherever creating that
    // subdirectory was denied, and a logger that cannot open its file is worse than none, because
    // every call site believes the line was recorded.
    wchar_t base[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, base);
    if (length == 0 || length >= MAX_PATH) {
        base[0] = L'\0';   // no known writable location
    }

    std::swprintf(g_path, MAX_PATH, L"%sisland_bar.log", base);
    return g_path;
}

void OpenLog() {
    if (g_file) return;
    g_tried = true;

    // Opened with SHARING ALLOWED, not with _wfopen_s.
    //
    // _wfopen_s opens with no sharing at all, so while the bar is running its own log cannot be read
    // by anything else -- not by a viewer, not by the tooling used to develop it. The file was
    // written correctly and was completely unreadable for as long as it mattered, which is the worst
    // possible property for a diagnostic: it works only after the thing being diagnosed has stopped.
    //
    // _fsopen with _SH_DENYNO is the version that lets another process read the file while it is
    // being written. The "ccs=UTF-8" from the previous form has no _fsopen equivalent, so the
    // encoding is produced by writing wide strings and letting the stream translate them -- which is
    // what the previous form did under the hood anyway.
    //
    // "w" truncates: the log describes the current run, and an append-only file would grow without
    // bound for a program meant to run for days.
    //
    // _wfsopen and not _fsopen: the latter takes a narrow path, and the path here is wide.
    g_file = _wfsopen(LogPath(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (!g_file) {
        // Refused, which means SOMETHING ELSE ALREADY HAS THE FILE -- another island_bar, in another
        // session. Falling back to %TEMP% keeps this run's diagnostics, and it is announced because the
        // alternative is a reader looking at the other process's output and believing it is this one's.
        // That happened: four consecutive runs were diagnosed against a stale log held open by a bar
        // running in a different session, and the numbers never matched what the source said.
        wchar_t base[MAX_PATH] = {};
        const DWORD length = GetTempPathW(MAX_PATH, base);
        if (length > 0 && length < MAX_PATH) {
            wchar_t fallback[MAX_PATH] = {};
            std::swprintf(fallback, MAX_PATH, L"%sisland_bar.log", base);
            g_file = _wfsopen(fallback, L"w, ccs=UTF-8", _SH_DENYNO);
            if (g_file) {
                std::swprintf(g_path, MAX_PATH, L"%s", fallback);
                LogLine(L"--- log beside the executable was unavailable; ANOTHER island_bar may be "
                        L"running ---");
                LogAndPrint("  log: the file beside the executable is in use, so this run logs to %ls",
                            fallback);
            }
        }
    }
    if (!g_file) {
        // The shared temp name can itself be held by another session or a
        // scanner. A PID-qualified final fallback guarantees this run still
        // leaves diagnostics instead of becoming invisible.
        wchar_t base[MAX_PATH] = {};
        const DWORD length = GetTempPathW(MAX_PATH, base);
        if (length > 0 && length < MAX_PATH) {
            wchar_t fallback[MAX_PATH] = {};
            std::swprintf(fallback, MAX_PATH, L"%sisland_bar-%lu.log", base,
                          static_cast<unsigned long>(GetCurrentProcessId()));
            g_file = _wfsopen(fallback, L"w, ccs=UTF-8", _SH_DENYNO);
            if (g_file) std::swprintf(g_path, MAX_PATH, L"%s", fallback);
        }
    }
    if (!g_file) {
        return;
    }

    LogLine(L"--- island_bar starting (pid %lu) ---",
            static_cast<unsigned long>(GetCurrentProcessId()));

    // The path is recorded on its own line, and printed to the console.
    //
    // It was asked for twice over the course of a session and not found either time: the path is
    // under %TEMP%, but %TEMP% in the shell that asked was not the %TEMP% this process had, so the
    // file was looked for in a directory it was never written to. A logger nobody can find is
    // worth no more than no logger, so the program states where it wrote. Printed as well as
    // logged, because the console is where someone reporting a problem is actually looking.
    //
    // Written through LogLine rather than LogAndPrint on purpose: this is reached FROM OpenLog,
    // and LogAndPrint -> LogLine -> OpenLog would re-enter. Safe today only because g_file is
    // already set, which is a fragile thing to depend on.
    const wchar_t* path = LogPath();
    LogLine(L"log file: %ls", path);

    // The console copy, guarded so it cannot come back here.
    if (StdoutIsUsable()) {
        wchar_t line[MAX_PATH + 16] = {};
        std::swprintf(line, CountOf(line), L"log file: %ls", path);
        PrintOneLineImpl(line);
    }
}

void CloseLog() {
    if (g_file) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void LogLine(const wchar_t* format, ...) {
    if (!g_tried) OpenLog();

    wchar_t body[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(body, CountOf(body), _TRUNCATE, format, args);
    va_end(args);

    wchar_t stamp[32] = {};
    Timestamp(stamp, 32);

    if (g_file) {
        std::fwprintf(g_file, L"[%s] %s", stamp, body);
        const size_t length = std::wcslen(body);
        if (length == 0 || body[length - 1] != L'\n') {
            std::fputwc(L'\n', g_file);
        }
        // Flushed on every line on purpose: this process can be killed at any
        // moment, and a lost log is how an undiagnosable failure happens.
        std::fflush(g_file);
    }
}

void LogAndPrint(const wchar_t* format, ...) {
    wchar_t body[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(body, CountOf(body), _TRUNCATE, format, args);
    va_end(args);

    // The log keeps the caller's text as written; only the console trims, so a
    // deliberate blank line in the report still shows up in the file.
    LogLine(L"%s", body);
    PrintLine(body);
}

void LogAndPrint(const char* format, ...) {
    char narrow[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(narrow, CountOf(narrow), _TRUNCATE, format, args);
    va_end(args);

    // Diagnostics are ASCII by design, so a byte-to-wide widening is enough; this
    // deliberately does not pretend to handle a multi-byte codepage.
    wchar_t wide[2048] = {};
    for (size_t i = 0; i < CountOf(wide) - 1 && narrow[i] != '\0'; ++i) {
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(narrow[i]));
    }

    LogLine(L"%s", wide);
    PrintLine(wide);
}

void PrintLine(const char* text) {
    if (!StdoutIsUsable() || !text) return;
    // See the wide overload: the copy exists so a literal can be trimmed in place.
    wchar_t buffer[2048] = {};
    size_t i = 0;
    for (; i < CountOf(buffer) - 1 && text[i] != '\0'; ++i) {
        buffer[i] = static_cast<wchar_t>(static_cast<unsigned char>(text[i]));
    }
    PrintOneLineImpl(buffer);
}

} // namespace island
