#include "shell/TrayCallbackCache.h"

#include "core/Log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace island {
namespace {

constexpr wchar_t kSchemaHeader[] = L"# islandbar-tray-callbacks-v2";
constexpr wchar_t kLegacySchemaHeader[] = L"# islandbar-tray-callbacks-v1";
constexpr uint64_t kMaxCacheBytes = 4ull * 1024ull * 1024ull;

std::mutex g_cacheLock;
std::atomic<unsigned> g_tempCounter{0};

std::wstring ResolvePath(const std::wstring& supplied) {
    if (!supplied.empty()) return supplied;
    return TrayCallbackCache::DefaultPath();
}

bool ReadUtf8File(const std::wstring& path, std::vector<std::wstring>* lines) {
    lines->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return true;
        LogAndPrint(L"  tray cache: read open failed: %ls (error %lu)\n", path.c_str(), GetLastError());
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxCacheBytes) {
        LogAndPrint(L"  tray cache: refusing '%ls' -- invalid or oversized cache\n", path.c_str());
        CloseHandle(file);
        return false;
    }

    std::vector<char> bytes(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD wanted = static_cast<DWORD>(
            (bytes.size() - offset) > 0x10000u ? 0x10000u : (bytes.size() - offset));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + offset, wanted, &read, nullptr) || read == 0) {
            LogAndPrint(L"  tray cache: read failed: %ls (error %lu)\n", path.c_str(), GetLastError());
            CloseHandle(file);
            return false;
        }
        offset += read;
    }
    CloseHandle(file);

    size_t byteOffset = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        byteOffset = 3;
    }
    const int byteCount = static_cast<int>(bytes.size() - byteOffset);
    if (byteCount == 0) return true;
    const int wideCount =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + byteOffset, byteCount, nullptr, 0);
    if (wideCount <= 0) {
        LogAndPrint(L"  tray cache: invalid UTF-8 in '%ls'\n", path.c_str());
        return false;
    }
    std::wstring text(static_cast<size_t>(wideCount), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + byteOffset, byteCount, text.data(),
                        wideCount);

    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find(L'\n', start);
        std::wstring line =
            end == std::wstring::npos ? text.substr(start) : text.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.find(L'\0') != std::wstring::npos) {
            LogAndPrint(L"  tray cache: embedded NUL in '%ls'\n", path.c_str());
            lines->clear();
            return false;
        }
        lines->push_back(std::move(line));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    for (const std::wstring& line : *lines) {
        if (line.empty()) continue;
        if (line[0] != L'#') break;
        if (line == kSchemaHeader || line == kLegacySchemaHeader) return true;
        if (line.rfind(L"# islandbar-tray-callbacks-", 0) == 0) {
            LogAndPrint(L"  tray cache: unsupported schema in '%ls': %ls\n", path.c_str(), line.c_str());
            lines->clear();
            return false;
        }
    }

    // A cache written before the schema header was introduced is still accepted. The next successful update
    // adds the header in place; a read alone never mutates the user's file.
    return true;
}

bool EnsureParentDirectory(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return true;
    const std::wstring directory = path.substr(0, slash);
    if (CreateDirectoryW(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    LogAndPrint(L"  tray cache: cannot create directory '%ls' (error %lu)\n", directory.c_str(), GetLastError());
    return false;
}

bool WriteAll(HANDLE file, const char* bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const DWORD wanted =
            static_cast<DWORD>((size - offset) > 0x10000u ? 0x10000u : (size - offset));
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, wanted, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

bool WriteAtomic(const std::wstring& path, const std::vector<std::wstring>& sourceLines) {
    if (!EnsureParentDirectory(path)) return false;

    std::wstring text;
    text.reserve(4096);
    text += kSchemaHeader;
    text += L"\r\n";
    for (const std::wstring& line : sourceLines) {
        if (line == kSchemaHeader) continue;
        text += line;
        text += L"\r\n";
    }

    const int byteCount =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) return false;
    std::vector<char> bytes(static_cast<size_t>(byteCount), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), bytes.data(), byteCount, nullptr,
                        nullptr);

    wchar_t temp[MAX_PATH] = {};
    _snwprintf_s(temp, MAX_PATH, _TRUNCATE, L"%ls.tmp.%lu.%u", path.c_str(),
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 g_tempCounter.fetch_add(1, std::memory_order_relaxed));

    HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LogAndPrint(L"  tray cache: temp open failed: %ls (error %lu)\n", temp, GetLastError());
        return false;
    }
    const bool written = WriteAll(file, bytes.data(), bytes.size());
    const bool flushed = written && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written || !flushed) {
        LogAndPrint(L"  tray cache: temp write failed: %ls (error %lu)\n", temp, GetLastError());
        DeleteFileW(temp);
        return false;
    }

    bool replaced = false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        replaced = MoveFileExW(temp, path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    } else {
        replaced = ReplaceFileW(path.c_str(), temp, nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
    }
    if (!replaced) {
        LogAndPrint(L"  tray cache: atomic replace failed: %ls (error %lu)\n", path.c_str(), GetLastError());
        DeleteFileW(temp);
    }
    return replaced;
}

} // namespace

std::wstring TrayCallbackCache::DefaultPath() {
    wchar_t home[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::wstring();
    return std::wstring(home) + L"\\IslandBar\\tray_callbacks.txt";
}

bool TrayCallbackCache::Read(std::vector<std::wstring>* lines, const std::wstring& path) {
    if (!lines) return false;
    const std::wstring resolved = ResolvePath(path);
    if (resolved.empty()) return false;
    std::lock_guard<std::mutex> lock(g_cacheLock);
    return ReadUtf8File(resolved, lines);
}

bool TrayCallbackCache::Update(const std::function<bool(std::vector<std::wstring>&)>& mutate,
                               const std::wstring& path) {
    if (!mutate) return false;
    const std::wstring resolved = ResolvePath(path);
    if (resolved.empty()) return false;
    std::lock_guard<std::mutex> lock(g_cacheLock);

    std::vector<std::wstring> lines;
    if (!ReadUtf8File(resolved, &lines)) return false;
    if (!mutate(lines)) return true;
    return WriteAtomic(resolved, lines);
}

} // namespace island
