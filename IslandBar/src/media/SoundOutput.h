// =====================================================================
//  SoundOutput.h
//  What the "声音输出" page needs: the output devices, and the per-application
//  volumes.
//
//  Two of the page's three sections come from here:
//
//   * 输出设备 -- every ACTIVE render endpoint, with its friendly name and whether it is the one the
//     system is playing through. Enumerated from IMMDeviceEnumerator, the same source the shell's own
//     flyout reads.
//
//   * 音量合成器 -- one row per application with an audio session on the default device, each with its
//     own volume (IAudioSessionManager2 -> ISimpleAudioVolume). This is the section that can be READ AND
//     WRITTEN with documented interfaces, and it is the reason this class exists.
//
//  ... and one thing about it does not: switching the DEFAULT device has no documented interface. The
//  shell does it through IPolicyConfig, which is neither documented nor in any SDK header; it is declared
//  by hand in the .cpp, used if the object answers for it, and reported honestly when it does not. A page
//  that listed the devices and pretended to switch between them would be worse than one that says so.
//
//  THE WORKER. Enumeration touches the audio service and the disk (an application's icon comes out of its
//  executable), and either can block for tens of milliseconds while the audio stack wakes up. That is
//  exactly what the stutter reports were about, so the reading happens on a worker thread and the frame
//  loop only ever copies a finished snapshot -- the same arrangement AudioVolume already uses for the
//  master volume, and for the same reason.
// =====================================================================
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <memory>

#include "domain/CardLayout.h"   // kMaxSoundDevices, kMaxSoundSessions
#include "platform/CriticalSection.h"
#include "platform/WorkerThread.h"

namespace island {

// How much room one application's icon gets. A 32x32 PNG is a couple of kilobytes at the very most, and
// the encoder in WicImage stops at the capacity it is handed rather than overrunning it.
inline constexpr size_t kSoundIconBytes = 4096;
inline constexpr int kSoundIconCacheEntries = 12;

// One output device, as the list needs it.
struct SoundDeviceInfo {
    wchar_t name[128] = {};    // "扬声器 (Realtek(R) Audio)"
    wchar_t id[256] = {};      // the endpoint id, which is what a switch is asked for by
    int formFactor = 0;        // EndpointFormFactor: speakers, headphones, a screen, ...
    bool isDefault = false;
};

// One application in the mixer.
struct SoundSessionInfo {
    wchar_t name[128] = {};         // the session's own name when it has one, else the executable's
    wchar_t source[260] = {};       // where the icon came from, for the log
    unsigned long pid = 0;
    float volume = 1.0f;
    // Whether this session is making sound RIGHT NOW.
    //
    // `AudioSessionStateActive` means audio is flowing; `Inactive` means the application still holds a stream
    // open but is silent. The difference is the whole reason this field exists: an application that keeps its
    // audio engine open all day (WeChat does) has a session from the moment it starts, so "the session exists"
    // says nothing -- "the session became ACTIVE" is a sound being played, which is what a notification is when
    // the application tells the system nothing else.
    bool active = false;
    bool system = false;            // the system-sounds session, which has no process
    bool hasIcon = false;
    // Bumped when this row's icon is (re)loaded, so the renderer knows when its uploaded copy is stale.
    unsigned long long iconRevision = 0;
    unsigned char icon[kSoundIconBytes] = {};
    size_t iconSize = 0;
};

struct SoundOutputSnapshot {
    SoundDeviceInfo devices[kMaxSoundDevices];
    int deviceCount = 0;
    int defaultDevice = -1;         // index into `devices`, or -1
    wchar_t defaultDeviceName[128] = {};   // the mixer's first row is this device
    SoundSessionInfo sessions[kMaxSoundSessions];
    int sessionCount = 0;
    unsigned long long revision = 0;
};

class SoundOutput : public std::enable_shared_from_this<SoundOutput> {
public:
    SoundOutput() = default;
    ~SoundOutput();

    SoundOutput(const SoundOutput&) = delete;
    SoundOutput& operator=(const SoundOutput&) = delete;

    // Starts the worker and waits for the first reading. Returns false when the audio stack cannot be
    // reached at all, in which case the page draws what it can and says so rather than guessing.
    bool Start();
    void Stop();

    // Copies the last finished reading. Cheap enough for a frame: a handful of strings and the icons.
    void Snapshot(SoundOutputSnapshot* out) const;

    // Bumped whenever a reading completes, so the frame loop can redraw on change and only then.
    unsigned long long Revision() const;

    // Asks for a fresh reading. The worker answers at once rather than waiting out its poll interval.
    void Refresh();

    // Writes, handed to the worker because the interfaces belong to its apartment.
    //
    // Switching the default device is the ONE undocumented thing here -- see the header's own note. The
    // worker logs the interface's own answer, which is how a silent failure can be told from a success.
    void SetDefaultDevice(const wchar_t* deviceId);
    void SetSessionVolume(unsigned long pid, float level);

private:
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();
    void ReadAll();
    // Each fills its half of a snapshot the caller owns; both run on the worker and publish through
    // ReadAll, so the page can never show a device list from one reading beside a mixer from another.
    bool ReadDevices(SoundOutputSnapshot& out);
    bool ReadSessions(SoundOutputSnapshot& out);
    void ApplyWrites();
    bool ApplyDefaultDevice(const wchar_t* deviceId, long* result);

    // Loads an application's icon once and keeps it. Keyed by process id, because that is what the mixer's
    // rows are keyed by -- an id that keeps its icon across the application's own restarts is not needed.
    bool IconForPid(unsigned long pid, const wchar_t* executable, unsigned char* out, size_t* written,
                    unsigned long long* revision);

    // The stop event, the thread and the rule for ending both. See platform/WorkerThread.h.
    WorkerThread worker_;
    HANDLE workEvent_ = nullptr;    // auto-reset: a write or a refresh request
    std::atomic<bool> running_{false};

    mutable CriticalSection lock_;

    SoundOutputSnapshot snapshot_{};
    volatile long revision_ = 0;

    // What the last REPORTED reading said, so the line below is written when the page's contents CHANGE rather
    // than once per reading. The reading runs four times a second, so "once per reading" was four lines a
    // second of the same sentence -- and a log nobody reads is the same as no log at all, which is the opposite
    // of why these lines exist (see ReadAll).
    int loggedDeviceCount_ = -1;
    int loggedSessionCount_ = -1;
    int loggedDevicesReadable_ = -1;
    int loggedSessionsReadable_ = -1;
    wchar_t loggedDefaultName_[128] = {};
    bool haveReading_ = false;

    // Pending writes, set by the frame loop and taken by the worker.
    wchar_t pendingDevice_[256] = {};
    bool pendingDefault_ = false;
    unsigned long pendingPid_ = 0;
    float pendingLevel_ = 0.0f;
    bool pendingVolume_ = false;
    bool refreshRequested_ = false;

    // The icon cache, owned by the worker thread.
    struct IconEntry {
        unsigned long pid = 0;
        unsigned long long revision = 0;
        size_t size = 0;
        unsigned char bytes[kSoundIconBytes] = {};
    };
    IconEntry icons_[kSoundIconCacheEntries] = {};
    int iconCount_ = 0;
    unsigned long long nextIconRevision_ = 1;
};

} // namespace island
