// =====================================================================
//  AutoStart.cpp -- see the header.
// =====================================================================
#include "platform/AutoStart.h"

#include <windows.h>

#include <cwchar>

namespace island {
namespace {

constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kValueName = L"IslandBar";

bool CurrentExecutable(std::wstring* path) {
    if (!path) return false;
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    *path = buffer;
    return true;
}

} // namespace

std::wstring AutoStartCommandFor(const std::wstring& executablePath) {
    if (executablePath.empty()) return {};
    std::wstring command = L"\"";
    command += executablePath;
    command += L"\"";
    return command;
}

bool AutoStartCommandMatches(const std::wstring& stored, const std::wstring& executablePath) {
    return !stored.empty() && !executablePath.empty() &&
           _wcsicmp(stored.c_str(), AutoStartCommandFor(executablePath).c_str()) == 0;
}

bool IsAutoStartEnabled() {
    std::wstring executable;
    if (!CurrentExecutable(&executable)) return false;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return false;
    }
    std::wstring stored(bytes / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key, kValueName, nullptr, &type,
                              reinterpret_cast<BYTE*>(stored.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return false;
    stored.resize(bytes / sizeof(wchar_t));
    while (!stored.empty() && stored.back() == L'\0') stored.pop_back();
    return AutoStartCommandMatches(stored, executable);
}

bool SetAutoStartEnabled(bool enabled) {
    std::wstring executable;
    if (!CurrentExecutable(&executable)) return false;

    HKEY key = nullptr;
    const LSTATUS opened =
        RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr);
    if (opened != ERROR_SUCCESS || !key) return false;

    LSTATUS result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = AutoStartCommandFor(executable);
        result = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kValueName);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

} // namespace island
