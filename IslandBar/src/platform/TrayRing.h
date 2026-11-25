// =====================================================================
//  TrayRing.h -- THE BOUNDED, CROSS-PROCESS CHANNEL FROM EXPLORER TO THE BAR
//
//  WHY THIS IS LOCKED. A fixed-size shared-memory ring used to publish with
//  per-slot sequence numbers. That was not safe: multiple Hook threads could
//  wrap onto a slot another writer still owned, and a reader copied a live C++
//  object. A matching sequence before and after does not make concurrent
//  non-atomic reads defined.
//
//  ABI 6 therefore uses one named mutex for every access to the shared bytes.
//  The queue never overwrites an unconsumed record. The publisher gives up after
//  a very short wait instead of ever blocking Explorer; the single consumer
//  leaves its cursor untouched when it cannot obtain the lock and retries later.
//  A lock abandoned by a crashed process is recovered before it is reused.
//
//  The object names carry v6, so an older pinned module (ABI 5) can coexist
//  safely with a newer bar during an upgrade.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "domain/TrayRegistration.h"

namespace island {

inline constexpr unsigned kTrayRingAbi = 6;
#if defined(ISLANDBAR_TRAY_PROBE_RING)
inline constexpr wchar_t kTrayRingName[] = L"Local\\IslandBar.TrayProbeRing.v6";
inline constexpr wchar_t kTrayRingEventName[] = L"Local\\IslandBar.TrayProbeRing.Event.v6";
#else
inline constexpr wchar_t kTrayRingName[] = L"Local\\IslandBar.TrayRing.v6";
inline constexpr wchar_t kTrayRingEventName[] = L"Local\\IslandBar.TrayRing.Event.v6";
#endif
inline constexpr unsigned kTrayRingCapacity = 64;
inline constexpr LONG kTrayRingMagic = 0x36525254;  // 'TRR6'
inline constexpr DWORD kTrayPublisherLockWaitMs = 1;
inline constexpr DWORD kTrayReaderLockWaitMs = 20;

enum class TrayRingKind : unsigned {
    Registration = 0,
    HookState = 1,
};

struct TrayHookState {
    unsigned pid = 0;
    bool inShell = false;
    bool ready = false;
    unsigned trayThread = 0;
    unsigned hooks = 0;
    unsigned attempts = 0;
    unsigned lastError = 0;
};

struct TrayRingRecord {
    TrayRingKind kind = TrayRingKind::Registration;
    std::uint64_t publishedTick = 0;
    TrayRegistration registration;
    TrayHookState hook;
};

static_assert(std::is_same_v<decltype(TrayRegistration::trayWindow), std::uint64_t>);
static_assert(offsetof(TrayRingRecord, registration) >= sizeof(TrayRingKind));
static_assert(offsetof(TrayRingRecord, hook) > offsetof(TrayRingRecord, registration));
static_assert(sizeof(TrayRingRecord) <= 512);

struct TrayRingHeader {
    LONG magic = 0;
    LONG abi = 0;
    unsigned version = 0;
    unsigned recordSize = 0;
    unsigned capacity = 0;
    LONG writeInFlight = 0;
    unsigned reserved = 0;
    volatile LONG64 written = 0;
    volatile LONG64 read = 0;
    volatile LONG64 pendingIndex = 0;
    volatile LONG64 droppedFull = 0;
    volatile LONG64 droppedLock = 0;
    DWORD consumerPid = 0;
    DWORD consumerSession = 0;
    std::uint64_t consumerCreationTime = 0;
};

inline constexpr std::size_t kTrayRingBytes =
    sizeof(TrayRingHeader) + sizeof(TrayRingRecord) * kTrayRingCapacity;

namespace detail {

inline bool TrayProcessIdentity(DWORD pid, std::uint64_t* creationTime, DWORD* sessionId) {
    if (creationTime) *creationTime = 0;
    if (sessionId) *sessionId = 0;
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    const bool times = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!times) return false;
    if (creationTime) {
        ULARGE_INTEGER value = {};
        value.LowPart = created.dwLowDateTime;
        value.HighPart = created.dwHighDateTime;
        *creationTime = value.QuadPart;
    }
    if (sessionId) (void)ProcessIdToSessionId(pid, sessionId);
    return creationTime != nullptr;
}

inline bool TrayProcessAlive(DWORD pid, std::uint64_t creationTime, DWORD sessionId) {
    if (!pid || !creationTime) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    bool alive = false;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER value = {};
        value.LowPart = created.dwLowDateTime;
        value.HighPart = created.dwHighDateTime;
        DWORD observedSession = 0;
        (void)ProcessIdToSessionId(pid, &observedSession);
        alive = value.QuadPart == creationTime && observedSession == sessionId &&
                WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }
    CloseHandle(process);
    return alive;
}

class TrayRingEndpoint {
public:
    TrayRingEndpoint() = default;
    ~TrayRingEndpoint() { Close(); }
    TrayRingEndpoint(const TrayRingEndpoint&) = delete;
    TrayRingEndpoint& operator=(const TrayRingEndpoint&) = delete;

    HANDLE mapping = nullptr;
    HANDLE event = nullptr;
    TrayRingHeader* header = nullptr;
    TrayRingRecord* records = nullptr;
    unsigned abiSeen = 0;

protected:
    void* view_ = nullptr;
    HANDLE mutex_ = nullptr;
    bool refused_ = false;
    SRWLOCK openLock_ = SRWLOCK_INIT;

    bool OpenEndpoint(const wchar_t* name, const wchar_t* eventName) {
        if (header) return true;
        if (refused_) return false;
        if (!name || !eventName) {
            refused_ = true;
            return false;
        }

        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(kTrayRingBytes), name);
        if (!mapping) {
            refused_ = true;
            return false;
        }
        view_ = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kTrayRingBytes);
        if (!view_) {
            refused_ = true;
            return false;
        }

        wchar_t mutexName[256] = {};
        if (_snwprintf_s(mutexName, 256, _TRUNCATE, L"%ls.Mutex", name) < 0) {
            refused_ = true;
            return false;
        }
        mutex_ = CreateMutexW(nullptr, FALSE, mutexName);
        event = CreateEventW(nullptr, FALSE, FALSE, eventName);
        if (!mutex_) {
            refused_ = true;
            return false;
        }

        if (!Lock(1000)) {
            refused_ = true;
            return false;
        }
        TrayRingHeader* candidate = static_cast<TrayRingHeader*>(view_);
        if (candidate->magic == 0) {
            std::memset(view_, 0, kTrayRingBytes);
            candidate->abi = static_cast<LONG>(kTrayRingAbi);
            candidate->version = kTrayRingAbi;
            candidate->recordSize = static_cast<unsigned>(sizeof(TrayRingRecord));
            candidate->capacity = kTrayRingCapacity;
            MemoryBarrier();
            InterlockedExchange(&candidate->magic, kTrayRingMagic);
        }
        abiSeen = static_cast<unsigned>(candidate->abi);
        const bool valid = candidate->magic == kTrayRingMagic &&
                           candidate->abi == static_cast<LONG>(kTrayRingAbi) &&
                           candidate->version == kTrayRingAbi &&
                           candidate->recordSize == sizeof(TrayRingRecord) &&
                           candidate->capacity == kTrayRingCapacity;
        if (valid) {
            header = candidate;
            records = reinterpret_cast<TrayRingRecord*>(
                static_cast<unsigned char*>(view_) + sizeof(TrayRingHeader));
        } else {
            refused_ = true;
        }
        Unlock();
        return valid;
    }

    bool Lock(DWORD timeoutMs) {
        if (!mutex_) return false;
        const DWORD result = WaitForSingleObject(mutex_, timeoutMs);
        if (result == WAIT_OBJECT_0) return true;
        if (result != WAIT_ABANDONED) return false;
        RecoverAbandoned();
        return true;
    }

    void Unlock() {
        if (mutex_) ReleaseMutex(mutex_);
    }

    void RecoverAbandoned() {
        TrayRingHeader* candidate = static_cast<TrayRingHeader*>(view_);
        if (!candidate) return;
        if (candidate->magic == 0) {
            std::memset(view_, 0, kTrayRingBytes);
            return;
        }
        if (candidate->magic == kTrayRingMagic &&
            candidate->version == kTrayRingAbi &&
            candidate->recordSize == sizeof(TrayRingRecord) &&
            candidate->capacity == kTrayRingCapacity) {
            TrayRingRecord* candidateRecords = reinterpret_cast<TrayRingRecord*>(
                static_cast<unsigned char*>(view_) + sizeof(TrayRingHeader));
            if (candidate->writeInFlight != 0) {
                const std::uint64_t slotIndex = candidate->pendingIndex % kTrayRingCapacity;
                std::memset(&candidateRecords[slotIndex], 0, sizeof(TrayRingRecord));
                candidate->writeInFlight = 0;
            }
            if (candidate->read > candidate->written) {
                InterlockedExchange64(&candidate->read, candidate->written);
            }
        }
    }

    bool EnsureConsumer() {
        if (!header) return false;
        if (!Lock(kTrayReaderLockWaitMs)) return false;
        const DWORD pid = GetCurrentProcessId();
        DWORD session = 0;
        std::uint64_t creationTime = 0;
        (void)TrayProcessIdentity(pid, &creationTime, &session);
        bool accepted = false;
        if (header->consumerPid == 0 ||
            (header->consumerPid == pid && header->consumerCreationTime == creationTime)) {
            accepted = true;
        } else if (!TrayProcessAlive(header->consumerPid, header->consumerCreationTime,
                                     header->consumerSession)) {
            accepted = true;
        }
        if (accepted) {
            header->consumerPid = pid;
            header->consumerSession = session;
            header->consumerCreationTime = creationTime;
        }
        Unlock();
        return accepted;
    }

    void Close() {
        if (view_) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        header = nullptr;
        records = nullptr;
        if (event) CloseHandle(event);
        if (mutex_) CloseHandle(mutex_);
        if (mapping) CloseHandle(mapping);
        event = nullptr;
        mutex_ = nullptr;
        mapping = nullptr;
    }
};

} // namespace detail

struct TrayRingPublisher : detail::TrayRingEndpoint {
    bool refused = false;
    bool reported = false;

    bool Open(const wchar_t* name = kTrayRingName,
              const wchar_t* eventName = kTrayRingEventName) {
        AcquireSRWLockExclusive(&openLock_);
        struct UnlockOpen {
            SRWLOCK* lock;
            ~UnlockOpen() { ReleaseSRWLockExclusive(lock); }
        } unlockOpen{&openLock_};
        if (header) return true;
        if (refused_) return false;
        const bool opened = OpenEndpoint(name, eventName);
        refused = !opened;
        return opened;
    }

    void Publish(const TrayRegistration& registration) {
        if (!Open() || !records) return;
        if (!Lock(kTrayPublisherLockWaitMs)) {
            InterlockedIncrement64(&header->droppedLock);
            return;
        }
        if (header->read > header->written) {
            InterlockedExchange64(&header->read, header->written);
        }
        if (header->written - header->read >= kTrayRingCapacity) {
            InterlockedIncrement64(&header->droppedFull);
            Unlock();
            return;
        }
        const std::uint64_t index = static_cast<std::uint64_t>(header->written);
        TrayRingRecord& slot = records[index % kTrayRingCapacity];
        InterlockedExchange64(&header->pendingIndex, static_cast<LONG64>(index));
        header->writeInFlight = 1;
        MemoryBarrier();
        slot = {};
        slot.kind = TrayRingKind::Registration;
        slot.publishedTick = GetTickCount64();
        slot.registration = registration;
        MemoryBarrier();
        InterlockedIncrement64(&header->written);
        header->writeInFlight = 0;
        Unlock();
        if (event) SetEvent(event);
    }

    void PublishHookState(const TrayHookState& state) {
        if (!Open() || !records) return;
        if (!Lock(kTrayPublisherLockWaitMs)) return;
        if (header->read > header->written) {
            InterlockedExchange64(&header->read, header->written);
        }
        if (header->written - header->read >= kTrayRingCapacity) {
            InterlockedIncrement64(&header->droppedFull);
            Unlock();
            return;
        }
        const std::uint64_t index = static_cast<std::uint64_t>(header->written);
        TrayRingRecord& slot = records[index % kTrayRingCapacity];
        InterlockedExchange64(&header->pendingIndex, static_cast<LONG64>(index));
        header->writeInFlight = 1;
        MemoryBarrier();
        slot = {};
        slot.kind = TrayRingKind::HookState;
        slot.publishedTick = GetTickCount64();
        slot.hook = state;
        MemoryBarrier();
        InterlockedIncrement64(&header->written);
        header->writeInFlight = 0;
        Unlock();
        if (event) SetEvent(event);
    }
};

struct TrayRingReader : detail::TrayRingEndpoint {
    bool Open(const wchar_t* name = kTrayRingName,
              const wchar_t* eventName = kTrayRingEventName) {
        AcquireSRWLockExclusive(&openLock_);
        struct UnlockOpen {
            SRWLOCK* lock;
            ~UnlockOpen() { ReleaseSRWLockExclusive(lock); }
        } unlockOpen{&openLock_};
        if (header) return EnsureConsumer();
        if (refused_) return false;
        if (!OpenEndpoint(name, eventName)) return false;
        return EnsureConsumer();
    }

    unsigned Drain(std::uint64_t& cursor, TrayRingRecord* out, unsigned max) {
        if (!Open() || !records || !out || max == 0) return 0;
        if (!Lock(kTrayReaderLockWaitMs)) return 0;
        if (header->consumerPid != GetCurrentProcessId() ||
            header->read > header->written) {
            Unlock();
            return 0;
        }
        const std::uint64_t read = static_cast<std::uint64_t>(header->read);
        const std::uint64_t written = static_cast<std::uint64_t>(header->written);
        if (cursor < read) {
            cursor = read;
        } else if (cursor > written) {
            cursor = written;
        }
        std::uint64_t available = written - cursor;
        if (available > kTrayRingCapacity) {
            cursor = written - kTrayRingCapacity;
            available = kTrayRingCapacity;
        }
        const unsigned count =
            static_cast<unsigned>(available < max ? available : static_cast<std::uint64_t>(max));
        for (unsigned i = 0; i < count; ++i) {
            const unsigned globalIndex =
                static_cast<unsigned>((cursor + i) % kTrayRingCapacity);
            out[i] = records[globalIndex];
        }
        cursor += count;
        InterlockedExchange64(&header->read, static_cast<LONG64>(cursor));
        Unlock();
        return count;
    }

    std::uint64_t Published() {
        if (!Open()) return 0;
        if (!Lock(kTrayReaderLockWaitMs)) {
            return static_cast<std::uint64_t>(
                InterlockedCompareExchange64(&header->written, 0, 0));
        }
        const std::uint64_t value = header->written;
        Unlock();
        return value;
    }

    std::uint64_t DroppedFull() {
        if (!Open()) return 0;
        if (!Lock(kTrayReaderLockWaitMs)) {
            return static_cast<std::uint64_t>(
                InterlockedCompareExchange64(&header->droppedFull, 0, 0));
        }
        const std::uint64_t value = header->droppedFull;
        Unlock();
        return value;
    }

    std::uint64_t DroppedLock() {
        if (!header) return 0;
        return static_cast<std::uint64_t>(
            InterlockedCompareExchange64(&header->droppedLock, 0, 0));
    }
};

} // namespace island
