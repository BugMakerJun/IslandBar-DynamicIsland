// =====================================================================
//  TaskbarRecovery.cpp -- see TaskbarRecovery.h
// =====================================================================
#include "platform/TaskbarRecovery.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace island {
namespace {

constexpr LONG kGuardianMagic = 0x47524249;  // 'IBRG'
constexpr LONG kRecoveryFileMagic = 0x46425249;  // 'IRBF'
constexpr DWORD kControlMutexTimeoutMs = 1000;
constexpr DWORD kGuardianClaimTimeoutMs = 3000;
constexpr DWORD kRecoveryReadyTimeoutMs = 3000;
constexpr DWORD kRecoveryRetryDelayMs = 100;

struct GuardianControl {
    LONG magic = 0;
    LONG version = 0;
    std::uint64_t generation = 0;
    std::uint64_t readyGeneration = 0;
    std::uint64_t cleanGeneration = 0;
    DWORD ownerPid = 0;
    DWORD ownerSession = 0;
    std::uint64_t ownerCreationTime = 0;
    DWORD guardianPid = 0;
    DWORD guardianSession = 0;
    std::uint64_t guardianCreationTime = 0;
};

struct GuardianChannel {
    HANDLE mapping = nullptr;
    HANDLE mutex = nullptr;
    HANDLE ownerEvent = nullptr;
    HANDLE readyEvent = nullptr;
    GuardianControl* control = nullptr;
};

struct RecoveryFile {
    LONG magic = 0;
    unsigned version = 0;
    unsigned size = 0;
    std::uint64_t checksum = 0;
    TaskbarRecoverySnapshot snapshot = {};
};

GuardianChannel g_clientChannel;
GuardianChannel g_guardianChannel;
SRWLOCK g_clientChannelLock = SRWLOCK_INIT;
SRWLOCK g_guardianChannelLock = SRWLOCK_INIT;
SRWLOCK g_recoveryLogLock = SRWLOCK_INIT;
volatile LONG g_guardianCreateState = 0;
std::uint64_t g_armedGeneration = 0;

bool AcquireControl(GuardianChannel* channel, DWORD timeoutMs) {
    if (!channel || !channel->mutex) return false;
    const DWORD result = WaitForSingleObject(channel->mutex, timeoutMs);
    return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

void ReleaseControl(GuardianChannel* channel) {
    if (channel && channel->mutex) ReleaseMutex(channel->mutex);
}

void CloseChannel(GuardianChannel* channel) {
    if (!channel) return;
    if (channel->control && channel->mapping) UnmapViewOfFile(channel->control);
    if (channel->ownerEvent) CloseHandle(channel->ownerEvent);
    if (channel->readyEvent) CloseHandle(channel->readyEvent);
    if (channel->mutex) CloseHandle(channel->mutex);
    if (channel->mapping) CloseHandle(channel->mapping);
    *channel = {};
}

bool EnsureChannel(GuardianChannel* channel, SRWLOCK* channelLock) {
    if (!channel || !channelLock) return false;
    AcquireSRWLockExclusive(channelLock);
    struct Unlock {
        SRWLOCK* lock;
        ~Unlock() { ReleaseSRWLockExclusive(lock); }
    } unlock{channelLock};

    if (channel->control) return true;

    channel->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          static_cast<DWORD>(sizeof(GuardianControl)),
                                          kTaskbarGuardianMappingName);
    if (!channel->mapping) return false;
    void* view = MapViewOfFile(channel->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GuardianControl));
    if (!view) {
        CloseChannel(channel);
        return false;
    }
    channel->control = static_cast<GuardianControl*>(view);
    channel->mutex = CreateMutexW(nullptr, FALSE, kTaskbarGuardianMutexName);
    channel->ownerEvent = CreateEventW(nullptr, FALSE, FALSE, kTaskbarGuardianOwnerEventName);
    channel->readyEvent = CreateEventW(nullptr, FALSE, FALSE, kTaskbarGuardianReadyEventName);
    if (!channel->mutex || !channel->ownerEvent || !channel->readyEvent) {
        CloseChannel(channel);
        return false;
    }

    if (!AcquireControl(channel, kControlMutexTimeoutMs)) {
        CloseChannel(channel);
        return false;
    }
    const bool initialized = channel->control->magic == kGuardianMagic &&
                             channel->control->version == static_cast<LONG>(kTaskbarRecoveryVersion);
    if (!initialized && channel->control->magic == 0) {
        std::memset(channel->control, 0, sizeof(GuardianControl));
        channel->control->version = static_cast<LONG>(kTaskbarRecoveryVersion);
        MemoryBarrier();
        InterlockedExchange(&channel->control->magic, kGuardianMagic);
    }
    const bool valid = channel->control->magic == kGuardianMagic &&
                       channel->control->version == static_cast<LONG>(kTaskbarRecoveryVersion);
    ReleaseControl(channel);
    if (!valid) {
        CloseChannel(channel);
        return false;
    }
    return true;
}

bool RecoveryPath(wchar_t* out, std::size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = L'\0';
    wchar_t home[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const std::wstring directory = std::wstring(home) + L"\\IslandBar";
    CreateDirectoryW(directory.c_str(), nullptr);
    return _snwprintf_s(out, capacity, _TRUNCATE, L"%ls\\taskbar_recovery.dat", directory.c_str()) >= 0;
}

void RecoveryLog(const wchar_t* path, const wchar_t* format, ...) {
    if (!format) return;
    wchar_t body[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(body, 1024, _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t line[1200] = {};
    _snwprintf_s(line, 1200, _TRUNCATE, L"[%02u:%02u:%02u.%03u] %ls\r\n",
                 static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
                 static_cast<unsigned>(now.wSecond), static_cast<unsigned>(now.wMilliseconds),
                 body);

    char utf8[4096] = {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (bytes <= 1) return;

    AcquireSRWLockExclusive(&g_recoveryLogLock);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (path && path[0]) {
        file = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) {
        wchar_t temp[MAX_PATH] = {};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (length > 0 && length < MAX_PATH) {
            wcscat_s(temp, MAX_PATH, L"island_bar.log");
            file = CreateFileW(temp, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        (void)WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&g_recoveryLogLock);
}

std::uint64_t Checksum(const TaskbarRecoverySnapshot& snapshot) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&snapshot);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < sizeof(snapshot); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool ReadRecoveryFile(TaskbarRecoverySnapshot* snapshot) {
    if (!snapshot) return false;
    wchar_t path[MAX_PATH] = {};
    if (!RecoveryPath(path, MAX_PATH)) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    RecoveryFile record;
    DWORD read = 0;
    const bool readOk = ReadFile(file, &record, sizeof(record), &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!readOk || read != sizeof(record) || record.magic != kRecoveryFileMagic ||
        record.version != kTaskbarRecoveryVersion || record.size != sizeof(record) ||
        record.checksum != Checksum(record.snapshot)) {
        return false;
    }
    *snapshot = record.snapshot;
    return true;
}

bool ProcessMatches(DWORD pid, std::uint64_t creationTime) {
    if (pid == 0 || creationTime == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    std::uint64_t observed = 0;
    bool matches = false;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER value = {};
        value.LowPart = created.dwLowDateTime;
        value.HighPart = created.dwHighDateTime;
        observed = value.QuadPart;
        matches = observed == creationTime && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }
    CloseHandle(process);
    return matches;
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool RestoreSnapshot(const TaskbarRecoverySnapshot& snapshot) {
    const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!primary) return false;

    DWORD shellPid = 0;
    GetWindowThreadProcessId(primary, &shellPid);
    const bool sameShell = snapshot.shellPid == 0 || shellPid == snapshot.shellPid;

    if (snapshot.mode == TaskbarRecoveryMode::ClipToTray) {
        SetWindowRgn(primary, nullptr, TRUE);
    }
    if (snapshot.mode == TaskbarRecoveryMode::ClipToTray || snapshot.primaryWasVisible) {
        ShowWindow(primary, snapshot.primaryWasVisible ? SW_SHOWNA : SW_HIDE);
    }
    const HWND notify = FindWindowExW(primary, nullptr, L"TrayNotifyWnd", nullptr);
    if (notify) {
        ShowWindow(notify, snapshot.primaryWasVisible ? SW_SHOWNA : SW_HIDE);
    }

    if (sameShell) {
        for (unsigned i = 0; i < snapshot.secondaryCount; ++i) {
            const HWND window = reinterpret_cast<HWND>(
                static_cast<std::uintptr_t>(snapshot.secondary[i].window));
            if (!window || !IsWindow(window)) continue;
            SetWindowRgn(window, nullptr, TRUE);
            ShowWindow(window, snapshot.secondary[i].wasVisible ? SW_SHOWNA : SW_HIDE);
        }
    }

    if (snapshot.workAreaValid) {
        RECT current = {};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &current, 0) &&
            !SameRect(current, snapshot.workArea) && SameRect(current, snapshot.monitor)) {
            RECT wanted = snapshot.workArea;
            (void)SystemParametersInfoW(SPI_SETWORKAREA, 0, &wanted, 0);
        }
    }

    RecoveryLog(snapshot.logPath,
                L"  taskbar guardian: restored reason=owner-terminated pid %lu shell %lu mode %u",
                static_cast<unsigned long>(snapshot.ownerPid),
                static_cast<unsigned long>(shellPid), static_cast<unsigned>(snapshot.mode));
    return true;
}

bool LoadAndRestoreForOwner(const GuardianControl& owner, bool requireOwnerDead) {
    TaskbarRecoverySnapshot snapshot;
    if (!ReadRecoveryFile(&snapshot)) return true;
    if (snapshot.ownerPid != owner.ownerPid ||
        snapshot.ownerCreationTime != owner.ownerCreationTime ||
        snapshot.guardianGeneration != owner.generation) {
        return false;
    }
    if (requireOwnerDead && ProcessMatches(snapshot.ownerPid, snapshot.ownerCreationTime)) return false;
    if (!RestoreSnapshot(snapshot)) {
        RecoveryLog(snapshot.logPath,
                    L"  taskbar guardian: restore failed pid %lu; marker retained",
                    static_cast<unsigned long>(snapshot.ownerPid));
        return false;
    }
    (void)DeleteTaskbarRecoverySnapshot();
    return true;
}

void DeleteSnapshotForOwner(const GuardianControl& owner) {
    TaskbarRecoverySnapshot snapshot;
    if (!ReadRecoveryFile(&snapshot)) return;
    if (snapshot.ownerPid == owner.ownerPid &&
        snapshot.ownerCreationTime == owner.ownerCreationTime &&
        snapshot.guardianGeneration == owner.generation) {
        (void)DeleteTaskbarRecoverySnapshot();
    }
}

void GuardianLoop() {
    // A new content-stamped module may be loaded after an update while the old
    // pinned copy is still present. Only one guardian loop may own the channel:
    // the others exit without touching the mutex or the taskbar.
    HANDLE singleton = CreateMutexW(nullptr, FALSE, kTaskbarGuardianThreadMutexName);
    if (!singleton) return;
    const DWORD ownerResult = WaitForSingleObject(singleton, 0);
    if (ownerResult != WAIT_OBJECT_0 && ownerResult != WAIT_ABANDONED) {
        CloseHandle(singleton);
        return;
    }

    GuardianChannel* channel = &g_guardianChannel;
    if (!EnsureChannel(channel, &g_guardianChannelLock)) {
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        return;
    }

    for (;;) {
        if (WaitForSingleObject(channel->ownerEvent, INFINITE) != WAIT_OBJECT_0) break;

        GuardianControl owner;
        if (!AcquireControl(channel, kControlMutexTimeoutMs)) continue;
        owner = *channel->control;
        ReleaseControl(channel);
        if (owner.ownerPid == 0 || owner.ownerCreationTime == 0 ||
            !ProcessMatches(owner.ownerPid, owner.ownerCreationTime)) {
            continue;
        }

        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     owner.ownerPid);
        if (!process) continue;
        std::uint64_t guardianCreation = 0;
        if (!ProcessCreationTimeFor(GetCurrentProcessId(), &guardianCreation)) {
            CloseHandle(process);
            continue;
        }
        DWORD guardianSession = 0;
        if (!CurrentProcessSessionId(&guardianSession)) {
            CloseHandle(process);
            continue;
        }

        if (AcquireControl(channel, kControlMutexTimeoutMs)) {
            channel->control->guardianPid = GetCurrentProcessId();
            channel->control->guardianSession = guardianSession;
            channel->control->guardianCreationTime = guardianCreation;
            channel->control->readyGeneration = owner.generation;
            ReleaseControl(channel);
            SetEvent(channel->readyEvent);
        }

        (void)WaitForSingleObject(process, INFINITE);
        CloseHandle(process);

        bool clean = false;
        if (AcquireControl(channel, kControlMutexTimeoutMs)) {
            clean = channel->control->cleanGeneration == owner.generation;
            if (channel->control->generation == owner.generation) {
                channel->control->guardianPid = 0;
                channel->control->guardianSession = 0;
                channel->control->guardianCreationTime = 0;
                channel->control->readyGeneration = 0;
            }
            ReleaseControl(channel);
        }
        if (!clean) {
            (void)LoadAndRestoreForOwner(owner, true);
        } else {
            DeleteSnapshotForOwner(owner);
        }
    }
    ReleaseMutex(singleton);
    CloseHandle(singleton);
}

unsigned long __stdcall GuardianThreadEntry(void*) {
    GuardianLoop();
    return 0;
}

} // namespace

bool ProcessCreationTimeFor(DWORD pid, std::uint64_t* creationTime) {
    if (creationTime) *creationTime = 0;
    if (!creationTime || pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!ok) return false;
    ULARGE_INTEGER value = {};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    *creationTime = value.QuadPart;
    return true;
}

bool CurrentProcessSessionId(DWORD* sessionId) {
    if (!sessionId) return false;
    return ProcessIdToSessionId(GetCurrentProcessId(), sessionId) != FALSE;
}

bool WriteTaskbarRecoverySnapshot(const TaskbarRecoverySnapshot& snapshot) {
    wchar_t path[MAX_PATH] = {};
    if (!RecoveryPath(path, MAX_PATH)) return false;
    RecoveryFile record;
    record.magic = kRecoveryFileMagic;
    record.version = kTaskbarRecoveryVersion;
    record.size = sizeof(record);
    record.snapshot = snapshot;
    record.snapshot.ownerPid = GetCurrentProcessId();
    record.snapshot.ownerCreationTime = 0;
    record.snapshot.ownerSession = 0;
    record.snapshot.guardianGeneration = g_armedGeneration;
    (void)ProcessCreationTimeFor(GetCurrentProcessId(), &record.snapshot.ownerCreationTime);
    (void)CurrentProcessSessionId(&record.snapshot.ownerSession);
    record.checksum = Checksum(record.snapshot);

    wchar_t temporary[MAX_PATH] = {};
    _snwprintf_s(temporary, MAX_PATH, _TRUNCATE, L"%ls.tmp.%lu", path,
                 static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = WriteFile(file, &record, sizeof(record), &written, nullptr) != FALSE &&
                       written == sizeof(record) && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!wrote) {
        DeleteFileW(temporary);
        return false;
    }
    if (!MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary);
        return false;
    }
    return true;
}

bool DeleteTaskbarRecoverySnapshot() {
    wchar_t path[MAX_PATH] = {};
    if (!RecoveryPath(path, MAX_PATH)) return false;
    return DeleteFileW(path) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool RecoverStaleTaskbarIfAny() {
    TaskbarRecoverySnapshot snapshot;
    if (!ReadRecoveryFile(&snapshot)) {
        wchar_t path[MAX_PATH] = {};
        if (RecoveryPath(path, MAX_PATH) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            // The final path is published atomically, so a record that cannot be
            // validated is debris rather than a write in progress. Leaving it
            // forever would prevent every later startup from using recovery.
            (void)DeleteFileW(path);
        }
        return false;
    }
    DWORD session = 0;
    if (!CurrentProcessSessionId(&session) || session != snapshot.ownerSession) {
        (void)DeleteTaskbarRecoverySnapshot();
        return false;
    }
    if (ProcessMatches(snapshot.ownerPid, snapshot.ownerCreationTime)) return false;
    if (!RestoreSnapshot(snapshot)) return false;
    return DeleteTaskbarRecoverySnapshot();
}

bool ArmTaskbarGuardian(std::uint64_t* generationOut) {
    if (!EnsureChannel(&g_clientChannel, &g_clientChannelLock)) return false;
    GuardianChannel* channel = &g_clientChannel;
    if (!AcquireControl(channel, kControlMutexTimeoutMs)) return false;
    channel->control->generation += 1;
    const std::uint64_t generation = channel->control->generation;
    channel->control->ownerPid = GetCurrentProcessId();
    channel->control->ownerSession = 0;
    (void)CurrentProcessSessionId(&channel->control->ownerSession);
    channel->control->ownerCreationTime = 0;
    (void)ProcessCreationTimeFor(GetCurrentProcessId(), &channel->control->ownerCreationTime);
    channel->control->readyGeneration = 0;
    channel->control->cleanGeneration = 0;
    channel->control->guardianPid = 0;
    channel->control->guardianSession = 0;
    channel->control->guardianCreationTime = 0;
    ResetEvent(channel->readyEvent);
    ReleaseControl(channel);
    g_armedGeneration = generation;
    SetEvent(channel->ownerEvent);
    if (generationOut) *generationOut = generation;
    return true;
}

bool AnnounceTaskbarGuardian() {
    if (!g_armedGeneration || !EnsureChannel(&g_clientChannel, &g_clientChannelLock)) return false;
    SetEvent(g_clientChannel.ownerEvent);
    return true;
}

bool WaitForTaskbarGuardian(std::uint64_t generation, DWORD timeoutMs) {
    if (!generation || !EnsureChannel(&g_clientChannel, &g_clientChannelLock)) return false;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        if (WaitForSingleObject(g_clientChannel.readyEvent, remaining) != WAIT_OBJECT_0) return false;
        if (!AcquireControl(&g_clientChannel, kControlMutexTimeoutMs)) continue;
        const GuardianControl& control = *g_clientChannel.control;
        const bool ready =
            control.readyGeneration == generation && control.ownerPid == GetCurrentProcessId() &&
            ProcessMatches(control.guardianPid, control.guardianCreationTime);
        ReleaseControl(&g_clientChannel);
        if (ready) return true;
    }
}

bool WaitForCurrentTaskbarGuardian(DWORD timeoutMs) {
    return g_armedGeneration != 0 && WaitForTaskbarGuardian(g_armedGeneration, timeoutMs);
}

bool TaskbarGuardianHealthy() {
    if (!g_armedGeneration || !EnsureChannel(&g_clientChannel, &g_clientChannelLock)) return false;
    if (!AcquireControl(&g_clientChannel, kControlMutexTimeoutMs)) return false;
    const GuardianControl& control = *g_clientChannel.control;
    const bool healthy =
        control.ownerPid == GetCurrentProcessId() &&
        control.readyGeneration == g_armedGeneration &&
        control.cleanGeneration != g_armedGeneration &&
        ProcessMatches(control.guardianPid, control.guardianCreationTime);
    ReleaseControl(&g_clientChannel);
    return healthy;
}

void MarkTaskbarGuardianClean() {
    if (!g_armedGeneration || !EnsureChannel(&g_clientChannel, &g_clientChannelLock)) return;
    if (!AcquireControl(&g_clientChannel, kControlMutexTimeoutMs)) return;
    if (g_clientChannel.control->ownerPid == GetCurrentProcessId() &&
        g_clientChannel.control->generation == g_armedGeneration) {
        g_clientChannel.control->cleanGeneration = g_armedGeneration;
    }
    ReleaseControl(&g_clientChannel);
}

bool EnsureTaskbarGuardianThread() {
    if (InterlockedCompareExchange(&g_guardianCreateState, 1, 0) != 0) {
        return g_guardianCreateState == 1;
    }
    if (!EnsureChannel(&g_guardianChannel, &g_guardianChannelLock)) {
        InterlockedExchange(&g_guardianCreateState, 2);
        return false;
    }
    HANDLE thread = CreateThread(nullptr, 0, &GuardianThreadEntry, nullptr, 0, nullptr);
    if (!thread) {
        InterlockedExchange(&g_guardianCreateState, 2);
        return false;
    }
    CloseHandle(thread);
    return true;
}

} // namespace island
