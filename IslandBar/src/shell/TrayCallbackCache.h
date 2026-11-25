// =====================================================================
//  TrayCallbackCache.h
//  The one serialized reader/writer for the learned tray callback cache.
//
//  The learning worker, the press worker, and the frame loop used to open the same file directly. A reader
//  could therefore see an in-place rewrite halfway through, and two workers could lose each other's updates.
//  This module makes every mutation a read-modify-write under one process-wide lock and publishes the result
//  with a same-directory temporary file followed by an atomic replace.
// =====================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace island {

class TrayCallbackCache {
public:
    // `%USERPROFILE%\IslandBar\tray_callbacks.txt`, or empty when USERPROFILE is unavailable.
    static std::wstring DefaultPath();

    // Reads a complete cache version. A missing file is an empty, successful read. An unreadable, oversized,
    // invalid UTF-8, or unknown-schema file is a failed read: guessing from half a cache is worse than
    // reporting that nothing is known.
    static bool Read(std::vector<std::wstring>* lines, const std::wstring& path = std::wstring());

    // Reads, mutates, and atomically replaces the cache while holding the single-writer lock. `mutate`
    // returns false when it made no change; that leaves the file untouched.
    static bool Update(const std::function<bool(std::vector<std::wstring>&)>& mutate,
                       const std::wstring& path = std::wstring());
};

} // namespace island
