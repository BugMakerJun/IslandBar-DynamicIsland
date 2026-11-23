// =====================================================================
//  ChecksJson.cpp -- see selftest/Checks.h for why the self-test is split like this
//
//  The includes are the ones the single file carried; pruning them per file is a smaller,
//  separate exercise and is deliberately not mixed into the move.
// =====================================================================
// =====================================================================
//  Diagnostics.cpp
//  The runtime self-test and the native-taskbar probe.
//
//  Every assertion below is deliberately phrased as a question about what the
//  system actually did, not about what was requested. The interesting failures
//  in this area of Windows are the silent ones -- a per-monitor-v2 awareness
//  that never took effect, a z-order that quietly dropped out of topmost, a
//  swap chain that presented black outside the pill -- and none of them raise
//  an HRESULT.
//
//  The probe's report goes to the log file as well as to stdout, because
//  stdout has been lost on this machine three separate times.
// =====================================================================
#include "selftest/Diagnostics.h"

#include <windows.h>
#include <shellapi.h>   // APPBARDATA / ABM_* for the native-taskbar probe

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>   // strtoul: unpacking the hex of a captured registration in the tray checks
#include <memory>
#include <thread>
#include <vector>
#include <cwchar>
#include <string>

#include "domain/Geometry.h"
#include "core/Log.h"
#include "domain/MiniJson.h"
#include "domain/TrayRegistration.h"
#include "domain/TrayLedger.h"     // which registrations the shell is holding (see the tray checks)
#include "platform/TrayRing.h"     // the channel the module publishes registrations on
#include "domain/CardLayout.h"
#include "domain/StatePolicy.h"
#include "domain/TrayEncoding.h"   // the only place a tray click is encoded (see its header)
#include "media/Lyrics.h"
#include "app/AppBar.h"
#include "domain/TaskStripLayout.h"
#include "platform/WindowList.h"
#include "media/MusicSession.h"
#include "app/PlayerWindow.h"
#include "media/WicImage.h"
#include "services/Notification.h"
#include "services/NotificationIcon.h"
#include "services/NotificationPipe.h"
#include "services/ToastStore.h"
#include "services/WeChatWatch.h"
#include "shell/Weather.h"
#include "platform/ThumbnailPreview.h"
#include "app/IslandWindow.h"
#include "platform/Monitor.h"
#include "platform/NativeTaskbar.h"   // the three answers to "what happens to the shell's taskbar"
#include "shell/SystemToggles.h"
#include "render/Theme.h"
#include "render/GlassMaterial.h"
#include "shell/Weather.h"   // SkyMarkForCode: the word and the mark come from the same WMO code
#include "domain/Motion.h"
#include "render/Renderer.h"
#include "render/TextRenderer.h"
#include "shell/TrayIcons.h"
#include "shell/TrayPress.h"
#include "shell/TrayLearn.h"   // CountCoveredIcons: how much of the tray a press can reach (see the tray checks)

#include "selftest/Checks.h"
#include "selftest/Harness.h"

namespace island {

void TestMiniJson() {
    Section(L"JSON reader (wire format)");

    const char* simple = R"({"app":"dsh","title":"Build finished","body":"3 warnings"})";
    char value[256] = {};

    Check(json::GetString(simple, "app", value, sizeof(value)) &&
          std::strcmp(value, "dsh") == 0,
          L"reads a flat string");
    Check(json::GetString(simple, "title", value, sizeof(value)) &&
          std::strcmp(value, "Build finished") == 0,
          L"reads a value containing a space");
    Check(!json::GetString(simple, "missing", value, sizeof(value)),
          L"missing key reports false");
    Check(json::GetString(simple, "app", value, 3) && std::strcmp(value, "ds") == 0,
          L"truncates into a small buffer");

    const char* spaced = "{ \"title\" : \"x\" , \"n\" : 42 }";
    Check(json::GetString(spaced, "title", value, sizeof(value)) &&
          std::strcmp(value, "x") == 0,
          L"tolerates whitespace around key and colon");

    long long number = 0;
    Check(json::GetInt(spaced, "n", &number) && number == 42, L"reads an integer");
    Check(!json::GetInt(spaced, "title", &number), L"a string is not an integer");

    const char* negative = R"({"n":-17,"f":1.5})";
    Check(json::GetInt(negative, "n", &number) && number == -17,
          L"reads a negative integer");
    Check(json::GetInt(negative, "f", &number) && number == 1,
          L"truncates a fraction instead of failing");

    // --- the two cases a strstr-based reader gets wrong ---
    //
    // A key inside a nested object, and the key's own name appearing inside another
    // string value. A naive scan matches both, and an untrusted sender can produce
    // both trivially.
    const char* nested = R"({"outer":{"title":"WRONG"},"title":"RIGHT"})";
    Check(json::GetString(nested, "title", value, sizeof(value)) &&
          std::strcmp(value, "RIGHT") == 0,
          L"ignores a key nested inside an object");

    const char* inValue = R"({"body":"the word \"title\":\"nope\" is inside me"})";
    Check(!json::GetString(inValue, "title", value, sizeof(value)),
          L"does not match a key inside another value");

    const char* array = R"({"list":[{"title":"WRONG"}],"title":"RIGHT"})";
    Check(json::GetString(array, "title", value, sizeof(value)) &&
          std::strcmp(value, "RIGHT") == 0,
          L"ignores a key inside an array element");

    // --- escapes ---
    const char* escaped = R"({"title":"a\"b\\c\nd\te"})";
    Check(json::GetString(escaped, "title", value, sizeof(value)) &&
          std::strcmp(value, "a\"b\\c\nd\te") == 0,
          L"decodes quote, backslash, newline and tab");

    const char* unicode = R"({"title":"\u4e2d\u6587"})";
    Check(json::GetString(unicode, "title", value, sizeof(value)) &&
          std::strcmp(value, "\xe4\xb8\xad\xe6\x96\x87") == 0,
          L"decodes \\uXXXX to UTF-8");

    // A surrogate pair is one character; a lone surrogate must not emit invalid
    // UTF-8.
    const char* pair = R"({"title":"\ud83d\ude00"})";
    Check(json::GetString(pair, "title", value, sizeof(value)) &&
          std::strcmp(value, "\xf0\x9f\x98\x80") == 0,
          L"decodes a surrogate pair to one code point");

    const char* lone = R"({"title":"\ud83d"})";
    {
        // A lone surrogate becomes U+FFFD, which is the three UTF-8 bytes
        // EF BF BD -- not a literal '?'. Asserting the '?' byte is what made an
        // earlier version of this check fail against a decoder that was correct.
        char scratch[64] = {};
        const bool ok = json::GetString(lone, "title", scratch, sizeof(scratch));
        wchar_t detail[192];
        std::swprintf(detail, 192, L"ok=%d bytes=%02X %02X %02X len=%zu",
                      ok ? 1 : 0,
                      static_cast<unsigned char>(scratch[0]),
                      static_cast<unsigned char>(scratch[1]),
                      static_cast<unsigned char>(scratch[2]),
                      std::strlen(scratch));
        Check(ok && std::strcmp(scratch, "\xef\xbf\xbd") == 0,
              L"a lone surrogate becomes U+FFFD, not invalid UTF-8", detail);

        // And the pair case must differ: a valid pair is a real character, not the
        // replacement.
        char pairBytes[64] = {};
        Check(json::GetString(pair, "title", pairBytes, sizeof(pairBytes)) &&
              std::strcmp(pairBytes, "\xef\xbf\xbd") != 0,
              L"a valid surrogate pair is not the replacement character");
    }

    // --- malformed input must return, not crash or invent values ---
    const char* cases[] = {
        "",
        "{",
        "}",
        "{\"title\"",
        "{\"title\":}",
        "{\"title\":\"unterminated",
        "[]",
        "null",
        "{\"title\":\"ok\"}garbage",
    };
    int survived = 0;
    for (const char* document : cases) {
        char scratch[256] = {};
        (void)json::GetString(document, "title", scratch, sizeof(scratch));
        (void)json::HasKey(document, "title");
        long long n = 0;
        (void)json::GetInt(document, "title", &n);
        ++survived;
    }
    Check(survived == static_cast<int>(sizeof(cases) / sizeof(cases[0])),
          L"malformed documents return instead of crashing");

    Check(json::HasKey("{\"a\":null}", "a"), L"null counts as present");
    Check(json::HasKey("{\"a\":true}", "a"), L"true counts as present");

    // --- escaping round-trip ---
    char escapedOut[512] = {};
    json::Escape("quote\" back\\ newline\n tab\t end", escapedOut, sizeof(escapedOut));
    Check(std::strcmp(escapedOut, "quote\\\" back\\\\ newline\\n tab\\t end") == 0,
          L"escapes the characters JSON requires");

    char decoded[512] = {};
    json::Unescape(escapedOut, std::strlen(escapedOut), decoded, sizeof(decoded));
    Check(std::strcmp(decoded, "quote\" back\\ newline\n tab\t end") == 0,
          L"escape then unescape is the identity");

    // A control character has no short escape and must come out as \uXXXX, or the
    // document we emit would itself be invalid.
    char control[64] = {};
    json::Escape("\x01", control, sizeof(control));
    Check(std::strcmp(control, "\\u0001") == 0, L"escapes a control character");
}

void TestNotifications() {
    Section(L"notifications");

    Notification parsed;
    Check(NotificationFromJson(R"({"app":"dsh","title":"Build ok","body":"clean"})", &parsed),
          L"parses a complete message");
    Check(std::strcmp(parsed.app, "dsh") == 0 &&
          std::strcmp(parsed.title, "Build ok") == 0 &&
          std::strcmp(parsed.body, "clean") == 0,
          L"fields land in the right places");

    // The one rejection that matters: a blank card is indistinguishable from a
    // rendering bug, so an empty message must never reach the screen.
    Check(!NotificationFromJson(R"({"app":"dsh"})", &parsed),
          L"rejects a message with neither title nor body");
    Check(!NotificationFromJson(R"({"title":"","body":""})", &parsed),
          L"rejects an empty title and body");
    Check(NotificationFromJson(R"({"title":"only a title"})", &parsed),
          L"accepts a title with no body");
    Check(NotificationFromJson(R"({"body":"only a body"})", &parsed),
          L"accepts a body with no title");

    Check(NotificationFromJson(R"({"title":"x","urgency":"high"})", &parsed) &&
          parsed.urgency == Urgency::High,
          L"parses urgency");
    Check(NotificationFromJson(R"({"title":"x","urgency":"nonsense"})", &parsed) &&
          parsed.urgency == Urgency::Normal,
          L"an unknown urgency falls back to normal");
    Check(NotificationFromJson(R"({"title":"x","sticky":1})", &parsed) && parsed.sticky,
          L"parses sticky");

    // --- round-trip through the wire format ---
    {
        char document[2048] = {};
        NotificationToJson(parsed, document, sizeof(document), false);
        Notification roundTripped;
        Check(NotificationFromJson(document, &roundTripped) &&
              std::strcmp(roundTripped.title, parsed.title) == 0 &&
              roundTripped.sticky == parsed.sticky,
              L"serialise then parse is the identity");

        // Delimiter characters in a title must survive, or the wire format would
        // corrupt ordinary text.
        Notification tricky;
        strncpy_s(tricky.title, kTitleCapacity, "quote\" brace} comma, newline\n", _TRUNCATE);
        char trickyDoc[2048] = {};
        NotificationToJson(tricky, trickyDoc, sizeof(trickyDoc), false);
        Notification back;
        Check(NotificationFromJson(trickyDoc, &back) &&
              std::strcmp(back.title, tricky.title) == 0,
              L"delimiter characters survive a round trip");
    }

    // --- the store ---
    NotificationStore store;
    Check(store.Count() == 0 && store.UnreadCount() == 0, L"a new store is empty");

    const Notification* first = store.Push(parsed);
    Check(first != nullptr && first->id == 1, L"push assigns an increasing id");
    Check(store.Count() == 1 && store.UnreadCount() == 1, L"push counts as unread");

    Notification second = parsed;
    strncpy_s(second.title, kTitleCapacity, "second", _TRUNCATE);
    const Notification* stored = store.Push(second);
    Check(stored != nullptr && stored->id == 2, L"the second id follows");

    Check(store.Count() == 2 && store.UnreadCount() == 2, L"both are unread");
    Check(store.At(0) != nullptr && std::strcmp(store.At(0)->title, "second") == 0,
          L"index 0 is the newest");
    Check(store.At(1) != nullptr && std::strcmp(store.At(1)->title, parsed.title) == 0,
          L"index 1 is the older one");
    Check(store.At(2) == nullptr, L"out-of-range index is null");

    // --- the display window ---
    {
        NotificationStore display;
        Notification item;
        strncpy_s(item.title, kTitleCapacity, "visible", _TRUNCATE);
        const Notification* pushed = display.Push(item);
        const unsigned long long stamp = pushed->timestampMs;

        Check(display.ActiveDisplay(stamp) != nullptr, L"a fresh item is displayed");
        Check(display.ActiveDisplay(stamp + kNotificationDisplayMs - 1) != nullptr,
              L"still displayed just inside the window");
        Check(display.ActiveDisplay(stamp + kNotificationDisplayMs + 1) == nullptr,
              L"stops being displayed after the window");
        // A timestamp before the item (a tick wrap, or a test) must read as new,
        // not as expired.
        Check(display.ActiveDisplay(stamp - 1000) != nullptr,
              L"a future timestamp reads as fresh");

        display.MarkAllRead();
        Check(display.ActiveDisplay(stamp) == nullptr, L"a read item is not displayed");

        Notification sticky;
        strncpy_s(sticky.title, kTitleCapacity, "sticky", _TRUNCATE);
        sticky.sticky = true;
        const Notification* stickyStored = display.Push(sticky);
        Check(display.ActiveDisplay(stickyStored->timestampMs + 10 * kNotificationDisplayMs)
                  != nullptr,
              L"a sticky item outlives the display window");

        // A CALL SURVIVES THE MESSAGE THAT ARRIVED DURING IT.
        //
        // The call is pushed sticky so the island holds it for the whole call, and the store keeps it -- but
        // `ActiveDisplay` answers with the NEWEST item only. So a message arriving one second into the call
        // took the island, expired six seconds later, and the call was never shown again while it was still
        // ringing. Reported as "微信通话 不显示了", with the log to match (`call announced as '通话中'` at
        // 13:37:22, `有新消息` at 13:37:23, `state: music-strip` at 13:37:29 -- call still up, island empty).
        //
        // The frame loop's rule is these three facts together, and all three are asserted here: a fresh
        // message WINS, the call is still there to fall back to once the message is done, and reading it
        // (a click on the island) retires it for good.
        {
            NotificationStore calls;
            Notification call;
            std::snprintf(call.title, kTitleCapacity, "call");
            call.sticky = true;
            const Notification* storedCall = calls.Push(call);
            const long long callId = storedCall->id;

            Notification message;
            std::snprintf(message.title, kTitleCapacity, "message");
            const Notification* storedMessage = calls.Push(message);
            const unsigned long long messageAt = storedMessage->timestampMs;

            Check(calls.ActiveDisplay(messageAt + 100) == storedMessage,
                  L"a fresh message wins over the call that is still up");
            Check(calls.ActiveDisplay(messageAt + kNotificationDisplayMs + 100) == nullptr,
                  L"... and stops being displayed when its window is over");
            const Notification* held = calls.Find(callId);
            Check(held != nullptr && held->sticky && !held->read,
                  L"the call is still there to hold the island after the message");

            Check(calls.Find(callId + 999) == nullptr, L"an unknown id finds nothing");
            calls.MarkRead(callId);
            const Notification* retired = calls.Find(callId);
            Check(retired != nullptr && retired->read, L"a read call is not held again");

            // ... and A CLICK DISMISSES MESSAGES, NOT THE CALL. `MarkAllReadExcept` is what the click handler
            // calls now; with `MarkAllRead` the call was read by the same click that dismissed a message, and
            // the strip never came back for the rest of the call (see the log line in Notification.h).
            NotificationStore clicking;
            Notification call2;
            std::snprintf(call2.title, kTitleCapacity, "call");
            call2.sticky = true;
            const long long call2Id = clicking.Push(call2)->id;
            Notification message2;
            std::snprintf(message2.title, kTitleCapacity, "message");
            clicking.Push(message2);
            const int dismissed = clicking.MarkAllReadExcept(call2Id);
            const Notification* keptCall = clicking.Find(call2Id);
            Check(dismissed == 1 && keptCall != nullptr && !keptCall->read,
                  L"a click reads the message and leaves the call alone");
            Check(clicking.UnreadCount() == 1, L"the call is the only thing still unread");
        }
    }

    // --- bounded capacity ---
    {
        NotificationStore bounded;
        Notification item;
        for (int i = 0; i < kMaxNotifications + 10; ++i) {
            std::snprintf(item.title, kTitleCapacity, "item %d", i);
            bounded.Push(item);
        }
        Check(bounded.Count() == kMaxNotifications,
              L"the store never exceeds its capacity");

        // The newest must still be present, and must be the last one pushed: a ring
        // that overwrote the wrong end would lose exactly what matters.
        char expected[32] = {};
        std::snprintf(expected, sizeof(expected), "item %d", kMaxNotifications + 9);
        Check(bounded.At(0) != nullptr &&
              std::strcmp(bounded.At(0)->title, expected) == 0,
              L"the newest item survived the overflow");
    }

    // --- marking read ---
    {
        NotificationStore marks;
        Notification item;
        strncpy_s(item.title, kTitleCapacity, "x", _TRUNCATE);
        marks.Push(item);
        marks.Push(item);
        Check(marks.MarkAllRead() == 2, L"marking reports how many changed");
        Check(marks.UnreadCount() == 0, L"nothing unread afterwards");
        Check(marks.MarkAllRead() == 0, L"marking again changes nothing");

        marks.Clear();
        Check(marks.Count() == 0 && marks.UnreadCount() == 0, L"clear empties the store");
    }

    // --- cancellable server I/O -------------------------------------------------
    //
    // These use a private pipe name so they cannot collide with the running bar. The first client
    // connects and sends nothing; the second sends only two bytes of the length header. Stop() must
    // return promptly in both cases because both operations are now overlapped and cancellable.
    {
        // A client must not wait forever for an acknowledgement the server never sends.
        wchar_t timeoutPipeName[80] = {};
        std::swprintf(timeoutPipeName, 80, L"IslandBar.Verify.Timeout.%lu", GetCurrentProcessId());
        wchar_t timeoutPipe[96] = {};
        std::swprintf(timeoutPipe, 96, L"\\\\.\\pipe\\%s", timeoutPipeName);
        HANDLE silentServer = CreateNamedPipeW(
            timeoutPipe, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, kMaxMessageBytes + sizeof(uint32_t), kMaxMessageBytes + sizeof(uint32_t),
            0, nullptr);
        Check(silentServer != INVALID_HANDLE_VALUE, L"a no-response test server is created");
        HANDLE releaseSilent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread silentThread([&]() {
            (void)ConnectNamedPipe(silentServer, nullptr);
            WaitForSingleObject(releaseSilent, 5000);
        });
        Notification timeoutNotification;
        std::snprintf(timeoutNotification.title, kTitleCapacity, "timeout");
        char timeoutResponse[256] = {};
        const DWORD timeoutStarted = GetTickCount();
        const bool timeoutAcknowledged =
            SendNotification(timeoutNotification, timeoutPipeName, timeoutResponse,
                             sizeof(timeoutResponse), 2000, 200);
        const DWORD timeoutElapsed = GetTickCount() - timeoutStarted;
        SetEvent(releaseSilent);
        silentThread.join();
        CloseHandle(releaseSilent);
        if (silentServer != INVALID_HANDLE_VALUE) CloseHandle(silentServer);
        Check(!timeoutAcknowledged && timeoutElapsed < 1500,
              L"the notification client times out when no acknowledgement arrives");

        wchar_t pipeName[80] = {};
        std::swprintf(pipeName, 80, L"IslandBar.Verify.%lu", GetCurrentProcessId());
        wchar_t fullName[96] = {};
        std::swprintf(fullName, 96, L"\\\\.\\pipe\\%s", pipeName);
        const auto connectClient = [&]() {
            (void)WaitNamedPipeW(fullName, 2000);
            return CreateFileW(fullName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        };

        auto server = std::make_shared<NotificationServer>();
        Check(server->Start(pipeName), L"the notification server starts on a test pipe");
        wchar_t remoteName[112] = {};
        std::swprintf(remoteName, 112, L"\\\\127.0.0.1\\pipe\\%s", pipeName);
        const HANDLE remoteClient = CreateFileW(
            remoteName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        const DWORD remoteError = GetLastError();
        const bool remoteRejected = remoteClient == INVALID_HANDLE_VALUE;
        if (remoteClient != INVALID_HANDLE_VALUE) CloseHandle(remoteClient);
        wchar_t remoteDetail[96] = {};
        std::swprintf(remoteDetail, 96, L"CreateFile win32=%lu", remoteError);
        Check(remoteRejected && remoteError == ERROR_ACCESS_DENIED,
              L"the notification pipe rejects a remote loopback client", remoteDetail);
        HANDLE silentClient = connectClient();
        Check(silentClient != INVALID_HANDLE_VALUE,
              L"a test client connects before sending anything");
        const DWORD silentStarted = GetTickCount();
        server->Stop();
        const DWORD silentStoppedMs = GetTickCount() - silentStarted;
        Check(!server->IsRunning() && silentStoppedMs < 2000,
              L"Stop() cancels a connected client that sends no data");
        if (silentClient != INVALID_HANDLE_VALUE) CloseHandle(silentClient);

        Check(server->Start(pipeName), L"the notification server restarts after cancellation");
        HANDLE partialClient = connectClient();
        const uint32_t claimedLength = 128;
        DWORD partialWritten = 0;
        Check(partialClient != INVALID_HANDLE_VALUE &&
                  WriteFile(partialClient, &claimedLength, 2, &partialWritten, nullptr) &&
                  partialWritten == 2,
              L"a test client sends a deliberately partial frame header");
        const DWORD partialStarted = GetTickCount();
        server->Stop();
        const DWORD partialStoppedMs = GetTickCount() - partialStarted;
        Check(!server->IsRunning() && partialStoppedMs < 2000,
              L"Stop() cancels a partially received frame");
        if (partialClient != INVALID_HANDLE_VALUE) CloseHandle(partialClient);

        Check(server->Start(pipeName), L"the notification server starts for a complete frame");
        Notification complete;
        std::snprintf(complete.title, kTitleCapacity, "pipe test");
        char response[256] = {};
        wchar_t responseDetail[320] = {};
        const bool acknowledged =
            SendNotification(complete, pipeName, response, sizeof(response), 2000);
        std::swprintf(responseDetail, 320, L"response='%hs'", response);
        Check(acknowledged, L"a complete framed notification is acknowledged", responseDetail);
        Notification received;
        bool took = false;
        for (int i = 0; i < 50 && !took; ++i) {
            took = server->Take(&received);
            if (!took) Sleep(2);
        }
        Check(took && std::strcmp(received.title, "pipe test") == 0,
              L"the acknowledged notification reaches the queue");
        server->Stop();
    }
}

} // namespace island
