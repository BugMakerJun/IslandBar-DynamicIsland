// =====================================================================
//  TrayReport.h -- fields in the shell's tray registration report.
//
//  The executable field is the one value that can contain spaces:
//  "DeepSeek Harness.exe". Reading it as an ordinary whitespace-delimited
//  token truncated it to "DeepSeek", which made the learned callback
//  unreachable even though the application had registered one.
// =====================================================================
#pragma once

#include <cstddef>
#include <cwchar>

namespace island {

inline bool TrayReportExeName(const wchar_t* line, wchar_t* out, size_t capacity) {
    if (!line || !out || capacity == 0) return false;
    out[0] = L'\0';

    const wchar_t* found = std::wcsstr(line, L" exe ");
    if (!found) return false;
    found += 5;

    const wchar_t* begin = found;
    const wchar_t* end = nullptr;
    if (*begin == L'"') {
        ++begin;
        const wchar_t* quotedEnd = std::wcsstr(begin, L"\" caller ");
        if (quotedEnd) {
            end = quotedEnd;
        } else {
            end = std::wcsstr(begin, L"\" hwnd ");
        }
    } else {
        // The shell's ring report goes straight from `exe` to `hwnd`; the tray hook's log
        // also names the caller in between. Stop at whichever field follows `exe`.
        const wchar_t* caller = std::wcsstr(begin, L" caller ");
        const wchar_t* hwnd = std::wcsstr(begin, L" hwnd ");
        end = caller && (!hwnd || caller < hwnd) ? caller : hwnd;
    }
    if (!end) return false;

    while (begin < end && (*begin == L' ' || *begin == L'\t')) ++begin;
    while (end > begin && (end[-1] == L' ' || end[-1] == L'\t')) --end;
    if (begin == end) return false;

    size_t used = 0;
    while (begin < end && used + 1 < capacity) out[used++] = *begin++;
    out[used] = L'\0';
    return used > 0;
}

} // namespace island
