// =====================================================================
//  ScrollTiling.h -- event-driven Niri-like window-strip lifecycle.
//
//  WinEvents identify a new/focused/destroyed window. The controller decides
//  when to admit, remove or focus a column; the viewport animator supplies the
//  offset; the platform layer performs and verifies HWND placement. Recovery
//  information is persisted before the first frame that may move a window.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "app/ScrollTilingViewport.h"
#include "domain/ScrollTiling.h"
#include "platform/Monitor.h"
#include "platform/ScrollTilingWindows.h"
#include "platform/WindowEventMonitor.h"
#include "platform/WindowList.h"

namespace island {

struct ScrollTilingStatus {
    bool enabled = false;
    bool active = false;
    int columns = 0;
    int activeColumn = -1;
    int viewportOffset = 0;
};

class ScrollTilingController {
public:
    ScrollTilingController();
    ~ScrollTilingController();
    ScrollTilingController(const ScrollTilingController&) = delete;
    ScrollTilingController& operator=(const ScrollTilingController&) = delete;

    void SetFeatureEnabled(bool enabled);
    bool FeatureEnabled() const { return featureEnabled_; }
    bool Active() const { return active_; }
    bool GrabActive() const { return grabActive_; }
    bool Animating(unsigned long long nowMs) const {
        return active_ && viewport_.Animating(nowMs);
    }
    ScrollTilingStatus Status() const;

    void RecoverPreviousRun();

    bool Enter(WindowList& windows, const MonitorGeometry& monitor, int topInsetPx,
               std::wstring* detail);
    bool Exit(const char* reason);
    bool RevealAndFocus(HWND window, WindowList& windows,
                        const MonitorGeometry& monitor, int topInsetPx,
                        std::wstring* detail);
    bool ScrollBy(int columns, const MonitorGeometry& monitor, int topInsetPx,
                   std::wstring* detail);
    bool ResizeActiveColumn(int steps, const MonitorGeometry& monitor,
                            int topInsetPx, std::wstring* detail);
    bool BeginGrab(POINT screenPoint, const MonitorGeometry& monitor,
                   int topInsetPx, std::wstring* detail);
    bool UpdateGrab(POINT screenPoint, std::wstring* detail);
    bool EndGrab(const char* reason, const MonitorGeometry& monitor,
                 int topInsetPx, std::wstring* detail);
    void OnEvents(std::span<const WindowEvent> events, WindowList& windows,
                  const MonitorGeometry& monitor, int topInsetPx,
                  unsigned long long nowMs);
    bool Tick(unsigned long long nowMs, const MonitorGeometry& monitor,
              int topInsetPx, bool* animating, std::wstring* detail);
    void OnWindowRefresh(WindowList& windows, const MonitorGeometry& monitor,
                         int topInsetPx, unsigned long long nowMs);
    void OnFullscreenChanged(bool fullscreen);
    bool Contains(HWND window) const;

private:
    struct JournalHeader {
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint16_t count = 0;
        std::uint16_t flags = 0;
        std::uint16_t reserved = 0;
    };

    struct PendingAdmission {
        HWND hwnd = nullptr;
        unsigned long long dueMs = 0;
    };

    ScrollTilingWindowRecord* Find(HWND window);
    const ScrollTilingWindowRecord* Find(HWND window) const;
    bool SameContext(const MonitorGeometry& monitor, int topInsetPx) const;
    bool CaptureContext(const MonitorGeometry& monitor, int topInsetPx);
    void QueueAdmission(HWND window, unsigned long long nowMs,
                        unsigned long long delayMs);
    bool AdmitNow(HWND window, const MonitorGeometry& monitor,
                  ScrollTilingColumnWidth width, std::wstring* detail);
    bool RemoveWindow(HWND window);
    bool PrepareViewport(const char* event, unsigned long long nowMs,
                         bool animate, std::wstring* detail);
    bool ApplyViewportOffset(const char* event, int offset,
                             std::wstring* detail);
    bool IsolateFailedWindows(unsigned long long nowMs,
                              std::wstring* detail);
    int DropExternallyMovedWindows();
    void AbortAndRestore(const char* event);
    void EndEmptyStrip();
    void ClearActiveState();

    bool SaveJournal();
    bool LoadJournal(std::vector<ScrollTilingWindowRecord>* records,
                     bool* interruptedGrab);
    void DeleteJournal();
    bool JournalExists() const;

    bool featureEnabled_ = false;
    bool active_ = false;
    bool grabActive_ = false;
    bool needsFrame_ = false;
    std::uint64_t nextWindowId_ = 1;
    std::uint64_t transactionId_ = 0;
    ScrollTilingModel model_;
    ScrollTilingLayout layout_;
    ScrollTilingViewport viewport_;
    std::vector<ScrollTilingWindowRecord> records_;
    std::vector<PendingAdmission> pending_;
    std::vector<HWND> rejected_;
    std::vector<ScrollTilingPlacement> placements_;
    POINT grabStartPoint_ = {};
    POINT grabLastPoint_ = {};
    int grabStartOffset_ = 0;
    int grabCurrentOffset_ = 0;
    bool grabLogPending_ = false;
    std::string grabLogReason_;
    int grabLogPanPx_ = 0;
    int grabLogNearest_ = -1;
    int grabLogTarget_ = 0;
    unsigned long long layoutSettleUntilMs_ = 0;
    std::vector<HWND> layoutFailures_;
    MonitorGeometry monitor_ = {};
    RECT workArea_ = {};
    UINT dpi_ = 96;
    int topInsetPx_ = 0;
    unsigned long long lastContextCheckMs_ = 0;
    unsigned long long lastReconcileMs_ = 0;
    std::wstring journalPath_;
};

} // namespace island
