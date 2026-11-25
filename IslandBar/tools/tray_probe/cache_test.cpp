// =====================================================================
//  tools/tray_probe/cache_test.cpp
//  Concurrency and failure-preservation checks for TrayCallbackCache.
// =====================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "shell/TrayCallbackCache.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

bool AddLine(const std::wstring& path, const wchar_t* line) {
    return island::TrayCallbackCache::Update(
        [line](std::vector<std::wstring>& lines) {
            lines.emplace_back(line);
            return true;
        },
        path);
}

bool WriteAscii(const std::wstring& path, const char* text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(std::strlen(text));
    const bool ok = WriteFile(file, text, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

} // namespace

int main() {
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return 2;
    const std::wstring parent = std::wstring(base) + L"\\IslandBar\\TrayProbe";
    CreateDirectoryW((std::wstring(base) + L"\\IslandBar").c_str(), nullptr);
    CreateDirectoryW(parent.c_str(), nullptr);
    wchar_t directory[MAX_PATH] = {};
    _snwprintf_s(directory, MAX_PATH, _TRUNCATE, L"%ls\\CacheTest-%lu-%llu", parent.c_str(),
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long long>(GetTickCount64()));
    if (!CreateDirectoryW(directory, nullptr)) {
        std::printf("[FAIL] create temp directory: error=%lu path=%ls\n", GetLastError(), directory);
        return 3;
    }
    const std::wstring cache = std::wstring(directory) + L"\\tray_callbacks.txt";

    Check(AddLine(cache, L"alpha.exe|0x0400|AlphaWindow|1|both|4"), "first update creates a cache");

    std::vector<std::wstring> lines;
    Check(island::TrayCallbackCache::Read(&lines, cache), "cache can be read");
    Check(lines.size() == 2 && lines[0] == L"# islandbar-tray-callbacks-v2",
          "cache carries the schema header");

    constexpr int kThreads = 8;
    std::vector<std::thread> workers;
    std::vector<bool> ok(kThreads, false);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([i, &ok, &cache]() {
            wchar_t line[128] = {};
            _snwprintf_s(line, 128, _TRUNCATE, L"app%02d.exe|0x04%02X|Window%02d|%d|both|4", i, i, i, i);
            ok[static_cast<size_t>(i)] = AddLine(cache, line);
        });
    }
    for (std::thread& worker : workers) worker.join();
    Check(std::all_of(ok.begin(), ok.end(), [](bool value) { return value; }),
          "eight concurrent updates all report success");

    lines.clear();
    const bool reread = island::TrayCallbackCache::Read(&lines, cache);
    int records = 0;
    for (const std::wstring& line : lines) {
        if (!line.empty() && line[0] != L'#') ++records;
    }
    Check(reread && records == 9, "no concurrent update was lost");

    const std::wstring legacy = std::wstring(directory) + L"\\legacy.txt";
    Check(WriteAscii(legacy, "# islandbar-tray-callbacks-v1\r\nlegacy.exe|0x0400|LegacyWindow|1|both|4\r\n"),
          "legacy v1 cache can be prepared");
    lines.clear();
    Check(island::TrayCallbackCache::Read(&lines, legacy) && lines.size() == 2,
          "legacy v1 cache remains readable");
    Check(AddLine(legacy, L"new.exe|0x0401|NewWindow|2|both|4"), "legacy cache can be updated");
    lines.clear();
    Check(island::TrayCallbackCache::Read(&lines, legacy) && !lines.empty() &&
              lines[0] == L"# islandbar-tray-callbacks-v2",
          "a legacy update migrates the schema to v2");

    const HANDLE exclusive = CreateFileW(cache.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(exclusive != INVALID_HANDLE_VALUE, "cache opened for failure-preservation test");
    const bool failedUpdate = !AddLine(cache, L"should-not-appear.exe|0x0500|NoWindow|0|both|4");
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    Check(failedUpdate, "update fails while the cache cannot be replaced");

    lines.clear();
    const bool oldStillReadable = island::TrayCallbackCache::Read(&lines, cache);
    bool shouldNotAppear = false;
    for (const std::wstring& line : lines) {
        if (line.find(L"should-not-appear.exe") != std::wstring::npos) shouldNotAppear = true;
    }
    Check(oldStillReadable && !shouldNotAppear, "failed update leaves the old cache complete");

    DeleteFileW(cache.c_str());
    DeleteFileW(legacy.c_str());
    RemoveDirectoryW(directory);

    std::printf("RESULT: %s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
