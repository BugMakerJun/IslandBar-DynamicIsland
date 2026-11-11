// =====================================================================
//  ToastStore.h
//  The SECOND notification channel: Windows' own toast store.
//
//  The push channel (a named pipe) is for things on this machine that want
//  to tell the bar something. This one is for everything else: a message that
//  Windows itself is showing in the corner -- WeChat, a build tool, an installer.
//  It reads the same database the shell's notification centre does:
//
//      %LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db
//
//  TWO THINGS ABOUT THAT DATABASE, both measured rather than assumed:
//
//   * It is SQLite, and Windows SHIPS THE ENGINE: `winsqlite3.dll` (3.51.1 here) exports the ordinary
//     sqlite3 API. So this file binds to it at runtime and the project does not vendor a 9 MB amalgamation,
//     does not need a network at configure time, and still works on a machine where the DLL is missing --
//     the channel is simply unavailable, which is reported rather than fatal.
//
//   * The payload column is a plain UTF-8 toast XML document with no binary prefix (checked against a real
//     row: it starts at `3C 74 6F 61 73 74` = "<toast"). The sender's identity is NOT in it -- it is in
//     `NotificationHandler.PrimaryId`, an AppUserModelID like "OpenAI.Codex_2p2nqsd0c76g0!App".
//
//  The reader OPENS THE LIVE DATABASE read-only and tolerates it being busy: the same file is held by the
//  notification service, and a reader that insisted on a lock would see an empty channel on every machine
//  where notifications happen. If the read-only open is refused it falls back to a copy under %TEMP%.
// =====================================================================
#pragma once

#include <cstddef>
#include <cwchar>

#include "services/Notification.h"

namespace island {

// The title and body out of a toast XML document.
//
// Pure, so the parsing can be asserted against hand-written documents rather than against whatever happens to
// be in the database today -- including the entities, which is where this kind of code usually goes wrong
// ("&amp;" left as-is, or a body truncated at the first "&").
//
// The first <text> element is the title and the rest are joined into the body, which is what a ToastGeneric
// template means: its `<text>` elements are the title line and then the body lines.
bool ToastTextFromXml(const char* xml, size_t size, wchar_t* title, size_t titleCapacity,
                      wchar_t* body, size_t bodyCapacity);

// The name a user would recognise, out of an AppUserModelID.
//
// "OpenAI.Codex_2p2nqsd0c76g0!App" -> "Codex": the part before the "!" is the package family name, whose
// hash after "_" means nothing to a person, and the last dot-separated piece of what is left is the name.
void FriendlyNameFromHandlerId(const wchar_t* primaryId, wchar_t* out, size_t capacity);

// Reads the toast database and hands what it finds to the store.
class ToastReader {
public:
    ToastReader() = default;
    ~ToastReader();

    ToastReader(const ToastReader&) = delete;
    ToastReader& operator=(const ToastReader&) = delete;

    // Loads the SQLite entry points and opens the database.
    //
    // `databasePath` is for the self-test; null asks Windows where the file is.
    bool Start(const wchar_t* databasePath = nullptr);
    void Stop();
    bool Available() const { return database_ != nullptr; }

    // Reads everything that arrived since the last call and pushes it into `store`. Returns how many were
    // added. The FIRST call only records where the database currently ends: an empty bar that suddenly
    // replays the last three days of toasts is not what "show me my notifications" means.
    int Poll(NotificationStore* store);

    // Seeds the notification centre with recent records that already existed
    // before this process started. These are marked read so they appear in
    // history without firing the transient six-second banner.
    int SeedHistory(NotificationStore* store, int limit);

    // How many toasts have been taken from the database since Start. For the log.
    int Taken() const { return taken_; }

    // Reads the newest `limit` toasts WITHOUT the store, for `--toast-dump`: the reader has to be checkable
    // from a shell, and a channel whose only evidence is a card on screen is a channel nobody can debug.
    int Dump(int limit, wchar_t* out, size_t capacity) const;

private:
    struct Sqlite;
    Sqlite* sqlite_ = nullptr;
    void* database_ = nullptr;      // sqlite3*
    long long lastId_ = 0;
    int taken_ = 0;
    bool primed_ = false;
    wchar_t openedPath_[260] = {};
    wchar_t copyPath_[260] = {};
};

} // namespace island
