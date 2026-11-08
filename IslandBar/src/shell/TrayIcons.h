// =====================================================================
//  TrayIcons.h
//  The system tray's application icons, for the right-hand end of the bar.
//
//  WHERE THE ICONS COME FROM, and why not from the tray itself.
//
//  The classic way to read the notification area -- walk Shell_TrayWnd down to
//  ToolbarWindow32 and read the buttons out of Explorer's memory -- is dead on
//  Windows 11 (measured on build 26220: that subtree does not exist; the tray is
//  not a toolbar any more).
//
//  What IS available, measured with scripts/probe-tray-uia.ps1:
//
//    * the shell publishes the tray through UI Automation -- names, rectangles and
//      an Invoke pattern, so the icons can be SEEN and PRESSED -- but only while the
//      taskbar is visible, and this bar hides it. That makes UIA the way to press a
//      tray icon, and not the way to draw one.
//    * the icons themselves are in the registry, one subkey per icon, with a PNG of
//      the image:
//
//          HKCU\Control Panel\NotifyIconSettings\<hash>
//              ExecutablePath  the application that registered it
//              IconSnapshot    a PNG, 400-2300 bytes (magic 89 50 4E 47)
//              InitialTooltip  what the application calls it (often empty)
//              IsPromoted      1 = the shell shows it in the tray proper
//
//  So the images come from the registry, which needs no taskbar and no Explorer, and
//  the JSON is not involved at all.
//
//  WHICH ONES ARE SHOWN, and why "the application is running" turned out not to be the
//  answer.
//
//  The registry keeps an entry for every application that has EVER put an icon in the
//  tray -- that alone is a graveyard of old installs -- and the first filter over it was
//  "is the executable that subkey names running". That is a different question from "is
//  there an icon in the tray", and on this machine it is visibly the wrong one
//  (MEASURED 2026-10-16): thirteen cells against the shell's nine, the surplus being
//  three NVIDIA entries (NVDisplay.Container.exe is a process that runs all day and
//  registered nothing in that session) and one dead explorer GUID whose stored picture is
//  a blank 210-byte PNG.
//
//  So the filter is the shell's own answer: an entry is drawn while -- and only while --
//  one of the registrations the module watched arrive at Shell_TrayWnd names it
//  (shell/TrayLearn.h keeps that ledger, domain/TrayLedger.h holds the rule). When the
//  ledger is empty -- no module in the shell yet, or a shell that was just replaced -- the
//  card falls back to "the executable is running" and says so in the log, because a bar
//  that draws nothing while it cannot see the tray is worse than one that draws a cell it
//  is not yet sure about.
//
//  ONE ICON, ONE CELL, and that is the rule this card exists to keep. The registry is not a
//  set: one icon can be stored under two entries (NVIDIA's nvcontainer.exe and
//  NVDisplay.Container.exe, WeChat's GUID entry and its uid 0 one), and drawing the entries
//  instead of the icons is what the user saw on 2026-11-06 -- two WeChat cells and two NVIDIA
//  cells where the tray shows one of each. Two registry entries that name the same
//  registration are one cell (domain/TrayLedger.h: SameTrayIcon).
//
//  WHAT THE LEDGER ALONE CANNOT SEE, and why there is still a fallback. A ledger is complete
//  only while the module sits in the shell and feeds it: before the first learning pass, and
//  after a shell that replaced Explorer, it is a snapshot, and an application that registers
//  afterwards is missing from it. So the card trusts it alone only when the last pass said the
//  module is resident (LiveTrayObserverResident); otherwise it draws the registry list and
//  says in the log that the list may be wider than the tray. The per-entry union that used to
//  combine the two ("or its process is running") was what re-introduced the duplicates, and it
//  is gone: the question "is this icon in the tray" has one answer, and it is the ledger's.
//
//  AND WHAT EACH ONE *IS*, for pressing it. The same registry entry the icon is drawn from carries the
//  identity the shell itself uses for that icon -- an IconGuid, or a UID belonging to one of the
//  application's windows -- and Shell_NotifyIconGetRect answers WHERE the icon is from that identity
//  alone. That is what makes a click in the card possible without matching names: the shell's own name
//  for an icon is the application's runtime tooltip (" 微信"), while the bar's is the registry's
//  ("Weixin.exe"), and no amount of string work reconciles the two. See TrayPress.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace island {

// One icon, as the bar draws it.
struct TrayIcon {
    // What the application calls it, or its file name when it never said. Wide, because the tooltip is
    // whatever the application wrote and that is usually a sentence.
    //
    // WHICH NAME, in order: the registry's InitialTooltip, then the tip the application sent the shell with
    // its registration, then the file name. The registration is what saves the icons the registry has no
    // name for -- InitialTooltip is EMPTY for those the shell registers on somebody else's behalf, which is
    // how the shell's own Bluetooth icon was drawn here as "explorer.exe" (its registration says 蓝牙设备).
    //
    // THE REGISTRY GOES FIRST because a registration's tip is live text, not a label: MEASURED 2026-10-16,
    // Task Manager re-sends its icon with "CPU 24%\n内存 58%\n磁盘 0%\n网络 0%" as the tip, and Clash
    // Verge's carries a version and a rule count over several lines. The tray shows that on HOVER; a cell is
    // one line wide and wants the name.
    wchar_t name[96] = {};
    // The PNG the shell stores for it: not owned by this struct's caller's frame, but held by the list
    // that produced it, which keeps it alive until the next refresh.
    const uint8_t* png = nullptr;
    size_t pngSize = 0;
    // 1 when the shell shows it in the tray proper rather than in the overflow.
    bool promoted = false;
    // The shell's own identity for this icon, straight out of the same registry entry: the `UID` of one
    // of the registered application's windows, or an `IconGuid`. One of the two is always present, and
    // TrayPress needs it to ask the shell where the icon is.
    //
    // `hasUid` is separate from `uid` because ZERO IS A REAL UID: WeChat registers with 0 on this
    // machine, and reading "uid == 0" as "no UID" cost a round of debugging (its icon reported as "not
    // displayed" while the shell was happily answering for it).
    unsigned uid = 0;
    bool hasUid = false;
    bool hasGuid = false;
    GUID guid = {};
};

// The live tray icons, newest registration first.
//
// Refreshed on a timer rather than every frame: it reads the registry, and the tray changes on the
// scale of applications starting and stopping.
class TrayIcons {
public:
    // Reads the registry and keeps the icons the shell is holding. Returns how many.
    int Refresh(bool upgradeLowResolutionIcons = false);

    int Count() const { return static_cast<int>(entries_.size()); }
    // The CONTIGUOUS list, which is what the renderer walks: `icons[i]` has to be the i-th icon, and the
    // storage behind these has them interleaved with strings and vectors (see `list_`).
    const TrayIcon& At(int index) const { return list_[static_cast<size_t>(index)]; }

    // The executable behind one icon, for the log and for pressing it later.
    const wchar_t* ExePathAt(int index) const {
        return entries_[static_cast<size_t>(index)].exePath.c_str();
    }

    // Bumped when the set changes, so the frame loop can redraw once and the renderer can re-decode.
    unsigned long long Revision() const { return revision_; }

    // What the last refresh looked at, for the log: the entries the registry holds, and how many of
    // their applications were running. "No icons" has two very different causes -- an empty registry and
    // a failed process snapshot -- and these are what tell them apart.
    int Examined() const { return examined_; }
    // ... and how many of those entries were kept. "Kept" rather than "running": with the ledger in charge
    // the two are not the same question, and the number the log needs is the one the card ended up drawing.
    int Kept() const { return kept_; }
    int ProcessesSeen() const { return processesSeen_; }

    // The registry entries that were NOT drawn, by the file name the entry names, for the log line: "nine
    // against thirty-one" is a number nobody can check without the names (see TrayIcons.cpp).
    const std::vector<std::wstring>& NotLive() const { return notLive_; }
    // False when the ledger was empty this time, i.e. the card fell back to the older rule (see the header).
    bool LiveFilter() const { return liveFilter_; }
    // The rest of the log line, from whichever of the two rules decided this list: how many of the
    // registry's entries the shell is holding, and which ones it is not (see TrayIcons.cpp). Written here
    // because three call sites print it and a number nobody can check is not a number.
    std::wstring SourceClause() const;

    // Where the icons live, so nothing else has to know the key name.
    static const wchar_t* SettingsKeyPath() { return L"Control Panel\\NotifyIconSettings"; }

private:
    struct Entry {
        TrayIcon icon;
        std::wstring exePath;
        std::wstring tooltip;       // the registry's InitialTooltip, read once: it is the fallback match too
        std::vector<uint8_t> png;   // the storage `icon.png` points into
        long long writtenAt = 0;    // the subkey's last write, for the newest-first order
    };

    std::vector<Entry> entries_;
    // The same icons, contiguous, for the renderer.
    //
    // `At()` used to return `entries_[index].icon` and the frame handed the renderer `&At(0)` -- so the
    // renderer walked a pointer to the FIRST icon as if the icons were an array, while they are members of
    // `Entry` structs that also hold a wstring and a vector. `icons[1]` therefore read the middle of
    // entry 0: a size of 0xFEFEFEFEFEFEFEFE (the debug heap's fill for memory nobody wrote) and a picture
    // that would not decode. Measured, in the draw trace: "icon 1 has a 18374403900871474942 byte PNG".
    std::vector<TrayIcon> list_;
    unsigned long long revision_ = 1;
    // How many registry entries were looked at and how many were kept, for the log.
    int examined_ = 0;
    int kept_ = 0;
    int processesSeen_ = 0;
    // The entries the shell is not holding a registration for, and whether the ledger decided that at all.
    std::vector<std::wstring> notLive_;
    bool liveFilter_ = false;
    // The ledger alone decided this pass: seeded, and the module is resident in the shell (see the header).
    bool ledgerOnly_ = false;
    // Registry entries that named a registration already drawn -- one icon, one cell.
    int collapsed_ = 0;
    // How many registrations the shell holds, for the log line (an icon can be one of several).
    int liveRegistrations_ = 0;
    // The identities already put in the list this pass, so several registry entries naming one icon make one
    // cell. Used by the two fallback rules, where the registry's own identity is all there is (see
    // TrayIcons.cpp); the ledger rule keys on the shell's registration instead (SameTrayIcon).
    std::vector<std::wstring> seenIdentities;
    // Registry snapshots are frequently 16x16. Application executables usually carry a much better icon, so
    // the UI-thread refresh can substitute one after the first successful extraction and keep that work cached.
    std::unordered_map<std::wstring, std::vector<std::uint8_t>> upgradedIcons_;
};

} // namespace island
