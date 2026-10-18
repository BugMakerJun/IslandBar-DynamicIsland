// =====================================================================
//  StartApps.h
//  The applications a self-drawn Start card can offer, and the ones the user pinned himself.
//
//  WHERE THE LIST COMES FROM. Not from the Start menu's own storage: the pinned order of the shell's
//  Start lives in `%LOCALAPPDATA%\Packages\Microsoft.Windows.StartMenuExperienceHost_*\LocalState\start2.bin`,
//  and that file is opaque -- measured: 13,276 bytes, and a UTF-16 scan of the whole thing finds no
//  application id at all (every run of "text" is noise). So the shell's pinned order is NOT readable.
//
//  What IS readable is the list underneath it, and it is the same list the shell builds its "all apps"
//  view from: the virtual folder `shell:AppsFolder`. Measured on this machine: 121 items -- 89 Win32
//  shortcuts and 32 packaged applications -- with LOCALISED names (微信, 记事本, 便签) and a "path" in one
//  of three shapes:
//
//      C:\SOFTWARE\QQMusic\QQMusic.exe                            a file
//      {1AC14E77-...}\cleanmgr.exe                                a shell folder item
//      Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe!App           a packaged application
//
//  and all three launch through one call, `ShellExecute(L"shell:AppsFolder\\" + target)`.
//
//  Reading the whole list costs about 600 ms, which is why it happens off the UI thread and only when it
//  is asked for.
//
//  THE PINS ARE OURS, AND THEY ARE A FOLDER OF SHORTCUTS. Since the shell's pinned list cannot be read,
//  this card keeps its own -- and the user, after the first version of that lost his pins, said how he
//  wanted it kept: "要不你创建一个文件夹 然后在里面放快捷方式 这样每次都还在 但是这个文件夹 可以修改位置".
//
//  WHY THE FIRST VERSION LOST THEM, because the reason is why this is a folder and not a file: the pins
//  were a text file of targets, and loading it VALIDATED every line against the shell's application list.
//  That list is filled by a worker and takes about 600 ms -- measured in the log, the lookup ran 300 ms
//  BEFORE it arrived, so all thirteen pins were rejected as "not in shell:AppsFolder" and the card came up
//  empty. A folder of shortcuts cannot fail that way: the folder IS the record, nothing has to agree with
//  it, and a shortcut is a thing the user can copy, rename, reorder (by name) or throw away in Explorer.
//
//  WHERE IT IS. `start_folder.txt` beside the bar, one line, comment lines allowed -- written out with the
//  default the first time, so the answer to "where do I change this" is in the file itself. The default is
//  `<the bar's folder>\Start`.
//
//  WHAT A SHORTCUT CARRIES. The AppsFolder item's own ID list, which is how the shell's Start menu makes
//  its own shortcuts, plus the AppUserModelID for a packaged application (the property the taskbar pins
//  by). That is what makes one call -- ShellExecute on the .lnk -- launch any of the three shapes.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace island {

// One application, as the shell's own list describes it.
struct StartApp {
    // What the shell calls it, in the user's language.
    wchar_t name[112] = {};
    // What launches it: the AppsFolder path, in whichever of the three shapes it came in.
    wchar_t target[512] = {};
};

class StartApps {
public:
    // Reads `shell:AppsFolder` into the list. Takes a few hundred milliseconds; call it off the UI thread.
    // Returns how many applications were found (0 also means "could not be read").
    int RefreshList();

    // Reloads the pins from the folder of shortcuts, and creates the folder if it is not there. Cheap -- a
    // directory listing -- and it no longer depends on the shell's application list at all, which is what
    // used to make it lose everything when it ran before that list was ready.
    void LoadPins();

    int ListCount() const { return static_cast<int>(list_.size()); }
    const StartApp& ListAt(int index) const { return list_[static_cast<size_t>(index)]; }

    int PinCount() const { return static_cast<int>(pins_.size()); }
    const StartApp& PinAt(int index) const { return pins_[static_cast<size_t>(index)]; }

    // Pins an application by writing a shortcut into the folder: `target` is its AppsFolder name (see the
    // list this class reads), `name` is what the shortcut should be called. False when a shortcut with that
    // name already exists.
    bool Pin(const wchar_t* target, const wchar_t* name);
    bool Unpin(int index);

    // The pinned application's icon as a PNG, or null while it is still being resolved. Resolved on a
    // worker when the pin is added, because getting a shell item's image takes a few milliseconds and the
    // frame loop cannot spend them.
    const uint8_t* PinIconPng(int index, size_t* sizeOut) const;
    // The same for one entry of the ALL-APPLICATIONS list, which the 全部 grid draws as icons too.
    const uint8_t* ListIconPng(int index, size_t* sizeOut) const;

    // Bumped whenever the list or the pins change, so the renderer can re-decode what it has cached.
    unsigned long long Revision() const { return revision_; }
    // Bumped when an icon arrives, which happens separately from a pin being added.
    unsigned long long IconRevision() const { return iconRevision_; }

    // Where the pins live, so nothing else has to know: the folder, and the one-line file that names it.
    static void PinsFolderPath(wchar_t* out, size_t capacity);
    static void PinsFolderSettingPath(wchar_t* out, size_t capacity);

private:
    void ResolveIconAsync(const wchar_t* target);
    void ResolveListIconsAsync();
    // Reads the folder into the pins, without touching the shell's application list.
    void ReadPinsFolder(const wchar_t* folder);

    std::vector<StartApp> list_;
    // Two vectors rather than a vector of structs, and that is not an accident: the renderer is handed a
    // POINTER to the first entry and walks them by index, which only works if the entries are contiguous.
    // The tray taught this the hard way -- it handed over `&entries_[0].icon` from a struct that also held
    // a wstring, and the renderer read the middle of the first entry as the second icon.
    std::vector<StartApp> pins_;
    std::vector<std::vector<uint8_t>> pinIcons_;   // one PNG per pin, in the same order
    std::vector<std::vector<uint8_t>> listIcons_;  // one PNG per list entry, in the same order
    // The folder the pins were read from, so a later Pin/Unpin knows where to write.
    std::wstring pinsFolder_;
    unsigned long long revision_ = 1;
    unsigned long long iconRevision_ = 1;
};

// One AppsFolder item's icon as a PNG of `side` x `side` pixels. False when the item has no image.
//
// `shell:AppsFolder\<target>` is parsed for every shape of target, so a packaged application and a file
// are the same call here.
bool StartAppIconPng(const wchar_t* target, int side, std::vector<uint8_t>* png);

// Launches one AppsFolder item. One call for all three shapes of target.
bool LaunchStartApp(const wchar_t* target);

} // namespace island
