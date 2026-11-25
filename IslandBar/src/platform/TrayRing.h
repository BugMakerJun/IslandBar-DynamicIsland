// =====================================================================
//  TrayRing.h -- HOW THE MODULE TELLS THE BAR ABOUT A REGISTRATION
//
//  WHAT THIS REPLACES, AND WHY. The module used to append a text line to a file beside the bar and the bar
//  polled that file every five seconds. Two things were wrong with it. First, the write happens INSIDE the
//  application's registration call: WM_COPYDATA is synchronous, so the application waits for the shell to
//  finish handling it, and the shell is where this module runs -- a file open/write/close in that path is the
//  shell making somebody else's application wait for a disk. Second, "the report starts empty" depended on the
//  bar being able to delete a file that a pinned module was holding open, which is exactly the kind of coupling
//  that fails silently (it did: see HANDOFF 8.121).
//
//  WHAT IT IS INSTEAD. A shared-memory ring of fixed-size records and one named event. The module copies a
//  record and sets the event (microseconds, no I/O, no allocation); the bar drains whatever is new when it
//  looks. The file does not go away -- it becomes the DURABLE COPY, written by the bar when it drains, so the
//  corpus of what has been learned survives a restart exactly as before, and the reader that turns lines into
//  cache entries is unchanged.
//
//  THE VERSION IS IN THE DATA, not in a comment: the section starts with a header carrying a magic, an ABI
//  version, the record size and the capacity. A publisher that finds a different ABI does not write (and says
//  so once); a reader that finds one does not read. Refusing loudly is the whole point -- two builds that
//  disagree about the layout must not exchange half-understood records, because the failure that produces is a
//  press aimed at the wrong message number, which looks like nothing at all.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

#include "domain/TrayRegistration.h"

namespace island {

// Bump this when TrayRingRecord's layout changes in any way. Nothing else needs to move with it: the name
// carries the version too, so two builds simply use two different rings and neither reads the other.
//
// ABI 2 added the CONTROL half, ABI 3 widened the decoded window identity, ABI 4 added the publication
// timestamp, and ABI 5 added per-slot sequencing so readers never return a half-written record.
inline constexpr unsigned kTrayRingAbi = 5;
#if defined(ISLANDBAR_TRAY_PROBE_RING)
// The development probe owns a ring of its own. It uses the same validated record layout and ABI, but it
// must never compete with a running bar for the production cursor.
inline constexpr wchar_t kTrayRingName[] = L"Local\\IslandBar.TrayProbeRing.v5";
inline constexpr wchar_t kTrayRingEventName[] = L"Local\\IslandBar.TrayProbeRing.Event.v5";
#else
inline constexpr wchar_t kTrayRingName[] = L"Local\\IslandBar.TrayRing.v5";
inline constexpr wchar_t kTrayRingEventName[] = L"Local\\IslandBar.TrayRing.Event.v5";
#endif
inline constexpr unsigned kTrayRingCapacity = 64;
inline constexpr unsigned kTrayRingMagic = 0x59415254u;   // 'TRAY'

// WHAT A RECORD IS. A registration is what the product is for; the hook state is a CONTROL record -- the
// module saying, in the one channel that survives, what it managed to install.
//
// Why this channel and not the report file: the bar TRUNCATES that file before every learning pass, and a
// pinned module holds its handle and keeps writing into the now-unlinked file (HANDOFF 8.121) -- so a line
// written from inside the shell after a pass starts may never be readable by the bar. Shared memory has no
// such failure mode, and it is where the bar is already looking.
enum class TrayRingKind : unsigned {
    Registration = 0,
    HookState = 1,
};

// What the module installed inside the shell, and the answer to a question the bar used to answer with a
// sleep: "is anybody listening yet?". MEASURED 2026-10-16: the first pass after Explorer restarted captured
// 0 registrations of the 143 thread hooks it reported, while the pass minutes later, same build, captured 42.
// The broadcast that makes applications register again is worth nothing before this side is up.
struct TrayHookState {
    unsigned pid = 0;
    bool inShell = false;      // this copy of the module runs inside the shell's own process
    bool ready = false;        // ... and the thread that receives registrations is watched
    unsigned trayThread = 0;   // that thread (0 when there is no Shell_TrayWnd to ask)
    unsigned hooks = 0;        // WH_CALLWNDPROC hooks this attempt accepted
    unsigned attempts = 0;     // 1-based: which attempt produced this state
    unsigned lastError = 0;    // what SetWindowsHookEx said when it failed
};

// One registration, as it travels between the two processes. It is the decoded form and not the raw copy: the
// raw bytes are the shell's private structure, and sending them across a process boundary would mean the reader
// decoding a layout it cannot check.
struct TrayRingRecord {
    // 0 = empty/being written; 2 * globalIndex + 2 = this slot is complete and stable.
    volatile LONG publishSequence = 0;
    TrayRingKind kind = TrayRingKind::Registration;
    std::uint64_t publishedTick = 0;   // GetTickCount64 when the observer published this record
    TrayRegistration registration;
    TrayHookState hook;
};

static_assert(std::is_same_v<decltype(TrayRegistration::trayWindow), std::uint64_t>,
              "the ring ABI carries a 64-bit window value");
static_assert(std::is_same_v<decltype(TrayRingRecord::publishSequence), volatile LONG>);
static_assert(offsetof(TrayRingRecord, registration) >= sizeof(TrayRingKind),
              "the registration follows the record kind");
static_assert(offsetof(TrayRingRecord, hook) > offsetof(TrayRingRecord, registration),
              "the hook state follows the registration or overlaps it deliberately");
static_assert(sizeof(TrayRingRecord) <= 512, "one ring record must remain intentionally small");

struct TrayRingHeader {
    unsigned magic = kTrayRingMagic;
    unsigned abi = kTrayRingAbi;
    unsigned recordSize = static_cast<unsigned>(sizeof(TrayRingRecord));
    unsigned capacity = kTrayRingCapacity;
    // How many records have ever been written. The reader keeps its own cursor and compares; a writer that laps
    // the reader loses the oldest records rather than blocking, which is the right trade for a learning path.
    volatile LONG written = 0;
};

inline constexpr size_t kTrayRingBytes = sizeof(TrayRingHeader) + sizeof(TrayRingRecord) * kTrayRingCapacity;

// A FRESH SECTION IS ALL ZEROS, and zero is not a valid header: created with CreateFileMappingW, the page has
// no magic, no ABI and no record size, so a reader that simply compared fields would refuse the very ring it
// just created -- which is exactly what the first version did, and the module said so ("the ring answered with
// a different ABI") while publishing nothing at all. So whoever maps it first FILLS THE HEADER, writing the
// magic LAST: that field is the "this ring is ready" flag, and a reader that sees it has also seen the fields
// it describes.
inline bool EnsureRingHeader(TrayRingHeader* header) {
    if (!header) return false;
    if (header->magic == 0 && header->abi == 0 && header->recordSize == 0) {
        header->capacity = kTrayRingCapacity;
        header->recordSize = static_cast<unsigned>(sizeof(TrayRingRecord));
        header->abi = kTrayRingAbi;
        header->written = 0;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&header->magic),
                            static_cast<LONG>(kTrayRingMagic));
    }
    return header->magic == kTrayRingMagic && header->abi == kTrayRingAbi &&
           header->recordSize == sizeof(TrayRingRecord);
}

// --- the publisher: the module's side ---------------------------------------------------------------
//
// Opened lazily on the first registration, and never closed: this module lives in somebody else's process and
// is pinned there, so there is no good moment to clean up that is not "process exit", which the OS handles.
struct TrayRingPublisher {
    HANDLE mapping = nullptr;
    HANDLE event = nullptr;
    TrayRingHeader* header = nullptr;
    TrayRingRecord* records = nullptr;
    bool refused = false;      // a ring with a different ABI answered: do not write to it
    bool reported = false;     // ... and say so exactly once

    // `name`/`eventName` exist so a TEST can have a ring of its own: publishing into the production ring from a
    // self-test would put records in a running bar's queue, and the ABI check below deliberately corrupts a
    // header to prove it is refused -- which must never be done to the ring the bar is reading.
    bool Open(const wchar_t* name = kTrayRingName, const wchar_t* eventName = kTrayRingEventName) {
        if (header || refused) return header != nullptr;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(kTrayRingBytes), name);
        if (!mapping) { refused = true; return false; }
        void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kTrayRingBytes);
        if (!view) { refused = true; return false; }
        header = static_cast<TrayRingHeader*>(view);
        records = reinterpret_cast<TrayRingRecord*>(reinterpret_cast<unsigned char*>(view) +
                                                    sizeof(TrayRingHeader));
        if (!EnsureRingHeader(header)) {
            refused = true;
            header = nullptr;
            records = nullptr;
            return false;
        }
        event = CreateEventW(nullptr, FALSE, FALSE, eventName);
        return true;
    }

    // Appends one registration. Cheap on purpose: this runs while an application waits for the shell.
    void Publish(const TrayRegistration& registration) {
        if (!Open() || !records) return;
        const LONG index = InterlockedIncrement(&header->written) - 1;
        TrayRingRecord& slot = records[index % kTrayRingCapacity];
        const LONG complete = static_cast<LONG>((static_cast<unsigned>(index) * 2u) + 2u);
        InterlockedExchange(&slot.publishSequence, complete - 1);
        MemoryBarrier();
        // The kind is written on EVERY publish: the slot it lands in may still hold a control record from an
        // earlier lap round this ring, and a reader that trusted the old one would file a hook state as an
        // application's callback.
        slot.kind = TrayRingKind::Registration;
        slot.publishedTick = GetTickCount64();
        slot.registration = registration;
        MemoryBarrier();
        InterlockedExchange(&slot.publishSequence, complete);
        if (event) SetEvent(event);
    }

    // ... and one control record, which is not a registration and must never be read as one.
    void PublishHookState(const TrayHookState& state) {
        if (!Open() || !records) return;
        const LONG index = InterlockedIncrement(&header->written) - 1;
        TrayRingRecord& slot = records[index % kTrayRingCapacity];
        const LONG complete = static_cast<LONG>((static_cast<unsigned>(index) * 2u) + 2u);
        InterlockedExchange(&slot.publishSequence, complete - 1);
        MemoryBarrier();
        slot.kind = TrayRingKind::HookState;
        slot.publishedTick = GetTickCount64();
        slot.hook = state;
        MemoryBarrier();
        InterlockedExchange(&slot.publishSequence, complete);
        if (event) SetEvent(event);
    }
};

// --- the reader: the bar's side --------------------------------------------------------------------
//
// `cursor` is the caller's own count of records consumed so far; the reader reports how many it handed back.
struct TrayRingReader {
    HANDLE mapping = nullptr;
    HANDLE event = nullptr;
    TrayRingHeader* header = nullptr;
    TrayRingRecord* records = nullptr;
    unsigned abiSeen = 0;

    bool Open(const wchar_t* name = kTrayRingName, const wchar_t* eventName = kTrayRingEventName) {
        if (header) return true;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(kTrayRingBytes), name);
        if (!mapping) return false;
        void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kTrayRingBytes);
        if (!view) return false;
        header = static_cast<TrayRingHeader*>(view);
        records = reinterpret_cast<TrayRingRecord*>(reinterpret_cast<unsigned char*>(view) +
                                                    sizeof(TrayRingHeader));
        abiSeen = header->abi;
        if (!EnsureRingHeader(header)) {
            return false;   // the caller decides what to say about it; the fields above say what it saw
        }
        event = CreateEventW(nullptr, FALSE, FALSE, eventName);
        return true;
    }

    // Hands back up to `max` records written since `cursor`, and advances it. Returns how many.
    unsigned Drain(unsigned& cursor, TrayRingRecord* out, unsigned max) {
        if (!Open() || !records || !out || max == 0) return 0;
        const LONG written = header->written;
        unsigned available = (written > static_cast<LONG>(cursor)) ? static_cast<unsigned>(written) - cursor : 0;
        if (available > kTrayRingCapacity) {
            // The writer lapped us. Skip the records that were overwritten rather than reading a torn one.
            cursor = static_cast<unsigned>(written) - kTrayRingCapacity;
            available = kTrayRingCapacity;
        }
        if (available > max) available = max;
        unsigned stableCount = 0;
        for (unsigned i = 0; i < available; ++i) {
            const unsigned globalIndex = cursor + i;
            const TrayRingRecord& slot = records[globalIndex % kTrayRingCapacity];
            const LONG expected = static_cast<LONG>(globalIndex * 2u + 2u);
            if (slot.publishSequence != expected) break;
            TrayRingRecord snapshot = slot;
            MemoryBarrier();
            if (slot.publishSequence != expected) break;
            out[i] = snapshot;
            ++stableCount;
        }
        cursor += stableCount;
        return stableCount;
    }
};

} // namespace island
