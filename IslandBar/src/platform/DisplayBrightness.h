// =====================================================================
//  DisplayBrightness.h
//  The monitor's own brightness, over DDC/CI.
//
//  WHICH INTERFACE, and it was measured rather than assumed (scripts/probe-brightness.ps1):
//
//   * WMI (`WmiMonitorBrightness` / `WmiMonitorBrightnessMethods`) is the interface for an INTERNAL
//     panel. On this machine it answers NOTHING -- the query returns an empty set -- because this is a
//     desktop with an external monitor, which is exactly the case WMI does not cover. The note in this
//     project that said "WMI, cannot be verified here" was half right: it cannot be verified here, and
//     it also cannot be used here.
//   * DDC/CI talks to the MONITOR over the video cable through the display driver. Measured here:
//     one physical monitor, capabilities 0x1C17 with the brightness bit set, minimum 0, current 77,
//     maximum 100 -- and a write of 60 read back as 60 and restored to 77.
//
//  DDC/CI only works when the monitor has it enabled in its own on-screen menu (LG has it on by
//  default). When it is off, or the monitor does not support it, the read fails -- which is reported
//  rather than papered over, and the panel then has no brightness row at all.
// =====================================================================
#pragma once

#include <windows.h>

namespace island {

// Whether the monitor's brightness can be read. Cheap after the first call: the physical-monitor handle
// is kept and only re-acquired when the bar moves to another display or the handle stops answering.
bool BrightnessAvailable(HMONITOR monitor);

// The current brightness as a percentage, from the monitor's own minimum and maximum. False when it
// cannot be read.
bool ReadBrightnessPercent(HMONITOR monitor, int* percent);

// --- the same reading, off the frame loop ----------------------------------------------------------
//
// A DDC/CI read is an I2C transaction over the video cable. Measured on this machine (2026-10-18): 52-63
// ms per read, and 1.8 seconds for the first one after the monitor had been asleep. It was being taken
// on the frame loop every two seconds, so every animation froze for ~60 ms twice a second and for two
// whole seconds once a run -- part of what "所有的动画都卡卡的" was.
//
// Request once per status tick and collect whenever the answer is ready; the loop never waits.
bool RequestBrightnessPercent(HMONITOR monitor);

// Collects a finished request. False when nothing has finished since the last call; when it returns
// true, `ok` says whether the monitor answered and `percent` carries the reading.
bool TakeBrightnessPercent(bool* ok, int* percent);

// Sets the brightness, as a percentage of the monitor's own range. The value is read back before this
// reports success, because DDC/CI silently drops a write the monitor does not accept and "the call
// returned" has never been evidence in this project.
bool SetBrightnessPercent(HMONITOR monitor, int percent, int* landedPercent);

} // namespace island
