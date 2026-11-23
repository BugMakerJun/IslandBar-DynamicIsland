// =====================================================================
//  SoundOutput.cpp
// =====================================================================
#include "media/SoundOutput.h"

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shellapi.h>

#include <cwchar>
#include <cstring>

#include "core/Log.h"
#include "media/MusicSession.h"   // FetchAppIconForTest: the same icon path the media card uses
#include "platform/ComApartment.h"
#include "platform/ScopeExit.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")

namespace island {
namespace {

// How long between readings while the page is open. The mixer's rows come and go as applications start
// and stop playing, so the list has to be refreshed -- but not per frame, and not often enough to be
// doing work nobody sees.
// 250 ms, down from 1000 and then 500. A notification sound lasts about a second, and this list is what
// notices it -- measured on this machine, WeChat holds NO audio session while it is quiet, so the sound creates
// one and the whole signal is the appearance. At 500 ms a short sound could start and finish between two reads.
// The query is one enumerator walk on a worker thread.
constexpr DWORD kSoundPollMs = 250;

// The render endpoint's form factors, for choosing a glyph. EndpointFormFactor is an enum in mmdeviceapi.h
// and its values are stable; the ones that matter are headphones (a headset), the digital and HDMI outputs
// (audio that comes out of a screen) and everything else (a speaker).
bool IsHeadphoneFormFactor(int formFactor) {
    // Headphones = 3, Headset = 5.
    return formFactor == 3 || formFactor == 5;
}

bool IsDisplayFormFactor(int formFactor) {
    // SPDIF = 4, DigitalAudio = 6, HDMI = 8, DisplayPort = 10.
    return formFactor == 4 || formFactor == 6 || formFactor == 8 || formFactor == 10;
}

// ---------------------------------------------------------------------
//  IPolicyConfig -- the one undocumented interface this file uses.
//
//  There is no supported API for "make this endpoint the default". The shell has one, the audio control
//  panel has one, and they go through this COM object; several well-known audio switchers do the same.
//
//  The interface is declared BY HAND because it is in no SDK header. Its vtable order is what matters and
//  it is the documented-by-observation one: twelve methods, of which SetDefaultEndpoint is the eleventh.
//  A wrong ORDER here would not fail to compile -- it would call something else entirely -- which is why
//  the declaration is kept in one piece, next to this note, and why the result of the call is logged.
//
//  Nothing depends on it being there: when CoCreateInstance or the QueryInterface fails the page still
//  lists the devices and says the switch did not happen.
// ---------------------------------------------------------------------

const CLSID kPolicyConfigClientClass = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

// PKEY_AudioEndpoint_FormFactor, {1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E}, 0.
//
// Declared here rather than taken from the SDK: the SDK header declares it as an external symbol that is
// resolved from a library this program does not link, and the link error names the key rather than the
// reason. The value itself is fixed and has been since Vista.
const PROPERTYKEY kEndpointFormFactor = {
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};

// Windows 10 and 11 answer for this one.
const IID kPolicyConfig10 = {
    0x294935ce, 0xf637, 0x4e7c, {0xa4, 0x1b, 0xab, 0x25, 0x54, 0x60, 0xb8, 0x62}};

struct DeviceShareMode {
    DWORD mode;
    LONG  role;
};

MIDL_INTERFACE("294935CE-F637-4E7C-A41B-AB255460B862")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

// Copies a wide string into a fixed buffer, always terminated.
void CopyTo(wchar_t* out, size_t capacity, const wchar_t* text) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (!text) return;
    std::wcsncpy(out, text, capacity - 1);
    out[capacity - 1] = L'\0';
}

// The short name a device's own friendly name starts with: "扬声器 (Realtek(R) Audio)" -> "扬声器". The
// mixer's first row shows the device, and the shell prints the short name there.
void ShortNameOf(const wchar_t* friendly, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (!friendly) return;

    size_t length = 0;
    for (; friendly[length] != L'\0' && friendly[length] != L'('; ++length) {
    }
    // Trailing spaces before the bracket are not part of the name.
    while (length > 0 && friendly[length - 1] == L' ') --length;
    if (length == 0) {
        // No bracket at all: the whole thing is the name.
        CopyTo(out, capacity, friendly);
        return;
    }
    if (length >= capacity) length = capacity - 1;
    std::wcsncpy(out, friendly, length);
    out[length] = L'\0';
}

// The file name out of a full path, for a session that has no display name of its own.
void FileNameOf(const wchar_t* path, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (!path) return;
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    CopyTo(out, capacity, slash ? slash + 1 : path);
}

}  // namespace

SoundOutput::~SoundOutput() {
    Stop();
}

bool SoundOutput::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    workEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    // Stop() ends whatever exists rather than unwinding by hand (see platform/WorkerThread.h).
    if (!workEvent_) {
        Stop();
        return false;
    }

    if (!worker_.Start(&SoundOutput::ThreadEntry, weak_from_this().lock())) {
        Stop();
        return false;
    }

    // Wait for the first reading, so the page has something to draw the first time it is opened rather
    // than filling in a second later.
    const auto haveReading = [this]() {
        CriticalSection::Guard guard(lock_);
        return haveReading_;
    };
    for (int i = 0; i < 800 && !haveReading(); ++i) {
        Sleep(5);
    }
    const bool firstReadArrived = haveReading();
    running_.store(firstReadArrived, std::memory_order_release);
    // NOT stopped when the first reading never came: the worker keeps asking, and the page can still fill
    // in later. What it must not do is leak the attempt -- Prepare() ends it before the next Start().
    return firstReadArrived;
}

void SoundOutput::Stop() {
    const WorkerThread::StopResult stopped = worker_.Stop();
    running_.store(false, std::memory_order_release);
    if (stopped != WorkerThread::StopResult::Stopped) return;
    if (workEvent_) {
        CloseHandle(workEvent_);
        workEvent_ = nullptr;
    }
}

void SoundOutput::Snapshot(SoundOutputSnapshot* out) const {
    if (!out) return;
    if (!worker_.Prepared()) {
        *out = SoundOutputSnapshot{};
        return;
    }
    lock_.Enter();
    *out = snapshot_;
    lock_.Leave();
}

unsigned long long SoundOutput::Revision() const {
    return static_cast<unsigned long long>(revision_);
}

void SoundOutput::Refresh() {
    if (!worker_.Prepared()) return;
    lock_.Enter();
    refreshRequested_ = true;
    lock_.Leave();
    if (workEvent_) SetEvent(workEvent_);
}

void SoundOutput::SetDefaultDevice(const wchar_t* deviceId) {
    if (!worker_.Prepared() || !deviceId) return;
    lock_.Enter();
    CopyTo(pendingDevice_, 256, deviceId);
    pendingDefault_ = true;
    lock_.Leave();
    if (workEvent_) SetEvent(workEvent_);
}

void SoundOutput::SetSessionVolume(unsigned long pid, float level) {
    if (!worker_.Prepared()) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    lock_.Enter();
    pendingPid_ = pid;
    pendingLevel_ = level;
    pendingVolume_ = true;
    lock_.Leave();
    if (workEvent_) SetEvent(workEvent_);
}

unsigned long __stdcall SoundOutput::ThreadEntry(void* self) {
    static_cast<SoundOutput*>(self)->Run();
    return 0;
}

void SoundOutput::Run() {
    const WorkerThread::Signal workerSignal = WorkerThread::CurrentSignal();
    const HANDLE stopEvent = workerSignal.StopEvent();
    auto runExit = MakeScopeExit([this]() noexcept {
        running_.store(false, std::memory_order_release);
    });
    // This thread owns its own apartment: every interface created here is used here and released here, so
    // none of them crosses into the frame loop's apartment.
    ComApartment apartment(COINIT_MULTITHREADED);
    const bool haveApartment = apartment.Usable();

    if (haveApartment) ReadAll();

    while (true) {
        HANDLE waits[2] = {stopEvent, workEvent_};
        const DWORD signalled = WaitForMultipleObjects(2, waits, FALSE, kSoundPollMs);
        if (signalled == WAIT_OBJECT_0) break;   // stop
        if (!haveApartment) break;

        bool wantRefresh = (signalled == WAIT_TIMEOUT);
        if (signalled == WAIT_OBJECT_0 + 1) {
            lock_.Enter();
            wantRefresh = wantRefresh || refreshRequested_;
            refreshRequested_ = false;
            lock_.Leave();
        }

        if (wantRefresh) ReadAll();
        ApplyWrites();
    }

}

void SoundOutput::ReadAll() {
    // Both halves are read into a local snapshot and published together.
    SoundOutputSnapshot fresh;
    const bool devices = ReadDevices(fresh);
    const bool sessions = ReadSessions(fresh);

    lock_.Enter();
    fresh.revision = static_cast<unsigned long long>(InterlockedIncrement(&revision_));
    snapshot_ = fresh;
    haveReading_ = true;
    lock_.Leave();

    // Reported ONCE PER CHANGE, because the page's own contents are the one thing about it that cannot be walked
    // backwards from the screen: "the mixer is empty" and "the mixer could not be read" look the same in a
    // screenshot, and the two need opposite fixes.
    //
    // It used to be once per READING, which is four times a second, so the same sentence arrived four times a
    // second for as long as the bar ran. A log nobody can read is worth no more than no log (the same argument
    // the tray's own reporting settled on), and what these lines are FOR -- noticing that the mixer went empty,
    // or that a device list stopped answering -- is a change, so a change is what is reported.
    const bool changed = fresh.deviceCount != loggedDeviceCount_ ||
                         fresh.sessionCount != loggedSessionCount_ ||
                         (devices ? 1 : 0) != loggedDevicesReadable_ ||
                         (sessions ? 1 : 0) != loggedSessionsReadable_ ||
                         wcscmp(fresh.defaultDeviceName, loggedDefaultName_) != 0;
    if ((devices || sessions) && changed) {
        LogLine(L"  sound page: %d device(s), default '%ls', %d session(s) [devices=%d sessions=%d]",
                fresh.deviceCount, fresh.defaultDeviceName, fresh.sessionCount, devices ? 1 : 0,
                sessions ? 1 : 0);
        loggedDeviceCount_ = fresh.deviceCount;
        loggedSessionCount_ = fresh.sessionCount;
        loggedDevicesReadable_ = devices ? 1 : 0;
        loggedSessionsReadable_ = sessions ? 1 : 0;
        wcsncpy_s(loggedDefaultName_, fresh.defaultDeviceName, _TRUNCATE);
    }
}

bool SoundOutput::ReadDevices(SoundOutputSnapshot& out) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return false;

    // Which one is in use, by ID: IMMDevice pointers are not comparable, and the id is what would be
    // passed back to switch between them.
    wchar_t defaultId[256] = {};
    IMMDevice* defaultDevice = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice)) &&
        defaultDevice) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
            CopyTo(defaultId, 256, id);
            CoTaskMemFree(id);
        }
        defaultDevice->Release();
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (FAILED(hr) || !collection) return false;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count && out.deviceCount < kMaxSoundDevices; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device) continue;

        SoundDeviceInfo info;

        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            CopyTo(info.id, 256, id);
            info.isDefault = (defaultId[0] != L'\0' && std::wcscmp(id, defaultId) == 0);
            CoTaskMemFree(id);
        }

        IPropertyStore* properties = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) && properties) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR) {
                CopyTo(info.name, 128, value.pwszVal);
            }
            PropVariantClear(&value);

            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(kEndpointFormFactor, &value)) &&
                value.vt == VT_UI4) {
                info.formFactor = static_cast<int>(value.ulVal);
            }
            PropVariantClear(&value);
            properties->Release();
        }

        // A device whose name could not be read is still a device: it is listed by its id's tail rather
        // than dropped, because a missing row is worse than an ugly one when the user is looking for it.
        if (info.name[0] == L'\0') CopyTo(info.name, 128, L"Output device");

        out.devices[out.deviceCount] = info;
        if (info.isDefault) {
            out.defaultDevice = out.deviceCount;
            ShortNameOf(info.name, out.defaultDeviceName, 128);
        }
        ++out.deviceCount;
        device->Release();
    }
    collection->Release();

    if (out.defaultDevice < 0 && out.deviceCount > 0) {
        // No default endpoint reported (it happens while the audio service is starting): the first row is
        // the best answer, and it is marked so the page still shows which one is in use.
        out.defaultDevice = 0;
        out.devices[0].isDefault = true;
        ShortNameOf(out.devices[0].name, out.defaultDeviceName, 128);
    }
    return out.deviceCount > 0;
}

bool SoundOutput::ReadSessions(SoundOutputSnapshot& out) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return false;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) return false;

    IAudioSessionManager2* manager = nullptr;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&manager));
    device->Release();
    if (FAILED(hr) || !manager) return false;

    IAudioSessionEnumerator* sessions = nullptr;
    hr = manager->GetSessionEnumerator(&sessions);
    manager->Release();
    if (FAILED(hr) || !sessions) return false;

    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count && out.sessionCount < kMaxSoundSessions; ++i) {
        IAudioSessionControl* control = nullptr;
        if (FAILED(sessions->GetSession(i, &control)) || !control) continue;

        // The applications that are NOT playing are not in the shell's mixer either: a session exists from
        // the moment an application opens audio until it closes it, and a row per sleeping application is
        // noise. Expired sessions go too.
        AudioSessionState state = AudioSessionStateExpired;
        control->GetState(&state);
        if (state == AudioSessionStateExpired) {
            control->Release();
            continue;
        }

        ISimpleAudioVolume* volume = nullptr;
        control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                reinterpret_cast<void**>(&volume));

        SoundSessionInfo info;
        // ACTIVE means sound is flowing right now; Inactive means the application still holds a stream open
        // while silent. See the field's own comment for why that difference is the whole point.
        info.active = (state == AudioSessionStateActive);
        if (volume) {
            float level = 1.0f;
            if (SUCCEEDED(volume->GetMasterVolume(&level))) info.volume = level;
        }

        // The process, and through it the name and the icon. The system-sounds session has pid 0, which
        // is not a process at all -- the shell lists it too, and calls it the system.
        DWORD pid = 0;
        IAudioSessionControl2* control2 = nullptr;
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                              reinterpret_cast<void**>(&control2))) &&
            control2) {
            control2->GetProcessId(&pid);
            control2->Release();
        }
        info.pid = pid;
        info.system = (pid == 0);

        if (pid == 0) {
            CopyTo(info.name, 128, L"System sounds");
        } else {
            // The executable is what the icon comes from and what the log names the row by. A process that
            // has already gone, or one this process may not open, leaves the row unnamed but still listed:
            // its slider works either way, because the session does not need its process to be readable.
            wchar_t executable[260] = {};
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process) {
                DWORD length = 260;
                if (!QueryFullProcessImageNameW(process, 0, executable, &length)) {
                    executable[0] = L'\0';
                }
                CloseHandle(process);
            }
            FileNameOf(executable, info.name, 128);
            if (info.name[0] == L'\0') CopyTo(info.name, 128, L"Application");

            size_t written = 0;
            unsigned long long iconRevision = 0;
            if (IconForPid(pid, executable, info.icon, &written, &iconRevision)) {
                info.iconSize = written;
                info.hasIcon = true;
                info.iconRevision = iconRevision;
                CopyTo(info.source, 260, executable);
            }
        }

        out.sessions[out.sessionCount] = info;
        ++out.sessionCount;
        if (volume) volume->Release();
        control->Release();
    }
    sessions->Release();

    // The shell lists the session that makes the most noise first and the system's own last; the mixer's
    // first row here is the DEVICE (drawn by the caller from the master volume), so these follow it in the
    // order the audio service reported them, with the system sounds at the end where the shell puts them.
    for (int i = 0; i < out.sessionCount; ++i) {
        if (!out.sessions[i].system) continue;
        const SoundSessionInfo system = out.sessions[i];
        for (int j = i; j + 1 < out.sessionCount; ++j) out.sessions[j] = out.sessions[j + 1];
        out.sessions[out.sessionCount - 1] = system;
        break;
    }
    return true;
}

void SoundOutput::ApplyWrites() {
    wchar_t device[256] = {};
    bool doDefault = false;
    bool doVolume = false;
    unsigned long pid = 0;
    float level = 0.0f;

    lock_.Enter();
    if (pendingDefault_) {
        CopyTo(device, 256, pendingDevice_);
        pendingDefault_ = false;
        doDefault = true;
    }
    if (pendingVolume_) {
        pid = pendingPid_;
        level = pendingLevel_;
        pendingVolume_ = false;
        doVolume = true;
    }
    lock_.Leave();

    if (doDefault) {
        long result = 0;
        const bool applied = ApplyDefaultDevice(device, &result);
        LogLine(L"  sound page: switch default device %s (hr 0x%08lX)", applied ? L"ok" : L"FAILED",
                static_cast<unsigned long>(result));
    }

    if (doVolume) {
        IMMDeviceEnumerator* enumerator = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&enumerator))) &&
            enumerator) {
            IMMDevice* device2 = nullptr;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device2)) &&
                device2) {
                IAudioSessionManager2* manager = nullptr;
                if (SUCCEEDED(device2->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                                nullptr, reinterpret_cast<void**>(&manager))) &&
                    manager) {
                    IAudioSessionEnumerator* sessions = nullptr;
                    if (SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && sessions) {
                        int count = 0;
                        sessions->GetCount(&count);
                        for (int i = 0; i < count; ++i) {
                            IAudioSessionControl* control = nullptr;
                            if (FAILED(sessions->GetSession(i, &control)) || !control) continue;
                            DWORD sessionPid = 1;
                            IAudioSessionControl2* control2 = nullptr;
                            if (SUCCEEDED(control->QueryInterface(
                                    __uuidof(IAudioSessionControl2),
                                    reinterpret_cast<void**>(&control2))) &&
                                control2) {
                                control2->GetProcessId(&sessionPid);
                                control2->Release();
                            }
                            if (sessionPid == pid) {
                                ISimpleAudioVolume* sessionVolume = nullptr;
                                if (SUCCEEDED(control->QueryInterface(
                                        __uuidof(ISimpleAudioVolume),
                                        reinterpret_cast<void**>(&sessionVolume))) &&
                                    sessionVolume) {
                                    const HRESULT set = sessionVolume->SetMasterVolume(level, nullptr);
                                    LogLine(L"  sound page: session %lu -> %.0f%% (hr 0x%08lX)", pid,
                                            level * 100.0f, static_cast<unsigned long>(set));
                                    sessionVolume->Release();
                                }
                                control->Release();
                                break;
                            }
                            control->Release();
                        }
                        sessions->Release();
                    }
                    manager->Release();
                }
                device2->Release();
            }
            enumerator->Release();
        }
    }
}

bool SoundOutput::ApplyDefaultDevice(const wchar_t* deviceId, long* result) {
    if (result) *result = E_FAIL;
    if (!deviceId || deviceId[0] == L'\0') return false;

    IPolicyConfig* policy = nullptr;
    const HRESULT created =
        CoCreateInstance(kPolicyConfigClientClass, nullptr, CLSCTX_ALL, kPolicyConfig10,
                         reinterpret_cast<void**>(&policy));
    if (FAILED(created) || !policy) {
        if (result) *result = static_cast<long>(created);
        return false;
    }

    // All three roles, which is what "make it the default" means: the console, the multimedia stream and
    // the one communications sessions use. Setting only eConsole leaves a headset still answering calls.
    const ERole roles[3] = {eConsole, eMultimedia, eCommunications};
    HRESULT last = S_OK;
    for (int i = 0; i < 3; ++i) {
        last = policy->SetDefaultEndpoint(deviceId, roles[i]);
        if (FAILED(last)) break;
    }
    policy->Release();

    if (result) *result = static_cast<long>(last);
    return SUCCEEDED(last);
}

bool SoundOutput::IconForPid(unsigned long pid, const wchar_t* executable, unsigned char* out,
                             size_t* written, unsigned long long* revision) {
    if (!out || !written || !revision || pid == 0) return false;

    for (int i = 0; i < iconCount_; ++i) {
        if (icons_[i].pid != pid) continue;
        // Already loaded: hand back the same bytes, and the same revision -- the renderer keeps the bitmap
        // it uploaded and does not decode this again.
        std::memcpy(out, icons_[i].bytes, icons_[i].size);
        *written = icons_[i].size;
        *revision = icons_[i].revision;
        return icons_[i].size > 0;
    }

    if (!executable || executable[0] == L'\0') return false;

    IconEntry& entry = icons_[iconCount_];
    entry.pid = pid;
    entry.revision = nextIconRevision_++;
    entry.size = 0;
    const size_t writtenNow =
        FetchAppIconForTest(executable, entry.bytes, kSoundIconBytes);
    if (writtenNow == 0) {
        // Nothing to cache for this one, and no room to remember that: the entry is left in place with a
        // zero size, which costs one slot and stops the disk being read again for the same process.
        entry.size = 0;
    } else {
        entry.size = writtenNow;
    }
    if (iconCount_ + 1 < kSoundIconCacheEntries) ++iconCount_;

    if (entry.size == 0) return false;
    std::memcpy(out, entry.bytes, entry.size);
    *written = entry.size;
    *revision = entry.revision;
    return true;
}

}  // namespace island
