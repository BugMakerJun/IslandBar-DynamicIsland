// =====================================================================
//  SystemStatus.h
//  The state of the machine: network, Bluetooth, volume, power.
//
//  WHY THIS IS A SEPARATE, PURE LAYER
//
//  Every value here comes from a different Windows API with its own failure modes, and most
//  of them are CONDITIONAL rather than absent:
//
//    * the WLAN interface state is always readable, but the SSID and signal strength only
//      exist while connected -- asking for them otherwise returns an error, not "disconnected";
//    * the Bluetooth radio enumerates even when it is switched off;
//    * the battery APIs SUCCEED on a desktop and report "unknown", which must not be drawn as
//      0% or as a missing icon;
//    * the WLAN radio on/off state is not exposed at all on this build (measured: the query
//      succeeds and reports zero PHYs), so anything drawn from it would be invented.
//
//  So the API calls live in one place and produce a plain struct, and every DECISION about
//  what that struct means -- which icon, which strength, whether to draw anything at all --
//  is a pure function that can be asserted. "The icon is wrong" and "the API returned
//  something unexpected" are then two different investigations.
// =====================================================================
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include "platform/CriticalSection.h"
#include "platform/WorkerThread.h"

namespace island {

// How strong the wireless signal is, in the four steps an indicator can show.
//
// Mapped from the percentage the WLAN API reports. The thresholds are the ones the shell's own
// icon uses, so the bar and the settings page agree about what "three bars" means.
enum class SignalStrength : int {
    None = 0,      // not connected
    Weak = 1,      // 1-24%
    Fair = 2,      // 25-49%
    Good = 3,      // 50-74%
    Excellent = 4, // 75-100%
};

// Which icon the network indicator should show.
enum class NetworkKind : int {
    None,        // no wireless interface at all
    Disconnected,
    Connected,
};

struct SystemStatus {
    // --- network ---
    NetworkKind network = NetworkKind::None;
    SignalStrength signal = SignalStrength::None;
    int signalPercent = 0;
    wchar_t ssid[33] = {};
    bool wirelessRadioOn = true;

    // --- bluetooth ---
    // `bluetoothPresent` and `bluetoothOn` are separate because a radio that is switched off
    // still enumerates: presence is a fact about the machine and on/off is a fact about the
    // radio, and drawing "no Bluetooth" for a switched-off radio would be wrong.
    bool bluetoothPresent = false;
    bool bluetoothOn = false;
    wchar_t bluetoothName[248] = {};

    // --- the two switches that are not radios ---
    //
    // Night light is read from the state the OS keeps for it (see island::NightLightOn): there is no API,
    // and the registry value is what the Settings page itself shows. Live captions has no state either --
    // what is readable is whether its process is running, which is what the tile reflects.
    //
    // Both are here rather than asked for at the moment of the draw because the panel is rebuilt every
    // frame and a Toolhelp snapshot per frame is not.
    bool nightLightOn = false;
    bool liveCaptionsOn = false;

    // --- volume ---
    bool muted = false;
    int volumePercent = 0;

    // --- the wireless radio ---
    //
    // False once the Wi-Fi radio has been switched off. Read from the WLAN radio-state QUERY (which works
    // here), while the WRITE goes through `Windows.Devices.Radios` -- see SystemToggles.h: the WLAN write
    // that used to serve this reported success and changed nothing on this machine, twice in one session,
    // and the tile is only honest if what it shows is a reading.
    //
    // Both radios are switchable now, so the 飞行模式 tile switches both (Bluetooth has `bluetoothOn` below).
    //
    // True is the default because a reading that never happened must not report a radio the user turned off:
    // the Wi-Fi tile would then say 已关闭 on a machine whose radio is on.
    bool wifiRadioOn = true;

    // --- power ---
    // A desktop reports success with "unknown", so this is a tri-state and not a bool: drawing
    // nothing is correct, while drawing an empty battery is a lie.
    bool batteryPresent = false;
    bool onAcPower = true;
    int batteryPercent = 0;

    // Battery saver, which Windows reports in its own field rather than as a power scheme to query.
    //
    // Readable but not settable from here: turning it on and off is a power-scheme write, so the tile
    // reflects the state and opens the page that can change it.
    bool batterySaverOn = false;

    // --- input method ---
    //
    // The keyboard layout of the foreground window, as a language id (the low word of the HKL).
    //
    // This is the route that WORKS, and it was reached by elimination: ImmGetContext returns null
    // for a window that is not an IME client (which is every window the bar will ever see), and
    // WM_IME_CONTROL sent to the IME window returns zero for IMC_GETCONVERSIONMODE,
    // IMC_GETOPENSTATUS and everything else -- measured, all of them. The layout, by contrast, is
    // readable for any foreground window with no cooperation from it at all.
    //
    // A language id is all this gives: which LANGUAGE is active, not whether the IME is in Chinese
    // or English mode. That distinction needs the conversion status, which is exactly what returns
    // zero -- so the indicator shows the language, and does not pretend to show the mode.
    unsigned short inputLanguageId = 0;

    // --- freshness ---
    // Polled, not pushed: there is no notification for most of this, so the values are sampled
    // on a timer. `ageMs` is carried so a caller can grey out a reading that has gone stale
    // rather than showing an old value as if it were current.
    unsigned long long ageMs = 0;
};

// A wireless signal percentage as one of the five steps.
//
// Pure, so the mapping is testable without a network: the boundary values are where indicators
// get this wrong, and a percentage of 0 is NOT the same as "not connected".
SignalStrength SignalFromPercent(int percent);

// The network indicator for an interface state name and its signal.
//
// Windows reports eight interface states; four of them mean "working on it" and are shown as
// disconnected rather than as a separate spinning icon, because a bar that flickers through
// four states while connecting is worse than one that says "not yet".
NetworkKind NetworkKindFor(bool hasInterface, bool connected);

// Whether the power indicator should be drawn at all.
//
// False on a machine without a battery. This exists as a function rather than a comparison at
// the call site because the desktop case is the one that produces a WRONG icon rather than no
// icon: the API succeeds, reports 255, and 255 percent of a battery is what gets drawn.
bool ShouldDrawBattery(const SystemStatus& status);

// The glyph for the input-method indicator, as up to two characters.
//
// Returns the language's own character rather than a Latin abbreviation, because that is what the
// shell shows in the same place: the indicator is read at a glance, and a user looking for "中"
// does not find "CN". A language with no distinctive single character gets its country code.
void InputMethodLabel(unsigned short languageId, wchar_t* out, size_t capacity);

// Whether the input-method indicator is worth drawing.
//
// False when only one keyboard layout is installed, because then the indicator can never change
// and a permanent "中" beside the clock is decoration rather than information. The shell hides it
// in the same situation.
bool ShouldDrawInputMethod(const SystemStatus& status, int installedLayoutCount);

// Switches the Wi-Fi radio on or off, and reports the WLAN API's own error code.
//
// (The Wi-Fi radio's WRITE used to live here. It has moved to SystemToggles.h with the Bluetooth one --
// both radios go through `Windows.Devices.Radios` now, and the note at the end of SystemStatus.cpp
// records what the WLAN write did on this machine and why it is not the route. The radio-state QUERY
// above is still this file's.)

// Reads the machine. Never fails as a whole: a value that cannot be read keeps its previous
// meaning rather than being zeroed, because "the API failed" and "the value is zero" look
// identical on screen and only one of them is worth showing.
class SystemStatusReader : public std::enable_shared_from_this<SystemStatusReader> {
public:
    SystemStatusReader() = default;
    ~SystemStatusReader();

    SystemStatusReader(const SystemStatusReader&) = delete;
    SystemStatusReader& operator=(const SystemStatusReader&) = delete;

    // Opens the machine handles on its own worker and takes the first reading.
    // The worker then samples every pollMs and wakes the UI after publishing.
    bool Start(HWND wakeWindow, unsigned long pollMs = 2000);
    void Stop();

    // Asks the worker to take a reading now, used after a user changes a radio.
    void RequestSample();

    // Copies the latest complete reading. This is a small struct and is the
    // only method the UI thread calls.
    void Snapshot(SystemStatus* out) const;

    // How many keyboard layouts are installed.
    //
    // Counted once at Open, because the indicator is only worth drawing when there is more than
    // one and the installed set does not change while the bar runs. Exposed rather than folded
    // into the status because it is a fact about the machine, not a reading.
    int InstalledLayoutCount() const {
        return installedLayoutCount_.load(std::memory_order_acquire);
    }

private:
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();
    void Open();
    void Close();
    void Sample(SystemStatus* out);
    bool Publish(const SystemStatus& status);

    WorkerThread worker_;
    HANDLE requestEvent_ = nullptr;
    HWND wakeWindow_ = nullptr;
    unsigned long pollMs_ = 2000;
    std::atomic<bool> running_{false};
    std::atomic<bool> haveSample_{false};
    std::atomic<int> installedLayoutCount_{0};
    mutable CriticalSection lock_;
    SystemStatus reading_{};

    void* wlanClient_ = nullptr;
    bool wlanOpen_ = false;

    // Cached so the enumeration is not repeated per frame: the radio set does not change while
    // the bar runs, and BluetoothFindFirstRadio walks a device tree.
    bool bluetoothProbed_ = false;
};

} // namespace island
