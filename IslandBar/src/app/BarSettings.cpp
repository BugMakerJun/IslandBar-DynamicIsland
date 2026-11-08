// =====================================================================
//  BarSettings.cpp -- see the header for what this file is for
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================
#include "app/BarSettings.h"

#include "core/Log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace island {
namespace {

const wchar_t* kFileName = L"bar.txt";

// The same home the theme, the Start card's folder list and the tray's cache live in:
// %USERPROFILE%\IslandBar\<leaf>. Written here rather than shared because each of those files already
// carries its own copy of this two-line rule, and a settings module that reached into the theme's would
// make the theme's path its dependency.
bool DataPath(const wchar_t* leaf, wchar_t* out, size_t count) {
    wchar_t home[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    _snwprintf_s(out, count, _TRUNCATE, L"%s\\IslandBar\\%s", home, leaf);
    return true;
}

// The value of `key` in a key=value file, or empty. Comments (#) and blank lines are skipped; the first
// line that names the key wins, which is the same rule the theme's parser follows.
std::string ReadValue(const char* text, size_t length, const char* key) {
    const size_t keyLength = std::strlen(key);
    size_t at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n') ++end;
        size_t lineEnd = end;
        if (lineEnd > at && text[lineEnd - 1] == '\r') --lineEnd;

        size_t start = at;
        while (start < lineEnd && (text[start] == ' ' || text[start] == '\t')) ++start;
        if (start < lineEnd && text[start] != '#' && lineEnd - start > keyLength &&
            std::strncmp(text + start, key, keyLength) == 0 && text[start + keyLength] == '=') {
            size_t valueStart = start + keyLength + 1;
            while (valueStart < lineEnd && (text[valueStart] == ' ' || text[valueStart] == '\t')) {
                ++valueStart;
            }
            size_t valueEnd = lineEnd;
            while (valueEnd > valueStart && (text[valueEnd - 1] == ' ' || text[valueEnd - 1] == '\t')) {
                --valueEnd;
            }
            return std::string(text + valueStart, valueEnd - valueStart);
        }
        at = end + 1;
    }
    return std::string();
}

} // namespace

BarShape LoadBarShape() {
    wchar_t path[MAX_PATH] = {};
    if (!DataPath(kFileName, path, MAX_PATH)) {
        LogAndPrint(L"  bar shape: capsule (no home directory to read %ls from)", kFileName);
        return BarShape::Capsule;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") != 0 || !file) {
        LogAndPrint(L"  bar shape: capsule (no %ls -- the default)", kFileName);
        return BarShape::Capsule;
    }
    char buffer[1024] = {};
    const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);

    const std::string value = ReadValue(buffer, read, "shape");
    if (value.empty()) {
        LogAndPrint(L"  bar shape: capsule (%ls names no shape -- the default)", kFileName);
        return BarShape::Capsule;
    }
    BarShape shape = BarShape::Capsule;
    if (!BarShapeFromText(value.c_str(), &shape)) {
        // REFUSED rather than ignored: the file says something, and this build does not know it.
        LogAndPrint(L"  bar shape: capsule (%ls says shape=%hs, which this build does not know)", kFileName,
                    value.c_str());
        return BarShape::Capsule;
    }
    LogAndPrint(L"  bar shape: %ls (from %ls)", BarShapeName(shape), kFileName);
    return shape;
}

bool SaveBarShape(BarShape shape) {
    wchar_t path[MAX_PATH] = {};
    if (!DataPath(kFileName, path, MAX_PATH)) return false;

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) {
        LogAndPrint(L"  bar shape: %ls -- NOT saved (%ls could not be written)", BarShapeName(shape),
                    kFileName);
        return false;
    }
    std::fprintf(file, "# The bar's own settings. One key per line.\n");
    std::fprintf(file, "# shape: capsule (floating pill) or notch (hangs from the screen's top edge).\n");
    std::fprintf(file, "shape=%s\n", BarShapeKey(shape));
    std::fclose(file);
    LogAndPrint(L"  bar shape: %ls -- saved to %ls", BarShapeName(shape), kFileName);
    return true;
}

} // namespace island
