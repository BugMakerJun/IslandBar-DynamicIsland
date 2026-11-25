// =====================================================================
//  tools/tray_probe/main.cpp
//  An independent, read-only tray data probe.
//
//  It owns a ring that production never reads, installs an observer-only hook into the current Shell process,
//  asks applications to register once, and prints the registry/live/visible relationship. It does not change
//  the bar, the tray UI, callback caches, or production ring state.
// =====================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

#include "domain/TrayLedger.h"
#include "domain/TrayRegistration.h"
#include "platform/TrayRing.h"

namespace {

constexpr wchar_t kProbeHookName[] = L"island_tray_probe_hook.dll";
constexpr wchar_t kNotifyIconSettings[] = L"Control Panel\\NotifyIconSettings";

struct Options {
    int samples = 1;
    DWORD intervalMs = 2000;
    bool install = true;
    bool broadcast = true;
    bool details = false;
    bool showHelp = false;
};

struct RegistryIcon {
    island::TrayLiveRegistration registrationIdentity;
    bool hasUid = false;
    std::wstring exePath;
    std::wstring exeLeaf;
    std::wstring stableLabel;
    std::wstring registryTooltip;
    std::vector<uint8_t> png;
    uint64_t pngHash = 0;
    bool promoted = false;
};

struct ProbeContact {
    unsigned pid = 0;
    unsigned long long window = 0;
    unsigned uid = 0;
    bool hasGuid = false;
    unsigned char guid[16] = {};
    unsigned callback = 0;
    unsigned version = 0;
    wchar_t tip[128] = {};
    uint64_t lastEventTick = 0;
};

struct ProbeState {
    island::TrayLedger ledger;
    std::vector<ProbeContact> contacts;
    island::TrayRingReader reader;
    std::uint64_t cursor = 0;
    unsigned generation = 0;
    DWORD shellPid = 0;
    ULONGLONG startedTick = 0;
    uint64_t recordsRead = 0;
    int staleRemoved = 0;
    int duplicatesCollapsed = 0;
    bool observerReady = false;
    unsigned observerTid = 0;
    unsigned observerHooks = 0;
};

struct VisibleIcon {
    RegistryIcon registry;
    island::TrayLiveRegistration live;
    ProbeContact contact;
};

struct VisibleResult {
    std::vector<VisibleIcon> icons;
    int registryCount = 0;
    int duplicates = 0;
};

struct IconObservation {
    uint64_t pngHash = 0;
    wchar_t tooltip[256] = {};
};

void PrintUtf8(const wchar_t* text) {
    if (!text || !text[0]) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return;
    std::vector<char> utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), bytes, nullptr, nullptr);
    std::fputs(utf8.data(), stdout);
}

const wchar_t* FileLeaf(const std::wstring& path) {
    const wchar_t* slash = std::wcsrchr(path.c_str(), L'\\');
    return slash ? slash + 1 : path.c_str();
}

std::wstring LowerFileLeaf(const std::wstring& path) {
    std::wstring leaf = FileLeaf(path);
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), ::towlower);
    return leaf;
}

bool ReadStringValue(HKEY key, const wchar_t* name, std::wstring* out) {
    out->clear();
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes == 0) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(bytes / sizeof(wchar_t)) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) !=
        ERROR_SUCCESS) {
        return false;
    }
    *out = buffer.data();
    return true;
}

bool ReadBinaryValue(HKEY key, const wchar_t* name, std::vector<uint8_t>* out) {
    out->clear();
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY || bytes == 0) {
        return false;
    }
    std::vector<uint8_t> buffer(bytes);
    if (RegQueryValueExW(key, name, nullptr, &type, buffer.data(), &bytes) != ERROR_SUCCESS) return false;
    buffer.resize(bytes);
    *out = std::move(buffer);
    return true;
}

bool ReadDwordValue(HKEY key, const wchar_t* name, DWORD* out) {
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(out), &bytes) == ERROR_SUCCESS &&
           type == REG_DWORD && bytes == sizeof(DWORD);
}

uint64_t Fnv1a(const uint8_t* bytes, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool ParseGuidText(std::wstring text, GUID* out) {
    if (!out) return false;
    if (!text.empty() && text.front() == L'{' && text.back() == L'}') {
        text = text.substr(1, text.size() - 2);
    }
    if (text.empty()) return false;
    return SUCCEEDED(CLSIDFromString((L"{" + text + L"}").c_str(), out));
}

std::wstring ProcessImageLeafForPid(DWORD pid) {
    if (!pid) return std::wstring();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::wstring();
    wchar_t path[32768] = {};
    DWORD size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    std::wstring leaf;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        leaf = FileLeaf(path);
        std::transform(leaf.begin(), leaf.end(), leaf.begin(), ::towlower);
    }
    CloseHandle(process);
    return leaf;
}

std::vector<RegistryIcon> ReadRegistryIcons() {
    std::vector<RegistryIcon> icons;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyIconSettings, 0, KEY_READ, &root) != ERROR_SUCCESS) return icons;

    DWORD index = 0;
    wchar_t subkey[256] = {};
    for (;;) {
        DWORD subkeyLength = static_cast<DWORD>(sizeof(subkey) / sizeof(subkey[0]));
        const LSTATUS status =
            RegEnumKeyExW(root, index++, subkey, &subkeyLength, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS) break;

        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;

        RegistryIcon icon;
        (void)ReadStringValue(key, L"ExecutablePath", &icon.exePath);
        icon.exeLeaf = LowerFileLeaf(icon.exePath);
        (void)ReadStringValue(key, L"InitialTooltip", &icon.registryTooltip);

        DWORD promoted = 0;
        if (ReadDwordValue(key, L"IsPromoted", &promoted)) icon.promoted = promoted != 0;

        DWORD uid = 0;
        if (ReadDwordValue(key, L"UID", &uid)) {
            icon.registrationIdentity.uid = uid;
            icon.registrationIdentity.hasGuid = false;
            icon.hasUid = true;
        }

        std::wstring guidText;
        if (ReadStringValue(key, L"IconGuid", &guidText)) {
            GUID guid = {};
            if (ParseGuidText(guidText, &guid)) {
                icon.registrationIdentity.hasGuid = true;
                std::memcpy(icon.registrationIdentity.guid, &guid, sizeof(guid));
            }
        }

        if (icon.exeLeaf.empty() || !ReadBinaryValue(key, L"IconSnapshot", &icon.png) || icon.png.size() < 8 ||
            icon.png[0] != 0x89 || icon.png[1] != 0x50) {
            RegCloseKey(key);
            continue;
        }
        icon.pngHash = Fnv1a(icon.png.data(), icon.png.size());
        icon.stableLabel = icon.registryTooltip.empty() ? icon.exeLeaf : icon.registryTooltip;
        const size_t lineBreak = icon.stableLabel.find_first_of(L"\r\n");
        if (lineBreak != std::wstring::npos) icon.stableLabel.resize(lineBreak);
        icons.push_back(std::move(icon));
        RegCloseKey(key);
    }
    RegCloseKey(root);
    return icons;
}

bool SameContact(const ProbeContact& contact, const island::TrayRegistration& registration) {
    if (contact.hasGuid || registration.hasGuid) {
        return contact.hasGuid && registration.hasGuid &&
               std::memcmp(contact.guid, registration.guid, sizeof(contact.guid)) == 0;
    }
    return contact.window == registration.trayWindow && contact.uid == registration.uid;
}

void UpdateContacts(ProbeState* state, const island::TrayRegistration& registration, unsigned ownerPid,
                    uint64_t publishedTick) {
    if (!state) return;
    for (size_t i = 0; i < state->contacts.size(); ++i) {
        if (!SameContact(state->contacts[i], registration)) continue;
        if (registration.command == island::kTrayCopyDelete) {
            state->contacts.erase(state->contacts.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
        ProbeContact& contact = state->contacts[i];
        contact.pid = ownerPid;
        contact.lastEventTick = publishedTick;
        if (registration.callback) contact.callback = registration.callback;
        if (registration.version) contact.version = registration.version;
        if (registration.tip[0]) wcsncpy_s(contact.tip, registration.tip, _TRUNCATE);
        return;
    }

    if (registration.command == island::kTrayCopyDelete) return;
    ProbeContact contact;
    contact.pid = ownerPid;
    contact.window = registration.trayWindow;
    contact.uid = registration.uid;
    contact.hasGuid = registration.hasGuid;
    if (registration.hasGuid) std::memcpy(contact.guid, registration.guid, sizeof(contact.guid));
    contact.callback = registration.callback;
    contact.version = registration.version;
    contact.lastEventTick = publishedTick;
    wcsncpy_s(contact.tip, registration.tip, _TRUNCATE);
    state->contacts.push_back(contact);
}

DWORD ShellProcessId() {
    const HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (shell) GetWindowThreadProcessId(shell, &pid);
    return pid;
}

void DrainRecords(ProbeState* state) {
    if (!state) return;
    island::TrayRingRecord records[island::kTrayRingCapacity] = {};
    const unsigned taken = state->reader.Drain(state->cursor, records, island::kTrayRingCapacity);
    state->recordsRead += taken;
    for (unsigned i = 0; i < taken; ++i) {
        if (records[i].kind == island::TrayRingKind::HookState) {
            const island::TrayHookState& hook = records[i].hook;
            if (hook.pid == state->shellPid) {
                state->observerReady = hook.ready;
                state->observerTid = hook.trayThread;
                state->observerHooks = hook.hooks;
            }
            continue;
        }

        const island::TrayRegistration& registration = records[i].registration;
        if (!registration.valid) continue;
        const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(registration.trayWindow));
        DWORD ownerPid = 0;
        if (window) GetWindowThreadProcessId(window, &ownerPid);
        const std::wstring exe = ProcessImageLeafForPid(ownerPid);
        UpdateContacts(state, registration, ownerPid, records[i].publishedTick);
        state->ledger.Apply(static_cast<island::TrayCopyCommand>(registration.command), ownerPid,
                            static_cast<unsigned long long>(registration.trayWindow), registration.uid,
                            registration.hasGuid, registration.guid, registration.tip, exe.c_str());
    }
}

void ForgetStaleWindows(ProbeState* state) {
    if (!state) return;
    std::vector<island::TrayLiveRegistration> entries;
    entries.reserve(static_cast<size_t>(state->ledger.Count()));
    for (int i = 0; i < state->ledger.Count(); ++i) entries.push_back(state->ledger.At(i));

    for (const island::TrayLiveRegistration& entry : entries) {
        const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(entry.window));
        DWORD ownerPid = 0;
        if (window && IsWindow(window)) GetWindowThreadProcessId(window, &ownerPid);
        if (window && ownerPid == entry.pid) continue;
        state->ledger.ForgetWindow(entry.window);
        ++state->staleRemoved;
    }
}

ProbeContact ContactFor(const ProbeState& state, const island::TrayLiveRegistration& live) {
    for (const ProbeContact& contact : state.contacts) {
        if (contact.hasGuid || live.hasGuid) {
            if (contact.hasGuid && live.hasGuid &&
                std::memcmp(contact.guid, live.guid, sizeof(contact.guid)) == 0) {
                return contact;
            }
            continue;
        }
        if (contact.window == live.window && contact.uid == live.uid) return contact;
    }
    return ProbeContact{};
}

VisibleResult BuildVisibleIcons(const ProbeState& state, const std::vector<RegistryIcon>& registryIcons) {
    VisibleResult result;
    result.registryCount = static_cast<int>(registryIcons.size());
    std::vector<island::TrayLiveRegistration> drawn;
    for (const RegistryIcon& source : registryIcons) {
        RegistryIcon icon = source;
        if (!icon.hasUid && !icon.registrationIdentity.hasGuid) continue;
        const unsigned char* guid = icon.registrationIdentity.hasGuid
                                        ? reinterpret_cast<const unsigned char*>(icon.registrationIdentity.guid)
                                        : nullptr;
        const island::TrayLiveRegistration* live =
            state.ledger.Find(icon.exeLeaf.c_str(), icon.registrationIdentity.uid,
                              icon.registrationIdentity.hasGuid, guid);
        if (!live && !icon.registrationIdentity.hasGuid) {
            live = state.ledger.FindByTip(icon.registrationIdentity.uid, icon.registryTooltip.c_str());
        }
        if (!live) continue;

        const bool duplicate =
            std::any_of(drawn.begin(), drawn.end(),
                        [live](const island::TrayLiveRegistration& item) {
                            return island::SameTrayIcon(item, *live);
                        });
        if (duplicate) {
            ++result.duplicates;
            continue;
        }
        drawn.push_back(*live);
        result.icons.push_back(VisibleIcon{std::move(icon), *live, ContactFor(state, *live)});
    }
    return result;
}

void PrintIconDetails(const VisibleIcon& icon, int index, unsigned generation) {
    char guidText[64] = "-";
    if (icon.live.hasGuid) {
        std::snprintf(guidText, sizeof(guidText),
                      "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                      static_cast<unsigned long>(icon.live.guid[0]) |
                          (static_cast<unsigned long>(icon.live.guid[1]) << 8) |
                          (static_cast<unsigned long>(icon.live.guid[2]) << 16) |
                          (static_cast<unsigned long>(icon.live.guid[3]) << 24),
                      static_cast<unsigned>(icon.live.guid[4]) |
                          (static_cast<unsigned>(icon.live.guid[5]) << 8),
                      static_cast<unsigned>(icon.live.guid[6]) |
                          (static_cast<unsigned>(icon.live.guid[7]) << 8),
                      icon.live.guid[8], icon.live.guid[9], icon.live.guid[10], icon.live.guid[11],
                      icon.live.guid[12], icon.live.guid[13], icon.live.guid[14], icon.live.guid[15]);
    }

    std::printf(
        "tray-probe-icon: index=%d generation=%u pid=%u hwnd=0x%llX uid=%u guid=%s msg=0x%04X version=%u "
        "exe=\"",
        index, generation, icon.live.pid, icon.live.window, icon.live.uid, guidText,
        icon.contact.callback, icon.contact.version);
    PrintUtf8(icon.registry.exeLeaf.c_str());
    std::printf("\" stable=\"");
    PrintUtf8(icon.registry.stableLabel.c_str());
    std::printf("\" tooltip=\"");
    PrintUtf8(icon.live.tip);
    std::printf("\" promoted=%d png_hash=%016llX\n", icon.registry.promoted ? 1 : 0,
                static_cast<unsigned long long>(icon.registry.pngHash));
}

std::wstring ObservationKey(const VisibleIcon& icon) {
    wchar_t key[128] = {};
    if (icon.live.hasGuid) {
        _snwprintf_s(key, 128, _TRUNCATE,
                     L"g:%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                     icon.live.guid[0], icon.live.guid[1], icon.live.guid[2], icon.live.guid[3],
                     icon.live.guid[4], icon.live.guid[5], icon.live.guid[6], icon.live.guid[7],
                     icon.live.guid[8], icon.live.guid[9], icon.live.guid[10], icon.live.guid[11],
                     icon.live.guid[12], icon.live.guid[13], icon.live.guid[14], icon.live.guid[15]);
    } else {
        _snwprintf_s(key, 128, _TRUNCATE, L"u:%u:%llX:%u", icon.live.pid,
                     static_cast<unsigned long long>(icon.live.window), icon.live.uid);
    }
    return key;
}

void MeasureVisibleChange(const VisibleIcon& icon, ULONGLONG now,
                          std::map<std::wstring, IconObservation>* previous,
                          std::vector<long long>* latencies) {
    if (!previous || !latencies) return;
    IconObservation current;
    current.pngHash = icon.registry.pngHash;
    wcsncpy_s(current.tooltip, icon.live.tip, _TRUNCATE);

    const std::wstring key = ObservationKey(icon);
    const auto found = previous->find(key);
    if (found != previous->end()) {
        const IconObservation& old = found->second;
        if (old.pngHash != current.pngHash || std::wcscmp(old.tooltip, current.tooltip) != 0) {
            const long long latency =
                icon.contact.lastEventTick != 0 && now >= icon.contact.lastEventTick
                    ? static_cast<long long>(now - icon.contact.lastEventTick)
                    : -1;
            if (latency >= 0) latencies->push_back(latency);
        }
    }
    (*previous)[key] = current;
}

int Percentile(const std::vector<long long>& values, int percent) {
    if (values.empty()) return -1;
    size_t index = static_cast<size_t>((values.size() - 1) * static_cast<size_t>(percent) / 100);
    return static_cast<int>(values[index]);
}

uint64_t HashFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    uint64_t hash = 14695981039346656037ull;
    uint8_t buffer[16384] = {};
    DWORD read = 0;
    while (ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) && read > 0) {
        hash ^= Fnv1a(buffer, read);
        hash *= 1099511628211ull;
    }
    CloseHandle(file);
    return hash;
}

std::wstring ProbeHomeDirectory() {
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::wstring();
    std::wstring directory = std::wstring(localAppData) + L"\\IslandBar\\TrayProbe";
    (void)SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    return directory;
}

HMODULE LoadProbeHook() {
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return nullptr;
    std::wstring source = self;
    const size_t slash = source.find_last_of(L'\\');
    if (slash == std::wstring::npos) return nullptr;
    source.resize(slash + 1);
    source += kProbeHookName;

    const uint64_t hash = HashFile(source);
    if (hash == 0) {
        std::printf("tray-probe: hook_missing path=\"");
        PrintUtf8(source.c_str());
        std::printf("\"\n");
        return nullptr;
    }

    const std::wstring directory = ProbeHomeDirectory();
    if (directory.empty()) return nullptr;
    wchar_t leaf[96] = {};
    std::swprintf(leaf, sizeof(leaf) / sizeof(leaf[0]), L"island_tray_probe_hook-%016llX.dll",
                  static_cast<unsigned long long>(hash));
    const std::wstring staged = directory + L"\\" + leaf;
    if (GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !CopyFileW(source.c_str(), staged.c_str(), FALSE)) {
        std::printf("tray-probe: hook_stage_failed error=%lu\n", GetLastError());
        return nullptr;
    }
    HMODULE module = LoadLibraryW(staged.c_str());
    if (!module) {
        std::printf("tray-probe: hook_load_failed error=%lu path=\"", GetLastError());
        PrintUtf8(staged.c_str());
        std::printf("\"\n");
    }
    return module;
}

bool WaitForObserver(ProbeState* state, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeoutMs) {
        DrainRecords(state);
        if (state->observerReady) return true;
        Sleep(25);
    }
    DrainRecords(state);
    return state->observerReady;
}

bool InstallObserver(ProbeState* state) {
    if (!state) return false;
    state->shellPid = ShellProcessId();
    if (!state->shellPid) {
        std::printf("tray-probe: explorer_missing reason=Shell_TrayWnd_not_found\n");
        return false;
    }
    ++state->generation;

    // Discard records from an earlier probe session. The observer publishes new records after the hook lands.
    if (state->reader.Open()) {
        island::TrayRingRecord discard[island::kTrayRingCapacity] = {};
        std::uint64_t discardCursor = 0;
        (void)state->reader.Drain(discardCursor, discard, island::kTrayRingCapacity);
        state->cursor = state->reader.header ? state->reader.Published() : 0;
    }

    HMODULE module = LoadProbeHook();
    if (!module) return false;
    FARPROC procedure = GetProcAddress(module, "IslandTrayHookProc");
    if (!procedure) {
        std::printf("tray-probe: hook_entry_missing\n");
        return false;
    }

    struct HookHandle {
        HHOOK hook = nullptr;
        DWORD threadId = 0;
    };
    std::vector<HookHandle> hooks;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 thread = {};
        thread.dwSize = sizeof(thread);
        if (Thread32First(snapshot, &thread)) {
            do {
                if (thread.th32OwnerProcessID != state->shellPid) continue;
                HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, reinterpret_cast<HOOKPROC>(procedure), module,
                                               thread.th32ThreadID);
                if (hook) hooks.push_back(HookHandle{hook, thread.th32ThreadID});
            } while (Thread32Next(snapshot, &thread));
        }
        CloseHandle(snapshot);
    }

    for (const HookHandle& item : hooks) PostThreadMessageW(item.threadId, WM_NULL, 0, 0);
    state->observerReady = WaitForObserver(state, 3000);
    for (const HookHandle& item : hooks) UnhookWindowsHookEx(item.hook);

    std::printf("tray-probe: observer generation=%u pid=%lu ready=%d hooks=%d tray_tid=%u\n",
                state->generation, static_cast<unsigned long>(state->shellPid),
                state->observerReady ? 1 : 0, static_cast<int>(hooks.size()), state->observerTid);
    // A module already pinned by an earlier probe run does not publish its one-time handshake again. An
    // accepted thread hook still loads the module and wakes the observer-only channel, so the probe proceeds.
    return state->observerReady || !hooks.empty();
}

void BroadcastTaskbarCreated(ProbeState* state) {
    const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    if (!message) return;
    DWORD_PTR ignored = 0;
    (void)SendMessageTimeoutW(HWND_BROADCAST, message, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000,
                              &ignored);

    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 3000) {
        DrainRecords(state);
        Sleep(100);
    }
}

bool ParseOptions(int argc, char** argv, Options* options) {
    if (!options) return false;
    for (int i = 1; i < argc; ++i) {
        const char* value = argv[i];
        if (std::strcmp(value, "--details") == 0) {
            options->details = true;
        } else if (std::strcmp(value, "--no-install") == 0) {
            options->install = false;
        } else if (std::strcmp(value, "--no-broadcast") == 0) {
            options->broadcast = false;
        } else if (std::strcmp(value, "--samples") == 0 && i + 1 < argc) {
            options->samples = std::atoi(argv[++i]);
            if (options->samples < 1) options->samples = 1;
        } else if (std::strcmp(value, "--interval-ms") == 0 && i + 1 < argc) {
            const int parsed = std::atoi(argv[++i]);
            options->intervalMs = parsed > 0 ? static_cast<DWORD>(parsed) : 0;
        } else if (std::strcmp(value, "--help") == 0) {
            options->showHelp = true;
            return true;
        } else {
            std::printf("tray-probe: unknown_argument=\"%s\"\n", value);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options options;
    if (!ParseOptions(argc, argv, &options)) return 2;
    if (options.showHelp) {
        std::printf(
            "tray-probe [--details] [--samples N] [--interval-ms N]\n"
            "           [--no-install] [--no-broadcast]\n");
        return 0;
    }

    ProbeState state;
    state.startedTick = GetTickCount64();
    state.shellPid = ShellProcessId();
    if (!state.shellPid) {
        std::printf("tray-probe: explorer_missing reason=Shell_TrayWnd_not_found\n");
        return 3;
    }
    std::printf("tray-probe: explorer_pid=%lu generation=0\n", static_cast<unsigned long>(state.shellPid));

    if (!state.reader.Open()) {
        std::printf("tray-probe: ring_open_failed\n");
        return 4;
    }

    if (options.install && !InstallObserver(&state)) {
        std::printf("tray-probe: observer_install_failed\n");
        return 5;
    }
    if (options.broadcast) BroadcastTaskbarCreated(&state);

    std::map<std::wstring, IconObservation> previous;
    std::vector<long long> updateLatencies;
    for (int sample = 0; sample < options.samples; ++sample) {
        const DWORD shellNow = ShellProcessId();
        if (shellNow && shellNow != state.shellPid) {
            state.shellPid = shellNow;
            ++state.generation;
            state.ledger.Reset();
            state.contacts.clear();
            state.observerReady = false;
            state.cursor = state.reader.header ? state.reader.Published() : 0;
            std::printf("tray-probe: explorer_pid_changed pid=%lu generation=%u\n",
                        static_cast<unsigned long>(state.shellPid), state.generation);
        }

        DrainRecords(&state);
        ForgetStaleWindows(&state);
        const std::vector<RegistryIcon> registryIcons = ReadRegistryIcons();
        const VisibleResult visible = BuildVisibleIcons(state, registryIcons);
        const ULONGLONG now = GetTickCount64();
        for (const VisibleIcon& icon : visible.icons) {
            MeasureVisibleChange(icon, now, &previous, &updateLatencies);
        }

        std::printf(
            "tray-probe: registry=%d live=%d visible=%d duplicates=%d stale=%d records=%llu "
            "observer_ready=%d\n",
            visible.registryCount, state.ledger.Count(),
            static_cast<int>(visible.icons.size()), visible.duplicates, state.staleRemoved,
            static_cast<unsigned long long>(state.recordsRead), state.observerReady ? 1 : 0);
        if (options.details) {
            for (int i = 0; i < static_cast<int>(visible.icons.size()); ++i) {
                PrintIconDetails(visible.icons[i], i, state.generation);
            }
        }
        std::fflush(stdout);

        if (sample + 1 < options.samples && options.intervalMs > 0) Sleep(options.intervalMs);
    }

    std::sort(updateLatencies.begin(), updateLatencies.end());
    const int p50 = Percentile(updateLatencies, 50);
    const int p95 = Percentile(updateLatencies, 95);
    if (p50 < 0) {
        std::printf("tray-probe: modify-to-paint p50=N/A p95=N/A samples=0\n");
    } else {
        std::printf("tray-probe: modify-to-paint p50=%dms p95=%dms samples=%d\n", p50, p95,
                    static_cast<int>(updateLatencies.size()));
    }
    std::printf("tray-probe: done elapsed_ms=%llu\n",
                static_cast<unsigned long long>(GetTickCount64() - state.startedTick));
    return 0;
}
