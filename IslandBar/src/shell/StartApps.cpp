// =====================================================================
//  StartApps.cpp -- see the header for where the list comes from, and for why the pins
//  are a folder of shortcuts.
// =====================================================================
#include "shell/StartApps.h"

#include "core/Log.h"
#include "media/WicImage.h"

#include <shellapi.h>
#include <appmodel.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propkey.h>      // PKEY_AppUserModel_ID
#include <propvarutil.h>  // InitPropVariantFromString

#include <algorithm>
#include <cstdio>
#include <thread>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")

namespace island {
namespace {

// THE SIZE THE START CARD'S ICONS ARE CAPTURED AT.
//
// The card draws them at 56 DIP (see kStartAppIconDip in domain/CardLayout.h). A 32 DIP snapshot -- the old
// size, when the tile was 28 -- was upscaled into 56 and read as a soft square in a card whose reference has
// crisp icons. The shell can hand out a larger image from the same source, so it is asked for at 64 and
// scaled DOWN to 56, which is the direction an icon wants to be scaled in.
constexpr int kStartIconCaptureSide = 64;

bool BitmapToPng(HBITMAP bitmap, int side, std::vector<uint8_t>* png) {
    if (!bitmap || !png || side <= 0) return false;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = side;
    info.bmiHeader.biHeight = -side;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(static_cast<size_t>(side) * side * 4, 0);

    HDC screen = GetDC(nullptr);
    bool ok = false;
    if (screen) {
        ok = GetDIBits(screen, bitmap, 0, static_cast<UINT>(side), pixels.data(), &info,
                       DIB_RGB_COLORS) != 0;
        ReleaseDC(nullptr, screen);
    }
    if (!ok) return false;

    uint8_t scratch[32 * 1024] = {};
    const size_t size = EncodeBgraToPng(pixels.data(), side, side, scratch, sizeof(scratch));
    if (size == 0) return false;
    png->assign(scratch, scratch + size);
    return true;
}

bool IconToPng(HICON icon, int side, std::vector<uint8_t>* png) {
    if (!icon || !png || side <= 0) return false;
    ICONINFO iconInfo = {};
    if (!GetIconInfo(icon, &iconInfo)) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) {
        if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
        if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
        return false;
    }

    BITMAPINFO colorInfo = {};
    colorInfo.bmiHeader.biSize = sizeof(colorInfo.bmiHeader);
    colorInfo.bmiHeader.biWidth = side;
    colorInfo.bmiHeader.biHeight = -side;
    colorInfo.bmiHeader.biPlanes = 1;
    colorInfo.bmiHeader.biBitCount = 32;
    colorInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP drawing = CreateDIBSection(screen, &colorInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC memory = CreateCompatibleDC(screen);
    HGDIOBJ old = drawing && memory ? SelectObject(memory, drawing) : nullptr;
    bool ok = drawing && memory && pixels;
    if (ok) {
        PatBlt(memory, 0, 0, side, side, BLACKNESS);
        ok = DrawIconEx(memory, 0, 0, icon, side, side, 0, nullptr, DI_NORMAL) != FALSE;
    }
    if (old) SelectObject(memory, old);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    std::vector<uint8_t> colorPixels;
    if (ok) {
        const auto* bytes = static_cast<const uint8_t*>(pixels);
        colorPixels.assign(bytes, bytes + static_cast<size_t>(side) * side * 4);
    }
    if (drawing) DeleteObject(drawing);

    if (ok && iconInfo.hbmMask) {
        BITMAPINFO maskInfo = {};
        maskInfo.bmiHeader.biSize = sizeof(maskInfo.bmiHeader);
        maskInfo.bmiHeader.biWidth = side;
        maskInfo.bmiHeader.biHeight = -side;
        maskInfo.bmiHeader.biPlanes = 1;
        maskInfo.bmiHeader.biBitCount = 1;
        maskInfo.bmiHeader.biCompression = BI_RGB;
        const size_t maskStride = ((static_cast<size_t>(side) + 31) / 32) * 4;
        std::vector<uint8_t> mask(maskStride * static_cast<size_t>(side));
        HDC maskDc = GetDC(nullptr);
        const bool haveMask =
            maskDc && GetDIBits(maskDc, iconInfo.hbmMask, 0, static_cast<UINT>(side), mask.data(),
                                &maskInfo, DIB_RGB_COLORS) != 0;
        if (maskDc) ReleaseDC(nullptr, maskDc);
        if (haveMask) {
            for (int y = 0; y < side; ++y) {
                for (int x = 0; x < side; ++x) {
                    const size_t bit = static_cast<size_t>(y) * maskStride +
                                       static_cast<size_t>(x / 8);
                    const bool transparent = (mask[bit] & (0x80u >> (x % 8))) != 0;
                    const size_t alpha = (static_cast<size_t>(y) * side + x) * 4 + 3;
                    if (transparent) colorPixels[alpha] = 0;
                    else if (colorPixels[alpha] == 0) colorPixels[alpha] = 255;
                }
            }
        }
    }
    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
    if (!ok) return false;

    uint8_t scratch[32 * 1024] = {};
    const size_t size =
        EncodeBgraToPng(colorPixels.data(), side, side, scratch, sizeof(scratch));
    if (size == 0) return false;
    png->assign(scratch, scratch + size);
    return true;
}

bool EndsWithNoCase(const std::wstring& value, const wchar_t* suffix) {
    const size_t suffixLength = suffix ? std::wcslen(suffix) : 0;
    return suffixLength > 0 && value.size() >= suffixLength &&
           _wcsicmp(value.c_str() + value.size() - suffixLength, suffix) == 0;
}

struct ShortcutTarget {
    std::wstring path;
    std::wstring iconPath;
    int iconIndex = 0;
};

ShortcutTarget ReadShortcutTarget(const wchar_t* target) {
    ShortcutTarget result;
    if (!target || !*target) return result;
    const std::wstring targetText(target);
    if (!EndsWithNoCase(targetText, L".lnk")) {
        result.path = targetText;
        return result;
    }

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) ||
        !link) {
        return result;
    }
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file &&
        SUCCEEDED(file->Load(target, STGM_READ))) {
        wchar_t path[MAX_PATH] = {};
        if (SUCCEEDED(link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH)) && path[0]) {
            result.path = path;
        }
        wchar_t iconPath[MAX_PATH] = {};
        int iconIndex = 0;
        if (SUCCEEDED(link->GetIconLocation(iconPath, MAX_PATH, &iconIndex)) && iconPath[0]) {
            result.iconPath = iconPath;
            result.iconIndex = iconIndex;
        }
    }
    if (file) file->Release();
    link->Release();
    return result;
}

bool ExtractPng(const std::wstring& path, int iconIndex, int side,
                std::vector<uint8_t>* png) {
    if (path.empty() || !png) return false;
    HICON iconLarge = nullptr;
    HICON iconSmall = nullptr;
    if (ExtractIconExW(path.c_str(), iconIndex, &iconLarge, &iconSmall, 1) == 0) return false;
    const bool ok = IconToPng(iconLarge ? iconLarge : iconSmall, side, png);
    if (iconLarge) DestroyIcon(iconLarge);
    if (iconSmall) DestroyIcon(iconSmall);
    return ok;
}

bool ShellFileIconPng(const std::wstring& path, int side, std::vector<uint8_t>* png) {
    if (path.empty() || !png) return false;
    SHFILEINFOW info = {};
    if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                        SHGFI_ICON | SHGFI_LARGEICON) ||
        !info.hIcon) {
        return false;
    }
    const bool ok = IconToPng(info.hIcon, side, png);
    DestroyIcon(info.hIcon);
    return ok;
}

// `shell:AppsFolder` as a shell item, which is the only handle on the list the shell's own "all apps"
// view is built from.
IShellItem* AppsFolderItem() {
    IShellItem* folder = nullptr;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder)))) {
        return nullptr;
    }
    return folder;
}

// One item's name and target, from the item itself rather than from a string built here: the parsing name
// is what the shell uses for the same item, in whichever of the three shapes it has.
bool ReadItem(IShellItem* item, StartApp* out) {
    if (!item || !out) return false;
    PWSTR name = nullptr;
    PWSTR target = nullptr;
    const bool haveName = SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name;
    // SIGDN_PARENTRELATIVEPARSING is the name within `shell:AppsFolder`, which is exactly what
    // ShellExecute wants appended to it.
    const bool haveTarget =
        SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &target)) && target;
    if (haveName && haveTarget) {
        wcsncpy_s(out->name, 112, name, _TRUNCATE);
        wcsncpy_s(out->target, 512, target, _TRUNCATE);
    }
    if (name) CoTaskMemFree(name);
    if (target) CoTaskMemFree(target);
    return haveName && haveTarget;
}

// Sorts the way the shell's own list reads: by the name the user sees, in the user's language.
bool NameBefore(const StartApp& a, const StartApp& b) {
    const int order = CompareStringEx(LOCALE_NAME_USER_DEFAULT, 0, a.name, -1, b.name, -1, nullptr,
                                      nullptr, 0);
    if (order == CSTR_LESS_THAN) return true;
    if (order == CSTR_GREATER_THAN) return false;
    return std::wcscmp(a.target, b.target) < 0;
}

// The last path component without its extension: what a shortcut in the folder is called, which is what
// the tile shows. The user renames it in Explorer to name the tile.
std::wstring StemOf(const wchar_t* path) {
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    std::wstring name = slash ? std::wstring(slash + 1) : std::wstring(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

// A display name that can be a file name: the characters Windows reserves are replaced, because a pin
// called "A/B" would otherwise fail to save with an error nobody reads.
std::wstring FileNameFrom(const wchar_t* name) {
    std::wstring out;
    for (const wchar_t* p = name; p && *p; ++p) {
        const wchar_t c = *p;
        out.push_back((c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
                       c == L'<' || c == L'>' || c == L'|')
                          ? L'_'
                          : c);
    }
    if (out.empty()) out = L"Shortcut";
    return out;
}

// One shortcut in the folder: named `name`, launching the AppsFolder item `target`.
//
// The ID list of the ITEM, rather than a path: that is how the shell's own Start menu stores a shortcut to
// a packaged application or to one of the `{GUID}\name.exe` folder items, and it is what makes one call
// -- ShellExecute on the .lnk -- launch all three shapes.
bool CreateShortcut(const std::wstring& folder, const std::wstring& name, const wchar_t* appsTarget) {
    if (folder.empty() || name.empty() || !appsTarget || !*appsTarget) return false;
    wchar_t parsing[600] = {};
    std::swprintf(parsing, 600, L"shell:AppsFolder\\%ls", appsTarget);

    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(parsing, nullptr, IID_PPV_ARGS(&item))) || !item) {
        LogLine(L"  start: no shortcut: '%ls' is not a shell item", parsing);
        return false;
    }
    PIDLIST_ABSOLUTE idList = nullptr;
    SHGetIDListFromObject(item, &idList);
    item->Release();

    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr) || !link) {
        if (idList) CoTaskMemFree(idList);
        return false;
    }
    if (idList) link->SetIDList(idList);
    link->SetDescription(appsTarget);

    // ... AND THE APPUSERMODELID FOR A PACKAGED APPLICATION, which is the property the shell's own
    // shortcuts carry and the one the taskbar identifies an application by. Without it a packaged
    // application's shortcut launches but is not "the same application" afterwards.
    if (std::wcschr(appsTarget, L'!') != nullptr) {
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store))) && store) {
            PROPVARIANT value;
            if (SUCCEEDED(InitPropVariantFromString(appsTarget, &value))) {
                store->SetValue(PKEY_AppUserModel_ID, value);
                store->Commit();
                PropVariantClear(&value);
            }
            store->Release();
        }
    }

    wchar_t path[MAX_PATH] = {};
    std::swprintf(path, MAX_PATH, L"%ls\\%ls.lnk", folder.c_str(), name.c_str());
    IPersistFile* file = nullptr;
    bool saved = false;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file) {
        saved = SUCCEEDED(file->Save(path, TRUE));
        file->Release();
    }
    link->Release();
    if (idList) CoTaskMemFree(idList);
    LogLine(L"  start: shortcut '%ls' -> %ls", saved ? L"written" : L"FAILED", path);
    return saved;
}

// Deletes a shortcut to the RECYCLE BIN: a pin is the user's, and a right-click that removes it for good
// is not a reasonable thing for a menu to do. (FOF_ALLOWUNDO without FOF_NOCONFIRMATION would ask; with
// both it goes quietly and is still recoverable.)
bool RecycleFile(const std::wstring& path) {
    if (path.empty()) return false;
    // The list is double-null-terminated, and the buffer is not owned by SHFileOperation.
    wchar_t from[MAX_PATH + 2] = {};
    wcsncpy_s(from, MAX_PATH, path.c_str(), _TRUNCATE);
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const int result = SHFileOperationW(&op);
    LogLine(L"  start: recycle '%ls' -> %s", path.c_str(), result == 0 ? "ok" : "FAILED");
    return result == 0;
}

// Reads the first line of a small text file, skipping `#` comments and blank lines. Empty when there is
// no file, or nothing in it that says anything.
std::wstring FirstSettingLine(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || !file) return std::wstring();
    wchar_t line[1024] = {};
    std::wstring found;
    while (fgetws(line, 1024, file)) {
        std::wstring text(line);
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' ||
                                 text.back() == L'\t')) {
            text.pop_back();
        }
        while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
            text.erase(text.begin());
        }
        if (text.empty() || text[0] == L'#') continue;
        found = text;
        break;
    }
    std::fclose(file);
    return found;
}

} // namespace

StartApps::~StartApps() {
    StopIconWorker();
}

void StartApps::PinsFolderSettingPath(wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    wchar_t* lastSlash = std::wcsrchr(module, L'\\');
    if (!lastSlash) return;
    *(lastSlash + 1) = L'\0';
    std::swprintf(out, capacity, L"%lsstart_folder.txt", module);
}

void StartApps::PinsFolderPath(wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    wchar_t setting[MAX_PATH] = {};
    PinsFolderSettingPath(setting, MAX_PATH);
    if (setting[0] == L'\0') return;

    const std::wstring chosen = FirstSettingLine(setting);
    if (!chosen.empty()) {
        wcsncpy_s(out, capacity, chosen.c_str(), _TRUNCATE);
        return;
    }

    // No setting yet: the default is a folder beside the bar, and the file that names it is WRITTEN with
    // the answer in it -- the question "where do I change this" is one the file itself closes.
    wchar_t module[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) return;
    wchar_t* lastSlash = std::wcsrchr(module, L'\\');
    if (!lastSlash) return;
    *(lastSlash + 1) = L'\0';
    std::swprintf(out, capacity, L"%lsStart", module);

    FILE* file = nullptr;
    if (_wfopen_s(&file, setting, L"w, ccs=UTF-8") == 0 && file) {
        std::fwprintf(file,
                      L"# IslandBar 开始卡片里固定的应用放在哪个文件夹。\n"
                      L"# 改这一行就能换地方（文件夹不存在会自动建）；\n"
                      L"# 里面放快捷方式（.lnk）就行，名字就是卡片上显示的名字，\n"
                      L"# 想排序就用 01-、02- 这样的前缀，想删掉就在卡片里右键那个图标。\n"
                      L"%ls\n",
                      out);
        std::fclose(file);
    }
}

int StartApps::RefreshList() {
    list_.clear();
    IShellItem* folderItem = AppsFolderItem();
    if (!folderItem) {
        LogLine(L"  start: shell:AppsFolder could not be opened");
        return 0;
    }
    IShellFolder* folder = nullptr;
    HRESULT hr = folderItem->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder));
    folderItem->Release();
    if (FAILED(hr) || !folder) {
        LogLine(L"  start: shell:AppsFolder is not a folder (0x%08X)", static_cast<unsigned>(hr));
        return 0;
    }

    IEnumIDList* items = nullptr;
    const HWND owner = GetShellWindow();
    hr = folder->EnumObjects(owner, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &items);
    if (FAILED(hr) || !items) {
        folder->Release();
        LogLine(L"  start: shell:AppsFolder could not be enumerated (0x%08X)", static_cast<unsigned>(hr));
        return 0;
    }

    ITEMIDLIST* child = nullptr;
    while (items->Next(1, &child, nullptr) == S_OK) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemWithParent(nullptr, folder, child, IID_PPV_ARGS(&item))) && item) {
            StartApp app;
            if (ReadItem(item, &app) && app.name[0] != L'\0' && app.target[0] != L'\0') {
                list_.push_back(app);
            }
            item->Release();
        }
        CoTaskMemFree(child);
    }
    items->Release();
    folder->Release();

    std::sort(list_.begin(), list_.end(), NameBefore);
    ++revision_;
    LogLine(L"  start: %d application(s) in shell:AppsFolder", static_cast<int>(list_.size()));
    // Reserve one slot per application but do not start extracting 121 images while the card is closed.
    // RequestMissingIcons starts that work only when the Start card is actually open.
    listIcons_.assign(list_.size(), std::vector<uint8_t>());
    return static_cast<int>(list_.size());
}

void StartApps::ReadPinsFolder(const wchar_t* folder) {
    std::vector<StartApp> fresh;
    std::vector<std::wstring> paths;
    if (folder && *folder) {
        for (const wchar_t* pattern : {L"\\*.lnk", L"\\*.url"}) {
            wchar_t search[MAX_PATH] = {};
            std::swprintf(search, MAX_PATH, L"%ls%ls", folder, pattern);
            WIN32_FIND_DATAW found = {};
            HANDLE handle = FindFirstFileW(search, &found);
            if (handle == INVALID_HANDLE_VALUE) continue;
            do {
                if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                wchar_t full[MAX_PATH] = {};
                std::swprintf(full, MAX_PATH, L"%ls\\%ls", folder, found.cFileName);
                paths.push_back(full);
            } while (FindNextFileW(handle, &found));
            FindClose(handle);
        }
    }

    // Sorted by the shortcuct's own name -- which is the tile's name -- in the user's own language. The
    // order is therefore something the user controls, by renaming the files: the classic `01-` prefix
    // works, and so does simply moving them about in Explorer if that is clearer.
    std::sort(paths.begin(), paths.end(), [](const std::wstring& a, const std::wstring& b) {
        const std::wstring left = StemOf(a.c_str());
        const std::wstring right = StemOf(b.c_str());
        const int order = CompareStringEx(LOCALE_NAME_USER_DEFAULT, 0, left.c_str(), -1, right.c_str(),
                                          -1, nullptr, nullptr, 0);
        if (order == CSTR_LESS_THAN) return true;
        if (order == CSTR_GREATER_THAN) return false;
        return a < b;
    });

    for (const std::wstring& path : paths) {
        StartApp pinned;
        const std::wstring name = StemOf(path.c_str());
        wcsncpy_s(pinned.name, 112, name.c_str(), _TRUNCATE);
        // The SHORTCUT is the target: ShellExecute on a .lnk launches whatever it points at, whichever of
        // the three shapes that is.
        wcsncpy_s(pinned.target, 512, path.c_str(), _TRUNCATE);
        fresh.push_back(std::move(pinned));
    }

    pins_ = std::move(fresh);
    pinIcons_.assign(pins_.size(), std::vector<uint8_t>());
    ++revision_;
}

void StartApps::LoadPins() {
    wchar_t folder[MAX_PATH] = {};
    PinsFolderPath(folder, MAX_PATH);
    if (folder[0] == L'\0') {
        LogLine(L"  start: no pins folder could be worked out");
        return;
    }
    pinsFolder_ = folder;
    CreateDirectoryW(folder, nullptr);

    // MIGRATION, ONCE, from the text file the first version used: a file of `name|target` lines, which the
    // user had already filled in when an update appeared to throw it away (it was in fact being read before
    // the shell's application list arrived, and every line was rejected -- see the header). Rather than ask
    // him to pin thirteen things again, they are turned into shortcuts here, and the file is renamed so
    // this happens once.
    wchar_t legacy[MAX_PATH] = {};
    PinsFolderSettingPath(legacy, MAX_PATH);
    if (legacy[0] != L'\0') {
        std::wstring oldPath = legacy;
        const size_t nameAt = oldPath.find_last_of(L'\\');
        oldPath = oldPath.substr(0, nameAt + 1) + L"start_pins.txt";
        FILE* file = nullptr;
        if (_wfopen_s(&file, oldPath.c_str(), L"r, ccs=UTF-8") == 0 && file) {
            int migrated = 0;
            wchar_t line[1024] = {};
            while (fgetws(line, 1024, file)) {
                std::wstring text(line);
                while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' ||
                                         text.back() == L' ' || text.back() == L'\t')) {
                    text.pop_back();
                }
                if (text.empty() || text[0] == L'#') continue;
                const size_t bar = text.find(L'|');
                const std::wstring name = (bar == std::wstring::npos) ? text : text.substr(0, bar);
                const std::wstring target = (bar == std::wstring::npos) ? text : text.substr(bar + 1);
                if (CreateShortcut(std::wstring(folder), FileNameFrom(name.c_str()), target.c_str())) {
                    ++migrated;
                }
            }
            std::fclose(file);
            LogLine(L"  start: migrated %d pin(s) from start_pins.txt into %ls", migrated, folder);
            std::wstring retired = oldPath + L".migrated";
            if (!MoveFileExW(oldPath.c_str(), retired.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                LogLine(L"  start: could not rename %ls -- it will be read again next start",
                        oldPath.c_str());
            }
        }
    }

    ReadPinsFolder(folder);
    LogLine(L"  start: %d pinned application(s) from %ls", static_cast<int>(pins_.size()), folder);
}

bool StartApps::Pin(const wchar_t* target, const wchar_t* name) {
    if (!target || !*target || !name || !*name) return false;
    if (pinsFolder_.empty()) return false;
    const std::wstring file = FileNameFrom(name);
    if (!CreateShortcut(pinsFolder_, file, target)) return false;
    ReadPinsFolder(pinsFolder_.c_str());
    RequestMissingIcons();
    return true;
}

bool StartApps::Unpin(int index) {
    if (index < 0 || index >= static_cast<int>(pins_.size())) return false;
    const std::wstring path = pins_[static_cast<size_t>(index)].target;
    if (!RecycleFile(path)) return false;
    ReadPinsFolder(pinsFolder_.c_str());
    return true;
}

const uint8_t* StartApps::PinIconPng(int index, size_t* sizeOut) const {
    if (sizeOut) *sizeOut = 0;
    if (index < 0 || index >= static_cast<int>(pins_.size())) return nullptr;
    if (index >= static_cast<int>(pinIcons_.size())) return nullptr;
    const std::vector<uint8_t>& icon = pinIcons_[static_cast<size_t>(index)];
    if (icon.empty()) return nullptr;
    if (sizeOut) *sizeOut = icon.size();
    return icon.data();
}

const uint8_t* StartApps::ListIconPng(int index, size_t* sizeOut) const {
    if (sizeOut) *sizeOut = 0;
    if (index < 0 || index >= static_cast<int>(list_.size())) return nullptr;
    if (index >= static_cast<int>(listIcons_.size())) return nullptr;
    const std::vector<uint8_t>& icon = listIcons_[static_cast<size_t>(index)];
    if (icon.empty()) return nullptr;
    if (sizeOut) *sizeOut = icon.size();
    return icon.data();
}

void StartApps::ResolveIconAsync(const wchar_t* target, bool pin) {
    if (!target || !*target) return;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(iconMutex_);
        for (const IconRequest& request : iconQueue_) {
            if (request.pin == pin && _wcsicmp(request.target.c_str(), target) == 0) {
                queued = true;
                break;
            }
        }
        if (queued) return;
        iconQueue_.push_back({std::wstring(target), pin});
        iconRequested_.fetch_add(1, std::memory_order_relaxed);
    }
    StartIconWorker();
    if (iconWakeEvent_) SetEvent(iconWakeEvent_);
}

void StartApps::RequestMissingIcons() {
    for (int i = 0; i < static_cast<int>(pins_.size()); ++i) {
        if (i >= static_cast<int>(pinIcons_.size()) ||
            pinIcons_[static_cast<size_t>(i)].empty()) {
            ResolveIconAsync(pins_[static_cast<size_t>(i)].target, true);
        }
    }
    for (int i = 0; i < static_cast<int>(list_.size()); ++i) {
        if (i >= static_cast<int>(listIcons_.size()) ||
            listIcons_[static_cast<size_t>(i)].empty()) {
            ResolveIconAsync(list_[static_cast<size_t>(i)].target, false);
        }
    }
}

void StartApps::PrioritizeListIcon(int index) {
    if (index < 0 || index >= static_cast<int>(list_.size())) return;
    if (index < static_cast<int>(listIcons_.size()) &&
        !listIcons_[static_cast<size_t>(index)].empty()) {
        return;
    }
    const std::wstring target = list_[static_cast<size_t>(index)].target;
    {
        std::lock_guard<std::mutex> lock(iconMutex_);
        for (auto it = iconQueue_.begin(); it != iconQueue_.end(); ++it) {
            if (!it->pin && _wcsicmp(it->target.c_str(), target.c_str()) == 0) {
                iconQueue_.erase(it);
                break;
            }
        }
        iconQueue_.push_front({target, false});
        iconRequested_.fetch_add(1, std::memory_order_relaxed);
    }
    StartIconWorker();
    if (iconWakeEvent_) SetEvent(iconWakeEvent_);
}

void StartApps::DrainIconResults() {
    std::deque<IconResult> completed;
    {
        std::lock_guard<std::mutex> lock(iconMutex_);
        completed.swap(iconCompleted_);
    }
    bool changed = false;
    for (IconResult& result : completed) {
        if (result.png.empty()) continue;
        if (result.pin) {
            for (size_t i = 0; i < pins_.size() && i < pinIcons_.size(); ++i) {
                if (_wcsicmp(pins_[i].target, result.target.c_str()) == 0) {
                    pinIcons_[i] = std::move(result.png);
                    changed = true;
                    break;
                }
            }
        } else {
            for (size_t i = 0; i < list_.size() && i < listIcons_.size(); ++i) {
                if (_wcsicmp(list_[i].target, result.target.c_str()) == 0) {
                    listIcons_[i] = std::move(result.png);
                    changed = true;
                    break;
                }
            }
        }
    }
    if (changed) ++iconRevision_;
}

void StartApps::StopIconLoading() {
    {
        std::lock_guard<std::mutex> lock(iconMutex_);
        iconQueue_.clear();
    }
    StopIconWorker();
}

bool StartApps::StartIconWorker() {
    bool expected = false;
    if (!iconWorkerRunning_.compare_exchange_strong(expected, true)) return true;
    if (iconWakeEvent_) {
        CloseHandle(iconWakeEvent_);
        iconWakeEvent_ = nullptr;
    }
    iconWakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!iconWorker_.Prepare() || !iconWakeEvent_) {
        iconWorkerRunning_.store(false);
        if (iconWakeEvent_) {
            CloseHandle(iconWakeEvent_);
            iconWakeEvent_ = nullptr;
        }
        return false;
    }
    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, &StartApps::IconThreadEntry, this, 0, &threadId);
    if (!thread) {
        iconWorker_.Stop();
        CloseHandle(iconWakeEvent_);
        iconWakeEvent_ = nullptr;
        iconWorkerRunning_.store(false);
        return false;
    }
    iconWorker_.Attach(thread);
    return true;
}

void StartApps::StopIconWorker() {
    iconWorker_.Stop();
    if (iconWakeEvent_) {
        CloseHandle(iconWakeEvent_);
        iconWakeEvent_ = nullptr;
    }
    iconWorkerRunning_.store(false);
}

unsigned long __stdcall StartApps::IconThreadEntry(void* self) {
    static_cast<StartApps*>(self)->RunIconWorker();
    return 0;
}

void StartApps::RunIconWorker() {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool haveApartment = SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE;
    while (haveApartment) {
        IconRequest request;
        bool haveRequest = false;
        {
            std::lock_guard<std::mutex> lock(iconMutex_);
            if (!iconQueue_.empty()) {
                request = std::move(iconQueue_.front());
                iconQueue_.pop_front();
                haveRequest = true;
            }
        }
        if (!haveRequest) {
            HANDLE waits[2] = {iconWorker_.StopEvent(), iconWakeEvent_};
            const DWORD signalled = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (signalled == WAIT_OBJECT_0 || signalled == WAIT_FAILED) break;
            std::lock_guard<std::mutex> lock(iconMutex_);
            if (iconQueue_.empty()) {
                iconWorkerRunning_.store(false);
                break;
            }
            request = std::move(iconQueue_.front());
            iconQueue_.pop_front();
        }

        std::vector<uint8_t> png;
        if (!StartAppIconPng(request.target.c_str(), kStartIconCaptureSide, &png)) {
            iconFailed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        iconResolved_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(iconMutex_);
            iconCompleted_.push_back({std::move(request.target), request.pin, std::move(png)});
        }
    }
    if (haveApartment) CoUninitialize();
    iconWorkerRunning_.store(false);
    LogLine(L"  start.icons.summary requested=%d resolved=%d failed=%d threads_active=0",
            iconRequested_.load(std::memory_order_relaxed),
            iconResolved_.load(std::memory_order_relaxed),
            iconFailed_.load(std::memory_order_relaxed));
}

bool PackageIconPng(const wchar_t* aumid, int side, std::vector<uint8_t>* png) {
    if (!aumid || !png || !std::wcsstr(aumid, L"!")) return false;
    const wchar_t* bang = std::wcsrchr(aumid, L'!');
    if (!bang || bang == aumid) return false;
    const std::wstring family(aumid, static_cast<size_t>(bang - aumid));

    UINT32 packageCount = 0;
    UINT32 nameBufferLength = 0;
    LONG status = GetPackagesByPackageFamily(family.c_str(), &packageCount, nullptr,
                                             &nameBufferLength, nullptr);
    if (packageCount == 0 || nameBufferLength == 0) return false;
    std::vector<PWSTR> names(packageCount, nullptr);
    std::vector<wchar_t> nameBuffer(nameBufferLength);
    status = GetPackagesByPackageFamily(family.c_str(), &packageCount, names.data(),
                                        &nameBufferLength, nameBuffer.data());
    if (status != ERROR_SUCCESS || packageCount == 0 || !names[0]) return false;

    UINT32 pathLength = 0;
    status = GetPackagePathByFullName(names[0], &pathLength, nullptr);
    if (pathLength == 0) return false;
    std::vector<wchar_t> packagePath(pathLength);
    status = GetPackagePathByFullName(names[0], &pathLength, packagePath.data());
    if (status != ERROR_SUCCESS) return false;

    const std::wstring manifestPath = std::wstring(packagePath.data()) + L"\\AppxManifest.xml";
    FILE* manifest = nullptr;
    if (_wfopen_s(&manifest, manifestPath.c_str(), L"rb") != 0 || !manifest) return false;
    std::string manifestText;
    char chunk[4096] = {};
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), manifest)) > 0) {
        manifestText.append(chunk, read);
        if (manifestText.size() > 2 * 1024 * 1024) break;
    }
    std::fclose(manifest);

    const char* marker = "Square44x44Logo=\"";
    const size_t at = manifestText.find(marker);
    if (at == std::string::npos) return false;
    const size_t valueAt = at + std::strlen(marker);
    const size_t end = manifestText.find('"', valueAt);
    if (end == std::string::npos || end <= valueAt) return false;
    std::string relativeUtf8 = manifestText.substr(valueAt, end - valueAt);
    std::replace(relativeUtf8.begin(), relativeUtf8.end(), '/', '\\');
    wchar_t relative[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, relativeUtf8.c_str(), -1, relative, 512);
    if (!relative[0]) return false;

    std::wstring base = std::wstring(packagePath.data()) + L"\\" + relative;
    const size_t slash = base.find_last_of(L'\\');
    const size_t dot = base.find_last_of(L'.');
    const std::wstring folder = slash == std::wstring::npos ? base : base.substr(0, slash);
    const std::wstring stem = (dot == std::wstring::npos || dot < slash)
                                  ? base
                                  : base.substr(slash + 1, dot - slash - 1);
    const std::wstring extension = dot == std::wstring::npos ? L"" : base.substr(dot);

    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW((folder + L"\\" + stem + L"*" + extension).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    std::wstring best;
    int bestScore = -1;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        std::wstring candidate = found.cFileName;
        std::wstring folded = candidate;
        std::transform(folded.begin(), folded.end(), folded.begin(), ::towlower);
        int score = 0;
        if (folded == (stem + extension)) score = 5;
        else if (folded.find(L"targetsize-64_altform-unplated") != std::wstring::npos) score = 4;
        else if (folded.find(L"targetsize-64") != std::wstring::npos) score = 3;
        else if (folded.find(L"scale-200") != std::wstring::npos) score = 2;
        else if (folded.find(L"scale-100") != std::wstring::npos) score = 1;
        if (score > bestScore) {
            bestScore = score;
            best = folder + L"\\" + candidate;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (best.empty()) return false;

    FILE* imageFile = nullptr;
    if (_wfopen_s(&imageFile, best.c_str(), L"rb") != 0 || !imageFile) return false;
    std::vector<uint8_t> source(256 * 1024);
    const size_t sourceSize = std::fread(source.data(), 1, source.size(), imageFile);
    std::fclose(imageFile);
    if (sourceSize == 0) return false;
    uint8_t scratch[32 * 1024] = {};
    const size_t encoded = ReencodeToPng(source.data(), sourceSize, side, scratch, sizeof(scratch));
    if (encoded == 0) return false;
    png->assign(scratch, scratch + encoded);
    return true;
}

bool StartAppIconPng(const wchar_t* target, int side, std::vector<uint8_t>* png) {
    if (!target || !*target || !png || side <= 0) return false;
    png->clear();

    // Packaged apps often give IShellItemImageFactory a generic document glyph. Their manifest names
    // a Square44x44Logo resource, which is the same image the Start menu and taskbar use. Try that first
    // for an AUMID-shaped target.
    if (PackageIconPng(target, side, png)) return true;

    // A pin is a SHORTCUT and an application from the shell's list is an AppsFolder item, and the shell
    // answers both through the same interface -- which is why this does not have to know which it was
    // given. `shell:AppsFolder\<name>` is a valid parsing name for an item and a shortcut is a file.
    wchar_t parsing[600] = {};
    // Two kinds of thing arrive here: a name inside `shell:AppsFolder` (no drive letter) and a shortcut,
    // which is a real path. "Not a path" is the whole rule for which is which.
    if (PathIsRelativeW(target) && std::wcsstr(target, L"shell:") == nullptr) {
        std::swprintf(parsing, 600, L"shell:AppsFolder\\%ls", target);
    } else {
        wcsncpy_s(parsing, 600, target, _TRUNCATE);
    }

    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(parsing, nullptr, IID_PPV_ARGS(&item))) || !item) return false;
    IShellItem2* item2 = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&item2))) && item2) {
        PWSTR aumid = nullptr;
        if (SUCCEEDED(item2->GetString(PKEY_AppUserModel_ID, &aumid)) && aumid) {
            const bool packaged = PackageIconPng(aumid, side, png);
            CoTaskMemFree(aumid);
            item2->Release();
            item->Release();
            if (packaged) return true;
            // The item was released above; recreate it below only when the package path failed.
            if (FAILED(SHCreateItemFromParsingName(parsing, nullptr, IID_PPV_ARGS(&item))) || !item) {
                return false;
            }
        } else {
            item2->Release();
        }
    }
    IShellItemImageFactory* factory = nullptr;
    const HRESULT hr = item->QueryInterface(IID_PPV_ARGS(&factory));
    item->Release();
    HBITMAP bitmap = nullptr;
    if (SUCCEEDED(hr) && factory) {
        const SIZE wanted = {side, side};
        // ASK FOR THE LARGE ICON and let the Shell scale it from the best resource it has. `ICONONLY`
        // keeps documents from becoming thumbnails; `BIGGERSIZEOK` avoids stretching a 16px icon when a
        // 256px one is present in the same DLL.
        const HRESULT imageHr = factory->GetImage(
            wanted, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &bitmap);
        factory->Release();
        if (SUCCEEDED(imageHr) && bitmap) {
            const bool converted = BitmapToPng(bitmap, side, png);
            DeleteObject(bitmap);
            if (converted) return true;
        }
    }

    // Fall through in the order that preserves the most specific icon: a shortcut's own icon location,
    // the executable it points at, the shell's file icon, then the renderer's monogram.
    const ShortcutTarget shortcut = ReadShortcutTarget(target);
    if (!shortcut.iconPath.empty() &&
        ExtractPng(shortcut.iconPath, shortcut.iconIndex, side, png)) {
        return true;
    }
    if (!shortcut.path.empty() && ExtractPng(shortcut.path, 0, side, png)) return true;
    if (!shortcut.path.empty() && ShellFileIconPng(shortcut.path, side, png)) return true;
    return ShellFileIconPng(parsing, side, png);
}

bool LaunchStartApp(const wchar_t* target) {
    if (!target || !*target) return false;
    // A shortcut is launched as a file; an AppsFolder name is prefixed with the folder. Both arrive here.
    wchar_t parsing[600] = {};
    if (std::wcsstr(target, L":\\") != nullptr || std::wcsstr(target, L"shell:") != nullptr) {
        wcsncpy_s(parsing, 600, target, _TRUNCATE);
    } else {
        std::swprintf(parsing, 600, L"shell:AppsFolder\\%ls", target);
    }
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", parsing, nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecute answers with a value CAST to HINSTANCE: anything above 32 is success, and the numbers
    // below it are the shell's error codes. Documented, and worth reading rather than testing "!= null".
    const bool launched = reinterpret_cast<INT_PTR>(result) > 32;
    LogLine(L"  start: launch '%ls' -> %s (%lld)", target, launched ? "ok" : "FAILED",
            static_cast<long long>(reinterpret_cast<INT_PTR>(result)));
    return launched;
}

} // namespace island
