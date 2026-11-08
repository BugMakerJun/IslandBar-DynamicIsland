// =====================================================================
//  AutoStart.h -- the per-user "start when I sign in" setting.
// =====================================================================
#pragma once

#include <string>

namespace island {

// The command stored in HKCU\Software\Microsoft\Windows\CurrentVersion\Run. Quoting the executable path is
// part of the rule, because an unquoted path containing spaces is split by the logon launcher.
std::wstring AutoStartCommandFor(const std::wstring& executablePath);
bool AutoStartCommandMatches(const std::wstring& stored, const std::wstring& executablePath);

// Reads and updates this build's Run value. Every call targets HKCU, so no elevation is required.
bool IsAutoStartEnabled();
bool SetAutoStartEnabled(bool enabled);

} // namespace island
