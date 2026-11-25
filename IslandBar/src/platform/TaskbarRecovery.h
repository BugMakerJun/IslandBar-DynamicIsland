// =====================================================================
//  TaskbarRecovery.h -- A WATCHER FOR THE ONE PIECE OF DESKTOP STATE WE CHANGE
//
//  Hiding the shell's taskbar is unlike drawing the island: Explorer owns the
//  state, and a hard-killed bar never reaches NativeTaskbar::Restore(). The
//  recovery watcher therefore lives in the already pinned tray hook inside
//  Explorer. The bar arms it before it may hide anything and records exactly
//  what it changed. If the bar disappears without a clean restore, the watcher
//  restores the taskbar and the work area.
//
//  WHY THIS IS A SHARED-NOTHING CHANNEL. The watcher and the bar run in
//  different processes and either can die at any instruction. The control block
//  is therefore a small named mapping protected by a named mutex; the recovery
//  snapshot is a separate file published with a write-through replace. There is
//  no lock held while taskbar windows are touched, and an abandoned mutex is
//  recoverable.
//
//  THIS IS ASCII-ONLY because TrayHook.cpp compiles in Explorer as well.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace island {

inline constexpr wchar_t kTaskbarGuardianMappingName[] =
    L"Local\\IslandBar.TaskbarGuardian.v1";
inline constexpr wchar_t kTaskbarGuardianMutexName[] =
    L"Local\\IslandBar.TaskbarGuardian.Mutex.v1";
inline constexpr wchar_t kTaskbarGuardianOwnerEventName[] =
    L"Local\\IslandBar.TaskbarGuardian.Owner.v1";
inline constexpr wchar_t kTaskbarGuardianReadyEventName[] =
    L"Local\\IslandBar.TaskbarGuardian.Ready.v1";
inline constexpr wchar_t kTaskbarGuardianThreadMutexName[] =
    L"Local\\IslandBar.TaskbarGuardian.Thread.v1";

inline constexpr unsigned kTaskbarRecoveryVersion = 1;
inline constexpr std::size_t kTaskbarRecoverySecondaryCapacity = 16;
inline constexpr std::size_t kTaskbarRecoveryLogCapacity = 512;

enum class TaskbarRecoveryMode : unsigned {
    Hide = 1,
    ClipToTray = 2,
};

struct TaskbarRecoverySecondary {
    std::uintptr_t window = 0;
    bool wasVisible = false;
};

struct TaskbarRecoverySnapshot {
    unsigned ownerPid = 0;
    std::uint64_t ownerCreationTime = 0;
    std::uint64_t guardianGeneration = 0;
    DWORD ownerSession = 0;
    DWORD shellPid = 0;
    TaskbarRecoveryMode mode = TaskbarRecoveryMode::Hide;
    bool primaryWasVisible = false;
    bool workAreaValid = false;
    RECT workArea = {};
    RECT monitor = {};
    unsigned secondaryCount = 0;
    TaskbarRecoverySecondary secondary[kTaskbarRecoverySecondaryCapacity] = {};
    wchar_t logPath[kTaskbarRecoveryLogCapacity] = {};
};

// Process identity is not PID alone. Windows reuses PIDs, and a reused PID must
// never make an old recovery record look live.
bool ProcessCreationTimeFor(DWORD pid, std::uint64_t* creationTime);
bool CurrentProcessSessionId(DWORD* sessionId);

// The bar publishes what it is about to change before touching Explorer. The
// file is replaced atomically and contains a checksum.
bool WriteTaskbarRecoverySnapshot(const TaskbarRecoverySnapshot& snapshot);
bool DeleteTaskbarRecoverySnapshot();

// Used at startup and by the watcher before creating a new owner. A stale
// record is one whose owner PID/creation time is no longer alive in this
// session. Recovery is idempotent and never resurrects a taskbar the user had
// already hidden before IslandBar started.
bool RecoverStaleTaskbarIfAny();

// Bar side. Arm is deliberately separate from Announce: the bar creates the
// channel before the module is offered to Explorer, then announces again after
// a new Explorer loads the module.
bool ArmTaskbarGuardian(std::uint64_t* generationOut);
bool AnnounceTaskbarGuardian();
bool WaitForTaskbarGuardian(std::uint64_t generation, DWORD timeoutMs);
bool WaitForCurrentTaskbarGuardian(DWORD timeoutMs);
bool TaskbarGuardianHealthy();
void MarkTaskbarGuardianClean();

// Explorer side. Called from TrayHook.cpp after the module has pinned itself;
// idempotent, and the thread exits only with Explorer.
bool EnsureTaskbarGuardianThread();

} // namespace island
