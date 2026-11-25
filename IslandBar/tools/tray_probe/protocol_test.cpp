// =====================================================================
//  tools/tray_probe/protocol_test.cpp
//  Boundary checks for the decoded tray registration and ring ABI.
// =====================================================================

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
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

} // namespace

int main() {
    static_assert(sizeof(decltype(island::TrayRegistration::trayWindow)) == sizeof(std::uint64_t));
    static_assert(offsetof(island::TrayRingRecord, registration) >= sizeof(island::TrayRingKind));
    static_assert(island::kTrayRingAbi == 5);

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

    island::TrayRegistration registration = island::DecodeTrayRegistration(payload.data(), payload.size());
    registration.valid = true;
    registration.command = island::kTrayCopyAdd;
    std::thread writer([&publisher, registration]() {
        for (unsigned i = 0; i < 1000; ++i) {
            island::TrayRegistration item = registration;
            item.uid = i;
            publisher.Publish(item);
        }
    });

    unsigned cursor = 0;
    int returned = 0;
    bool allStable = true;
    island::TrayRingRecord records[island::kTrayRingCapacity] = {};
    while (returned < 64) {
        const unsigned count = reader.Drain(cursor, records, island::kTrayRingCapacity);
        for (unsigned i = 0; i < count; ++i) {
            if (!records[i].registration.valid || records[i].publishSequence == 0) allStable = false;
        }
        returned += static_cast<int>(count);
        if (count == 0) std::this_thread::yield();
    }
    writer.join();
    Check(returned > 0 && allStable, "concurrent ring drain never returned a torn record");

    std::printf("RESULT: %s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
