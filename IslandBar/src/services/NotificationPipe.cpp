// =====================================================================
//  NotificationPipe.cpp
// =====================================================================
#include "services/NotificationPipe.h"

#include <aclapi.h>    // SetEntriesInAcl / EXPLICIT_ACCESS for the pipe DACL
#include <sddl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "advapi32.lib")

#include "domain/MiniJson.h"

namespace island {
namespace {

// Security descriptor for the pipe.
//
// CreateNamedPipe's default DACL is created from the process token and, in
// practice, refuses same-user clients with ERROR_ACCESS_DENIED -- which is exactly
// what happened: the sender got win32 5 while the listener sat waiting.
//
// The descriptor grants full access to Everyone, and that is a deliberate,
// bounded decision:
//   * a named pipe is reachable only from this machine, never over the network;
//   * the only thing an attacker gains by writing is the ability to show a
//     notification -- the same thing the user can already do;
//   * notifications are parsed defensively (see MiniJson), so a hostile message
//     cannot corrupt the bar.
// The alternative -- a DACL naming only the current user -- would break the
// common case of sending from an elevated shell while the bar runs unelevated.
//
// Built once and freed by the destructor.
class PipeSecurity {
public:
    PipeSecurity() {
        // SECURITY_WORLD_SID_AUTHORITY with SECURITY_WORLD_RID is Everyone.
        SID_IDENTIFIER_AUTHORITY worldAuthority = SECURITY_WORLD_SID_AUTHORITY;
        if (!AllocateAndInitializeSid(&worldAuthority, 1, SECURITY_WORLD_RID,
                                      0, 0, 0, 0, 0, 0, 0, &everyone_)) {
            everyone_ = nullptr;
            return;
        }

        EXPLICIT_ACCESSW access = {};
        access.grfAccessPermissions = GENERIC_ALL;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone_);

        if (SetEntriesInAclW(1, &access, nullptr, &acl_) != ERROR_SUCCESS) {
            acl_ = nullptr;
            return;
        }

        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE);
        ready_ = true;
    }

    ~PipeSecurity() {
        if (acl_) LocalFree(acl_);
        if (everyone_) FreeSid(everyone_);
    }

    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    // Null when the descriptor could not be built, which makes CreateNamedPipe fall
    // back to its default rather than failing outright.
    SECURITY_ATTRIBUTES* Attributes() { return ready_ ? &attributes_ : nullptr; }

private:
    PSID everyone_ = nullptr;
    PACL acl_ = nullptr;
    SECURITY_DESCRIPTOR descriptor_ = {};
    SECURITY_ATTRIBUTES attributes_ = {};
    bool ready_ = false;
};

// MESSAGE mode keeps compatibility with callers that expect the original Windows named-pipe behaviour.
// The length-prefixed protocol writes its header, body and response with separate WriteFile calls, so each
// piece is one message. The kernel buffer must still hold the largest accepted payload.
constexpr DWORD kPipeBufferBytes = kMaxMessageBytes + sizeof(uint32_t);
constexpr DWORD kConnectTimeoutMs = 30000;
constexpr DWORD kFrameTimeoutMs = 5000;
constexpr DWORD kCancelDrainMs = 2000;
constexpr DWORD kResponseDrainMs = 1000;

enum class IoStatus {
    Done,
    Stopped,
    Timeout,
    Failed,
};

class OverlappedOperation {
public:
    OverlappedOperation() {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        overlapped_.hEvent = event_;
    }

    ~OverlappedOperation() {
        if (event_) CloseHandle(event_);
    }

    OverlappedOperation(const OverlappedOperation&) = delete;
    OverlappedOperation& operator=(const OverlappedOperation&) = delete;

    bool Valid() const { return event_ != nullptr; }
    DWORD Error() const { return lastError_; }

    OVERLAPPED* Begin() {
        ResetEvent(event_);
        return &overlapped_;
    }

    IoStatus Wait(HANDLE pipe, HANDLE stopEvent, BOOL started, DWORD timeoutMs,
                  DWORD* transferred) {
        lastError_ = ERROR_SUCCESS;
        if (started) return IoStatus::Done;

        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            lastError_ = error;
            return IoStatus::Failed;
        }

        DWORD signalled = 0;
        if (stopEvent) {
            const HANDLE waits[2] = {stopEvent, event_};
            signalled = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
        } else {
            const DWORD single = WaitForSingleObject(event_, timeoutMs);
            signalled = single == WAIT_OBJECT_0 ? WAIT_OBJECT_0 + 1 : single;
        }
        if (signalled == WAIT_OBJECT_0 + 1) {
            if (GetOverlappedResult(pipe, &overlapped_, transferred, FALSE)) {
                return IoStatus::Done;
            }
            lastError_ = GetLastError();
            return IoStatus::Failed;
        }

        const IoStatus reason =
            signalled == WAIT_OBJECT_0 ? IoStatus::Stopped
                                       : (signalled == WAIT_TIMEOUT ? IoStatus::Timeout
                                                                    : IoStatus::Failed);
        CancelAndWaitForCompletion(pipe, signalled);
        return reason;
    }

private:
    void CancelAndWaitForCompletion(HANDLE pipe, DWORD signalled) {
        if (!CancelIoEx(pipe, &overlapped_)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NOT_FOUND) {
                LogLine(L"  notifications: CancelIoEx failed status=%lu win32=%lu; "
                        L"operation context remains live",
                        signalled, error);
            }
        }

        if (WaitForSingleObject(event_, kCancelDrainMs) != WAIT_OBJECT_0) {
            // The operation context owns the event and THE CALLER still owns the buffer and pipe.
            // Waiting here keeps all three alive until the kernel has released the operation. This
            // thread may outlive Stop(), which is exactly why the worker owner is shared.
            LogLine(L"  notifications: cancelled I/O did not drain in %lu ms; "
                    L"waiting without releasing its context",
                    kCancelDrainMs);
            (void)WaitForSingleObject(event_, INFINITE);
        }

        DWORD ignored = 0;
        (void)GetOverlappedResult(pipe, &overlapped_, &ignored, FALSE);
    }

    HANDLE event_ = nullptr;
    OVERLAPPED overlapped_ = {};
    DWORD lastError_ = ERROR_SUCCESS;
};

IoStatus ConnectPipe(HANDLE pipe, HANDLE stopEvent) {
    OverlappedOperation operation;
    if (!operation.Valid()) return IoStatus::Failed;
    const BOOL started = ConnectNamedPipe(pipe, operation.Begin());
    const DWORD error = started ? ERROR_SUCCESS : GetLastError();
    if (started || error == ERROR_PIPE_CONNECTED) {
        return IoStatus::Done;
    }
    DWORD ignored = 0;
    return operation.Wait(pipe, stopEvent, FALSE, kConnectTimeoutMs, &ignored);
}

IoStatus ReadAllOverlapped(HANDLE pipe, HANDLE stopEvent, void* buffer, DWORD bytes,
                           DWORD timeoutMs) {
    auto* cursor = static_cast<unsigned char*>(buffer);
    const DWORD startedAt = GetTickCount();
    DWORD remaining = bytes;
    while (remaining > 0) {
        const DWORD elapsed = GetTickCount() - startedAt;
        if (elapsed >= timeoutMs) return IoStatus::Timeout;

        OverlappedOperation operation;
        if (!operation.Valid()) return IoStatus::Failed;
        DWORD got = 0;
        const BOOL started = ReadFile(pipe, cursor, remaining, &got, operation.Begin());
        const IoStatus status =
            operation.Wait(pipe, stopEvent, started, timeoutMs - elapsed, &got);
        if (status != IoStatus::Done) return status;
        if (got == 0) return IoStatus::Failed;
        cursor += got;
        remaining -= got;
    }
    return IoStatus::Done;
}

IoStatus WriteAllOverlapped(HANDLE pipe, HANDLE stopEvent, const void* buffer, DWORD bytes) {
    const auto* cursor = static_cast<const unsigned char*>(buffer);
    const DWORD startedAt = GetTickCount();
    DWORD remaining = bytes;
    while (remaining > 0) {
        const DWORD elapsed = GetTickCount() - startedAt;
        if (elapsed >= kFrameTimeoutMs) return IoStatus::Timeout;

        OverlappedOperation operation;
        if (!operation.Valid()) return IoStatus::Failed;
        DWORD written = 0;
        const BOOL started = WriteFile(pipe, cursor, remaining, &written, operation.Begin());
        const IoStatus status =
            operation.Wait(pipe, stopEvent, started, kFrameTimeoutMs - elapsed, &written);
        if (status != IoStatus::Done) return status;
        if (written == 0) return IoStatus::Failed;
        cursor += written;
        remaining -= written;
    }
    return IoStatus::Done;
}

// A server-side DisconnectNamedPipe discards data the client has not read yet. Wait until the
// client closes after reading the response, with a bounded timeout and the same cancellation path
// as every other overlapped operation. A client that keeps the connection open is disconnected
// after the grace period rather than being allowed to occupy the single listener indefinitely.
IoStatus WaitForResponseConsumed(HANDLE pipe, HANDLE stopEvent) {
    OverlappedOperation operation;
    if (!operation.Valid()) return IoStatus::Failed;
    char unexpected = 0;
    DWORD received = 0;
    const BOOL started = ReadFile(pipe, &unexpected, 1, &received, operation.Begin());
    const IoStatus status =
        operation.Wait(pipe, stopEvent, started, kResponseDrainMs, &received);
    if (status != IoStatus::Failed) return status;

    const DWORD error = operation.Error();
    return (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
               ? IoStatus::Done
               : IoStatus::Failed;
}

bool ReadAll(HANDLE pipe, void* buffer, DWORD bytes) {
    auto* cursor = static_cast<unsigned char*>(buffer);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, remaining, &read, nullptr)) return false;
        if (read == 0) return false;   // peer closed mid-frame
        cursor += read;
        remaining -= read;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const void* buffer, DWORD bytes) {
    const auto* cursor = static_cast<const unsigned char*>(buffer);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, remaining, &written, nullptr)) return false;
        if (written == 0) return false;
        cursor += written;
        remaining -= written;
    }
    return true;
}

} // namespace

bool WriteFrame(HANDLE pipe, const char* payload, size_t length) {
    if (!pipe || !payload) return false;
    if (length > kMaxMessageBytes) return false;

    const uint32_t header = static_cast<uint32_t>(length);
    if (!WriteAll(pipe, &header, sizeof(header))) return false;
    if (length == 0) return true;
    return WriteAll(pipe, payload, static_cast<DWORD>(length));
}

bool ReadFrame(HANDLE pipe, char* out, size_t capacity) {
    if (!pipe || !out || capacity == 0) return false;
    out[0] = '\0';

    uint32_t length = 0;
    if (!ReadAll(pipe, &length, sizeof(length))) return false;

    // The length prefix comes from another process, so it is a claim, not a fact.
    // Rejecting an over-large claim here is what stops a bogus header from turning
    // into a huge allocation.
    if (length == 0 || length > kMaxMessageBytes) return false;

    const size_t wanted = (length < capacity - 1) ? length : capacity - 1;
    if (!ReadAll(pipe, out, static_cast<DWORD>(wanted))) return false;
    out[wanted] = '\0';

    // Drain anything beyond what we kept, so the stream stays framed for the next
    // message instead of desynchronising.
    DWORD remaining = static_cast<DWORD>(length) - static_cast<DWORD>(wanted);
    char scratch[512];
    while (remaining > 0) {
        const DWORD chunk = (remaining < sizeof(scratch)) ? remaining
                                                          : static_cast<DWORD>(sizeof(scratch));
        if (!ReadAll(pipe, scratch, chunk)) return false;
        remaining -= chunk;
    }

    return true;
}

// --- server ----------------------------------------------------------

NotificationServer::~NotificationServer() {
    Stop();
}

bool NotificationServer::Start(const wchar_t* pipeName) {
    if (running_.load(std::memory_order_acquire)) return true;

    swprintf(name_, 256, L"\\\\.\\pipe\\%s", pipeName ? pipeName : kPipeName);

    // Stop() ends whatever exists rather than unwinding by hand (see platform/WorkerThread.h).
    if (!worker_.Start(&NotificationServer::ThreadEntry, weak_from_this().lock())) {
        Stop();
        return false;
    }

    // Created on the listener thread rather than here, so a failure to create the
    // pipe surfaces as "not running" instead of as a thread that quietly does
    // nothing.
    // Give the thread a moment to create the pipe, so Start() returning true means
    // a client can actually connect.
    bool started = false;
    for (int i = 0; i < 100; ++i) {
        if (running_.load(std::memory_order_acquire)) {
            started = true;
            break;
        }
        Sleep(5);
    }
    if (!started) {
        Stop();
        return false;
    }
    return true;
}

void NotificationServer::Stop() {
    // The rule -- signal, wait with a timeout, close -- lives in WorkerThread, including the case its own
    // comment describes: a blocked ConnectNamedPipe on a broken client means the thread may refuse to end,
    // and the handle must be closed and the thread detached rather than waited on forever.
    const WorkerThread::StopResult stopped = worker_.Stop();
    if (stopped == WorkerThread::StopResult::Stopped) {
        running_.store(false, std::memory_order_release);
    }
}

unsigned long __stdcall NotificationServer::ThreadEntry(void* self) {
    static_cast<NotificationServer*>(self)->Run();
    return 0;
}

void NotificationServer::Run() {
    const WorkerThread::Signal workerSignal = WorkerThread::CurrentSignal();
    const HANDLE stopEvent = workerSignal.StopEvent();
    // One security descriptor for every instance the loop creates.
    PipeSecurity security;

    // The loop owns one pipe instance at a time: create, wait for a client, serve
    // it, and close. That serialises clients without any extra locking, and a
    // notification is a small message that is written and forgotten.
    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = CreateNamedPipeW(
            name_,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,                       // one instance: clients are serialised
            kPipeBufferBytes, kPipeBufferBytes,
            0, security.Attributes());

        if (pipe == INVALID_HANDLE_VALUE) {
            // Nothing useful to do but back off; a persistent failure here would
            // otherwise spin the CPU.
            if (WaitForSingleObject(stopEvent, 200) == WAIT_OBJECT_0) break;
            continue;
        }

        running_.store(true, std::memory_order_release);

        const IoStatus connected = ConnectPipe(pipe, stopEvent);
        if (connected != IoStatus::Done) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            if (connected == IoStatus::Stopped) break;
            continue;
        }

        char request[kMaxMessageBytes] = {};
        uint32_t length = 0;
        const IoStatus header = ReadAllOverlapped(
            pipe, stopEvent, &length, sizeof(length), kFrameTimeoutMs);
        bool frameRead = false;
        if (header == IoStatus::Done && length > 0 && length < sizeof(request)) {
            const size_t wanted = length;
            const IoStatus body = ReadAllOverlapped(
                pipe, stopEvent, request, static_cast<DWORD>(wanted), kFrameTimeoutMs);
            if (body == IoStatus::Done) {
                request[wanted] = '\0';
                frameRead = true;
            } else if (body == IoStatus::Stopped) {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                break;
            }
        } else if (header == IoStatus::Stopped) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        if (frameRead) {
            Notification notification;
            bool responseWritten = false;
            if (NotificationFromJson(request, &notification)) {
                // WHO sent it, taken from the connection rather than from the message.
                //
                // The card shows this process's icon, which is what makes a notification from a tool look
                // like that tool -- and it cannot be asked for in the message, because a sender that has to
                // describe itself is a sender that will not. Asked of the pipe, it is free and it cannot lie.
                ULONG clientPid = 0;
                if (GetNamedPipeClientProcessId(pipe, &clientPid)) {
                    notification.senderPid = clientPid;
                }
                Enqueue(notification);
                InterlockedIncrement(&receivedCount_);

                const int written = std::snprintf(
                    request, sizeof(request),
                    "{\"ok\":1,\"received\":%lld}",
                    static_cast<long long>(receivedCount_));
                if (written > 0) {
                    const uint32_t responseLength = static_cast<uint32_t>(written);
                    responseWritten =
                        WriteAllOverlapped(pipe, stopEvent, &responseLength,
                                           sizeof(responseLength)) == IoStatus::Done &&
                        WriteAllOverlapped(pipe, stopEvent, request,
                                           responseLength) == IoStatus::Done;
                }
            } else {
                InterlockedIncrement(&rejectedCount_);
                static const char kRejection[] =
                    "{\"ok\":0,\"error\":\"no title or body\"}";
                const uint32_t responseLength = sizeof(kRejection) - 1;
                responseWritten =
                    WriteAllOverlapped(pipe, stopEvent, &responseLength,
                                       sizeof(responseLength)) == IoStatus::Done &&
                    WriteAllOverlapped(pipe, stopEvent, kRejection,
                                       responseLength) == IoStatus::Done;
            }

            if (responseWritten) {
                const IoStatus consumed = WaitForResponseConsumed(pipe, stopEvent);
                if (consumed == IoStatus::Stopped) {
                    DisconnectNamedPipe(pipe);
                    CloseHandle(pipe);
                    break;
                }
            }
        } else {
            InterlockedIncrement(&rejectedCount_);
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
    }

    running_.store(false, std::memory_order_release);
}

void NotificationServer::Enqueue(const Notification& notification) {
    if (!worker_.Prepared()) return;
    lock_.Enter();
    if (queueCount_ < static_cast<int>(sizeof(queue_) / sizeof(queue_[0]))) {
        queue_[queueCount_++] = notification;
    } else {
        // The UI thread has not kept up. Drop the oldest rather than the newest:
        // a burst should still show its most recent item.
        for (int i = 1; i < queueCount_; ++i) {
            queue_[i - 1] = queue_[i];
        }
        queue_[queueCount_ - 1] = notification;
    }
    lock_.Leave();
}

bool NotificationServer::Take(Notification* out) {
    if (!out || !worker_.Prepared()) return false;
    bool have = false;
    lock_.Enter();
    if (queueCount_ > 0) {
        *out = queue_[0];
        for (int i = 1; i < queueCount_; ++i) {
            queue_[i - 1] = queue_[i];
        }
        --queueCount_;
        have = true;
    }
    lock_.Leave();
    return have;
}

// --- client ----------------------------------------------------------

bool SendNotification(const Notification& notification, const wchar_t* pipeName,
                      char* responseOut, size_t responseCapacity,
                      DWORD connectTimeoutMs, DWORD responseTimeoutMs) {
    wchar_t name[256] = {};
    swprintf(name, 256, L"\\\\.\\pipe\\%s", pipeName ? pipeName : kPipeName);

    // WaitNamedPipe rather than a single CreateFile attempt: the bar serves one
    // client at a time, so a sender arriving while another is being served should be
    // waited out rather than failing.
    //
    // The reason is reported rather than swallowed. "could not reach the bar" with
    // no cause is what turned a two-line fix into a long detour: the two likely
    // cases look identical from outside but need opposite responses --
    // ERROR_FILE_NOT_FOUND means nothing is listening, while ERROR_SEM_TIMEOUT
    // means a listener exists but is busy or stuck.
    if (!WaitNamedPipeW(name, connectTimeoutMs)) {
        const DWORD error = GetLastError();
        if (responseOut && responseCapacity) {
            const char* reason = (error == ERROR_FILE_NOT_FOUND)
                                     ? "no listener (is the bar running?)"
                                     : (error == ERROR_SEM_TIMEOUT
                                            ? "listener busy or not accepting"
                                            : "WaitNamedPipe failed");
            std::snprintf(responseOut, responseCapacity, "%s (win32 %lu)", reason,
                          static_cast<unsigned long>(error));
        }
        return false;
    }

    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (responseOut && responseCapacity) {
            std::snprintf(responseOut, responseCapacity, "CreateFile failed (win32 %lu)",
                          static_cast<unsigned long>(error));
        }
        return false;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    char payload[kMaxMessageBytes] = {};
    NotificationToJson(notification, payload, sizeof(payload), false);

    const uint32_t payloadLength = static_cast<uint32_t>(std::strlen(payload));
    const bool sent =
        WriteAllOverlapped(pipe, nullptr, &payloadLength, sizeof(payloadLength)) ==
            IoStatus::Done &&
        (payloadLength == 0 ||
         WriteAllOverlapped(pipe, nullptr, payload, payloadLength) == IoStatus::Done);
    if (!sent && responseOut && responseCapacity) {
        std::snprintf(responseOut, responseCapacity, "WriteFrame failed (win32 %lu)",
                      static_cast<unsigned long>(GetLastError()));
    }
    bool ok = false;

    if (sent) {
        char response[1024] = {};
        uint32_t responseLength = 0;
        IoStatus readStatus = ReadAllOverlapped(
            pipe, nullptr, &responseLength, sizeof(responseLength), responseTimeoutMs);
        if (readStatus == IoStatus::Done &&
            responseLength > 0 &&
            responseLength < static_cast<uint32_t>(sizeof(response))) {
            readStatus = ReadAllOverlapped(
                pipe, nullptr, response, responseLength, responseTimeoutMs);
        } else if (readStatus == IoStatus::Done) {
            readStatus = IoStatus::Failed;
        }
        if (readStatus == IoStatus::Done) {
            response[responseLength] = '\0';
            long long acknowledged = 0;
            ok = json::GetInt(response, "ok", &acknowledged) && acknowledged != 0;
            if (responseOut && responseCapacity) {
                strncpy_s(responseOut, responseCapacity, response, _TRUNCATE);
            }
        } else if (responseOut && responseCapacity) {
            const char* reason = readStatus == IoStatus::Timeout
                                     ? "response read timed out"
                                     : "response read failed";
            std::snprintf(responseOut, responseCapacity, "%s (win32 %lu)", reason,
                          static_cast<unsigned long>(GetLastError()));
        }
    }

    CloseHandle(pipe);
    return ok;
}

} // namespace island
