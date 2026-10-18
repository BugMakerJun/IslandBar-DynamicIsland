// =====================================================================
//  TrayIcons.cpp
// =====================================================================
#include "shell/TrayIcons.h"

#include "core/Log.h"
// The ledger of registrations the shell is holding: this file is the one place that decides what to draw
// from it (see the header, and domain/TrayLedger.h for the rule itself).
#include "shell/TrayLearn.h"

#include <tlhelp32.h>
// CLSIDFromString, for the registry's textual IconGuid: with WIN32_LEAN_AND_MEAN the COM declarations
// are not part of windows.h.
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>

namespace island {
namespace {

// The file name of every process that is running, lower-cased, into `out`.
//
// Toolhelp rather than OpenProcess per pid: this runs on a timer and the machine has hundreds of
// processes, while a snapshot is one call and gives the names, which is all the filter needs.
void RunningProcessNames(std::vector<std::wstring>* out) {
    out->clear();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring name = entry.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            out->push_back(std::move(name));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

bool IsRunning(const std::vector<std::wstring>& running, const std::wstring& exePath) {
    const wchar_t* slash = std::wcsrchr(exePath.c_str(), L'\\');
    std::wstring name = slash ? std::wstring(slash + 1) : exePath;
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    for (const std::wstring& candidate : running) {
        if (candidate == name) return true;
    }
    return false;
}

// The file name of a path, spelled as the path spells it. The log line names the entries the shell is not
// holding, and a name that has been lower-cased for a comparison is not the name to print.
std::wstring FileName(const std::wstring& path) {
    const wchar_t* slash = std::wcsrchr(path.c_str(), L'\\');
    return slash ? std::wstring(slash + 1) : path;
}

// The first line of a tooltip, and nothing after it. A cell is one line wide, and an application is free to
// put a whole status block in its tooltip -- Task Manager's is "CPU 24%\n内存 58%\n磁盘 0%\n网络 0%" and
// Clash Verge's is several lines of version and rule counts (MEASURED 2026-10-16). The tray shows that text
// when the pointer rests on the icon; a label is not a hover, so the label gets the first line.
std::wstring FirstLine(const std::wstring& text) {
    const size_t cut = text.find_first_of(L"\r\n");
    std::wstring line = cut == std::wstring::npos ? text : text.substr(0, cut);
    while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
    return line;
}

// The identity of one registration, as a key: the same identity the liveness question is asked with (a guid
// on its own, otherwise the executable and the uid together).
//
// WHY IT IS NEEDED AT ALL: the registry is not a set. MEASURED 2026-10-16: the probe registrar leaves TWO
// entries that are identical down to the PNG bytes and NVIDIA's container leaves three, while the tray draws
// ONE icon for all of them -- so a card built from the entries alone draws two or three cells for one icon.
std::wstring IdentityKey(bool hasGuid, const GUID& guid, const std::wstring& exeLeaf, unsigned uid) {
    if (hasGuid) {
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&guid);
        wchar_t text[40] = {};
        for (int i = 0; i < 16; ++i) {
            _snwprintf_s(text + i * 2, 40 - static_cast<size_t>(i) * 2, _TRUNCATE, L"%02X", bytes[i]);
        }
        return std::wstring(L"g:") + text;
    }
    return std::wstring(L"u:") + exeLeaf + L"|" + std::to_wstring(uid);
}

void ReadStringValue(HKEY key, const wchar_t* name, std::wstring* out) {
    out->clear();
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes == 0) {
        return;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()),
                         &bytes) == ERROR_SUCCESS) {
        *out = buffer.data();
    }
}

void ReadBinaryValue(HKEY key, const wchar_t* name, std::vector<uint8_t>* out) {
    out->clear();
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY || bytes == 0) {
        return;
    }
    std::vector<uint8_t> buffer(bytes);
    if (RegQueryValueExW(key, name, nullptr, &type, buffer.data(), &bytes) == ERROR_SUCCESS) {
        buffer.resize(bytes);
        *out = std::move(buffer);
    }
}

// A file's modification time as a sortable number, or 0. Used for the newest-first order: a subkey's
// last write moves when its icon changes, which is the closest thing the tray has to "used recently".
long long KeyLastWriteTime(const std::wstring& subKey) {
    HKEY key = nullptr;
    const std::wstring path = std::wstring(L"Control Panel\\NotifyIconSettings\\") + subKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return 0;
    }
    FILETIME written = {};
    long long value = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, &written) == ERROR_SUCCESS) {
        ULARGE_INTEGER ticks = {};
        ticks.LowPart = written.dwLowDateTime;
        ticks.HighPart = written.dwHighDateTime;
        value = static_cast<long long>(ticks.QuadPart);
    }
    RegCloseKey(key);
    return value;
}

} // namespace

int TrayIcons::Refresh() {
    std::vector<std::wstring> running;
    RunningProcessNames(&running);
    processesSeen_ = static_cast<int>(running.size());
    // Whether this pass draws what the shell is holding, or falls back to the older rule (see the header).
    // The pass has to have finished once: before that the ledger is only what happened to arrive so far.
    liveFilter_ = LiveTraySeeded() && LiveTrayCount() > 0;
    notLive_.clear();
    seenIdentities.clear();

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SettingsKeyPath(), 0, KEY_READ, &root) != ERROR_SUCCESS) {
        entries_.clear();
        list_.clear();
        examined_ = 0;
        kept_ = 0;
        return 0;
    }

    std::vector<Entry> fresh;
    examined_ = 0;
    kept_ = 0;

    DWORD index = 0;
    wchar_t subKeyName[256] = {};
    DWORD subKeyLength = 256;
    while (RegEnumKeyExW(root, index, subKeyName, &subKeyLength, nullptr, nullptr, nullptr,
                         nullptr) == ERROR_SUCCESS) {
        ++index;
        subKeyLength = 256;

        HKEY subKey = nullptr;
        if (RegOpenKeyExW(root, subKeyName, 0, KEY_READ, &subKey) != ERROR_SUCCESS) continue;
        ++examined_;

        Entry entry;
        ReadStringValue(subKey, L"ExecutablePath", &entry.exePath);
        ReadBinaryValue(subKey, L"IconSnapshot", &entry.png);

        DWORD promoted = 0;
        DWORD promotedSize = sizeof(promoted);
        DWORD type = 0;
        if (RegQueryValueExW(subKey, L"IsPromoted", nullptr, &type,
                             reinterpret_cast<BYTE*>(&promoted), &promotedSize) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            entry.icon.promoted = promoted != 0;
        }

        // THE SHELL'S OWN IDENTITY FOR THE ICON, which is what a press needs (see TrayPress). A UID and
        // an IconGuid are the two ways an application registers an icon and each entry carries one or the
        // other -- measured: `UID` is a DWORD, `IconGuid` a string "{...}".
        {
            DWORD uid = 0;
            DWORD uidSize = sizeof(uid);
            DWORD uidType = 0;
            if (RegQueryValueExW(subKey, L"UID", nullptr, &uidType, reinterpret_cast<BYTE*>(&uid),
                                 &uidSize) == ERROR_SUCCESS &&
                uidType == REG_DWORD) {
                entry.icon.uid = uid;
                entry.icon.hasUid = true;
            }
            std::wstring guidText;
            ReadStringValue(subKey, L"IconGuid", &guidText);
            if (!guidText.empty()) {
                GUID parsed = {};
                // Through the string form rather than the bytes: the value is REG_SZ, braces and all.
                std::wstring trimmed = guidText;
                if (trimmed.size() >= 2 && trimmed.front() == L'{' && trimmed.back() == L'}') {
                    trimmed = trimmed.substr(1, trimmed.size() - 2);
                }
                if (SUCCEEDED(CLSIDFromString((L"{" + trimmed + L"}").c_str(), &parsed))) {
                    entry.icon.guid = parsed;
                    entry.icon.hasGuid = true;
                }
            }
        }
        RegCloseKey(subKey);

        // Only a PNG is drawn: the value is documented by measurement (see the header), and a future
        // Windows that changes the format must show nothing rather than a grey rectangle.
        if (entry.png.size() < 8 || entry.png[0] != 0x89 || entry.png[1] != 0x50) continue;

        // THE FILTER, which is the whole point of this file: is the shell holding this registration now?
        // A guid identifies one on its own; a uid needs the executable to agree with it (two applications
        // register uid 3 on this machine -- see domain/TrayLedger.h).
        const std::wstring leaf = FileName(entry.exePath);
        std::wstring wanted = leaf;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::towlower);
        std::wstring liveTip;
        if (liveFilter_) {
            // The guid goes over as its 16 BYTES: that is what the registration carried across the ring, and
            // the registry's "{...}" string was parsed back into the same layout (see TrayLearn.cpp).
            const bool live = LiveTrayFind(wanted.c_str(), entry.icon.uid, entry.icon.hasGuid,
                                           reinterpret_cast<const unsigned char*>(&entry.icon.guid),
                                           &liveTip);
            // ... OR IT IS SIMPLY RUNNING. The ledger is the better answer when it is complete, but it is
            // only as complete as the ring that feeds it, and the ring is only fed while the shell hook is
            // installed. Measured 2026-10-18: the hook did not load on this machine
            // ("the observer did NOT report ready"), the ledger held 43 registrations captured during the
            // learning pass, and the filter matched 8 of the 14 entries the registry lists -- so the tray
            // card drew eight icons where the tray itself showed fourteen. Reported as "托盘卡片里面的应用
            // 显示的不全".
            //
            // The union is the right bias: a process that is running may legitimately have registered an
            // icon the ledger has not seen, while an entry whose process is gone cannot be showing anything.
            //
            // IDENTITY IS REQUIRED AS WELL, and that is not a detail: a press is sent to what the
            // application registered (see TrayPress), so an entry with neither a UID nor a GUID is a tile
            // that could only open this bar's own menu. The self-test states the rule as "every icon in the
            // card carries the identity an application registers it with", and an entry that cannot be
            // pressed must not be drawn as if it could.
            const bool identified = entry.icon.hasUid || entry.icon.hasGuid;
            if (!live && (!identified || !IsRunning(running, entry.exePath))) {
                notLive_.push_back(leaf);
                continue;
            }
        } else if (!IsRunning(running, entry.exePath)) {
            continue;
        }

        // ... AND ONE CELL PER IDENTITY: entries that name the same icon ARE the same icon (see IdentityKey).
        const std::wstring identity =
            IdentityKey(entry.icon.hasGuid, entry.icon.guid, wanted, entry.icon.uid);
        if (std::find(seenIdentities.begin(), seenIdentities.end(), identity) != seenIdentities.end()) {
            continue;
        }
        seenIdentities.push_back(identity);
        ++kept_;

        // The name: what the application called itself, or failing that its file name. Read after the
        // filter so the registry is not walked twice for icons nobody will see.
        //
        // The name: the registry's, then the registration's own tip, then the file name -- see TrayIcon for
        // why the stable one goes first. Read after the filter so the registry is not walked twice for icons
        // nobody will see.
        std::wstring tooltip;
        HKEY subKeyAgain = nullptr;
        if (RegOpenKeyExW(root, subKeyName, 0, KEY_READ, &subKeyAgain) == ERROR_SUCCESS) {
            ReadStringValue(subKeyAgain, L"InitialTooltip", &tooltip);
            RegCloseKey(subKeyAgain);
        }
        if (tooltip.empty()) tooltip = liveTip;
        if (tooltip.empty()) tooltip = leaf;
        tooltip = FirstLine(tooltip);
        wcsncpy_s(entry.icon.name, 96, tooltip.c_str(), _TRUNCATE);
        entry.icon.pngSize = entry.png.size();
        entry.writtenAt = KeyLastWriteTime(subKeyName);

        fresh.push_back(std::move(entry));
    }
    RegCloseKey(root);

    // Newest registration first, so the icons that have changed recently -- which is what a running
    // application's tray icon does -- are the ones nearest the right-hand end.
    std::stable_sort(fresh.begin(), fresh.end(), [](const Entry& a, const Entry& b) {
        return a.writtenAt > b.writtenAt;
    });

    // The PNG pointers point into each entry's own buffer, so they are re-seated AFTER the sort and the
    // move: a pointer into a vector that has just been reordered is exactly the kind of dangling read a
    // screenshot cannot show.
    for (Entry& entry : fresh) entry.icon.png = entry.png.data();

    bool changed = fresh.size() != entries_.size();
    if (!changed) {
        for (size_t i = 0; i < fresh.size(); ++i) {
            if (std::wcscmp(fresh[i].icon.name, entries_[i].icon.name) != 0 ||
                fresh[i].png.size() != entries_[i].png.size() ||
                std::memcmp(fresh[i].png.data(), entries_[i].png.data(), fresh[i].png.size()) != 0) {
                changed = true;   // a different icon, or the same application's icon redrawn
                break;
            }
        }
    }

    entries_ = std::move(fresh);
    for (Entry& entry : entries_) entry.icon.png = entry.png.data();

    // ... and the contiguous view the renderer walks, built LAST so that every pointer in it refers to the
    // storage the entries will keep (see the note on `list_`).
    list_.clear();
    list_.reserve(entries_.size());
    for (const Entry& entry : entries_) list_.push_back(entry.icon);

    if (changed) ++revision_;
    return Count();
}

// The rest of the log line, from whichever rule decided the list (see the header).
//
// BOTH HALVES ARE STATED because "the card shows nine" and "the card shows nine because the shell is holding
// nine" are different claims, and the second one is the only one worth checking. MEASURED 2026-10-16, and
// the reason this clause exists: thirteen cells against the shell's nine, and nothing in the log said which
// four were surplus or why.
std::wstring TrayIcons::SourceClause() const {
    wchar_t text[640] = {};
    if (!liveFilter_) {
        _snwprintf_s(text, 640, _TRUNCATE,
                     L"-- the shell has not been asked for its registrations yet, so the registry list is "
                     L"drawn as it always was (%d entr(ies), %d process(es) seen)",
                     examined_, processesSeen_);
        return text;
    }
    std::wstring names;
    const size_t shown = 6;
    for (size_t i = 0; i < notLive_.size() && i < shown; ++i) {
        if (!names.empty()) names += L", ";
        names += notLive_[i];
    }
    if (notLive_.size() > shown) {
        wchar_t more[64] = {};
        _snwprintf_s(more, 64, _TRUNCATE, L" and %d more", static_cast<int>(notLive_.size() - shown));
        names += more;
    }
    if (names.empty()) names = L"nothing";
    _snwprintf_s(text, 640, _TRUNCATE, L"-- %d live of %d registry entr(ies) (not shown: %ls)", kept_,
                 examined_, names.c_str());
    return text;
}

} // namespace island

