// =====================================================================
//  tools/tray_probe/protocol_test.cpp
//  Boundary checks for the decoded tray registration and ring ABI.
// =====================================================================

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "domain/TrayRegistration.h"
#include "platform/TrayRing.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

template <typename T>
void Put(std::vector<unsigned char>* bytes, size_t offset, const T& value) {
    std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

bool WideFromAscii(const char* text, wchar_t* out, size_t capacity) {
    if (!text || !out || capacity == 0) return false;
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, out, static_cast<int>(capacity));
    return count > 0;
}

int RunRingWriter(const char* ring, const char* eventName) {
    wchar_t wideRing[128] = {};
    wchar_t wideEvent[160] = {};
    if (!WideFromAscii(ring, wideRing, 128) || !WideFromAscii(eventName, wideEvent, 160)) return 2;
    island::TrayRingPublisher publisher;
    if (!publisher.Open(wideRing, wideEvent)) return 3;
    island::TrayRegistration registration;
    registration.valid = true;
    registration.command = island::kTrayCopyAdd;
    registration.trayWindow = 0x1234;
    registration.callback = 0x0400;
    for (unsigned i = 0; i < 1000; ++i) {
        registration.uid = i;
        publisher.Publish(registration);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--ring-writer") == 0) {
        return RunRingWriter(argv[2], argv[3]);
    }
    static_assert(sizeof(decltype(island::TrayRegistration::trayWindow)) == sizeof(std::uint64_t));
    static_assert(offsetof(island::TrayRingRecord, registration) >= sizeof(island::TrayRingKind));
    static_assert(island::kTrayRingAbi == 6);

    std::vector<unsigned char> payload(1484, 0);
    Put(&payload, island::kTrayCopyOffsetCommand, island::kTrayCopyAdd);
    const unsigned maxWindow32 = 0xFFFFFFFFu;
    Put(&payload, island::kTrayCopyOffsetWindow, maxWindow32);
    const unsigned uid = 0xFFFFFFFFu;
    Put(&payload, island::kTrayCopyOffsetUid, uid);
    const unsigned flags = island::kTrayFlagsGuid;
    Put(&payload, island::kTrayCopyOffsetFlags, flags);
    const unsigned callback = 0xDEADu;
    Put(&payload, island::kTrayCopyOffsetCallback, callback);
    const unsigned version = 4;
    Put(&payload, island::kTrayCopyOffsetVersion, version);
    const wchar_t tip[] = L"protocol-test";
    std::memcpy(payload.data() + island::kTrayCopyOffsetTip, tip, sizeof(tip));
    const unsigned char guid[16] = {
        0x8B, 0x8D, 0x76, 0xE5, 0x36, 0x69, 0x45, 0x4F,
        0xB1, 0xAD, 0x4C, 0x5F, 0xB4, 0x14, 0xCB, 0x35,
    };
    std::memcpy(payload.data() + island::kTrayCopyOffsetGuid, guid, sizeof(guid));
    const unsigned tag = island::kTrayCopyTag;
    Put(&payload, 0, tag);

    const island::TrayRegistration decoded = island::DecodeTrayRegistration(payload.data(), payload.size());
    Check(decoded.valid, "valid registration decodes");
    Check(decoded.trayWindow == 0xFFFFFFFFull, "maximum 32-bit wire HWND widens without loss");
    Check(decoded.uid == uid, "uid boundary survives");
    Check(decoded.callback == callback, "callback message survives");
    Check(decoded.version == version, "version survives");
    Check(decoded.hasGuid && std::memcmp(decoded.guid, guid, sizeof(guid)) == 0, "guid survives");
    Check(std::wcscmp(decoded.tip, tip) == 0, "tooltip survives");

    payload[0] ^= 0xFF;
    Check(!island::DecodeTrayRegistration(payload.data(), payload.size()).valid, "wrong tag is rejected");
    payload[0] ^= 0xFF;
    Check(!island::DecodeTrayRegistration(payload.data(), 8).valid, "short payload is rejected");

    const unsigned unknown = 99;
    Put(&payload, island::kTrayCopyOffsetCommand, unknown);
    Check(!island::DecodeTrayRegistration(payload.data(), payload.size()).valid,
          "unknown command is rejected");

    wchar_t ringName[96] = {};
    wchar_t eventName[112] = {};
    _snwprintf_s(ringName, 96, _TRUNCATE, L"Local\\IslandBar.TrayProtocolTest.%lu.%llu",
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long long>(GetTickCount64()));
    _snwprintf_s(eventName, 112, _TRUNCATE, L"%ls.Event", ringName);

    island::TrayRingPublisher publisher;
    island::TrayRingReader reader;
    Check(publisher.Open(ringName, eventName) && reader.Open(ringName, eventName),
          "independent test ring opens");

    wchar_t self[MAX_PATH] = {};
    char ringAscii[160] = {};
    char eventAscii[192] = {};
    WideCharToMultiByte(CP_UTF8, 0, ringName, -1, ringAscii, sizeof(ringAscii), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, eventName, -1, eventAscii, sizeof(eventAscii), nullptr, nullptr);
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    PROCESS_INFORMATION children[2] = {};
    HANDLE childHandles[2] = {};
    bool childrenStarted = true;
    for (unsigned child = 0; child < 2; ++child) {
        wchar_t commandLine[512] = {};
        _snwprintf_s(commandLine, 512, _TRUNCATE, L"\"%ls\" --ring-writer %hs %hs", self,
                     ringAscii, eventAscii);
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(self, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &startup, &children[child])) {
            childrenStarted = false;
            break;
        }
        childHandles[child] = children[child].hProcess;
    }

    island::TrayRegistration registration = island::DecodeTrayRegistration(payload.data(), payload.size());
    registration.valid = true;
    registration.command = island::kTrayCopyAdd;
    constexpr unsigned kWriterCount = 4;
    constexpr unsigned kRecordsPerWriter = 1000;
    std::atomic<unsigned> activeWriters{kWriterCount};
    std::vector<std::thread> writers;
    for (unsigned writerIndex = 0; writerIndex < kWriterCount; ++writerIndex) {
        writers.emplace_back([&publisher, registration, writerIndex, &activeWriters]() {
            for (unsigned i = 0; i < kRecordsPerWriter; ++i) {
                island::TrayRegistration item = registration;
                item.uid = writerIndex * kRecordsPerWriter + i;
                publisher.Publish(item);
            }
            activeWriters.fetch_sub(1, std::memory_order_release);
        });
    }

    std::uint64_t cursor = 0;
    int returned = 0;
    bool allRecordsValid = true;
    island::TrayRingRecord records[island::kTrayRingCapacity] = {};
    while (activeWriters.load(std::memory_order_acquire) != 0) {
        const unsigned count = reader.Drain(cursor, records, island::kTrayRingCapacity);
        for (unsigned i = 0; i < count; ++i) {
            if (records[i].kind != island::TrayRingKind::Registration ||
                !records[i].registration.valid ||
                records[i].registration.uid >= kWriterCount * kRecordsPerWriter) {
                allRecordsValid = false;
            }
        }
        returned += static_cast<int>(count);
        if (count == 0) std::this_thread::yield();
    }
    for (std::thread& writer : writers) writer.join();
    bool childrenSucceeded = childrenStarted;
    if (childrenStarted) {
        const DWORD wait = WaitForMultipleObjects(2, childHandles, TRUE, 15000);
        childrenSucceeded = wait == WAIT_OBJECT_0;
        for (unsigned child = 0; child < 2; ++child) {
            DWORD exitCode = 1;
            if (!GetExitCodeProcess(children[child].hProcess, &exitCode) || exitCode != 0) {
                childrenSucceeded = false;
            }
        }
    }
    for (unsigned child = 0; child < 2; ++child) {
        if (children[child].hThread) CloseHandle(children[child].hThread);
        if (children[child].hProcess) CloseHandle(children[child].hProcess);
    }
    for (;;) {
        const unsigned count = reader.Drain(cursor, records, island::kTrayRingCapacity);
        for (unsigned i = 0; i < count; ++i) {
            if (records[i].kind != island::TrayRingKind::Registration ||
                !records[i].registration.valid ||
                records[i].registration.uid >= kWriterCount * kRecordsPerWriter) {
                allRecordsValid = false;
            }
        }
        if (count == 0) break;
        returned += static_cast<int>(count);
    }
    Check(returned > 0 && allRecordsValid,
          "thread and process writers never produced a torn or mistyped record");
    Check(childrenSucceeded, "two additional writer processes completed through the same ring");
    Check(reader.Published() <= (kWriterCount + 2) * kRecordsPerWriter,
          "the bounded ring cannot publish more records than were offered");

    wchar_t boundedName[112] = {};
    wchar_t boundedEvent[128] = {};
    _snwprintf_s(boundedName, 112, _TRUNCATE, L"%ls.Bounded", ringName);
    _snwprintf_s(boundedEvent, 128, _TRUNCATE, L"%ls.Event", boundedName);
    island::TrayRingPublisher boundedPublisher;
    island::TrayRingReader boundedReader;
    const bool boundedOpened =
        boundedPublisher.Open(boundedName, boundedEvent) &&
        boundedReader.Open(boundedName, boundedEvent);
    for (unsigned i = 0; i < island::kTrayRingCapacity; ++i) {
        island::TrayRegistration item = registration;
        item.uid = i;
        boundedPublisher.Publish(item);
    }
    boundedPublisher.Publish(registration);
    Check(boundedOpened && boundedReader.Published() == island::kTrayRingCapacity &&
              boundedReader.DroppedFull() == 1,
          "a full ring drops the new record instead of overwriting unread data");

    wchar_t stressName[112] = {};
    wchar_t stressEvent[128] = {};
    _snwprintf_s(stressName, 112, _TRUNCATE, L"%ls.Stress", ringName);
    _snwprintf_s(stressEvent, 128, _TRUNCATE, L"%ls.Event", stressName);
    island::TrayRingPublisher stressPublisher;
    island::TrayRingReader stressReader;
    constexpr unsigned kStressRecords = 100000;
    bool stressOpened =
        stressPublisher.Open(stressName, stressEvent) && stressReader.Open(stressName, stressEvent);
    std::uint64_t stressCursor = 0;
    unsigned stressRead = 0;
    bool stressOrdered = true;
    for (unsigned base = 0; stressOpened && base < kStressRecords; base += island::kTrayRingCapacity) {
        const unsigned batch =
            (kStressRecords - base < island::kTrayRingCapacity) ? kStressRecords - base
                                                                : island::kTrayRingCapacity;
        for (unsigned i = 0; i < batch; ++i) {
            island::TrayRegistration item = registration;
            item.uid = base + i;
            stressPublisher.Publish(item);
        }
        const unsigned count = stressReader.Drain(stressCursor, records, island::kTrayRingCapacity);
        if (count != batch) {
            stressOpened = false;
            break;
        }
        for (unsigned i = 0; i < count; ++i) {
            if (records[i].kind != island::TrayRingKind::Registration ||
                records[i].registration.uid != base + i) {
                stressOrdered = false;
            }
        }
        stressRead += count;
    }
    Check(stressOpened && stressRead == kStressRecords && stressOrdered,
          "100000 sustained registrations cross the 64-slot ring without loss or reordering");

    wchar_t wrapName[112] = {};
    wchar_t wrapEvent[128] = {};
    _snwprintf_s(wrapName, 112, _TRUNCATE, L"%ls.Wrap", ringName);
    _snwprintf_s(wrapEvent, 128, _TRUNCATE, L"%ls.Event", wrapName);
    island::TrayRingPublisher wrapPublisher;
    island::TrayRingReader wrapReader;
    const bool wrapOpened =
        wrapPublisher.Open(wrapName, wrapEvent) && wrapReader.Open(wrapName, wrapEvent);
    const std::uint64_t wrapStart = 0xFFFFFFF0ull;
    if (wrapOpened && wrapPublisher.header) {
        wrapPublisher.header->written = wrapStart;
        wrapPublisher.header->read = wrapStart;
    }
    constexpr unsigned kWrapRecords = 32;
    for (unsigned i = 0; i < kWrapRecords; ++i) {
        island::TrayRegistration item = registration;
        item.uid = i;
        wrapPublisher.Publish(item);
    }
    std::uint64_t wrapCursor = wrapStart;
    const unsigned wrapped = wrapReader.Drain(wrapCursor, records, island::kTrayRingCapacity);
    Check(wrapOpened && wrapped == kWrapRecords &&
              wrapCursor == wrapStart + kWrapRecords,
          "a 64-bit reader cursor crosses the 32-bit boundary without wrapping");

    wchar_t mismatchName[112] = {};
    wchar_t mismatchEvent[128] = {};
    _snwprintf_s(mismatchName, 112, _TRUNCATE, L"%ls.Mismatch", ringName);
    _snwprintf_s(mismatchEvent, 128, _TRUNCATE, L"%ls.Event", mismatchName);
    island::TrayRingPublisher mismatchPublisher;
    const bool mismatchOpened = mismatchPublisher.Open(mismatchName, mismatchEvent);
    if (mismatchOpened && mismatchPublisher.header) {
        mismatchPublisher.header->abi = static_cast<LONG>(island::kTrayRingAbi + 1);
    }
    island::TrayRingReader mismatchReader;
    std::uint64_t mismatchCursor = 0;
    island::TrayRingRecord mismatchRecord;
    Check(!mismatchReader.Open(mismatchName, mismatchEvent) &&
              !mismatchReader.Open(mismatchName, mismatchEvent) &&
              mismatchReader.Drain(mismatchCursor, &mismatchRecord, 1) == 0,
          "an incompatible ABI remains refused after the first Open");
    if (mismatchOpened && mismatchPublisher.header) {
        mismatchPublisher.header->abi = static_cast<LONG>(island::kTrayRingAbi);
    }

    std::printf("RESULT: %s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
