// =====================================================================
//  TrayLearn.h
//  Learning every application's tray callback, automatically, by being inside it when it registers.
//
//  WHY THIS EXISTS. A press is invisible only when it is the application's own callback posted to its own
//  tray window, and that needs the window, the message number and the uid. The message number cannot be
//  looked up (the shell's registry entry has uid, path, tooltip and picture, and no callback message) and it
//  cannot be observed from outside (the shell delivers it without a queue anyone else can read), so it used
//  to be learned by hand: a Win32k trace, an administrator prompt, and a third-party program clicking the
//  icon as the reference (HANDOFF 8.104). The user's verdict on that was correct: one application at a time
//  is not a mechanism.
//
//  What is a mechanism is being present when the application calls Shell_NotifyIcon: the structure it passes
//  carries the window, the uid, the callback message and the version. src/trayhook/TrayHook.cpp is that
//  presence -- a small module loaded into each running tray application with a thread hook -- and this file
//  is the driver that loads it, asks every application to register again (the standard TaskbarCreated
//  broadcast), reads what came back and merges it into the cache TrayPress already reads:
//
//      %USERPROFILE%\IslandBar\tray_callbacks.txt
//
//  AND THE MODULE STAYS BEHIND. It pins itself in each application it enters (see TrayHook.cpp) and its
//  import slot stays redirected, so the report keeps growing with every registration for the rest of the
//  session -- an application restarted from its own menu, an icon that only appears while a game runs. That
//  is what `HarvestLearnedCallbacks` is for: the bar calls it on its tray-refresh timer and picks those up
//  without installing anything again. The installing pass is only needed for applications this bar has not
//  reached yet, which is what `UncoveredApplications` reports.
//
//  WHAT IT CANNOT REACH, measured on this machine rather than assumed (HANDOFF 8.107): applications that
//  resolve Shell_NotifyIcon themselves and call through their own pointer (WeChat's Qt tray: 0 import
//  slots), applications that refuse an injected module outright (QQ Music), elevated ones (Task Manager is
//  High integrity) and packaged ones (ChatGPT is an AppContainer, where SetWindowsHookEx is refused with
//  error 87). Those keep whatever they had -- a hand-learned line, or the real click.
// =====================================================================
#pragma once

#include "shell/TrayIcons.h"

namespace island {

struct TrayLearnResult {
    int icons = 0;         // icons on the card at the time of the run
    int appsTried = 0;     // running tray applications the module was offered to
    int appsReached = 0;   // ... in which the module actually loaded
    // ... plus the ones that were ALREADY holding it, which is the normal case after the first session: the
    // module pins itself where it lands and stays for the life of that process (see TrayHook.cpp), so a pass
    // that loads into nobody can still be a machine where every application is watched. Without this number
    // the log reads "the module loaded in 0" on a machine that is fully covered.
    int appsAlreadyWatched = 0;
    int captured = 0;      // registrations seen (NIM_ADD / NIM_MODIFY), duplicates included
    // Set when the module is publishing on a different ABI than this bar reads. It is a field rather than only
    // a log line because "no registrations arrived" and "they arrived in a language this build cannot read"
    // are different answers, and only one of them is worth waiting for (see platform/TrayRing.h).
    bool ringAbiMismatch = false;
    int newEntries = 0;    // lines added to the callback cache
    int covered = 0;       // icons of the card that have an entry afterwards
    long long ms = 0;      // how long the whole thing took

    // WHAT THE OBSERVER INSIDE THE SHELL SAID, before anybody was asked to register. It is the difference
    // between "nobody registered" and "nobody was listening", and it used to be the second one without the log
    // saying so: measured 2026-10-16, the first pass after Explorer restarted reported 143 thread hooks, 57
    // accepted, and captured nothing, while the pass minutes later, same build, captured 42.
    //
    // The module publishes this over the ring (platform/TrayRing.h) and this pass WAITS for it, bounded by
    // kObserverWaitMs, before broadcasting TaskbarCreated. A pass whose wait runs out still asks -- an
    // application that registers late is better than one that is never asked -- and records that it did.
    bool observerReady = false;
    // The shell was ALREADY carrying this exact module when the pass started, because the module pins
    // itself in the process it lands in (TrayHook.cpp). Such a shell observes without publishing a fresh
    // handshake -- the handshake is written once per module load -- so "no handshake this pass" says
    // nothing about whether registrations are being read. Measured 2026-10-18: every pass reported
    // "the observer did NOT report ready" while 40 registrations arrived "read in the shell, over the
    // ring" during that same pass.
    bool observerResident = false;
    unsigned observerTid = 0;        // the thread the registrations are delivered to
    unsigned observerHooks = 0;
    unsigned observerAttempts = 0;
    unsigned observerError = 0;
    int poked = 0;                   // hooked threads that accepted the message that makes the module land
    long long observerMs = 0;        // how long the wait for the module actually lasted
};

// How long a pass waits for the module inside the shell to report that it is listening. It is an engineering
// number, not a Windows guarantee: the module publishes as soon as the thread hook is accepted, which is
// microseconds after the bar makes it land -- the wait is here because "the shell is starting up and its tray
// window does not exist yet" is a real state, not because the handshake is slow.
inline constexpr unsigned kObserverWaitMs = 3000;

// ... and how long a pass stays to collect the answers to its ONE ask.
//
// MEASURED 2026-10-16, three restarts of Explorer in a row: the registrations reached the shell 3.3 s, 3.5 s
// and "still not in" 3.5 s after the broadcast. The flat 1.5 s wait this replaces missed all 27 of the first
// run's copies and reported `0 registration(s) captured` from a machine that was learning perfectly well (the
// frame loop's tick picked them up on its next round). An application answers TaskbarCreated on its own
// schedule, so the pass drains in a loop while it waits, and this is the engineering parameter for how long:
// long enough for the slowest answer seen so far, and only ever on a worker thread that has nothing else to
// do. It is a measurement to be revised, not a Windows promise.
inline constexpr unsigned kAnswerWaitMs = 6000;

// One clause for the summary line every caller prints: whether the observer was up before the applications
// were asked. Written here rather than at each call site so the three of them cannot drift apart.
std::wstring ObserverClause(const TrayLearnResult& learned);

// Loads the staged copy of the module next to the bar and keeps it for the session. Called once from the main
// thread at startup, before any learning pass.
//
// THE MODULE IS OFFERED TO ONE PROCESS: THE SHELL'S OWN, in LearnTrayCallbacks below.
//
// An application does not keep its tray registration to itself -- it SENDS it to the window of class
// "Shell_TrayWnd" as a WM_COPYDATA, so a module inside that window's process reads every registration on the
// machine. That is what replaced the earlier pass that offered the module to each tray application in turn,
// and it is why the tray no longer has to be taught one application at a time. Measured 2026-10-15: an
// AppContainer application (ChatGPT.exe) and a self-protecting one (QQ Music, which accepts 98 thread hooks
// and never loads the module) both arrive this way, because both of them register through the shell.
//
// A GLOBAL hook was tried on 2026-10-15 and withdrawn the same day: with it installed Clash Verge's window
// came up black. Measured both ways -- `island running` gave 6 WebView2 processes and a client area of mean
// brightness 0, `island closed` gave 12 and brightness 56 with the UI rendered. Chromium refuses a pending
// injection of an UNSIGNED module into its renderer processes (RendererCodeIntegrity), and MyDockFinder's own
// DLL is a signed commercial binary, which is why one global hook works for it and not for this build. The
// shell's process is one process, so nothing has to be global. See TrayLearn.cpp.
//
// A failure is reported in the log and is not fatal: without the module a press uses whatever the cache holds.
bool InstallTrayHook();

// Learns what every running tray application's callback is, and writes the cache.
//
// `broadcast` sends TaskbarCreated first, which is what makes applications register again: without it, one
// that registered hours ago has no reason to call Shell_NotifyIcon while this runs. Blocking, for a worker
// thread; it takes about three seconds.
TrayLearnResult LearnTrayCallbacks(const TrayIcons& icons, bool broadcast);

// Reads whatever the resident module has written since the last call and merges it into the cache.
// Returns how many NEW cache entries appeared (0 when nothing has registered since). Cheap: one small file
// read, no injection, no waiting -- safe to call from the frame loop's tray-refresh tick.
int HarvestLearnedCallbacks();

// --- WHICH REGISTRATIONS THE SHELL IS HOLDING RIGHT NOW ------------------------------------------------
//
// The card draws a registry entry only while one of these says the shell still holds that registration
// (shell/TrayIcons.cpp), because the registry alone keeps a subkey for every icon an application has EVER
// registered and "the executable is running" is not the same question as "there is an icon in the tray".
// MEASURED 2026-10-16: thirteen cells against the shell's nine -- three NVIDIA entries (its container
// process runs all day and registered nothing that session) and one dead explorer GUID with a blank
// picture. The rule itself lives in domain/TrayLedger.h; the registrations arrive on the ring this module
// drains, which is why the ledger is kept and read from here.
//
// `exeLeaf` is lower case and without its directory -- the caller has it from the registry, and the ledger
// has it from the process that owns the tray window. A guid identifies a registration on its own; a uid does
// NOT (ChatGPT and RazerAppEngine both register uid 3 here), so a uid counts only with the executable's name.
bool LiveTrayFind(const wchar_t* exeLeaf, unsigned uid, bool hasGuid, const unsigned char* guid,
                  std::wstring* tipOut);
// How many registrations are held. Zero is the fallback: the card then draws what the registry holds, as it
// did before this existed, rather than showing nothing at all.
int LiveTrayCount();
// Whether a pass has finished asking every application to register again -- which is the moment the ledger
// can be believed. MEASURED 2026-10-16: before that pass finishes the ledger holds only whatever happened to
// arrive, so a card that trusted it showed ONE cell for the seconds the pass takes and then filled up. Until
// then the card draws the registry list, as it always did.
bool LiveTraySeeded();
// Registrations refused past the ledger's ceiling, so an application that re-registers in a loop shows up.
int LiveTrayRefused();
// The shell process was replaced, so nothing it held is registered in the new one.
void ResetLiveTray();
// Drops the registrations whose tray window is gone: an application that died without saying so, which the
// registry cannot see either. Called from the frame loop's tray tick.
//
// WHAT IT CANNOT DO: a registration whose tray window could not be resolved to a process at all -- the window
// was already gone when the bar looked it up, as our own registrar probe's is (it registers and then destroys
// its window) -- has no window to check and no process to name, so it keeps no cell. The shell, which removes
// an icon when its PROCESS exits, may still be showing it for as long as that process lives.
void ForgetDeadTrayWindows();

// The applications of the card's icons that still have NO callback entry, as one comma-joined, sorted
// string. The frame loop compares it with the string from its previous attempt and only tries again when it
// CHANGED: retrying the same four unreachable applications every few seconds would spend the whole session
// installing a module that they refuse for the same reason each time.
std::wstring UncoveredApplications(const TrayIcons& icons);

// The process that receives tray registrations -- the owner of the window of class "Shell_TrayWnd" -- or 0
// when there is no such window. The frame loop watches this: the module is pinned for the life of the process
// it lands in, so an Explorer that restarts leaves the tray unlearned until the module is offered again.
DWORD TrayShellProcessId();

// How many of the card's icons have an entry in the callback cache. The number the startup line prints, and
// the one that says whether the automatic route is carrying its weight.
int CountCoveredIcons(const TrayIcons& icons);

} // namespace island
