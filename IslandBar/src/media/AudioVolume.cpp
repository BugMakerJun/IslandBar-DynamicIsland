// =====================================================================
//  AudioVolume.cpp
// =====================================================================
#include "media/AudioVolume.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cmath>
#include <new>

#include "core/Log.h"
#include "platform/ComApartment.h"
#include "platform/ScopeExit.h"

#pragma comment(lib, "ole32.lib")

namespace island {
namespace {

// How long to wait between fallback re-reads of the device volume.
//
// The endpoint callback below is the fast path. This five-second read is only
// the reconciliation that catches a missed notification or a default-device
// swap.
constexpr DWORD kPollIntervalMs = 5000;

// How often the PEAK is read while the worker is awake. The meter the call strip draws has to move with a
// voice, and 500 ms would not: 20 Hz is a meter, 2 Hz is a slideshow.
constexpr DWORD kPeakIntervalMs = 50;

// A quiet step, matching one notch of the shell's own volume slider.
constexpr float kMinStep = 0.02f;

float Clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

class EndpointVolumeCallback final : public IAudioEndpointVolumeCallback {
public:
    explicit EndpointVolumeCallback(HANDLE changed) : changed_(changed) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
            *object = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        if (changed_) SetEvent(changed_);
        return S_OK;
    }

private:
    ~EndpointVolumeCallback() = default;
    volatile LONG references_ = 1;
    HANDLE changed_ = nullptr;
};

} // namespace

AudioVolume::~AudioVolume() {
    Stop();
}

bool AudioVolume::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    writeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    peakEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);    // auto-reset
    volumeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    // Stop() rather than a bare return: it is the one place that ends whatever exists, whether that is
    // nothing, one event, or a thread that is already running. The copies this class used to have each
    // unwound a different part of this by hand (see platform/WorkerThread.h).
    if (!writeEvent_ || !peakEvent_ || !volumeEvent_) {
        Stop();
        return false;
    }

    if (!worker_.Start(&AudioVolume::ThreadEntry, weak_from_this().lock())) {
        Stop();
        return false;
    }
    LogLine(L"  volume: peak meter demand=off cadence=%lu ms", kPollIntervalMs);

    // Wait for the first read, so Start() returning true means the level is known and the
    // control can be drawn on the very first frame rather than popping in half a second later.
    const auto haveLevel = [this]() {
        CriticalSection::Guard guard(lock_);
        return haveLevel_;
    };
    for (int i = 0; i < 400 && !haveLevel(); ++i) {
        Sleep(5);
    }

    const bool firstReadArrived = haveLevel();
    running_.store(firstReadArrived, std::memory_order_release);
    if (!firstReadArrived) {
        Stop();
        return false;
    }
    return true;
}

void AudioVolume::Stop() {
    const WorkerThread::StopResult stopped = worker_.Stop();
    running_.store(false, std::memory_order_release);
    peakMeterActive_.store(false, std::memory_order_relaxed);
    if (stopped != WorkerThread::StopResult::Stopped) return;
    if (writeEvent_) {
        CloseHandle(writeEvent_);
        writeEvent_ = nullptr;
    }
    if (peakEvent_) {
        CloseHandle(peakEvent_);
        peakEvent_ = nullptr;
    }
    if (volumeEvent_) {
        CloseHandle(volumeEvent_);
        volumeEvent_ = nullptr;
    }
}

bool AudioVolume::Available() const {
    CriticalSection::Guard guard(lock_);
    return haveLevel_;
}

bool AudioVolume::Get(float* level) const {
    if (!level || !worker_.Prepared()) return false;
    CriticalSection::Guard guard(lock_);
    if (!haveLevel_) return false;
    *level = level_;
    return true;
}

bool AudioVolume::IsMuted() const {
    if (!worker_.Prepared()) return false;
    lock_.Enter();
    const bool muted = muted_;
    lock_.Leave();
    return muted;
}

bool AudioVolume::Peak(float* level) const {
    if (!level || !worker_.Prepared()) return false;
    CriticalSection::Guard guard(lock_);
    if (!havePeak_) return false;
    *level = peak_;
    return true;
}

void AudioVolume::SetPeakMeterActive(bool active) {
    if (!worker_.Prepared()) return;
    const bool previous = peakMeterActive_.exchange(active, std::memory_order_relaxed);
    if (previous == active) return;
    SetEvent(peakEvent_);
    LogLine(L"  volume: peak meter demand=%ls cadence=%lu ms",
            active ? L"on" : L"off", active ? kPeakIntervalMs : kPollIntervalMs);
}

unsigned long long AudioVolume::Revision() const {
    return static_cast<unsigned long long>(revision_);
}

void AudioVolume::Adjust(float delta) {
    float current = 0.0f;
    if (!Get(&current)) return;
    SetLevel(current + delta);
}

void AudioVolume::SetLevel(float level) {
    if (!worker_.Prepared()) return;

    const float wanted = Clamp01(level);

    // Publish locally first, so the bar moves on the next frame instead of waiting for the
    // worker to get round to it. The worker reconciles with the device afterwards.
    lock_.Enter();
    level_ = wanted;
    haveLevel_ = true;
    lock_.Leave();
    InterlockedIncrement(&revision_);

    // The device write needs the same interfaces as the poll, so it is handed to the worker
    // through a shared request rather than done here on the caller's thread.
    lock_.Enter();
    pendingLevel_ = wanted;
    pendingWrite_ = true;
    lock_.Leave();
    SetEvent(writeEvent_);
}

void AudioVolume::ToggleMute() {
    lock_.Enter();
    pendingToggleMute_ = true;
    lock_.Leave();
    SetEvent(writeEvent_);
}

// The meter, and the endpoint it belongs to, kept between ticks.
//
// Acquired once and re-acquired only when it fails: the call strip's bars are drawn from this reading, and
// re-opening the endpoint twenty times a second to move four pixels would be the most expensive thing in the
// frame loop. The interfaces belong to this worker's apartment, so they are created and released here.
bool AudioVolume::ReadPeakFromDevice() {
    if (!meter_ || !meterDevice_) {
        if (meter_) {
            static_cast<IAudioMeterInformation*>(meter_)->Release();
            meter_ = nullptr;
        }
        if (meterDevice_) {
            static_cast<IMMDevice*>(meterDevice_)->Release();
            meterDevice_ = nullptr;
        }

        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator))) ||
            !enumerator) {
            return false;
        }
        IMMDevice* device = nullptr;
        const HRESULT got = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(got) || !device) return false;

        IAudioMeterInformation* meter = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&meter))) ||
            !meter) {
            device->Release();
            return false;
        }
        meter_ = meter;
        meterDevice_ = device;
        LogLine(L"  volume: peak meter acquired");
    }

    float peak = 0.0f;
    if (FAILED(static_cast<IAudioMeterInformation*>(meter_)->GetPeakValue(&peak))) return false;
    if (peak < 0.0f) peak = 0.0f;
    if (peak > 1.0f) peak = 1.0f;

    lock_.Enter();
    peak_ = peak;
    havePeak_ = true;
    lock_.Leave();
    return true;
}

bool AudioVolume::ReadFromDevice(void* callback) {
    IAudioEndpointVolume* volume = static_cast<IAudioEndpointVolume*>(endpointVolume_);
    if (!volume) {
        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) return false;

        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr) || !device) return false;

        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&volume));
        device->Release();
        if (FAILED(hr) || !volume) return false;
        endpointVolume_ = volume;
        endpointCallback_ = callback;
        if (callback) {
            volume->RegisterControlChangeNotify(
                static_cast<IAudioEndpointVolumeCallback*>(callback));
        }
    }

    float level = 0.0f;
    BOOL muted = FALSE;
    const bool ok = SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level)) &&
                    SUCCEEDED(volume->GetMute(&muted));

    if (ok) {
        lock_.Enter();
        // Only accept the device's value when the user is not mid-adjustment: an in-flight
        // click would otherwise be overwritten by the level the device still reports, and the
        // bar would snap back.
        if (!pendingWrite_) {
            level_ = Clamp01(level);
            muted_ = muted != FALSE;
        }
        haveLevel_ = true;
        lock_.Leave();
    }

    return ok;
}

void AudioVolume::ReleaseEndpointVolume() {
    IAudioEndpointVolume* volume = static_cast<IAudioEndpointVolume*>(endpointVolume_);
    if (!volume) return;
    if (endpointCallback_) {
        volume->UnregisterControlChangeNotify(
            static_cast<IAudioEndpointVolumeCallback*>(endpointCallback_));
        endpointCallback_ = nullptr;
    }
    volume->Release();
    endpointVolume_ = nullptr;
}

unsigned long __stdcall AudioVolume::ThreadEntry(void* self) {
    static_cast<AudioVolume*>(self)->Run();
    return 0;
}

void AudioVolume::Run() {
    const WorkerThread::Signal workerSignal = WorkerThread::CurrentSignal();
    const HANDLE stopEvent = workerSignal.StopEvent();
    // This thread owns its own apartment. `IAudioEndpointVolume` is created here and used
    // here, so it never crosses into the frame loop's apartment.
    ComApartment apartment(COINIT_MULTITHREADED);
    const bool haveApartment = apartment.Usable();

    EndpointVolumeCallback* callback = nullptr;
    auto runExit = MakeScopeExit([this, &callback]() noexcept {
        ReleaseEndpointVolume();
        if (callback) callback->Release();
        running_.store(false, std::memory_order_release);
    });
    if (haveApartment && volumeEvent_) {
        callback = new (std::nothrow) EndpointVolumeCallback(volumeEvent_);
    }
    unsigned long long lastVolumeRead_ = GetTickCount64();
    if (haveApartment) ReadFromDevice(callback);

    while (true) {
        HANDLE waits[4] = {stopEvent, writeEvent_, peakEvent_, volumeEvent_};
        // A visible meter needs the fast interval; otherwise the worker sleeps until the next volume read.
        // The volume re-read (which opens the device) stays on its own slower clock in either mode.
        const bool peakDemand = peakMeterActive_.load(std::memory_order_relaxed);
        const DWORD signalled =
            WaitForMultipleObjects(4, waits, FALSE, peakDemand ? kPeakIntervalMs : kPollIntervalMs);
        if (signalled == WAIT_OBJECT_0) break;   // stop
        if (signalled == WAIT_FAILED) break;

        if (!haveApartment) break;

        const bool writeRequested = (signalled == WAIT_OBJECT_0 + 1);
        const bool volumeChanged = signalled == WAIT_OBJECT_0 + 3;

        if (writeRequested) {
            float wanted = 0.0f;
            bool doWrite = false;
            bool doToggleMute = false;

            lock_.Enter();
            if (pendingWrite_) {
                wanted = pendingLevel_;
                pendingWrite_ = false;
                doWrite = true;
            }
            if (pendingToggleMute_) {
                pendingToggleMute_ = false;
                doToggleMute = true;
            }
            lock_.Leave();

            if (doWrite || doToggleMute) {
                IMMDeviceEnumerator* enumerator = nullptr;
                if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                               CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                               reinterpret_cast<void**>(&enumerator))) &&
                    enumerator) {
                    IMMDevice* device = nullptr;
                    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                                      &device)) &&
                        device) {
                        IAudioEndpointVolume* volume = nullptr;
                        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume),
                                                       CLSCTX_ALL, nullptr,
                                                       reinterpret_cast<void**>(&volume))) &&
                            volume) {
                            if (doWrite) {
                                volume->SetMasterVolumeLevelScalar(wanted, nullptr);
                            }
                            if (doToggleMute) {
                                BOOL muted = FALSE;
                                if (SUCCEEDED(volume->GetMute(&muted))) {
                                    volume->SetMute(!muted, nullptr);
                                }
                            }
                            volume->Release();
                        }
                        device->Release();
                    }
                    enumerator->Release();
                }
            }
        }

        // The PEAK every fast tick, but only while a meter is on screen. The VOLUME keeps its own slower
        // clock, because that one opens the device and walking it twenty times a second to catch a volume-key
        // change would be work for nothing.
        if (peakMeterActive_.load(std::memory_order_relaxed)) {
            ReadPeakFromDevice();
        }
        if (volumeChanged) {
            if (ReadFromDevice(nullptr)) InterlockedIncrement(&revision_);
        } else if (GetTickCount64() - lastVolumeRead_ >= kPollIntervalMs) {
            lastVolumeRead_ = GetTickCount64();
            // Re-read afterwards, so a change made elsewhere (the keyboard, the shell flyout) still shows up.
            // The read is skipped while a write is pending, to avoid fighting the user.
            ReleaseEndpointVolume();
            if (ReadFromDevice(callback)) {
                InterlockedIncrement(&revision_);
            }
        }
    }

}

} // namespace island
