// =====================================================================
//  ToastStore.cpp
// =====================================================================
#include "services/ToastStore.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "core/Log.h"

namespace island {
namespace {

// --- the SQLite entry points, taken from the DLL Windows ships -----------------------------
//
// Declared by hand rather than by including sqlite3.h, because the header is not part of the Windows SDK: the
// DLL is, the header is not. The signatures are the published ones and the calling convention is cdecl, which
// is what makes the hand-written declarations safe to use.
using sqlite3 = struct sqlite3;
using sqlite3_stmt = struct sqlite3_stmt;

constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadOnly = 0x00000001;

struct SqliteApi {
    HMODULE module = nullptr;
    int(__cdecl* open_v2)(const char*, sqlite3**, int, const char*) = nullptr;
    int(__cdecl* close)(sqlite3*) = nullptr;
    int(__cdecl* prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = nullptr;
    int(__cdecl* step)(sqlite3_stmt*) = nullptr;
    int(__cdecl* finalize)(sqlite3_stmt*) = nullptr;
    int(__cdecl* reset)(sqlite3_stmt*) = nullptr;
    int(__cdecl* clear_bindings)(sqlite3_stmt*) = nullptr;
    int(__cdecl* bind_int64)(sqlite3_stmt*, int, long long) = nullptr;
    long long(__cdecl* column_int64)(sqlite3_stmt*, int) = nullptr;
    const unsigned char* (__cdecl* column_text)(sqlite3_stmt*, int) = nullptr;
    const void* (__cdecl* column_blob)(sqlite3_stmt*, int) = nullptr;
    int(__cdecl* column_bytes)(sqlite3_stmt*, int) = nullptr;
    const char* (__cdecl* errmsg)(sqlite3*) = nullptr;
};

// The maximum payload taken from the database. A toast is text; anything larger is an attachment or a
// template this reader has no business decoding, and the store's own fields are smaller than this anyway.
constexpr int kMaxPayloadBytes = 64 * 1024;

// Appends `text` to `out`, returning the new length. Never overflows and always terminates.
size_t Append(wchar_t* out, size_t capacity, size_t used, const wchar_t* text) {
    if (!out || capacity == 0 || !text) return used;
    for (size_t i = 0; text[i] != L'\0' && used + 1 < capacity; ++i) {
        out[used++] = text[i];
    }
    out[used] = L'\0';
    return used;
}

// Turns one <text> element's bytes into the string the card will show.
//
// TWO steps, in this order, and the order is the whole point:
//   1. the bytes are UTF-8, so they are converted as UTF-8 -- one ByteToWideChar for the whole run. Reading
//      them a byte at a time "works" for ASCII and turns every Chinese character into two or three
//      look-alike Latin letters, which is exactly what the first version of this did: the title of a real
//      Codex toast came back as `æ£ç´¢é¡¹ç®` while the parse itself looked perfect.
//   2. the XML entities are decoded afterwards, on the WIDE string, because they are ASCII and can be
//      matched there without caring about the bytes around them.
size_t DecodeEntities(const wchar_t* text, size_t length, wchar_t* out, size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; i < length && used + 1 < capacity; ++i) {
        if (text[i] != '&') {
            out[used++] = text[i];
            continue;
        }
        if (i + 5 <= length && std::wcsncmp(text + i, L"&amp;", 5) == 0) {
            out[used++] = L'&';
            i += 4;
        } else if (i + 4 <= length && std::wcsncmp(text + i, L"&lt;", 4) == 0) {
            out[used++] = L'<';
            i += 3;
        } else if (i + 4 <= length && std::wcsncmp(text + i, L"&gt;", 4) == 0) {
            out[used++] = L'>';
            i += 3;
        } else if (i + 6 <= length && std::wcsncmp(text + i, L"&quot;", 6) == 0) {
            out[used++] = L'"';
            i += 5;
        } else if (i + 6 <= length && std::wcsncmp(text + i, L"&apos;", 6) == 0) {
            out[used++] = L'\'';
            i += 5;
        } else if (i + 3 <= length && text[i + 1] == '#') {
            // A numeric escape, decimal or hex. Anything malformed is taken literally.
            size_t j = i + 2;
            unsigned value = 0;
            bool hex = false;
            if (j < length && (text[j] == 'x' || text[j] == 'X')) {
                hex = true;
                ++j;
            }
            const size_t digitsStart = j;
            while (j < length && text[j] != ';') {
                const wchar_t c = text[j];
                const int digit = hex ? (c >= '0' && c <= '9'   ? c - '0'
                                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                 : -1)
                                       : (c >= '0' && c <= '9' ? c - '0' : -1);
                if (digit < 0) break;
                value = value * (hex ? 16u : 10u) + static_cast<unsigned>(digit);
                ++j;
            }
            if (j > digitsStart && j < length && text[j] == ';') {
                if (value != 0) out[used++] = static_cast<wchar_t>(value);
                i = j;
            } else {
                out[used++] = L'&';
            }
        } else {
            out[used++] = L'&';
        }
    }
    out[used] = L'\0';
    return used;
}

// Finds the next `<text ...>` ... `</text>` in a document, returning the inner range.
bool NextTextElement(const char* xml, size_t size, size_t* cursor, const char** text, size_t* length) {
    const char* open = nullptr;
    size_t openLength = 0;
    for (size_t i = *cursor; i + 5 <= size; ++i) {
        if (xml[i] != '<') continue;
        if (_strnicmp(xml + i, "<text", 5) != 0) continue;
        // Must be the tag itself, not <textSomething>.
        const char after = (i + 5 < size) ? xml[i + 5] : '>';
        if (after != '>' && after != ' ' && after != '\t' && after != '\r' && after != '\n') continue;
        const char* gt = static_cast<const char*>(std::memchr(xml + i, '>', size - i));
        if (!gt) return false;
        open = gt + 1;
        openLength = static_cast<size_t>(gt - xml) + 1;
        break;
    }
    if (!open) return false;

    const char* close = nullptr;
    for (size_t i = openLength; i + 7 <= size; ++i) {
        if (_strnicmp(xml + i, "</text>", 7) == 0 || _strnicmp(xml + i, "</text ", 7) == 0) {
            const char* gt = static_cast<const char*>(std::memchr(xml + i, '>', size - i));
            if (!gt) return false;
            close = gt + 1;
            break;
        }
    }
    if (!close) return false;

    *text = open;
    *length = static_cast<size_t>(close - open) - 7;   // minus "</text>"
    *cursor = static_cast<size_t>(close - xml);
    return true;
}

}  // namespace

// The DLL and the entry points, in the shape the header forward-declared.
struct ToastReader::Sqlite {
    SqliteApi api;
};

bool ToastTextFromXml(const char* xml, size_t size, wchar_t* title, size_t titleCapacity,
                      wchar_t* body, size_t bodyCapacity) {
    if (title && titleCapacity) title[0] = L'\0';
    if (body && bodyCapacity) body[0] = L'\0';
    if (!xml || size == 0) return false;

    size_t cursor = 0;
    const char* text = nullptr;
    size_t length = 0;
    int found = 0;
    size_t bodyUsed = 0;
    while (NextTextElement(xml, size, &cursor, &text, &length)) {
        // UTF-8 first, entities second: see DecodeEntities.
        //
        // The buffer is generous and the conversion is CAPPED, because a toast body is routinely longer than
        // the store's own field: converting into a buffer that is too small makes MultiByteToWideChar return 0
        // -- not a truncated string, NOTHING -- and the body would silently come out empty on exactly the
        // messages worth showing.
        wchar_t raw[8192] = {};
        const int sourceBytes = static_cast<int>(length < sizeof(raw) - 1 ? length : sizeof(raw) - 1);
        int wide = MultiByteToWideChar(CP_UTF8, 0, text, sourceBytes, raw,
                                       static_cast<int>(sizeof(raw) / sizeof(raw[0])) - 1);
        if (wide <= 1) {
            // A truncated multi-byte sequence at the cut: back off one byte and try again.
            wide = MultiByteToWideChar(CP_UTF8, 0, text, sourceBytes - 1, raw,
                                       static_cast<int>(sizeof(raw) / sizeof(raw[0])) - 1);
        }
        if (wide <= 0) {
            ++found;   // counted, but nothing to show for it
            continue;
        }
        raw[wide] = L'\0';

        if (found == 0) {
            DecodeEntities(raw, static_cast<size_t>(wide), title, titleCapacity);
        } else if (body && bodyCapacity) {
            // Lines are joined with a space: the store holds one string, and a toast's lines are a sentence
            // continued rather than a list.
            if (bodyUsed > 0) bodyUsed = Append(body, bodyCapacity, bodyUsed, L" ");
            wchar_t line[512] = {};
            DecodeEntities(raw, static_cast<size_t>(wide), line, static_cast<size_t>(512));
            bodyUsed = Append(body, bodyCapacity, bodyUsed, line);
        }
        ++found;
    }
    return found > 0;
}

void FriendlyNameFromHandlerId(const wchar_t* primaryId, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (!primaryId || primaryId[0] == L'\0') return;

    // Everything before the "!" of "Family_1.2.3.4!App", and nothing after the "_" of the family name.
    wchar_t trimmed[128] = {};
    size_t used = 0;
    for (size_t i = 0; primaryId[i] != L'\0' && used + 1 < 128; ++i) {
        if (primaryId[i] == L'!' || primaryId[i] == L'_') break;
        trimmed[used++] = primaryId[i];
    }
    trimmed[used] = L'\0';

    // An executable path is answered with its file name; anything else with its last dotted piece, which is
    // the part that names the product ("OpenAI.Codex" -> "Codex").
    const wchar_t* slash = std::wcsrchr(trimmed, L'\\');
    if (slash) {
        wcsncpy_s(out, capacity, slash + 1, _TRUNCATE);
        return;
    }
    const wchar_t* dot = std::wcsrchr(trimmed, L'.');
    const wchar_t* name = dot ? dot + 1 : trimmed;
    wcsncpy_s(out, capacity, (*name != L'\0') ? name : trimmed, _TRUNCATE);
}

ToastReader::~ToastReader() {
    Stop();
}

bool ToastReader::Start(const wchar_t* databasePath) {
    if (database_) return true;

    sqlite_ = new Sqlite();
    sqlite_->api.module = LoadLibraryW(L"winsqlite3.dll");
    if (!sqlite_->api.module) {
        LogLine(L"  toasts: winsqlite3.dll is not present; the system toast channel is unavailable");
        return false;
    }
    SqliteApi& api = sqlite_->api;
    api.open_v2 = reinterpret_cast<decltype(api.open_v2)>(GetProcAddress(api.module, "sqlite3_open_v2"));
    api.close = reinterpret_cast<decltype(api.close)>(GetProcAddress(api.module, "sqlite3_close"));
    api.prepare_v2 = reinterpret_cast<decltype(api.prepare_v2)>(
        GetProcAddress(api.module, "sqlite3_prepare_v2"));
    api.step = reinterpret_cast<decltype(api.step)>(GetProcAddress(api.module, "sqlite3_step"));
    api.finalize = reinterpret_cast<decltype(api.finalize)>(GetProcAddress(api.module, "sqlite3_finalize"));
    api.reset = reinterpret_cast<decltype(api.reset)>(GetProcAddress(api.module, "sqlite3_reset"));
    api.clear_bindings =
        reinterpret_cast<decltype(api.clear_bindings)>(GetProcAddress(api.module, "sqlite3_clear_bindings"));
    api.bind_int64 =
        reinterpret_cast<decltype(api.bind_int64)>(GetProcAddress(api.module, "sqlite3_bind_int64"));
    api.column_int64 =
        reinterpret_cast<decltype(api.column_int64)>(GetProcAddress(api.module, "sqlite3_column_int64"));
    api.column_text =
        reinterpret_cast<decltype(api.column_text)>(GetProcAddress(api.module, "sqlite3_column_text"));
    api.column_blob =
        reinterpret_cast<decltype(api.column_blob)>(GetProcAddress(api.module, "sqlite3_column_blob"));
    api.column_bytes =
        reinterpret_cast<decltype(api.column_bytes)>(GetProcAddress(api.module, "sqlite3_column_bytes"));
    api.errmsg = reinterpret_cast<decltype(api.errmsg)>(GetProcAddress(api.module, "sqlite3_errmsg"));
    if (!api.open_v2 || !api.prepare_v2 || !api.step || !api.column_text || !api.column_blob) {
        LogLine(L"  toasts: winsqlite3.dll does not export the sqlite3 API this reader needs");
        return false;
    }

    wchar_t path[260] = {};
    if (databasePath && databasePath[0]) {
        wcsncpy_s(path, 260, databasePath, _TRUNCATE);
    } else {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
            std::swprintf(path, 260, L"%ls\\Microsoft\\Windows\\Notifications\\wpndatabase.db", local);
            CoTaskMemFree(local);
        }
    }
    if (path[0] == L'\0') return false;

    char narrow[520] = {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
    const int opened = api.open_v2(narrow, reinterpret_cast<sqlite3**>(&database_), kSqliteOpenReadOnly,
                                   nullptr);
    if (opened != kSqliteOk || !database_) {
        // Reported with the code, because "the database is locked" and "the file is not there" are different
        // problems with the same symptom.
        LogLine(L"  toasts: could not open '%ls' read-only (sqlite rc %d); no system toasts", path,
                opened);
        return false;
    }
    wcsncpy_s(openedPath_, 260, path, _TRUNCATE);
    primed_ = false;
    LogLine(L"  toasts: reading '%ls'", path);
    return true;
}

void ToastReader::Stop() {
    if (database_ && sqlite_ && sqlite_->api.close) {
        sqlite_->api.close(reinterpret_cast<sqlite3*>(database_));
    }
    database_ = nullptr;
    if (sqlite_) {
        if (sqlite_->api.module) FreeLibrary(sqlite_->api.module);
        delete sqlite_;
        sqlite_ = nullptr;
    }
}

int ToastReader::Poll(NotificationStore* store) {
    if (!database_ || !sqlite_ || !store) return 0;
    SqliteApi& api = sqlite_->api;

    if (!primed_) {
        // The HIGHEST id already in the database, and nothing else: whatever is in there arrived before the
        // bar was watching, and replaying it would fill the card with yesterday's news on every start.
        sqlite3_stmt* stmt = nullptr;
        if (api.prepare_v2(reinterpret_cast<sqlite3*>(database_),
                           "SELECT IFNULL(MAX(Id), 0) FROM Notification", -1, &stmt, nullptr) ==
                kSqliteOk &&
            stmt) {
            if (api.step(stmt) == kSqliteRow) {
                lastId_ = api.column_int64(stmt, 0);
            }
            api.finalize(stmt);
        }
        primed_ = true;
        LogLine(L"  toasts: starting after id %lld", lastId_);
        return 0;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT n.Id, IFNULL(h.PrimaryId, ''), n.Payload "
        "FROM Notification n LEFT JOIN NotificationHandler h ON h.RecordId = n.HandlerId "
        "WHERE n.Id > ?1 AND n.Type = 'toast' ORDER BY n.Id ASC LIMIT 16";
    if (api.prepare_v2(reinterpret_cast<sqlite3*>(database_), sql, -1, &stmt, nullptr) != kSqliteOk ||
        !stmt) {
        return 0;
    }
    api.bind_int64(stmt, 1, lastId_);

    int added = 0;
    int rc = api.step(stmt);
    while (rc == kSqliteRow) {
        const long long id = api.column_int64(stmt, 0);
        const unsigned char* handler = api.column_text(stmt, 1);
        const void* blob = api.column_blob(stmt, 2);
        const int bytes = api.column_bytes(stmt, 2);
        if (id > lastId_) lastId_ = id;

        if (blob && bytes > 0 && bytes <= kMaxPayloadBytes) {
            Notification notification;
            const auto* xml = static_cast<const char*>(blob);
            wchar_t title[kTitleCapacity] = {};
            wchar_t body[kBodyCapacity] = {};
            if (ToastTextFromXml(xml, static_cast<size_t>(bytes), title, kTitleCapacity, body,
                                 kBodyCapacity)) {
                // UTF-8 out of the XML and into the store's own UTF-8 fields.
                WideCharToMultiByte(CP_UTF8, 0, title, -1, notification.title, kTitleCapacity, nullptr,
                                    nullptr);
                WideCharToMultiByte(CP_UTF8, 0, body, -1, notification.body, kBodyCapacity, nullptr,
                                    nullptr);
                wchar_t appWide[128] = {};
                wchar_t handlerWide[256] = {};
                if (handler) {
                    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(handler), -1,
                                        handlerWide, 256);
                }
                FriendlyNameFromHandlerId(handlerWide, appWide, 128);
                WideCharToMultiByte(CP_UTF8, 0, appWide, -1, notification.app, kAppNameCapacity, nullptr,
                                    nullptr);
                if (handlerWide[0] != L'\0') {
                    WideCharToMultiByte(CP_UTF8, 0, handlerWide, -1, notification.iconPath,
                                        static_cast<int>(sizeof(notification.iconPath)), nullptr,
                                        nullptr);
                }
                notification.sticky = false;
                if (notification.title[0] != '\0' || notification.body[0] != '\0') {
                    store->Push(notification);
                    ++added;
                    ++taken_;
                }
            }
        }
        rc = api.step(stmt);
    }
    api.finalize(stmt);
    return added;
}

int ToastReader::SeedHistory(NotificationStore* store, int limit) {
    if (!database_ || !sqlite_ || !store || limit <= 0) return 0;
    SqliteApi& api = sqlite_->api;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT n.Id, IFNULL(h.PrimaryId, ''), n.Payload "
        "FROM Notification n LEFT JOIN NotificationHandler h ON h.RecordId = n.HandlerId "
        "WHERE n.Type = 'toast' ORDER BY n.Id DESC LIMIT ?1";
    if (api.prepare_v2(reinterpret_cast<sqlite3*>(database_), sql, -1, &stmt, nullptr) !=
            kSqliteOk ||
        !stmt) {
        return 0;
    }
    api.bind_int64(stmt, 1, limit);

    int added = 0;
    long long newest = lastId_;
    while (api.step(stmt) == kSqliteRow) {
        const long long id = api.column_int64(stmt, 0);
        if (id > newest) newest = id;
        const unsigned char* handler = api.column_text(stmt, 1);
        const void* blob = api.column_blob(stmt, 2);
        const int bytes = api.column_bytes(stmt, 2);
        if (!blob || bytes <= 0 || bytes > kMaxPayloadBytes) continue;

        Notification notification;
        wchar_t title[kTitleCapacity] = {};
        wchar_t body[kBodyCapacity] = {};
        if (!ToastTextFromXml(static_cast<const char*>(blob), static_cast<size_t>(bytes), title,
                              kTitleCapacity, body, kBodyCapacity)) {
            continue;
        }
        WideCharToMultiByte(CP_UTF8, 0, title, -1, notification.title, kTitleCapacity, nullptr,
                            nullptr);
        WideCharToMultiByte(CP_UTF8, 0, body, -1, notification.body, kBodyCapacity, nullptr,
                            nullptr);
        wchar_t handlerWide[256] = {};
        wchar_t appWide[128] = {};
        if (handler) {
            MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(handler), -1,
                                handlerWide, 256);
        }
        FriendlyNameFromHandlerId(handlerWide, appWide, 128);
        WideCharToMultiByte(CP_UTF8, 0, appWide, -1, notification.app, kAppNameCapacity, nullptr,
                            nullptr);
        if (handlerWide[0] != L'\0') {
            WideCharToMultiByte(CP_UTF8, 0, handlerWide, -1, notification.iconPath,
                                static_cast<int>(sizeof(notification.iconPath)), nullptr,
                                nullptr);
        }
        if (notification.title[0] != '\0' || notification.body[0] != '\0') {
            store->Push(notification);
            ++added;
        }
    }
    api.finalize(stmt);
    lastId_ = newest;
    primed_ = true;
    // History is visible but must not claim the island as a fresh message.
    store->MarkAllRead();
    LogLine(L"  toasts: seeded %d history item(s), primed after id %lld", added, lastId_);
    return added;
}

int ToastReader::Dump(int limit, wchar_t* out, size_t capacity) const {
    if (!database_ || !sqlite_ || !out || capacity == 0) return 0;
    SqliteApi& api = sqlite_->api;
    out[0] = L'\0';

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT n.Id, IFNULL(h.PrimaryId, ''), n.Payload "
        "FROM Notification n LEFT JOIN NotificationHandler h ON h.RecordId = n.HandlerId "
        "WHERE n.Type = 'toast' ORDER BY n.Id DESC LIMIT ?1";
    if (api.prepare_v2(reinterpret_cast<sqlite3*>(database_), sql, -1, &stmt, nullptr) != kSqliteOk ||
        !stmt) {
        return 0;
    }
    api.bind_int64(stmt, 1, limit);

    size_t used = 0;
    int rows = 0;
    while (api.step(stmt) == kSqliteRow) {
        const long long id = api.column_int64(stmt, 0);
        const unsigned char* handler = api.column_text(stmt, 1);
        const void* blob = api.column_blob(stmt, 2);
        const int bytes = api.column_bytes(stmt, 2);

        wchar_t title[kTitleCapacity] = {};
        wchar_t body[kBodyCapacity] = {};
        if (blob && bytes > 0 && bytes <= kMaxPayloadBytes) {
            ToastTextFromXml(static_cast<const char*>(blob), static_cast<size_t>(bytes), title,
                             kTitleCapacity, body, kBodyCapacity);
        }
        wchar_t app[128] = {};
        wchar_t handlerWide[256] = {};
        if (handler) {
            MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(handler), -1, handlerWide, 256);
        }
        FriendlyNameFromHandlerId(handlerWide, app, 128);

        wchar_t line[1024] = {};
        std::swprintf(line, 1024, L"#%lld [%ls] %ls -- %.200ls\n", id, app, title,
                      body[0] ? body : L"(no body)");
        used = Append(out, capacity, used, line);
        ++rows;
    }
    api.finalize(stmt);
    return rows;
}

}  // namespace island
