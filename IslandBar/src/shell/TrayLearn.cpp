// =====================================================================
//  TrayLearn.cpp -- the driver behind TrayLearn.h
//
//  Four steps, and the third one is the only interesting one:
//
//      1. offer the hook module to every running tray application (one thread hook per thread);
//      2. ask every application to register its icon again -- TaskbarCreated;
//      3. read what the module wrote down, resolve each window's class, merge into the cache;
//      4. report coverage: how many of the card's icons can now be pressed without a real click.
//
//  THE REPORT IS A FILE, and that is a decision rather than a shortcut. The module runs inside someone
//  else's process, in one process per application, and a file append is the one channel that needs no
//  protocol, no lifetime agreement and no extra process -- and it survives the module being unable to ask us
//  anything. It is truncated at the start of every run, so it never grows without bound.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================
#include "shell/TrayLearn.h"

#include "core/Log.h"

// The channel the module publishes on, and the wire format of one record (see platform/TrayRing.h).
#include "domain/TrayRegistration.h"
#include "domain/TrayLedger.h"
#include "platform/TrayRing.h"

#include <psapi.h>   // GetProcessMemoryInfo: which of an application's processes carries its tray icon
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace island {

// Drains the shared-memory ring the module publishes on, writes the records it finds as report lines, and
// returns how many it took. Declared here (and not inside the anonymous namespace below, which is where its
// callers live) because its definition is a public function of this module; see platform/TrayRing.h.
int DrainTrayRing(TrayLearnResult* result);

// Defined with the ledger further down, and called by the pass when it has asked every application to
// register again -- which is the moment the ledger stops being a partial picture (see TrayLearn.h).
void MarkLiveTraySeeded();

namespace {

// A GUID as the registry and the cache file spell it: braces, upper-case hex. Hand-written rather than
// StringFromGUID2, which lives in ole32 and would add a dependency to a file that needs one call of it.
std::wstring GuidText(const GUID& guid) {
    wchar_t text[64] = {};
    _snwprintf_s(text, 64, _TRUNCATE, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                 static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
                 guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
                 guid.Data4[7]);
    return text;
}

const wchar_t* kCacheFile = L"tray_callbacks.txt";
const wchar_t* kReportFile = L"tray_learned.txt";
const wchar_t* kHookModule = L"island_tray_hook.dll";

// THE COPY OF THE MODULE THAT IS ACTUALLY OFFERED TO APPLICATIONS, NAMED AFTER ITS OWN CONTENTS.
//
// The module PINS itself inside every application it enters (see TrayHook.cpp), because the alternative
// shipped a crash: it used to be unloaded at the end of the learning window while the applications' import
// slots still pointed at it, and Explorer and wallpaper64.exe died of it on 2026-10-15. A pinned module --
// and, more simply, a loaded DLL file -- cannot be deleted or overwritten while any process has it, so the
// build's own `island_tray_hook.dll` must never be the file that gets loaded: replacing it would fail until
// every hooked application on the machine had restarted, which for Explorer means a reboot.
//
// So the bar stages a copy under a name derived from the module's CONTENT (`island_tray_hook-<fnv64>.dll`)
// and loads that. Rebuilding with a change produces a new name, which no lock can refuse; rebuilding
// without a change reuses the same file; and an older copy taken over by a newer one (the import walk
// accepts any `island_tray_hook*` pile, see TrayHook.cpp) stops being called even though it stays mapped
// in the applications that already had it.
//
// Copies that are no longer the current one are deleted here, and a copy that is still mapped somewhere
// simply refuses to be deleted -- which is the same information as "some application is still running the
// previous build", and costs nothing.
// WHETHER THIS PROCESS ALREADY HOLDS THE MODULE -- a different question from "did it load during this pass",
// and the one that matters now.
//
// The module PINS itself in every application it enters (see TrayHook.cpp), so an application reached by an
// earlier session, or by the startup pass ten seconds ago, keeps it for the rest of ITS life and keeps
// reporting every registration through it. The report file is truncated at the start of every pass, so such
// an application writes nothing during a later one -- and the log line then said "the module loaded in 0"
// about a machine where three applications were being watched the whole time. A number that reads like a
// failure while the mechanism is working is worse than no number at all.
// Whether THIS staged build is already mapped in that process.
//
// IT USED TO ASK "IS ANY island_tray_hook* IN THERE", AND THAT WAS WRONG THE MOMENT THE MODULE CHANGED.
// The staged file is named after its content hash (StageHookModule), so a new build is never blocked by a
// file lock -- but the question asked here was about the NAME PREFIX, so a process still holding yesterday's
// module was counted as "already watching" and the new build was never offered to it. Measured on 2026-10-15:
// after deploying a module whose only change was which messages it observes, `--tray-learn` reported
// "the module loaded in 0 of them and was ALREADY watching 3 more" -- Explorer among them, still running the
// old copy, which is why the experiment produced no data at all. Comparing the staged file's own name makes
// an upgrade take effect in the processes that are still running, which is the point of staging by hash.
bool AlreadyHasThisModule(DWORD pid, const wchar_t* stagedLeaf) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, stagedLeaf) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::wstring StageHookModule(const std::wstring& built) {
    const size_t slash = built.find_last_of(L'\\');
    const std::wstring directory = (slash == std::wstring::npos) ? std::wstring() : built.substr(0, slash + 1);

    unsigned long long hash = 1469598103934665603ull;   // FNV-1a, 64 bit
    {
        FILE* stream = nullptr;
        if (_wfopen_s(&stream, built.c_str(), L"rb") != 0 || !stream) return built;
        unsigned char buffer[65536];
        size_t got = 0;
        while ((got = fread(buffer, 1, sizeof(buffer), stream)) > 0) {
            for (size_t i = 0; i < got; ++i) {
                hash ^= buffer[i];
                hash *= 1099511628211ull;
            }
        }
        fclose(stream);
    }

    wchar_t name[64] = {};
    _snwprintf_s(name, 64, _TRUNCATE, L"island_tray_hook-%016llX.dll", hash);
    const std::wstring staged = directory + name;

    if (GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !CopyFileW(built.c_str(), staged.c_str(), TRUE)) {
        // A copy that cannot be made is not fatal: the built module still works. Only the upgrade path
        // suffers, and the bar says so in its log rather than pretending.
        LogAndPrint(L"  tray learn: could not stage %ls as %ls (%lu) -- loading it where it is\n",
                    kHookModule, name, GetLastError());
        return built;
    }

    WIN32_FIND_DATAW found = {};
    const std::wstring pattern = directory + L"island_tray_hook-*.dll";
    const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring candidate = directory + found.cFileName;
            if (_wcsicmp(candidate.c_str(), staged.c_str()) != 0) DeleteFileW(candidate.c_str());
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return staged;
}

std::wstring HomePath(const wchar_t* leaf) {
    wchar_t home[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::wstring();
    return std::wstring(home) + L"\\IslandBar\\" + leaf;
}

std::wstring BaseNameLower(const wchar_t* path) {
    if (!path || !*path) return std::wstring();
    const wchar_t* slash = wcsrchr(path, L'\\');
    std::wstring name = slash ? std::wstring(slash + 1) : std::wstring(path);
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    return name;
}

// The processes running an image, by file name. Toolhelp rather than a snapshot of every process, for the
// same reason TrayPress uses it: the name is what the icon's registry entry gives us.
std::vector<DWORD> PidsForImage(const wchar_t* exePath) {
    std::vector<DWORD> pids;
    const std::wstring wanted = BaseNameLower(exePath);
    if (wanted.empty()) return pids;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring name = entry.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == wanted) pids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}

// ONE process per application, and the heaviest one. Measured: WeChat runs five processes and the tray icon
// lives in the largest; hooking the lightest one sees nothing at all.
DWORD HeaviestPid(const std::vector<DWORD>& pids) {
    DWORD best = 0;
    SIZE_T bestBytes = 0;
    for (DWORD pid : pids) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) continue;
        PROCESS_MEMORY_COUNTERS counters = {};
        if (GetProcessMemoryInfo(process, &counters, sizeof(counters)) &&
            counters.WorkingSetSize > bestBytes) {
            bestBytes = counters.WorkingSetSize;
            best = pid;
        }
        CloseHandle(process);
    }
    return best;
}

// The value of ` key ` in a report line, as a bare token.
bool Field(const wchar_t* line, const wchar_t* key, wchar_t* out, size_t count) {
    wchar_t needle[32] = {};
    _snwprintf_s(needle, 32, _TRUNCATE, L" %ls ", key);
    const wchar_t* found = wcsstr(line, needle);
    if (!found) return false;
    found += wcslen(needle);
    size_t i = 0;
    while (found[i] && found[i] != L' ' && found[i] != L'"' && i + 1 < count) {
        out[i] = found[i];
        ++i;
    }
    out[i] = 0;
    return i > 0;
}

bool FieldNumber(const wchar_t* line, const wchar_t* key, unsigned* out, bool hex) {
    wchar_t text[32] = {};
    if (!Field(line, key, text, 32)) return false;
    wchar_t* end = nullptr;
    *out = static_cast<unsigned>(wcstoul(text, &end, hex ? 16 : 10));
    return end != text;
}

// The image name a report line points at.
//
// The shell-side lines (TrayHook.cpp::ReportShellCopy) name the process that owns the tray window, because the
// shell asks the window rather than the registration, and a process it does not own may refuse to be named --
// an elevated one does. The pid is always there, and this process table is readable for any process, so the
// name is filled in from here when the line could not state it. Lower case, like every other exe name in the
// cache file: the registry and the window both spell it however they like.
std::wstring ExeLeafForPid(unsigned pid) {
    static std::vector<std::pair<unsigned, std::wstring>> known;
    for (const std::pair<unsigned, std::wstring>& entry : known) {
        if (entry.first == pid) return entry.second;
    }
    std::wstring found;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID != pid) continue;
                found = entry.szExeFile;
                std::transform(found.begin(), found.end(), found.begin(), ::towlower);
                break;
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    known.push_back(std::make_pair(pid, found));
    return found;
}

// `exe` if the line could state it, the pid's image name otherwise. False when neither is available.
bool LineExe(const wchar_t* line, wchar_t* out, size_t count) {
    if (Field(line, L"exe", out, count) && out[0] && _wcsicmp(out, L"-") != 0) return true;
    unsigned pid = 0;
    if (!FieldNumber(line, L"pid", &pid, false) || pid == 0) return false;
    const std::wstring name = ExeLeafForPid(pid);
    if (name.empty()) return false;
    wcsncpy_s(out, count, name.c_str(), _TRUNCATE);
    return true;
}

// One learned entry, ready for the cache file.
struct Learned {
    std::wstring exe;
    unsigned message = 0;
    std::wstring windowClass;
    unsigned uid = 0;
    unsigned version = 0;
    std::wstring guid;   // "{...}" for an application that named its icon with a GUID instead of a uid
};

// THE ONE GLOBAL HOOK, HELD FOR THE WHOLE SESSION -- MYDOCKFINDER'S OWN MECHANISM, MEASURED ON THIS MACHINE.
//
// What was here before: one thread hook per thread of each running tray application, installed for the length
// of a learning pass and then removed. It covers the applications that were running at that moment, it needs
// a fresh pass every time a new application appears, and it has to be told to try again with a TaskbarCreated
// broadcast. It also -- measured, 2026-10-15, and written up in HANDOFF 8.109 -- is what left Explorer and
// wallpaper64.exe with import slots pointing at an unloaded module.
//
// What MyDockFinder does (HANDOFF 8.103, measured from its own binaries): `Dockmod.exe` calls
// `SetWindowsHookExA` and its `dockmod64.dll` is then mapped in the shell AND in a list of applications,
// including ones started later. ONE hook, installed once, and the operating system does the delivery to every
// GUI process that pumps messages -- which is exactly the coverage this project has been trying to assemble
// application by application.
//
// So: installed once at startup from the bar's main thread, never removed while the bar lives, and the module
// pins itself in each process it lands in (TrayHook.cpp), so an application keeps reporting every later
// registration even after the bar exits.
//
// The handle is never freed on purpose: freeing it would be the very thing that used to leave dangling import
// slots behind. When this process dies the system removes the hook, and the pinned modules stay mapped and
// inert in the applications that already have them.
HMODULE g_hookModule = nullptr;
// The staged copy's own file name ("island_tray_hook-<hash>.dll"), kept so the install pass can tell "this
// process already has THE BUILD WE ARE OFFERING" from "it has some older copy of ours" (see
// AlreadyHasThisModule).
std::wstring g_stagedLeaf;

const wchar_t* StagedLeaf() {
    if (!g_stagedLeaf.empty()) return g_stagedLeaf.c_str();
    // Nothing staged yet (InstallTrayHook has not run): fall back to the plain name, which is what a process
    // could only be holding if it was loaded by hand.
    return kHookModule;
}

// The lines already in the cache: what this run must not duplicate, and what it may have to correct (an
// application that changed its callback message).
std::vector<std::wstring> ReadCacheLines() {
    std::vector<std::wstring> lines;
    const std::wstring file = HomePath(kCacheFile);
    if (file.empty()) return lines;
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, file.c_str(), L"r, ccs=UTF-8") != 0 || !stream) return lines;
    wchar_t buffer[1024] = {};
    while (fgetws(buffer, 1024, stream)) lines.push_back(buffer);
    fclose(stream);
    return lines;
}

// The GUID a cache line carries, as the eighth field, or empty. The file's format:
//
//     exe|message|window class|uid|both|version|recipe|{GUID}
//
// and the GUID is written only for an application that named its icon with one (see TrayHook.cpp): such an
// icon's uid is 0, and 0 is not an identity -- several icons of one application share it.
std::wstring LineGuid(const std::wstring& line) {
    if (line.empty() || line[0] == L'#') return std::wstring();
    // BY SHAPE, NOT BY POSITION: the GUID is written after whatever else the line carries, and an entry with
    // no left-press recipe has it in the seventh field while one with a recipe has it in the eighth. Measured
    // the hard way -- a fixed field number silently never matched, and a GUID icon looked unpressable while
    // its entry sat in the file.
    size_t start = 0;
    for (;;) {
        const size_t bar = line.find(L'|', start);
        if (bar == std::wstring::npos) return std::wstring();
        start = bar + 1;
        size_t end = line.find_first_of(L"|\r\n", start);
        if (end == std::wstring::npos) end = line.size();
        if (end > start + 2 && line[start] == L'{' && line[end - 1] == L'}') {
            return line.substr(start, end - start);
        }
    }
}

// Whether a cache line belongs to this icon: the executable, and then EITHER the uid OR the GUID.
bool LineIsFor(const std::wstring& line, const std::wstring& exe, unsigned uid,
               const std::wstring& guid = std::wstring()) {
    if (line.empty() || line[0] == L'#') return false;
    const size_t first = line.find(L'|');
    const size_t second = first == std::wstring::npos ? std::wstring::npos : line.find(L'|', first + 1);
    const size_t third = second == std::wstring::npos ? std::wstring::npos : line.find(L'|', second + 1);
    const size_t fourth = third == std::wstring::npos ? std::wstring::npos : line.find(L'|', third + 1);
    if (fourth == std::wstring::npos) return false;
    std::wstring name = line.substr(0, first);
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    if (name != exe) return false;
    const std::wstring lineGuid = LineGuid(line);
    if (!guid.empty() || !lineGuid.empty()) return lineGuid == guid;
    const std::wstring uidText = line.substr(third + 1, fourth - third - 1);
    return static_cast<unsigned>(wcstoul(uidText.c_str(), nullptr, 10)) == uid;
}

// =====================================================================
//  HOW THIS SIDE READS THE REPORT -- FOR THE SAME REASON IT WRITES IT ITSELF
// =====================================================================
//
// A file handle that shares read, write AND delete, and the whole file read in one go. `_wfopen_s(..., L"r")`
// is REFUSED with a sharing violation while the module holds this file for writing, and it fails silently, so
// the pass reported `0 registration(s) captured` with its own lines sitting in the file it had just written
// (measured 2026-10-16) -- while the next process to run, whose pass deleted the file first and left the
// module holding a handle to an unlinked file, counted all of them. Reading the report is not allowed to
// depend on who happens to have it open.
class ReportReader {
public:
    bool Open(const std::wstring& path) {
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        std::vector<char> bytes(1 << 20, 0);
        DWORD read = 0;
        const BOOL ok = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size() - 1), &read, nullptr);
        CloseHandle(handle);
        if (!ok || read == 0) return text.empty();
        const int wide = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(read), nullptr, 0);
        if (wide <= 0) return false;
        text.assign(static_cast<size_t>(wide), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(read), text.data(), wide);
        offset = 0;
        return true;
    }

    // One line at a time, in the shape fgetws used to hand out: NUL-terminated, the newline removed.
    bool Next(wchar_t* out, size_t count) {
        if (!out || count == 0 || offset >= text.size()) return false;
        size_t end = text.find(L'\n', offset);
        if (end == std::wstring::npos) end = text.size();
        size_t length = end - offset;
        if (length > count - 1) length = count - 1;
        std::wmemcpy(out, text.data() + offset, length);
        out[length] = L'\0';
        offset = end + 1;
        return true;
    }

private:
    std::wstring text;
    size_t offset = 0;
};

// EVERYTHING THE MODULE HAS WRITTEN SO FAR, MERGED INTO THE CACHE THE PRESS PATH READS.
//
// Separate from the pass that installs the module, and that separation is what makes the module resident
// worth having: once an application has been reached, it keeps reporting (its own copy of the module stays
// mapped and its import slot stays redirected -- see TrayHook.cpp), so a registration that happens an hour
// later -- a new icon, an application restarted from its own menu -- arrives in this same file and is picked
// up by the next call of this function. The file is small, and merging twice is a no-op: a line that is
// already in the cache matches an existing entry and is skipped.
//
// Truncated by the installing pass (see LearnTrayCallbacks), so it never grows without bound.
void HarvestReport(TrayLearnResult* result) {
    if (!result) return;
    // FIRST THE RING, THEN THE FILE. The module publishes registrations into shared memory (it runs inside the
    // shell's message handling, where a disk write would be paid for by the registering application), and this
    // turns whatever is new into the same lines the file holds. Called from here rather than from one caller so
    // that every path -- the five-second tick and a --tray-learn pass -- sees registrations in the same order.
    DrainTrayRing(result);
    const std::wstring report = HomePath(kReportFile);
    std::vector<Learned> learned;
    if (!report.empty()) {
        // ---- PASS ONE: THE VERSION AN APPLICATION ACTUALLY ASKED FOR ------------------------------------
        //
        // NOTIFYICONDATA's `uVersion` shares its storage with `uTimeout` -- one `union` in the structure, and
        // the classic reference for this API prints it exactly that way (cnblogs.com/findumars/p/5086716,
        // "系统托盘编程完全指南", part 3). So the value read at NIM_ADD is the TIMEOUT slot for most
        // applications and reads 0, which means "never said" rather than "version 3"; the shell only learns
        // the real one from the NIM_SETVERSION call that normally follows it, and that call is a line this
        // reader used to skip entirely.
        //
        // Read first and applied afterwards, so the order the two calls arrived in cannot matter.
        struct VersionFact {
            std::wstring exe;
            unsigned uid = 0;
            std::wstring guid;
            unsigned version = 4;
        };
        std::vector<VersionFact> versions;
        {
            ReportReader reader;
            if (reader.Open(report)) {
                wchar_t line[1024] = {};
                while (reader.Next(line, 1024)) {
                    if (!wcsstr(line, L"NIM_SETVERSION")) continue;
                    wchar_t exe[260] = {};
                    if (!LineExe(line, exe, 260)) continue;
                    unsigned uid = 0;
                    unsigned version = 0;
                    if (!FieldNumber(line, L"uid", &uid, false)) continue;
                    if (!FieldNumber(line, L"version", &version, false)) continue;
                    if (version != 3 && version != 4) continue;
                    wchar_t guid[64] = {};
                    if (Field(line, L"guid", guid, 64) && guid[0] == L'-') guid[0] = 0;
                    versions.push_back(VersionFact{BaseNameLower(exe), uid, guid, version});
                }
            }
        }
        const auto statedVersion = [&versions](const std::wstring& exe, unsigned uid,
                                              const std::wstring& guid) -> unsigned {
            for (const VersionFact& fact : versions) {
                if (fact.exe != exe) continue;
                if (!guid.empty() || !fact.guid.empty()) {
                    if (fact.guid == guid) return fact.version;
                    continue;
                }
                if (fact.uid == uid) return fact.version;
            }
            return 0;   // nothing said: the caller keeps whatever the registration itself carried
        };

        ReportReader reader;
        if (reader.Open(report)) {
            wchar_t line[1024] = {};
            std::vector<DWORD> attached;
            while (reader.Next(line, 1024)) {
                if (wcsstr(line, L"attached pid")) {
                    unsigned pid = 0;
                    if (FieldNumber(line, L"pid", &pid, false) &&
                        std::find(attached.begin(), attached.end(), pid) == attached.end()) {
                        attached.push_back(pid);
                    }
                    continue;
                }
                // ... and the same line read on the shell's side ("S") counts exactly as much as the one read
                // inside the application ("W"/"A"): since HANDOFF 8.114 that is where the answers come from.
                if (!wcsstr(line, L"call W NIM_ADD") && !wcsstr(line, L"call W NIM_MODIFY") &&
                    !wcsstr(line, L"call A NIM_ADD") && !wcsstr(line, L"call A NIM_MODIFY") &&
                    !wcsstr(line, L"call S NIM_ADD") && !wcsstr(line, L"call S NIM_MODIFY")) {
                    continue;
                }
                Learned item;
                wchar_t exe[260] = {};
                wchar_t hwndText[32] = {};
                if (!LineExe(line, exe, 260) || !Field(line, L"hwnd", hwndText, 32)) continue;
                unsigned uid = 0;
                unsigned message = 0;
                unsigned version = 0;
                if (!FieldNumber(line, L"uid", &uid, false)) continue;
                if (!FieldNumber(line, L"msg", &message, true)) continue;
                if (!FieldNumber(line, L"version", &version, false)) version = 0;
                wchar_t guid[64] = {};
                if (Field(line, L"guid", guid, 64) && guid[0] == L'-') guid[0] = 0;
                if (message == 0) continue;
                const HWND window = reinterpret_cast<HWND>(wcstoull(hwndText, nullptr, 16));
                if (!window || !IsWindow(window)) continue;
                wchar_t cls[128] = {};
                if (GetClassNameW(window, cls, 128) == 0 || !cls[0]) continue;
                item.exe = BaseNameLower(exe);
                item.message = message;
                item.windowClass = cls;
                item.uid = uid;
                item.version = version;
                item.guid = guid;
                // ... and the version from the NIM_SETVERSION call wins when there was one: see pass one.
                const unsigned stated = statedVersion(item.exe, item.uid, item.guid);
                if (stated) item.version = stated;
                ++result->captured;
                learned.push_back(item);
            }
            result->appsReached = static_cast<int>(attached.size());
        }
    }

    std::vector<std::wstring> lines = ReadCacheLines();
    for (const Learned& item : learned) {
        bool replaced = false;
        bool duplicate = false;
        for (std::wstring& line : lines) {
            // THE SAME ICON, AND THAT IS NOT THE SAME QUESTION AS "THE SAME LINE" since GUIDs were added.
            //
            // A line written before this build knows nothing about the GUID (field 8 did not exist), and the
            // identity a press matches on is the GUID -- so matching the merge on the GUID alone would fail to
            // find that line and append a second one for the same icon, once per pass. Measured: Explorer's
            // Bluetooth icon ended up in the cache four times before this rule existed. The merge therefore
            // matches by executable and uid, and by GUID when both sides have one.
            const bool sameUid = LineIsFor(line, item.exe, item.uid);
            const std::wstring lineGuid = LineGuid(line);
            const bool sameGuid = !item.guid.empty() && !lineGuid.empty() && lineGuid == item.guid;
            const bool legacy = !item.guid.empty() && lineGuid.empty() && sameUid;
            if (!LineIsFor(line, item.exe, item.uid, item.guid) && !sameGuid && !legacy) continue;
            if (!item.guid.empty() && !lineGuid.empty() && lineGuid != item.guid) continue;
            if (item.guid.empty() && !lineGuid.empty()) continue;
            // A line that says `click` is a verdict from a press: this application ignores its own posted
            // callback. Learning must not talk the press path back into trying again every session.
            if (wcsstr(line.c_str(), L"|none") != nullptr || wcsstr(line.c_str(), L"|click") != nullptr) {
                duplicate = true;
                break;
            }
            if (wcsstr(line.c_str(), item.windowClass.c_str()) != nullptr) {
                duplicate = true;   // same application, same icon, same window class: nothing to say
                break;
            }
            wchar_t fresh[512] = {};
            _snwprintf_s(fresh, 512, _TRUNCATE, L"%ls|0x%04X|%ls|%u|both|%u%ls%ls\n", item.exe.c_str(),
                         item.message, item.windowClass.c_str(), item.uid, item.version,
                         item.guid.empty() ? L"" : L"|", item.guid.c_str());
            line = fresh;
            replaced = true;
            break;
        }
        if (duplicate) continue;
        if (!replaced) {
            wchar_t fresh[512] = {};
            _snwprintf_s(fresh, 512, _TRUNCATE, L"%ls|0x%04X|%ls|%u|both|%u%ls%ls\n", item.exe.c_str(),
                         item.message, item.windowClass.c_str(), item.uid, item.version,
                         item.guid.empty() ? L"" : L"|", item.guid.c_str());
            lines.push_back(fresh);
        }
        ++result->newEntries;
    }

    if (result->newEntries > 0) {
        const std::wstring file = HomePath(kCacheFile);
        FILE* stream = nullptr;
        if (!file.empty() && _wfopen_s(&stream, file.c_str(), L"w, ccs=UTF-8") == 0 && stream) {
            for (const std::wstring& line : lines) fputws(line.c_str(), stream);
            fclose(stream);
        }
    }
}

} // namespace

int CountCoveredIcons(const TrayIcons& icons) {
    const std::vector<std::wstring> lines = ReadCacheLines();
    int covered = 0;
    for (int i = 0; i < icons.Count(); ++i) {
        const std::wstring exe = BaseNameLower(icons.ExePathAt(i));
        if (exe.empty()) continue;
        const TrayIcon& icon = icons.At(i);
        // AN ICON REGISTERED BY GUID IS MATCHED BY THAT GUID, and until the module started reporting it that
        // icon could not be matched at all: its uid is 0, its entry's uid is 0, and the first version of this
        // function counted it as covered on the strength of that shared zero -- which on this machine meant
        // Explorer's Bluetooth icon answering for Explorer's network and volume icons.
        const std::wstring guidText = icon.hasGuid ? GuidText(icon.guid) : std::wstring();
        for (const std::wstring& line : lines) {
            if (LineIsFor(line, exe, icon.uid, guidText)) {
                ++covered;
                break;
            }
        }
    }
    return covered;
}

// Loads the staged copy of the module and keeps it for the session. NO HOOK IS INSTALLED HERE.
//
// A GLOBAL HOOK CANNOT BE USED WITH THIS DLL, and that is measured rather than cautious. The mechanism
// itself is MyDockFinder's (HANDOFF 8.110: `One global WH_GETMESSAGE hook`), and it was built and it worked
// -- but on 2026-10-15, minutes after it went in, the user reported "黑屏": Clash Verge's window came up
// black. Measured with the hook installed against the same application restarted without it:
//
//     island running   -> 6 msedgewebview2 processes, the window's client area mean brightness 0  (blank)
//     island closed    -> 12 msedgewebview2 processes, mean brightness 56 (its normal dark UI, rendered)
//
// and our module was mapped in clash-verge.exe but in NONE of the WebView2 processes. Chromium's
// RendererCodeIntegrity (Microsoft-signed images only, on by default for renderer processes) refuses a
// pending injection of an UNSIGNED module: the renderer never finishes starting, so nothing is ever painted.
// MyDockFinder's dockmod64.dll is a signed commercial binary, which is why one global hook works for it and
// not for this project's build.
//
// So the module is offered to the TRAY APPLICATIONS on the card, one process at a time (see
// LearnTrayCallbacks) -- a scope that never includes a browser or a webview, and that the pin (TrayHook.cpp)
// then makes permanent for the life of each of those processes.
bool InstallTrayHook() {
    // --- where this bar and the module are ------------------------------------------------
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return false;
    std::wstring modulePath = self;
    const size_t slash = modulePath.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;
    modulePath = modulePath.substr(0, slash + 1) + kHookModule;
    // The staged copy, not the built file: see StageHookModule for why the file that gets loaded may not be
    // the file a later build replaces.
    modulePath = StageHookModule(modulePath);
    {
        const size_t leaf = modulePath.find_last_of(L'\\');
        g_stagedLeaf = (leaf == std::wstring::npos) ? modulePath : modulePath.substr(leaf + 1);
    }

    if (!g_hookModule) {
        g_hookModule = LoadLibraryW(modulePath.c_str());
        if (!g_hookModule) {
            LogAndPrint(L"  tray hook: no %ls next to the bar (%lu) -- nothing can be learned\n", kHookModule,
                        GetLastError());
            return false;
        }
    }
    // THE ENTRY POINT MUST COME FROM THAT MODULE. A procedure address belonging to another binary is refused
    // for a cross-process hook, and refused silently: measured, the module loaded into the target and the
    // procedure was then never called once (HANDOFF 8.107).
    FARPROC procedure = GetProcAddress(g_hookModule, "IslandTrayHookProc");
    if (!procedure) {
        LogAndPrint(L"  tray hook: %ls does not export IslandTrayHookProc\n", kHookModule);
        return false;
    }

    LogAndPrint(L"  tray hook: module ready at %ls (offered to the shell's own process -- a global hook is "
                L"refused by signed-only renderer processes, see TrayLearn.cpp)\n",
                modulePath.c_str());
    return true;
}

// Defined beside the ring reader, below; this pass waits on what it fills in.
bool FillObserverResult(TrayLearnResult* result, DWORD pid);

TrayLearnResult LearnTrayCallbacks(const TrayIcons& icons, bool broadcast) {
    TrayLearnResult result;
    result.icons = icons.Count();
    const ULONGLONG start = GetTickCount64();

    // The staged module, loaded once (see InstallTrayHook for why there is no global hook any more).
    if (!InstallTrayHook()) return result;

    // --- the report starts empty, so everything in it belongs to this run -----------------
    const std::wstring report = HomePath(kReportFile);
    if (!report.empty()) DeleteFileW(report.c_str());

    // --- ONE PROCESS: THE SHELL'S OWN ------------------------------------------------------------
    //
    // WHICH ONE, AND WHY IT IS ENOUGH. An application does not keep its tray registration to itself: it SENDS
    // it to the window of class "Shell_TrayWnd" as a WM_COPYDATA (measured 2026-10-15, see ReportShellCopy in
    // TrayHook.cpp for the layout). So a module inside that window's process reads every registration on the
    // machine -- and it reads the ones an application-side hook can never reach: an AppContainer application
    // refuses the hook outright (ChatGPT.exe, SetWindowsHookEx error 87) and a self-protecting one accepts
    // 98 thread hooks and never loads the module (QQ Music). Both of them still register, and their copies
    // arrive here. Measured in the first run: ChatGPT.exe, AppContainer and all, arrived with uid 3,
    // message 0x8001, tip "ChatGPT" and guid E5768D8B-6936-4F45-B1AD-4C5FB414CB35.
    //
    // This replaced a pass that offered the module to every application on the card, one process at a time.
    // That pass was the reason the user kept asking why the tray had to be taught application by application;
    // it is gone (see HANDOFF 8.114). The module's in-application detour is still there and still writes what
    // it sees -- it simply is not offered to anybody unless a later decision asks for it again.
    //
    // A hook that cannot be installed is not an error: it is reported through `appsReached` below.
    std::vector<DWORD> tried;
    std::vector<HHOOK> hooks;
    std::vector<DWORD> hookThreads;
    const FARPROC procedure = GetProcAddress(g_hookModule, "IslandTrayHookProc");
    {
        // FindWindowW, not the card: the shell's tray window is the one that receives the registrations, and
        // it exists whether or not the card happens to show one of Explorer's own icons.
        const DWORD pid = TrayShellProcessId();
        if (pid == 0) {
            LogAndPrint(L"  tray hook: no Shell_TrayWnd on this desktop -- nothing can be learned\n");
        } else {
            tried.push_back(pid);
            ++result.appsTried;
            // A shell that already holds this exact module keeps observing without handing anything over
            // again (the handshake is written once per module load), so it is recorded rather than retried:
            // the wait below must not call that state a failure, and the summary line must not either.
            result.observerResident = AlreadyHasThisModule(pid, StagedLeaf());
        }
        if (procedure && pid != 0 && !AlreadyHasThisModule(pid, StagedLeaf())) {

            // The threads, from a snapshot: SetWindowsHookEx takes one thread per call, and a process already
            // holding THIS EXACT staged build needs no hook at all -- which is the normal case after the first
            // pass, because the module pins itself where it lands (TrayHook.cpp). A process holding an OLDER
            // copy is not skipped: see AlreadyHasThisModule for the measurement that forced the distinction.
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry = {};
                entry.dwSize = sizeof(entry);
                int seen = 0;
                DWORD lastError = 0;
                if (Thread32First(snapshot, &entry)) {
                    do {
                        if (entry.th32OwnerProcessID != pid) continue;
                        ++seen;
                        const HHOOK installed = SetWindowsHookExW(WH_GETMESSAGE,
                                                                  reinterpret_cast<HOOKPROC>(procedure),
                                                                  g_hookModule, entry.th32ThreadID);
                        if (installed) {
                            hooks.push_back(installed);
                            hookThreads.push_back(entry.th32ThreadID);
                        } else {
                            lastError = GetLastError();
                        }
                    } while (Thread32Next(snapshot, &entry));
                }
                CloseHandle(snapshot);
                // INSTALLED IS NOT THE SAME AS ARRIVED, and the difference used to be invisible: a hook is
                // accepted here and the module only lands in the target when one of those threads next takes a
                // message out of its queue. The count of accepted hooks is what tells the two failures apart
                // -- "the shell refused the hook" from "the shell never pumped".
                LogAndPrint(L"  tray hook: %d of %d thread hook(s) accepted for the shell (pid %lu, last "
                            L"error %lu)\n", static_cast<int>(hooks.size()), seen, pid, lastError);
            }
        }
    }

    // --- MAKE THE MODULE LAND NOW, INSTEAD OF WHEN SOMETHING ELSE WAKES THOSE THREADS -------------
    //
    // Accepting a hook is not the same as the module being resident: a WH_GETMESSAGE hook runs when the hooked
    // thread takes a message OUT OF ITS QUEUE, and until that happens this module is not in the shell's process
    // at all -- so everything below (the handshake included) waits on an event a quiet Explorer may not
    // produce. MEASURED 2026-10-16: the first pass after Explorer restarted captured 0 registrations from 143
    // thread hooks (57 accepted), and the pass minutes later, same build, captured 42 -- the module had landed
    // in between, on its own.
    //
    // A posted WM_NULL is the cheapest way to make a thread take a message out of its queue. It is inert (a
    // null window handle, no window procedure runs), and the retrieval is exactly the call the module is
    // waiting for.
    int poked = 0;
    for (DWORD threadId : hookThreads) {
        if (PostThreadMessageW(threadId, WM_NULL, 0, 0)) ++poked;
    }
    result.poked = poked;

    // --- WAIT FOR THE MODULE TO SAY IT IS LISTENING, THEN ASK ONCE ---------------------------------
    //
    // The module publishes what it installed (platform/TrayRing.h: a control record carrying pid, tray thread,
    // hook count, attempts and the error). Asking applications to register BEFORE that arrives is asking a
    // question nobody is there to hear: an application registers once per ask.
    //
    // The wait is bounded and is not a proxy for the handshake -- if it runs out, the pass still asks (an
    // application that registers late is better than one that is never asked) and says in the log that the
    // observer was not up.
    const DWORD shellPid = TrayShellProcessId();
    const ULONGLONG observerFrom = GetTickCount64();
    while (GetTickCount64() - observerFrom < kObserverWaitMs) {
        DrainTrayRing(&result);
        if (FillObserverResult(&result, shellPid)) break;
        Sleep(50);
    }
    result.observerMs = static_cast<long long>(GetTickCount64() - observerFrom);
    FillObserverResult(&result, shellPid);

    if (result.observerReady) {
        LogAndPrint(L"  tray hook: the observer is up in the shell (tray thread %u, %u hook(s), attempt %u, "
                    L"%d of %d hooked thread(s) took the message, %lld ms)\n",
                    result.observerTid, result.observerHooks, result.observerAttempts, poked,
                    static_cast<int>(hookThreads.size()), result.observerMs);
    } else if (result.observerResident) {
        // Not a failure: this shell has been watching since an earlier session, and it says so by
        // delivering registrations rather than by repeating its handshake.
        LogAndPrint(L"  tray hook: the shell has carried this module since an earlier session -- it observes "
                    L"without a fresh handshake; %d registration(s) arrived so far\n",
                    result.captured);
    } else {
        LogAndPrint(L"  tray hook: the observer did NOT report ready in %lld ms (tray thread %u, %u hook(s), "
                    L"attempt %u, last error %u) -- the registrations asked for below may go unread\n",
                    result.observerMs, result.observerTid, result.observerHooks, result.observerAttempts,
                    result.observerError);
    }
    std::fflush(stdout);

    // --- the signal that makes applications register again -------------------------------
    if (broadcast) {
        const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(HWND_BROADCAST, taskbarCreated, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000,
                            &ignored);
    }
    // --- WHAT THE ONE ASK PRODUCED, COLLECTED WHILE IT ARRIVES --------------------------------------
    //
    // One broadcast, then a drain loop: the second broadcast this used to make was hiding the ordering problem
    // the handshake above now solves (an ask that goes out before the observer is up fails the same way
    // whether it is the first or the second), while the flat sleep that replaced it was too short to see the
    // answers -- see kAnswerWaitMs for the measurement. Draining here rather than sleeping also means the
    // number this pass prints is what actually arrived, not what happened to be in the ring when it looked.
    const ULONGLONG answersFrom = GetTickCount64();
    while (GetTickCount64() - answersFrom < kAnswerWaitMs) {
        DrainTrayRing(&result);
        Sleep(200);
    }

    // THE HOOKS ARE UNHOOKED NOW, AND THAT IS HARMLESS, WHICH IS THE POINT OF THE PIN.
    //
    // Releasing the hook used to be what left Explorer and wallpaper64.exe with import slots pointing at an
    // unloaded module (HANDOFF 8.109). It is safe now because the module pinned itself inside each of those
    // processes: the code its slots point at stays mapped for the life of the process, and the detour keeps
    // reporting every registration from then on. Only this process's reference to the hook goes away -- the
    // handles are released rather than kept, because a hook held by a thread that has exited is a handle
    // nobody will ever release.
    for (HHOOK installed : hooks) UnhookWindowsHookEx(installed);

    // ... and "already watched" counts as reached: see AlreadyHasThisModule. It is counted per process, and
    // the same process can be counted twice if two icons of the card belong to it (Explorer's three), which
    // is why the log line calls these applications rather than processes.
    for (DWORD pid : tried) {
        if (AlreadyHasThisModule(pid, StagedLeaf())) ++result.appsAlreadyWatched;
    }

    HarvestReport(&result);

    // THE LEDGER CAN BE BELIEVED FROM HERE: every running tray application has just been asked to register
    // again, and everything that answered has been drained above. Until this line the card draws the
    // registry list instead of a ledger that only holds whatever happened to arrive first (see TrayLearn.h).
    MarkLiveTraySeeded();

    result.covered = CountCoveredIcons(icons);
    result.ms = static_cast<long long>(GetTickCount64() - start);
    return result;
}

DWORD TrayShellProcessId() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(tray, &pid);
    return pid;
}

// The NIM_ command as the report has always spelled it. The reader below parses these NAMES, so this is the one
// place the number becomes a word -- and it must match what the in-application path writes.
const char* CommandName(unsigned command) {
    switch (command) {
        case kTrayCopyAdd: return "NIM_ADD";
        case kTrayCopyModify: return "NIM_MODIFY";
        case kTrayCopyDelete: return "NIM_DELETE";
        case kTrayCopySetFocus: return "NIM_SETFOCUS";
        case kTrayCopySetVersion: return "NIM_SETVERSION";
        default: return "NIM_?";
    }
}

// =====================================================================
//  WHAT THE MODULE SAID IT INSTALLED, REMEMBERED PER PROCESS
// =====================================================================
//
// The hook state is a control record and not a registration (see platform/TrayRing.h), and it is kept HERE
// rather than inside whichever pass happens to read it: a module that is already resident published its
// verdict when it landed -- possibly minutes before this pass, and already consumed by the frame loop's tick
// -- so a pass that only looked at the records arriving DURING it would call a working observer absent. Two
// slots, because the only interesting pair is the shell that is running and the shell that just was.
namespace {
constexpr int kKnownObservers = 2;
struct KnownObserver {
    DWORD pid = 0;
    TrayHookState state;
    bool valid = false;
};
KnownObserver g_observers[kKnownObservers];
} // namespace

void RememberObserver(const TrayHookState& state) {
    if (state.pid == 0) return;
    for (KnownObserver& slot : g_observers) {
        if (slot.valid && slot.pid == state.pid) {
            slot.state = state;   // the newest verdict about a process replaces the one before it
            return;
        }
    }
    for (KnownObserver& slot : g_observers) {
        if (!slot.valid) {
            slot.pid = state.pid;
            slot.state = state;
            slot.valid = true;
            return;
        }
    }
    // Both slots hold a process that is no longer the shell: one of them is the shell that just went away.
    for (KnownObserver& slot : g_observers) {
        if (slot.pid != TrayShellProcessId()) {
            slot.pid = state.pid;
            slot.state = state;
            return;
        }
    }
    g_observers[0].pid = state.pid;
    g_observers[0].state = state;
}

const TrayHookState* KnownObserverFor(DWORD pid) {
    if (pid == 0) return nullptr;
    for (const KnownObserver& slot : g_observers) {
        if (slot.valid && slot.pid == pid) return &slot.state;
    }
    return nullptr;
}

// The observer's numbers into the result its caller prints. `pid` is the shell this pass is about. Returns
// true when the module is up, which is what the wait below stops on.
bool FillObserverResult(TrayLearnResult* result, DWORD pid) {
    const TrayHookState* state = KnownObserverFor(pid);
    if (!state || !result) return false;
    result->observerReady = state->ready;
    result->observerTid = state->trayThread;
    result->observerHooks = state->hooks;
    result->observerAttempts = state->attempts;
    result->observerError = state->lastError;
    return state->ready;
}

// =====================================================================
//  HOW THIS SIDE WRITES THE REPORT -- THE SAME WAY THE MODULE DOES
// =====================================================================
//
// A FILE_APPEND_DATA handle that shares read, write AND delete, and one WriteFile per line.
//
// WHY NOT _wfopen_s(..., L"a, ccs=UTF-8"), WHICH IS WHAT THIS USED TO DO. That is the exact failure the
// tray's first pass after an Explorer restart was made of, and it took a whole round to see it because it is
// SILENT: the module holds this file open for the life of the shell process (TrayHook.cpp: ReportHandle), and
// the CRT's append open is REFUSED with a sharing violation while that handle is alive -- measured 2026-10-16
// with the bar's own numbers: `35 record(s) taken` ... and not one line and not one error anywhere.
//
// The shape it produces is precisely the symptom this round started from: a pass that runs while the module
// has just landed in a NEW shell writes nothing and reports `0 registration(s) captured`, while the pass
// AFTER the file has been deleted once (the module then holding a handle to a file that no longer has a name)
// captures everything. The first ask learned nothing and the second one learned everything, and the
// difference was never the ask -- it was whether this side could write.
struct ReportAppender {
    HANDLE handle = INVALID_HANDLE_VALUE;

    ~ReportAppender() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }

    bool Open(const std::wstring& path) {
        handle = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle != INVALID_HANDLE_VALUE;
    }

    // UTF-8 explicitly, for the same reason the module is explicit: the file is read back by a reader that
    // parses it by name, whatever the codepage of the process that happens to be writing.
    bool Write(const wchar_t* line) {
        if (handle == INVALID_HANDLE_VALUE || !line) return false;
        char utf8[2048] = {};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        if (bytes <= 1) return false;
        DWORD written = 0;
        return WriteFile(handle, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr) != 0;
    }
};

// --- THE LEDGER OF WHAT THE SHELL IS HOLDING RIGHT NOW -------------------------------------------------
//
// Written by the drain below, read by the card on its own refresh tick, and those are two threads (the frame
// loop drains when nothing is being learned, the worker drains during a pass -- see main.cpp), so it is
// locked. The lock is held for one lookup at a time and never across a registry walk or a window call.
//
// WHY IT IS HERE rather than next to the card: this file owns the ring the registrations arrive on, so it is
// the only place that can keep a ledger up to date without inventing a second poll. See domain/TrayLedger.h
// for the rule, and shell/TrayIcons.cpp for the one place it decides what to draw.
std::mutex g_liveTrayLock;
TrayLedger g_liveTray;
bool g_liveTraySeeded = false;

void FeedLiveTray(const TrayRegistration& registration, unsigned ownerPid, const wchar_t* exeLeaf) {
    std::lock_guard<std::mutex> lock(g_liveTrayLock);
    g_liveTray.Apply(static_cast<TrayCopyCommand>(registration.command), ownerPid,
                     static_cast<unsigned long long>(registration.trayWindow), registration.uid,
                     registration.hasGuid, registration.guid, registration.tip, exeLeaf);
}

bool LiveTrayFind(const wchar_t* exeLeaf, unsigned uid, bool hasGuid, const unsigned char* guid,
                  std::wstring* tipOut) {
    std::lock_guard<std::mutex> lock(g_liveTrayLock);
    const TrayLiveRegistration* live = g_liveTray.Find(exeLeaf, uid, hasGuid, guid);
    if (!live) return false;
    if (tipOut) *tipOut = live->tip;
    return true;
}

int LiveTrayCount() {
    std::lock_guard<std::mutex> lock(g_liveTrayLock);
    return g_liveTray.Count();
}

int LiveTrayRefused() {
    std::lock_guard<std::mutex> lock(g_liveTrayLock);
    return g_liveTray.Refused();
}

void ResetLiveTray() {
    std::lock_guard<std::mutex> lock(g_liveTrayLock);
    g_liveTray.Reset();
    g_liveTraySeeded = false;
}

bool LiveTraySeeded() {
    std::lock_guard<std::mutex> lock(g_liveTrayLock);
    return g_liveTraySeeded;
}

// Called by the pass itself once it has asked every application to register again: until then the ledger is
// a partial picture and the card is not allowed to use it (see the header).
void MarkLiveTraySeeded() {
    std::lock_guard<std::mutex> lock(g_liveTrayLock);
    g_liveTraySeeded = true;
}

void ForgetDeadTrayWindows() {
    // Copied out first: `IsWindow` is a call into the window manager and there is no reason to keep the
    // ledger locked across it. The entries are then matched back by window, which is what they were copied
    // by.
    std::vector<TrayLiveRegistration> items;
    {
        std::lock_guard<std::mutex> lock(g_liveTrayLock);
        items.reserve(static_cast<size_t>(g_liveTray.Count()));
        for (int i = 0; i < g_liveTray.Count(); ++i) items.push_back(g_liveTray.At(i));
    }
    for (const TrayLiveRegistration& item : items) {
        const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(item.window));
        // A registration names the window that receives its callbacks; an application that has exited has
        // no windows left, and one that never had a readable window is not something to draw either.
        if (window != nullptr && IsWindow(window)) continue;
        std::lock_guard<std::mutex> lock(g_liveTrayLock);
        g_liveTray.ForgetWindow(item.window);
    }
}

// WHAT THE MODULE READ IN THE SHELL, TAKEN OUT OF THE RING AND WRITTEN DOWN.
//
// The module publishes one record per registration into shared memory and nothing else: it runs inside the
// shell's message handling, with the registering application waiting on it, so the reading, the formatting and
// the disk are the bar's job (see platform/TrayRing.h). This function is that job: it drains whatever is new,
// writes the same line the module used to write -- so the reader below, and the cache format, do not change --
// and reports how many records it took.
//
// THE OWNER IS RESOLVED HERE, from the window handle in the record, rather than being sent across. That is not
// an optimisation: it means the ring carries only what the shell actually told the application (the decoded
// registration) and the bar derives the rest the same way it does for a card cell.
int DrainTrayRing(TrayLearnResult* result) {
    static TrayRingReader reader;
    static unsigned cursor = 0;
    // THE TWO NUMBERS THAT SAY WHERE A RECORD WAS LOST, reported the first few times they are asked for. This
    // function is the only place where "the module published it" and "the bar learned it" can disagree, and it
    // disagreed silently once (2026-10-16): the ring held 170 records, one of them the probe registrar's own
    // icon, and the bar reported 0 captures with nothing anywhere saying which of the two sides was wrong.
    static volatile LONG explained = 0;
    static volatile LONG openFailures = 0;

    TrayRingRecord records[kTrayRingCapacity] = {};
    const unsigned taken = reader.Drain(cursor, records, kTrayRingCapacity);
    if (taken == 0) {
        // Nothing new. If a ring exists with a different ABI, say so once: two builds that disagree about the
        // layout must not sit there quietly learning nothing.
        //
        // ... and when the ring IS the one this build expects, the cursor and the writer's count are the whole
        // story: equal numbers mean every record has been read, and a cursor ahead of the writer means this
        // process read records another thread of this same bar consumed.
        if (reader.header && InterlockedIncrement(&explained) <= 3) {
            LogAndPrint(L"  tray ring: nothing new (written %ld, cursor %u)\n",
                        static_cast<long>(reader.header->written), cursor);
            std::fflush(stdout);
        }
        if (!reader.header && reader.abiSeen != 0 && reader.abiSeen != kTrayRingAbi && !result->ringAbiMismatch) {
            result->ringAbiMismatch = true;
            LogAndPrint(L"  tray learn: the module is publishing on ABI %u and this bar reads %u -- no "
                        L"registrations will arrive until the two match (see platform/TrayRing.h)\n",
                        reader.abiSeen, kTrayRingAbi);
            std::fflush(stdout);
        }
        return 0;
    }

    // CONTROL RECORDS FIRST, and never as a registration: the module's state is what the caller WAITS on, and
    // it is remembered per process rather than per pass (see RememberObserver).
    bool anyRegistration = false;
    for (unsigned i = 0; i < taken; ++i) {
        if (records[i].kind == TrayRingKind::HookState) {
            RememberObserver(records[i].hook);
            continue;
        }
        anyRegistration = true;
    }
    if (!anyRegistration) return static_cast<int>(taken);

    const std::wstring report = HomePath(kReportFile);
    int written = 0;
    ReportAppender appender;
    if (report.empty() || !appender.Open(report)) {
        // The line cannot be written, so nobody can learn from these records -- and that is said out loud
        // rather than counted as a capture. `captured` means "a registration that reached the cache's door",
        // and it is counted in ONE place (the reader that parses these lines, see HarvestReport): counting the
        // same record here as well made the number twice what it was.
        if (InterlockedIncrement(&openFailures) <= 3) {
            LogAndPrint(L"  tray ring: %u record(s) taken but '%ls' cannot be opened for append -- the "
                        L"registrations in them are LOST\n",
                        taken, report.c_str());
            std::fflush(stdout);
        }
        return static_cast<int>(taken);
    }
    for (unsigned i = 0; i < taken; ++i) {
        if (records[i].kind != TrayRingKind::Registration) continue;
        const TrayRegistration& registration = records[i].registration;
        if (!registration.valid) continue;

        const HWND trayWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(registration.trayWindow));
        DWORD ownerPid = 0;
        GetWindowThreadProcessId(trayWindow, &ownerPid);
        const std::wstring exe = ownerPid ? ExeLeafForPid(ownerPid) : std::wstring();

        // INTO THE LEDGER AS WELL AS ONTO DISK: this is the same registration, and it is what the card asks
        // about before it draws a cell (see the note on the ledger above, and domain/TrayLedger.h).
        FeedLiveTray(registration, ownerPid, exe.c_str());

        std::wstring guidText;
        if (registration.hasGuid) {
            GUID guid = {};
            std::memcpy(&guid, registration.guid, sizeof(guid));
            guidText = GuidText(guid);
        }
        // The same field names the in-application path writes (and the same ones this file has always held):
        // the reader below parses by NAME, so the two sources stay interchangeable.
        wchar_t line[1024] = {};
        _snwprintf_s(line, 1024, _TRUNCATE,
                     L"call S %hs pid %u exe %ls hwnd %p uid %u msg 0x%04X version %u flags 0x%X guid %ls "
                     L"tip \"%ls\" (read in the shell, over the ring)\n",
                     CommandName(registration.command), ownerPid, exe.empty() ? L"-" : exe.c_str(), trayWindow,
                     registration.uid, registration.callback, registration.version, registration.flags,
                     guidText.empty() ? L"-" : guidText.c_str(), registration.tip);
        if (appender.Write(line)) ++written;
    }
    // ONLY A DISCREPANCY IS WORTH A LINE, AND A DISCREPANCY IS WHAT COST A WHOLE ROUND HERE.
    //
    // The failure this reports on was invisible by construction: 35 records taken, no line written, no error
    // raised, and the pass reporting `0 registration(s) captured`. So the quiet case -- every record became a
    // line -- says nothing at all, and anything else says what was taken, what was written, where it went and
    // what the records claimed to be.
    if (written != static_cast<int>(taken)) {
        wchar_t kinds[200] = {};
        int used = 0;
        for (unsigned i = 0; i < taken && i < 6; ++i) {
            const int added = _snwprintf_s(kinds + used, 200 - used, _TRUNCATE, L" %u/%s",
                                           static_cast<unsigned>(records[i].kind),
                                           records[i].registration.valid ? L"valid" : L"invalid");
            if (added > 0) used += added;
        }
        LogAndPrint(L"  tray ring: %u record(s) taken, %d written to '%ls' (kind/valid:%ls)\n", taken, written,
                    report.c_str(), kinds);
        std::fflush(stdout);
    }
    return static_cast<int>(taken);
}

int HarvestLearnedCallbacks() {
    TrayLearnResult result;
    HarvestReport(&result);
    return result.newEntries;
}

std::wstring UncoveredApplications(const TrayIcons& icons) {
    const std::vector<std::wstring> lines = ReadCacheLines();
    std::vector<std::wstring> missing;
    for (int i = 0; i < icons.Count(); ++i) {
        const std::wstring exe = BaseNameLower(icons.ExePathAt(i));
        if (exe.empty()) continue;
        const std::wstring guidText = icons.At(i).hasGuid ? GuidText(icons.At(i).guid) : std::wstring();
        bool covered = false;
        for (const std::wstring& line : lines) {
            if (LineIsFor(line, exe, icons.At(i).uid, guidText)) {
                covered = true;
                break;
            }
        }
        if (covered) continue;
        if (std::find(missing.begin(), missing.end(), exe) == missing.end()) missing.push_back(exe);
    }
    std::sort(missing.begin(), missing.end());
    std::wstring joined;
    for (const std::wstring& exe : missing) {
        if (!joined.empty()) joined += L",";
        joined += exe;
    }
    return joined;
}

// One clause for the summary line every caller prints: was the observer in the shell up BEFORE the
// applications were asked? Written once here because three call sites print that line, and the difference
// between "nothing registered" and "nobody was listening" is the whole question this pass is about.
std::wstring ObserverClause(const TrayLearnResult& learned) {
    wchar_t text[320] = {};
    if (learned.observerReady) {
        _snwprintf_s(text, 320, _TRUNCATE,
                     L"observer up on the tray thread (tid %u, %u hook(s), %lld ms)",
                     learned.observerTid, learned.observerHooks, learned.observerMs);
    } else if (learned.observerResident) {
        _snwprintf_s(text, 320, _TRUNCATE,
                     L"observer resident since an earlier session (this shell pins it; %d registration(s) "
                     L"arrived)",
                     learned.captured);
    } else if (learned.observerAttempts > 0) {
        _snwprintf_s(text, 320, _TRUNCATE,
                     L"observer NOT up -- the hook was refused (tray thread %u, %u hook(s), attempt %u, "
                     L"last error %u, waited %lld ms)",
                     learned.observerTid, learned.observerHooks, learned.observerAttempts, learned.observerError,
                     learned.observerMs);
    } else {
        _snwprintf_s(text, 320, _TRUNCATE,
                     L"observer did not report in %lld ms (%d of the hooked threads took the message that "
                     L"loads the module)",
                     learned.observerMs, learned.poked);
    }
    return text;
}

} // namespace island
