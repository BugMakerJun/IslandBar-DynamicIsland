// =====================================================================
//  TrayRegistration.h -- WHAT AN APPLICATION SENDS THE SHELL WHEN IT REGISTERS A TRAY ICON
//
//  This is the wire format, in one place and with no Windows in it, for two reasons: the module that reads it
//  lives inside OTHER processes (src/trayhook) and the self-test has to be able to check the reading against
//  real bytes without a shell, a tray or an application anywhere in the room.
//
//  HOW THE LAYOUT WAS ESTABLISHED. Not by reading a header: `Shell_NotifyIcon` sends the shell a WM_COPYDATA
//  whose payload is the shell's own copy of the registration, and its fields were found by making a
//  REGISTRATION WITH KNOWN VALUES and looking for them in the bytes -- probe-tray-registrar.exe registers uid
//  7, message 0xABCD, tip "ISLANDBAR-PROBE-4711" and then NIM_SETVERSION 4, and the offsets below are where
//  those values appeared. Everything was then cross-checked against answers the bar already had: Steam's copy
//  names uid 1 with message 0x0401 and the tip "Steam", and the GUID-registered icons carry exactly the guids
//  the shell's own registry entries name. The raw captures are in reviews/tray-copydata-dump.txt.
//
//  WHAT THIS IS NOT: a stable ABI. It is the layout of ONE Windows build as measured on 2026-10-15 (Windows 11,
//  10.0.26220). The tag is checked, the size is checked, and a payload that does not match is REJECTED rather
//  than read at these offsets -- because the alternative is a bar that quietly learns wrong message numbers
//  from a format it never understood. If a future build moves a field, the right outcome is "no entries
//  learned", which is visible, and not "entries learned from the wrong bytes", which is not.
// =====================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace island {

// The shell's own marker at offset 0, in every copy seen on this machine.
inline constexpr unsigned kTrayCopyTag = 0x34753423u;

// The smallest payload worth looking at: the structure is about 1,484 bytes in practice (its size is its own
// field on the wire), and the guid does not appear until +0x3B0, so anything shorter cannot be a registration.
inline constexpr size_t kTrayCopyMinimum = 1024;

// The command, at +0x004. These are the NIM_ values, and they are the shell's copy of the call the
// application made, not a separate protocol.
enum TrayCopyCommand : unsigned {
    kTrayCopyAdd = 0,
    kTrayCopyModify = 1,
    kTrayCopyDelete = 2,
    kTrayCopySetFocus = 3,
    kTrayCopySetVersion = 4,
};

// The offsets, named once so the module and the test cannot drift apart.
inline constexpr size_t kTrayCopyOffsetCommand = 0x004;
inline constexpr size_t kTrayCopyOffsetWindow = 0x00C;   // the application's tray window, low 32 bits
inline constexpr size_t kTrayCopyOffsetUid = 0x010;
inline constexpr size_t kTrayCopyOffsetFlags = 0x014;    // NIF_GUID (0x20) says the icon is named by guid
inline constexpr size_t kTrayCopyOffsetCallback = 0x018; // uCallbackMessage: what to send the application
inline constexpr size_t kTrayCopyOffsetTip = 0x020;      // wide, 128 characters
inline constexpr size_t kTrayCopyOffsetVersion = 0x328;  // non-zero only in the SETVERSION copy
inline constexpr size_t kTrayCopyOffsetGuid = 0x3B0;     // 16 raw bytes

inline constexpr unsigned kTrayFlagsGuid = 0x20;         // NIF_GUID
inline constexpr size_t kTrayTipCharacters = 128;

// One registration, decoded. `valid` is the only thing a caller has to check before using the rest.
struct TrayRegistration {
    bool valid = false;
    unsigned command = 0;
    // The shell's private payload currently stores a DWORD here, but the decoded form is our ABI: keeping it
    // 64-bit prevents the rest of the product from treating a Win64 HWND as permanently 32-bit.
    std::uint64_t trayWindow = 0;
    unsigned uid = 0;
    unsigned flags = 0;
    unsigned callback = 0;
    unsigned version = 0;
    bool hasGuid = false;
    unsigned char guid[16] = {};
    wchar_t tip[kTrayTipCharacters] = {};
};

// Decodes one payload. Pure: no Windows, no allocation, no state -- `bytes` may be a COPYDATASTRUCT's lpData
// inside whatever process is calling, which is why nothing here is kept after it returns.
//
// A payload that is too small, or whose tag is not the shell's, comes back with valid == false and every
// other field zero. A payload whose tag IS the shell's but whose command is not a NIM_ value is rejected the
// same way: a copy from this format that is not a registration is not something to guess about.
inline TrayRegistration DecodeTrayRegistration(const void* bytes, size_t size) {
    TrayRegistration out;
    if (!bytes || size < kTrayCopyMinimum) return out;
    const unsigned char* raw = static_cast<const unsigned char*>(bytes);

    unsigned tag = 0;
    std::memcpy(&tag, raw, sizeof(tag));
    if (tag != kTrayCopyTag) return out;

    unsigned command = 0;
    std::memcpy(&command, raw + kTrayCopyOffsetCommand, sizeof(command));
    if (command > kTrayCopySetVersion) return out;
    out.command = command;

    unsigned trayWindow32 = 0;
    std::memcpy(&trayWindow32, raw + kTrayCopyOffsetWindow, sizeof(trayWindow32));
    out.trayWindow = trayWindow32;
    std::memcpy(&out.uid, raw + kTrayCopyOffsetUid, sizeof(out.uid));
    std::memcpy(&out.flags, raw + kTrayCopyOffsetFlags, sizeof(out.flags));
    std::memcpy(&out.callback, raw + kTrayCopyOffsetCallback, sizeof(out.callback));
    std::memcpy(&out.version, raw + kTrayCopyOffsetVersion, sizeof(out.version));
    std::memcpy(out.guid, raw + kTrayCopyOffsetGuid, sizeof(out.guid));
    out.hasGuid = (out.flags & kTrayFlagsGuid) != 0;

    for (size_t i = 0; i + 1 < kTrayTipCharacters; ++i) {
        wchar_t character = 0;
        std::memcpy(&character, raw + kTrayCopyOffsetTip + i * sizeof(wchar_t), sizeof(character));
        if (!character) break;
        out.tip[i] = character;
    }

    out.valid = true;
    return out;
}

} // namespace island
