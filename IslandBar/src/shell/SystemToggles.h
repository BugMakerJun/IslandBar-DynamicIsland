// =====================================================================
//  SystemToggles.h
//  The quick-settings tiles that SWITCH something instead of opening a page.
//
//  Until now exactly one tile did that (Wi-Fi, through the WLAN API's radio-state opcode) and the
//  rest opened `ms-settings:` pages, on the stated grounds that the interfaces did not exist. Three
//  of those grounds were WRONG, and measured so (scripts/probe-radios.ps1, probe-toggle-extras.ps1):
//
//   * Bluetooth. The note said `Windows.Devices.Radios` needs PACKAGE IDENTITY. It does not:
//     RequestAccessAsync answers Allowed for this ordinary unpackaged process, and the radio went
//     On -> Off -> On with every call returning Allowed. So the switch is here.
//   * Live captions. Win+Ctrl+L opens AND closes it (measured both ways by watching LiveCaptions.exe
//     appear and go away), which is the only interface there is.
//   * Night light. Writing the state blob the OS keeps in the registry does NOT switch it: measured,
//     the Settings page still offered "turn it on now" with the on-value written, after reopening the
//     page. What does work is the page's own switch, driven through UI Automation's Invoke pattern.
//     That route costs a Settings window on screen for a couple of seconds, and there is no cheaper
//     one -- which is a fact about Windows, not about this bar.
//
// Everything that blocks -- a WinRT completion, a Settings page that has to load -- runs on a worker
// thread and is collected by the frame loop. The frame loop itself must never wait for these.
// =====================================================================
#pragma once

#include <windows.h>

#include "platform/WorkerThread.h"

namespace island {

// --- the radios: Wi-Fi and Bluetooth ---------------------------------------------------------------
//
// ONE mechanism for both, and it is the one the shell itself uses: `Windows.Devices.Radios`. Measured on
// this machine, both directions, for both radios: `RequestAccessAsync` answers Allowed for this ordinary
// unpackaged process, and `SetStateAsync` switches the radio and reports Allowed.
//
// WHAT THIS REPLACED, and why it had to. The Wi-Fi radio used to be switched through the WLAN API's
// documented `wlan_intf_opcode_radio_state`, and on this machine that write FAILS: the user's own log
// carries two of them, from two presses of the 飞行模式 tile,
//
//     wifi radio: WlanSetInterface single=0 full=0 counted=0
//
// i.e. no phy accepted the state, nothing changed, and the tile had nothing to show for the press. The
// same two presses go through the Radios API here and both radios switch.
//
// The state is NOT remembered: the tiles' captions come from SystemStatus, which reads the radios on its
// own cadence, so what the panel shows after a press is what the machine says rather than what was asked.
//
// The job runs on a worker thread -- the WinRT calls block for a few hundred milliseconds and the frame
// loop must not wait for them.

// Asks for the named radios to be switched. False when a request is already running.
bool RequestRadios(bool wifi, bool bluetooth, bool on);

// Collects a finished request. False when none has finished since the last call.
//
// `wifiNow`/`bluetoothNow` are what the radios reported AFTER the call, `wifiAsked`/`bluetoothAsked` what
// the request named -- both, because a caller that asked for one radio must not report the other's state as
// if it had been part of the job -- and `hresult` is the WinRT failure code when a call was refused (the
// only part of a WinRT error an ASCII log line can carry).
bool TakeRadioOutcome(bool* wifiNow, bool* bluetoothNow, bool* wifiAsked, bool* bluetoothAsked,
                      bool* succeeded, long* hresult);

// --- night light ----------------------------------------------------------------------------------

enum class NightLightState { Unknown, Off, On };

// The state a blob encodes. PURE, and separate from the registry read, because the interesting part is the
// shape and the shapes are measured data: the self-test runs the three that were captured on this machine
// (see the table below) rather than trusting a parse written from memory.
//
// The blob, measured on this machine:
//   off (never switched on)  13 bytes  43 42 01 00 0A 02 01 00 2A 2A 00 00 00
//   off (was switched off)   41 bytes  ... 2A 2B 0E 13 43 42 01 00 D0 0A ...   -- no 10 00 after the
//   on                       43 bytes  ... 2A 2B 0E 15 43 42 01 00 10 00 D0 0A ...      inner header
//
// The inner 43 42 01 00 is the header of a nested blob and the two bytes after it are the switch:
// 10 00 when on, absent when off -- which is also why the length byte before it reads 0x15 against
// 0x13. LENGTH ALONE IS NOT THE TEST: an off blob is 13 bytes the first time and 41 once it has been
// switched off again.
NightLightState NightLightStateFromBlob(const unsigned char* bytes, unsigned long size);

// What the OS's own state blob says. Reads the registry; Unknown when the value is missing.
//
// NOT the same thing as being able to SWITCH it: measured, writing this value does not switch night light
// (the Settings page still offered "turn it on now" with the on-value written, after reopening the page).
bool NightLightOn();

// The state to SHOW on the panel's tile.
//
// Not the same as NightLightOn() once this session has pressed the switch: measured, the page's own label
// and the registry value can disagree -- the page is the actor, it applies the change, and the value it
// writes to the registry is its cached copy of the same setting, which came out of step here after a probe
// wrote that value by hand. So the page's answer is kept for the rest of the session and outranks the
// registry, while a machine that has never pressed the switch is read from the registry as before.
bool NightLightOnForDisplay();

// Asks for night light to be switched, by pressing the Settings page's own switch.
// False if a request is already running.
bool RequestNightLight(bool on);

// Collects a finished request. False when none has finished since the last call.
// `wanted` is what was asked for, `stateAfter` what the registry says afterwards -- read back rather
// than assumed, so a press the page ignored is visible as the difference between the two.
bool TakeNightLightOutcome(bool* wanted, bool* stateAfter, bool* succeeded);

// --- live captions --------------------------------------------------------------------------------

// Whether LiveCaptions.exe is running, which is the only state there is to read.
bool LiveCaptionsRunning();

// Sends Win+Ctrl+L. Reports only that the keys were put into the queue -- whether the shell acted on
// them is what LiveCaptionsRunning answers a moment later.
bool ToggleLiveCaptions();

// Stops the single tracked toggle worker. A timeout is returned rather than
// treated as a successful shutdown.
WorkerThread::StopResult StopSystemToggleWorker();

} // namespace island
