// =====================================================================
//  SystemToggles.cpp -- see the header for why each route is the one it is.
// =====================================================================
#include "shell/SystemToggles.h"

#include "core/Log.h"
#include "platform/WorkerThread.h"
#include "shell/StartMenu.h"   // SendWindowsCtrlKeyChord

#include <shellapi.h>
#include <tlhelp32.h>

#include <objbase.h>
#include <uiautomation.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Radios.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace island {
namespace {

// ---------------------------------------------------------------------------------------------
//  one job at a time, whichever kind it is
// ---------------------------------------------------------------------------------------------
//
// The two blocking jobs share one worker slot on purpose: they are both "press a switch the machine
// owns", the user cannot meaningfully start the second while the first is in flight, and one flag is
// easier to keep honest than two.
enum class JobKind { None, Radios, NightLight };

void RunRadioJob(bool wifi, bool bluetooth, bool on);
void RunNightLightJob(bool on);

struct ToggleJob {
    JobKind kind = JobKind::None;
    bool wifi = false;
    bool bluetooth = false;
    bool on = false;
};

WorkerThread g_toggleWorker;

unsigned long __stdcall ToggleThreadEntry(void* context) {
    std::unique_ptr<ToggleJob> job(static_cast<ToggleJob*>(context));
    if (job->kind == JobKind::Radios) {
        RunRadioJob(job->wifi, job->bluetooth, job->on);
    } else if (job->kind == JobKind::NightLight) {
        RunNightLightJob(job->on);
    }
    return 0;
}

std::atomic<bool> g_busy{false};
std::mutex g_resultMutex;

struct JobResult {
    bool finished = false;
    JobKind kind = JobKind::None;
    bool wanted = false;        // radios: what was asked for (both radios get the same answer)
    bool wifiRequested = false;
    bool bluetoothRequested = false;
    bool succeeded = false;
    long hresult = 0;           // radios: the WinRT failure code, when a call was refused
    bool wifiNow = false;       // radios: the state read back afterwards
    bool bluetoothNow = false;
    bool stateAfter = false;    // night light: the page's answer, or the registry, read back
};
JobResult g_result;

// What the Settings page said the last time this session pressed its switch, if it ever has. See
// NightLightOnForDisplay for why this exists at all.
bool g_haveNightLightPageState = false;
bool g_nightLightPageState = false;

// Claims the worker slot, or reports that one is already running.
bool ClaimJob(JobKind kind) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    std::lock_guard<std::mutex> lock(g_resultMutex);
    g_result = JobResult{};
    g_result.kind = kind;
    return true;
}

void Publish(const JobResult& result) {
    {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        g_result = result;
        g_result.finished = true;
    }
    g_busy.store(false);
}

// ---------------------------------------------------------------------------------------------
//  The radios -- Windows.Devices.Radios, one job for either or both
// ---------------------------------------------------------------------------------------------
//
// A worker thread with its own apartment (MTA: nothing here needs a message loop), because the WinRT
// calls are synchronous from this side.
void RunRadioJob(bool wifi, bool bluetooth, bool on) {
    JobResult result;
    result.kind = JobKind::Radios;
    result.wanted = on;
    result.wifiRequested = wifi;
    result.bluetoothRequested = bluetooth;

    bool comUp = false;
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comHr)) comUp = true;

    try {
        using namespace winrt::Windows::Devices::Radios;

        const RadioAccessStatus access = Radio::RequestAccessAsync().get();
        if (access != RadioAccessStatus::Allowed) {
            result.hresult = static_cast<long>(access);
            LogLine(L"  radios: access is NOT allowed (status %d); nothing was switched",
                    static_cast<int>(access));
        } else {
            const auto radios = Radio::GetRadiosAsync().get();
            bool requested = false;
            for (const Radio& radio : radios) {
                const RadioKind kind = radio.Kind();
                const bool wanted = (kind == RadioKind::WiFi && wifi) ||
                                    (kind == RadioKind::Bluetooth && bluetooth);
                if (!wanted) continue;
                requested = true;

                const RadioAccessStatus wrote =
                    radio.SetStateAsync(on ? RadioState::On : RadioState::Off).get();
                const bool now = radio.State() == RadioState::On;
                if (kind == RadioKind::WiFi) {
                    result.wifiNow = now;
                } else {
                    result.bluetoothNow = now;
                }
                LogLine(L"  radios: '%ls' (%ls) -> asked %ls, answered %d, now %ls",
                        radio.Name().c_str(), kind == RadioKind::WiFi ? L"wifi" : L"bluetooth",
                        on ? L"on" : L"off", static_cast<int>(wrote), now ? L"on" : L"off");

                // A radio that answered Allowed but did not move counts as a failure: the panel's whole
                // point is that its caption is a reading, and a press that changed nothing must not be
                // reported as one that did.
                if (wrote != RadioAccessStatus::Allowed || now != on) result.succeeded = false;
            }
            if (!requested) {
                LogLine(L"  radios: this machine has no %ls radio",
                        wifi && bluetooth ? L"Wi-Fi or Bluetooth"
                                          : (wifi ? L"Wi-Fi" : L"Bluetooth"));
            } else {
                // Both were asked for: success means every one of them moved.
                result.succeeded = true;
                if (wifi && result.wifiNow != on) result.succeeded = false;
                if (bluetooth && result.bluetoothNow != on) result.succeeded = false;
            }
        }
    } catch (const winrt::hresult_error& error) {
        result.hresult = static_cast<long>(error.code().value);
        LogLine(L"  radios: the call threw (hr 0x%08lX); nothing switched",
                static_cast<unsigned long>(result.hresult));
    } catch (...) {
        LogLine(L"  radios: the call failed with an unknown error");
    }

    if (comUp) CoUninitialize();
    Publish(result);
}

// ---------------------------------------------------------------------------------------------
//  night light -- the Settings page's own switch, through UI Automation
// ---------------------------------------------------------------------------------------------

constexpr const wchar_t* kNightLightOnId = L"SystemSettings_Display_BlueLight_ManualToggleOn_Button";
constexpr const wchar_t* kNightLightOffId =
    L"SystemSettings_Display_BlueLight_ManualToggleOff_Button";

// Every top-level frame window, so the one this job opens can be told apart from one the user had
// open already -- and closed again only if it is ours.
void CollectFrameWindows(std::vector<HWND>* out) {
    out->clear();
    struct Collector {
        std::vector<HWND>* out;
    } collector{out};

    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Collector*>(param);
            wchar_t cls[64] = {};
            if (GetClassNameW(hwnd, cls, 64) && _wcsicmp(cls, L"ApplicationFrameWindow") == 0) {
                c->out->push_back(hwnd);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collector));
}

bool WasThereBefore(const std::vector<HWND>& before, HWND hwnd) {
    return std::find(before.begin(), before.end(), hwnd) != before.end();
}

// The page's switch, by AutomationId, reporting WHICH of the two it is.
//
// The two ids are not two controls, they are one switch in two states, and the id says which state the
// PAGE believes the feature is in: `...ManualToggleOn_Button` is the button that turns it on, so it is
// published while the feature is off. That makes the id the page's own answer to "is it on", which is
// the answer this needs -- measured, and what made the difference: pressing by "the wanted state"
// alone pressed "turn it off now" while the wanted state was on, nothing moved, and the log said so.
bool FindNightLightSwitch(IUIAutomation* automation, IUIAutomationElement* window, bool* currentlyOn,
                          IUIAutomationElement** found) {
    for (const wchar_t* id : {kNightLightOnId, kNightLightOffId}) {
        VARIANT value = {};
        VariantInit(&value);
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(id);
        if (!value.bstrVal) continue;

        IUIAutomationCondition* condition = nullptr;
        HRESULT hr = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value,
                                                         &condition);
        VariantClear(&value);
        if (FAILED(hr) || !condition) continue;

        IUIAutomationElement* element = nullptr;
        hr = window->FindFirst(TreeScope_Descendants, condition, &element);
        condition->Release();
        if (SUCCEEDED(hr) && element) {
            *found = element;
            // The button that says "turn it on" exists while it is OFF, and the other way round.
            if (currentlyOn) *currentlyOn = (std::wcscmp(id, kNightLightOnId) == 0) ? false : true;
            return true;
        }
    }
    return false;
}

bool InvokeElement(IUIAutomationElement* element) {
    IUIAutomationInvokePattern* invoke = nullptr;
    if (FAILED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) ||
        !invoke) {
        return false;
    }
    const HRESULT hr = invoke->Invoke();
    invoke->Release();
    return SUCCEEDED(hr);
}

void RunNightLightJob(bool on) {
    JobResult result;
    result.kind = JobKind::NightLight;
    result.wanted = on;

    bool comUp = false;
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(comHr)) comUp = true;

    std::vector<HWND> framesBefore;
    CollectFrameWindows(&framesBefore);

    // What the PAGE said, when it said anything: the switch's own label is the state this feature has, and
    // it is the only reading that cannot disagree with the press that just happened.
    bool havePageState = false;
    bool pageStateAfter = false;

    // The page is asked for, and the switch pressed on it. This is the ONLY thing on this machine that
    // switches night light -- measured: writing the state the OS keeps in the registry does not.
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", L"ms-settings:nightlight", nullptr,
                                           nullptr, SW_SHOWNORMAL);
    LogLine(L"  night light: ms-settings:nightlight -> %lld; looking for the switch",
            static_cast<long long>(reinterpret_cast<INT_PTR>(opened)));

    IUIAutomation* automation = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&automation))) &&
        automation) {
        // Polled, because the page takes seconds to publish its tree on a cold start; measured at
        // about 2 s here, and up to 15 s is tolerated rather than pretending a slow page is a failure.
        IUIAutomationElement* element = nullptr;
        bool pageSaysOn = false;
        for (int attempt = 0; attempt < 150 && !element; ++attempt) {
            std::vector<HWND> framesNow;
            CollectFrameWindows(&framesNow);
            for (HWND hwnd : framesNow) {
                // VISIBLE WINDOWS FIRST, and the frame windows are logged: the Settings app keeps more than
                // one ApplicationFrameWindow alive, and a press delivered to a hidden one is a press nobody
                // sees -- measured: the switch was found and "pressed" twice with the registry never moving,
                // which is exactly what pressing a cached page in an invisible window would do.
                if (!IsWindowVisible(hwnd)) continue;
                IUIAutomationElement* window = nullptr;
                if (SUCCEEDED(automation->ElementFromHandle(hwnd, &window)) && window) {
                    IUIAutomationElement* candidate = nullptr;
                    if (FindNightLightSwitch(automation, window, &pageSaysOn, &candidate)) {
                        element = candidate;
                    }
                    window->Release();
                }
                if (element) break;
            }
            if (!element) Sleep(100);
        }

        if (!element) {
            LogLine(L"  night light: the switch never appeared in the Settings page");
        } else if (pageSaysOn == on) {
            // The page, which is the thing that owns this setting, already says what was asked for. Nothing
            // to press, and saying so is the honest answer rather than pressing the opposite button.
            result.succeeded = true;
            havePageState = true;
            pageStateAfter = on;
            LogLine(L"  night light: the page already says %ls; nothing to press",
                    on ? L"on" : L"off");
            element->Release();
        } else {
            // SETTLE FIRST, and this is measured rather than defensive: the page publishes its tree before
            // its own code is ready to act on it, and the first version of this pressed about 320 ms after
            // the page opened, which did nothing.
            Sleep(1500);

            const bool pressed = InvokeElement(element);
            element->Release();
            LogLine(L"  night light: pressed '%ls' (the page said %ls)", on ? L"turn it on now"
                                                                           : L"turn it off now",
                    pageSaysOn ? L"on" : L"off");

            // VERIFIED BY THE PAGE'S OWN LABEL, which is the state this press is supposed to change. The
            // registry blob is not the witness: measured, the two can disagree -- the page read "turn it off
            // now" while the blob held the off shape, after the value had been written by hand in a probe.
            // The button's id is the answer the page itself gives, so it is the one polled here.
            bool flipped = false;
            bool nowSaysOn = pageSaysOn;
            for (int attempt = 0; attempt < 20 && !flipped; ++attempt) {
                Sleep(400);
                std::vector<HWND> framesAgain;
                CollectFrameWindows(&framesAgain);
                for (HWND hwnd : framesAgain) {
                    if (!IsWindowVisible(hwnd)) continue;
                    IUIAutomationElement* window = nullptr;
                    if (SUCCEEDED(automation->ElementFromHandle(hwnd, &window)) && window) {
                        IUIAutomationElement* check = nullptr;
                        if (FindNightLightSwitch(automation, window, &nowSaysOn, &check)) {
                            check->Release();
                        }
                        window->Release();
                    }
                    if (nowSaysOn != pageSaysOn) break;
                }
                flipped = (nowSaysOn == on);
            }

            result.succeeded = pressed && flipped;
            pageStateAfter = nowSaysOn;
            havePageState = true;
            LogLine(L"  night light: the page now says %ls -- the press %ls", nowSaysOn ? L"on" : L"off",
                    result.succeeded ? L"took" : L"DID NOT take");
        }
        automation->Release();
    } else {
        LogLine(L"  night light: UI Automation could not be created");
    }

    // BOTH readings, and they can disagree: the page's own label is what the press changed, and the registry
    // blob is what the tile's caption is built from outside a press. Logged together so a disagreement is a
    // fact in the record rather than a mystery -- it happened once (the blob had been written by hand in a
    // probe), and the captions are the only place it would ever be noticed.
    result.stateAfter = havePageState ? pageStateAfter : NightLightOn();
    if (havePageState) {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        g_haveNightLightPageState = true;
        g_nightLightPageState = pageStateAfter;
    }
    LogLine(L"  night light: asked for %ls; the page says %ls, the registry says %ls",
            on ? L"on" : L"off", result.stateAfter ? L"on" : L"off",
            NightLightOn() ? L"on" : L"off");

    // The window this job opened is closed again -- and only that one: a Settings window the user had
    // open before this press is left alone, because closing somebody else's window is not this
    // feature's business.
    std::vector<HWND> framesNow;
    CollectFrameWindows(&framesNow);
    for (HWND hwnd : framesNow) {
        if (!WasThereBefore(framesBefore, hwnd)) {
            LogLine(L"  night light: closing the Settings window this press opened");
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    }

    if (comUp) CoUninitialize();
    Publish(result);
}

} // namespace

// ---------------------------------------------------------------------------------------------

bool RequestRadios(bool wifi, bool bluetooth, bool on) {
    if (!wifi && !bluetooth) return false;
    if (!ClaimJob(JobKind::Radios)) return false;
    auto* job = new (std::nothrow) ToggleJob{JobKind::Radios, wifi, bluetooth, on};
    if (job && g_toggleWorker.StartUnmanaged(&ToggleThreadEntry, job)) return true;
    delete job;
    g_busy.store(false);
    return false;
}

bool TakeRadioOutcome(bool* wifiNow, bool* bluetoothNow, bool* wifiAsked, bool* bluetoothAsked,
                      bool* succeeded, long* hresult) {
    std::lock_guard<std::mutex> lock(g_resultMutex);
    if (!g_result.finished || g_result.kind != JobKind::Radios) return false;
    if (wifiNow) *wifiNow = g_result.wifiNow;
    if (bluetoothNow) *bluetoothNow = g_result.bluetoothNow;
    if (wifiAsked) *wifiAsked = g_result.wifiRequested;
    if (bluetoothAsked) *bluetoothAsked = g_result.bluetoothRequested;
    if (succeeded) *succeeded = g_result.succeeded;
    if (hresult) *hresult = g_result.hresult;
    g_result = JobResult{};
    return true;
}

NightLightState NightLightStateFromBlob(const unsigned char* bytes, unsigned long size) {
    if (!bytes || size < 6) return NightLightState::Unknown;

    // Every shape starts with this outer header; anything else is not this value at all.
    if (!(bytes[0] == 0x43 && bytes[1] == 0x42 && bytes[2] == 0x01 && bytes[3] == 0x00)) {
        return NightLightState::Unknown;
    }

    // The inner blob is introduced by 2A 2B 0E <length> and then its own 43 42 01 00 header, and the two
    // bytes after THAT header are the switch: 10 00 when the feature is on, and the payload's own first
    // bytes when it is off. Laid out on the bytes that were captured (the ON blob, offsets from the 2A):
    //
    //     +0  +1  +2  +3     +4  +5  +6  +7     +8  +9
    //     2A  2B  0E  15     43  42  01  00     10  00
    //
    //     marker       length  inner header      the switch: on
    //
    // and the OFF-by-history blob is the same shape with 13 for the length and D0 0A in those last two
    // bytes. The +3 byte is the inner payload's LENGTH, which is what makes the two shapes differ by 0x15
    // against 0x13 -- worth writing down because a parse that skipped it (which is how this started) reads
    // every blob as OFF, including the 43-byte ON one the self-test carries.
    //
    // The outer header must not be mistaken for the inner one either: they share the same four bytes, which
    // is why this looks for the 2A 2B 0E marker rather than for 43 42 01 00 on its own.
    for (unsigned long i = 0; i + 10 <= size; ++i) {
        if (bytes[i] != 0x2A || bytes[i + 1] != 0x2B || bytes[i + 2] != 0x0E) continue;
        if (bytes[i + 4] != 0x43 || bytes[i + 5] != 0x42 || bytes[i + 6] != 0x01 ||
            bytes[i + 7] != 0x00) {
            continue;
        }
        const bool on = bytes[i + 8] == 0x10 && bytes[i + 9] == 0x00;
        return on ? NightLightState::On : NightLightState::Off;
    }

    // No inner blob at all: the 13-byte shape, which is the feature off and never yet switched on.
    return NightLightState::Off;
}

bool NightLightOn() {
    const wchar_t* path =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
        L"default$windows.data.bluelightreduction.bluelightreductionstate\\"
        L"windows.data.bluelightreduction.bluelightreductionstate";

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;

    BYTE buffer[128] = {};
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    const LONG read = RegQueryValueExW(key, L"Data", nullptr, &type, buffer, &bytes);
    RegCloseKey(key);
    if (read != ERROR_SUCCESS || type != REG_BINARY) return false;

    return NightLightStateFromBlob(buffer, bytes) == NightLightState::On;
}

bool NightLightOnForDisplay() {
    std::lock_guard<std::mutex> lock(g_resultMutex);
    if (g_haveNightLightPageState) return g_nightLightPageState;
    return NightLightOn();
}

bool RequestNightLight(bool on) {
    if (!ClaimJob(JobKind::NightLight)) return false;
    auto* job = new (std::nothrow) ToggleJob{JobKind::NightLight, false, false, on};
    if (job && g_toggleWorker.StartUnmanaged(&ToggleThreadEntry, job)) return true;
    delete job;
    g_busy.store(false);
    return false;
}

WorkerThread::StopResult StopSystemToggleWorker() {
    return g_toggleWorker.Stop();
}

bool TakeNightLightOutcome(bool* wanted, bool* stateAfter, bool* succeeded) {
    std::lock_guard<std::mutex> lock(g_resultMutex);
    if (!g_result.finished || g_result.kind != JobKind::NightLight) return false;
    if (wanted) *wanted = g_result.wanted;
    if (stateAfter) *stateAfter = g_result.stateAfter;
    if (succeeded) *succeeded = g_result.succeeded;
    g_result = JobResult{};
    return true;
}

bool LiveCaptionsRunning() {
    // Toolhelp, the same way TrayIcons asks which applications are running: one call for every process
    // on the machine.
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    bool running = false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"LiveCaptions.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

bool ToggleLiveCaptions() {
    // Win+Ctrl+L. Live captions has no API and no settings URI that switches it; the shortcut is the
    // interface, and it was measured to work in both directions (LiveCaptions.exe appears, and goes
    // away again on the second press).
    return SendWindowsCtrlKeyChord(L'L');
}

} // namespace island
