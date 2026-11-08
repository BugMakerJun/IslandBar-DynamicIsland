// =====================================================================
//  TrayIcons.cpp
// =====================================================================
#include "shell/TrayIcons.h"

#include "core/Log.h"
#include "shell/StartApps.h"
// The identity of a registration, for "one icon, one cell" (see SameTrayIcon).
#include "domain/TrayLedger.h"
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

int PngSide(const std::vector<std::uint8_t>& png) {
    if (png.size() < 24 || png[0] != 0x89 || png[1] != 0x50) return 0;
    const int width = (static_cast<int>(png[16]) << 24) | (static_cast<int>(png[17]) << 16) |
                      (static_cast<int>(png[18]) << 8) | static_cast<int>(png[19]);
    const int height = (static_cast<int>(png[20]) << 24) | (static_cast<int>(png[21]) << 16) |
                       (static_cast<int>(png[22]) << 8) | static_cast<int>(png[23]);
    return width < height ? width : height;
}

bool UseExecutableIcon(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring leaf = FileName(exePath);
    return _wcsicmp(leaf.c_str(), L"explorer.exe") != 0 &&
           _wcsicmp(leaf.c_str(), L"ShellExperienceHost.exe") != 0 &&
           _wcsicmp(leaf.c_str(), L"StartMenuExperienceHost.exe") != 0;
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

int TrayIcons::Refresh(bool upgradeLowResolutionIcons) {
    // THE TWO ANSWERS, decided once per pass (see the header).
    //
    // `liveFilter_`: a pass has finished, so there IS a ledger to ask. `ledgerOnly_`: that ledger is also
    // being kept up to date, because the module is resident in the shell -- the only case in which the card
    // may be the ledger alone. Before the first pass, and after a shell that replaced Explorer, the ledger
    // is a snapshot and the registry list is the wider, safer answer.
    liveFilter_ = LiveTraySeeded() && LiveTrayCount() > 0;
    ledgerOnly_ = liveFilter_ && LiveTrayObserverResident();
    liveRegistrations_ = liveFilter_ ? LiveTrayCount() : 0;

    // The process snapshot is the FALLBACK's question ("is the executable running"), and only that: a card
    // that trusts the ledger does not pay for a snapshot of hundreds of processes on every refresh.
    std::vector<std::wstring> running;
    processesSeen_ = 0;
    if (!ledgerOnly_) {
        RunningProcessNames(&running);
        processesSeen_ = static_cast<int>(running.size());
    }

    notLive_.clear();
    seenIdentities.clear();
    collapsed_ = 0;

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SettingsKeyPath(), 0, KEY_READ, &root) != ERROR_SUCCESS) {
        entries_.clear();
        list_.clear();
        examined_ = 0;
        kept_ = 0;
        return 0;
    }

    std::vector<Entry> fresh;
    // The registrations already drawn this pass, so a second registry entry naming the same icon makes no
    // second cell (see SameTrayIcon; NVIDIA's two entries and WeChat's two are the measured cases).
    std::vector<TrayLiveRegistration> drawn;
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
        if (upgradeLowResolutionIcons && PngSide(entry.png) < 40 &&
            UseExecutableIcon(entry.exePath)) {
            std::wstring cacheKey = entry.exePath;
            std::transform(cacheKey.begin(), cacheKey.end(), cacheKey.begin(), ::towlower);
            auto cached = upgradedIcons_.find(cacheKey);
            if (cached == upgradedIcons_.end()) {
                std::vector<std::uint8_t> upgraded;
                StartAppIconPng(entry.exePath.c_str(), 64, &upgraded);
                cached = upgradedIcons_.emplace(cacheKey, std::move(upgraded)).first;
            }
            if (!cached->second.empty()) entry.png = cached->second;
        }
        // Read here, once: it is the label the card shows AND the second chance at matching a registration
        // whose executable name is not the one the registry stored (see TrayLedger::FindByTip).
        ReadStringValue(subKey, L"InitialTooltip", &entry.tooltip);

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
        // register uid 3 on this machine -- see domain/TrayLedger.h), or the tooltip when the registry's
        // executable is not the process that registered the icon (TrayLedger::FindByTip).
        const std::wstring leaf = FileName(entry.exePath);
        std::wstring wanted = leaf;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::towlower);
        // An entry with neither identity cannot name a registration at all -- and asking with uid 0 would
        // match somebody else's icon (the probe registrar's uid is 7, WeChat registers uid 0).
        const bool identified = entry.icon.hasUid || entry.icon.hasGuid;

        // The registration this entry names, copied out: the ledger's storage can be reallocated by the
        // learning worker between two calls (see TrayLearn.h).
        TrayLiveRegistration registration;
        bool live = false;
        if (liveFilter_ && identified) {
            // The guid goes over as its 16 BYTES: that is what the registration carried across the ring, and
            // the registry's "{...}" string was parsed back into the same layout (see TrayLearn.cpp).
            const unsigned char* guidBytes =
                entry.icon.hasGuid ? reinterpret_cast<const unsigned char*>(&entry.icon.guid) : nullptr;
            live = LiveTrayFind(wanted.c_str(), entry.icon.uid, entry.icon.hasGuid, guidBytes,
                                entry.tooltip.c_str(), &registration);
        }

        if (ledgerOnly_) {
            // THE LEDGER ALONE, which is the rule that makes this card the tray's list (ROADMAP F5). A
            // registry entry the shell holds no registration for is not an icon in the tray: it is the second
            // entry of one icon (NVIDIA's two executable names, WeChat's GUID entry beside its uid 0 one), or
            // an icon the application took out while its process kept running. Drawing it is exactly the
            // duplicate the user saw on 2026-11-06 -- two WeChat cells and two NVIDIA cells.
            if (!live) {
                notLive_.push_back(leaf);
                continue;
            }
            const bool alreadyDrawn =
                std::any_of(drawn.begin(), drawn.end(), [&registration](const TrayLiveRegistration& shown) {
                    return SameTrayIcon(shown, registration);
                });
            if (alreadyDrawn) {
                ++collapsed_;   // one icon, one cell
                continue;
            }
            drawn.push_back(registration);
        } else if (liveFilter_) {
            // THE OBSERVER IS NOT RESIDENT, so the ledger is a snapshot: answering "not in the tray" from it
            // alone would under-draw, which is the failure the per-entry union was written for (2026-10-18:
            // the hook did not load and the ledger matched 8 of the 14 entries). The union is kept for this
            // state only, and the log says the list may be wider than the tray.
            //
            // IDENTITY IS REQUIRED AS WELL, and that is not a detail: a press is sent to what the application
            // registered (see TrayPress), so an entry with neither a UID nor a GUID is a tile that could only
            // open this bar's own menu.
            if (!live && (!identified || !IsRunning(running, entry.exePath))) {
                notLive_.push_back(leaf);
                continue;
            }
        } else if (!IsRunning(running, entry.exePath)) {
            continue;
        }

        // ... AND ONE CELL PER ICON under the two fallback rules, where the registry's own identity is what
        // there is to compare (see IdentityKey). The ledger rule above keys on the shell's registration.
        if (!ledgerOnly_) {
            const std::wstring identity =
                IdentityKey(entry.icon.hasGuid, entry.icon.guid, wanted, entry.icon.uid);
            if (std::find(seenIdentities.begin(), seenIdentities.end(), identity) != seenIdentities.end()) {
                ++collapsed_;
                continue;
            }
            seenIdentities.push_back(identity);
        }
        ++kept_;

        // The name: the registry's InitialTooltip first, then the tip the application sent the shell, then the
        // file name -- see TrayIcon for why the stable one goes first. Only the first line: a cell is one line
        // wide, and an application's tip is live status text rather than a label.
        std::wstring name = entry.tooltip;
        if (name.empty() && live) name = registration.tip;
        if (name.empty()) name = leaf;
        name = FirstLine(name);
        wcsncpy_s(entry.icon.name, 96, name.c_str(), _TRUNCATE);
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
    if (ledgerOnly_) {
        // The card IS the shell's list: how many entries that list matched, how many it collapsed as one
        // icon, and how many the shell is not holding at all. The three numbers are what let the reader check
        // "the card draws nine" against "the shell holds nine icons (in thirty-two registrations)".
        _snwprintf_s(text, 640, _TRUNCATE,
                     L"-- %d matched of %d registry entr(ies) (the shell holds %d registration(s)); "
                     L"%d duplicate(s) collapsed; not in the tray: %ls",
                     kept_, examined_, liveRegistrations_, collapsed_, names.c_str());
        return text;
    }
    // Seeded but the observer is not resident: the ledger is a snapshot, so this is the registry list with
    // the union, and the clause has to say that the count may be wider than the tray's.
    _snwprintf_s(text, 640, _TRUNCATE,
                 L"-- %d of %d registry entr(ies) drawn (the shell's observer is not resident, so the ledger "
                 L"is a snapshot and running entries are drawn too); not shown: %ls",
                 kept_, examined_, names.c_str());
    return text;
}

} // namespace island

