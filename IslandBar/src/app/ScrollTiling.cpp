#include "app/ScrollTiling.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <io.h>

#include "core/Log.h"

namespace island {
namespace {

constexpr std::uint32_t kJournalMagic = 0x54425349; // "ISBT"
constexpr std::uint16_t kJournalVersion = 3;
constexpr std::uint16_t kJournalFlagGrabActive = 1u << 0;
constexpr int kGapPx = 8;
constexpr int kPaddingPx = 8;
constexpr unsigned long long kAdmissionDelayMs = 50;
constexpr unsigned long long kReconcileMs = 1500;

std::wstring DataPath(const wchar_t* leaf) {
    wchar_t home[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring directory = std::wstring(home) + L"\\IslandBar";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\" + leaf;
}

bool TaskListContains(const WindowList& windows, HWND hwnd) {
    for (int i = 0; i < windows.Count(); ++i) {
        if (windows.At(i).handle == hwnd) return true;
    }
    return false;
}

bool IsTransitionRect(const RECT& current, const RECT& previous,
                      const RECT& target) {
    const int previousWidth = previous.right - previous.left;
    const int targetWidth = target.right - target.left;
    const int currentWidth = current.right - current.left;
    return std::abs(previousWidth - targetWidth) <= 1 &&
           std::abs(currentWidth - targetWidth) <= 1 &&
           std::abs(current.top - target.top) <= 1 &&
           std::abs(current.bottom - target.bottom) <= 1 &&
           current.left >= std::min(previous.left, target.left) - 1 &&
           current.left <= std::max(previous.left, target.left) + 1;
}

} // namespace

ScrollTilingController::ScrollTilingController()
    : journalPath_(DataPath(L"tiling_recovery.dat")) {
    records_.reserve(kScrollTilingMaxColumns);
    pending_.reserve(kScrollTilingMaxColumns);
    rejected_.reserve(kScrollTilingMaxColumns);
    placements_.reserve(kScrollTilingMaxColumns);
    layoutFailures_.reserve(kScrollTilingMaxColumns);
}

ScrollTilingController::~ScrollTilingController() {
    if (active_) Exit("destructor");
}

void ScrollTilingController::SetFeatureEnabled(bool enabled) {
    if (!enabled && active_) Exit("feature-disabled");
    featureEnabled_ = enabled;
}

ScrollTilingStatus ScrollTilingController::Status() const {
    ScrollTilingStatus status;
    status.enabled = featureEnabled_;
    status.active = active_;
    status.columns = model_.Count();
    status.activeColumn = status.columns > 0 ? model_.ActiveIndex() : -1;
    status.viewportOffset = viewport_.Current();
    return status;
}

void ScrollTilingController::RecoverPreviousRun() {
    if (!JournalExists()) return;
    std::vector<ScrollTilingWindowRecord> records;
    bool interruptedGrab = false;
    if (!LoadJournal(&records, &interruptedGrab)) {
        LogAndPrint("tiling: event=recovery_start result=corrupt action=discard");
        DeleteJournal();
        return;
    }

    int restored = 0;
    int external = 0;
    int invalid = 0;
    int failed = 0;
    for (const ScrollTilingWindowRecord& record : records) {
        const ScrollTilingRestoreResult result =
            interruptedGrab && ScrollTilingWindows::StillSameWindow(record)
                ? (ScrollTilingWindows::RestoreOriginal(record)
                       ? ScrollTilingRestoreResult::Restored
                       : ScrollTilingRestoreResult::Failed)
                : ScrollTilingWindows::Restore(record);
        switch (result) {
            case ScrollTilingRestoreResult::Restored: ++restored; break;
            case ScrollTilingRestoreResult::ExternallyMoved: ++external; break;
            case ScrollTilingRestoreResult::InvalidIdentity: ++invalid; break;
            case ScrollTilingRestoreResult::Failed: ++failed; break;
        }
    }
    LogAndPrint("tiling: event=recovery_complete interrupted_grab=%d restored=%d "
                "external=%d invalid=%d failed=%d",
                interruptedGrab ? 1 : 0, restored, external, invalid, failed);
    if (failed == 0) DeleteJournal();
}

bool ScrollTilingController::Enter(WindowList& windows,
                                   const MonitorGeometry& monitor,
                                   int topInsetPx, std::wstring* detail) {
    if (detail) detail->clear();
    if (active_) return true;
    if (!featureEnabled_) {
        if (detail) *detail = L"请先在设置中启用滚动平铺";
        return false;
    }
    if (JournalExists()) {
        RecoverPreviousRun();
        if (JournalExists()) {
            if (detail) *detail = L"上次窗口恢复尚未完成，请查看日志";
            return false;
        }
    }

    std::wstring summary;
    std::vector<ScrollTilingWindowRecord> candidates =
        ScrollTilingWindows::Collect(windows, monitor.monitor,
                                     kScrollTilingMaxColumns, &summary);
    if (candidates.empty()) {
        LogAndPrint("tiling: event=enable result=no-windows %ls", summary.c_str());
        if (detail) *detail = L"当前没有可接管的普通窗口";
        return false;
    }

    records_.clear();
    std::vector<ScrollTilingWindowId> ids;
    records_.reserve(candidates.size());
    ids.reserve(candidates.size());
    for (ScrollTilingWindowRecord& record : candidates) {
        record.id = {nextWindowId_++};
        record.previousAppliedRect = record.originalRect;
        record.lastAppliedRect = record.originalRect;
        ids.push_back(record.id);
        records_.push_back(record);
    }
    if (!model_.Configure(ids, ScrollTilingColumnWidth{})) {
        ClearActiveState();
        return false;
    }
    if (!CaptureContext(monitor, topInsetPx)) {
        ClearActiveState();
        return false;
    }
    transactionId_ = nextWindowId_++;
    active_ = true;
    lastContextCheckMs_ = GetTickCount64();

    const int target = model_.TargetViewportOffset(
        workArea_.right - workArea_.left, kGapPx, kPaddingPx);
    viewport_.Reset(target);
    if (!PrepareViewport("enable", GetTickCount64(), false, detail)) {
        AbortAndRestore("enable-failed");
        return false;
    }
    if (!ApplyViewportOffset("enable", viewport_.Current(), detail)) {
        AbortAndRestore("enable-failed");
        return false;
    }
    LogAndPrint("tiling: event=enable tx=%llx columns=%d summary='%ls'",
                static_cast<unsigned long long>(transactionId_), model_.Count(),
                summary.c_str());
    return true;
}

bool ScrollTilingController::Exit(const char* reason) {
    if (!active_) return true;

    int restored = 0;
    int external = 0;
    int invalid = 0;
    int failed = 0;
    std::vector<ScrollTilingWindowRecord> unresolved;
    for (const ScrollTilingWindowRecord& record : records_) {
        switch (ScrollTilingWindows::Restore(record)) {
            case ScrollTilingRestoreResult::Restored: ++restored; break;
            case ScrollTilingRestoreResult::ExternallyMoved: ++external; break;
            case ScrollTilingRestoreResult::InvalidIdentity: ++invalid; break;
            case ScrollTilingRestoreResult::Failed:
                ++failed;
                unresolved.push_back(record);
                break;
        }
    }
    LogAndPrint("tiling: event=disable tx=%llx reason=%s columns=%d restored=%d "
                "external=%d invalid=%d failed=%d",
                static_cast<unsigned long long>(transactionId_), reason ? reason : "unknown",
                model_.Count(), restored, external, invalid, failed);

    ClearActiveState();
    if (failed == 0) {
        records_.clear();
        DeleteJournal();
    } else {
        records_ = std::move(unresolved);
        SaveJournal();
    }
    return failed == 0;
}

bool ScrollTilingController::RevealAndFocus(HWND window, WindowList& windows,
                                            const MonitorGeometry& monitor,
                                            int topInsetPx, std::wstring* detail) {
    if (detail) detail->clear();
    if (!active_ || !window) return false;
    if (!CaptureContext(monitor, topInsetPx)) {
        Exit("context-changed");
        return false;
    }
    if (!TaskListContains(windows, window)) return false;
    const ScrollTilingWindowRecord* record = Find(window);
    if (!record || !ScrollTilingWindows::IsSafeForLayout(*record)) return false;
    if (!model_.Focus(record->id)) return false;
    transactionId_ = nextWindowId_++;
    return PrepareViewport("focus", GetTickCount64(), true, detail);
}

bool ScrollTilingController::ScrollBy(int columns, const MonitorGeometry& monitor,
                                      int topInsetPx, std::wstring* detail) {
    if (detail) detail->clear();
    if (!active_ || columns == 0) return false;
    if (!CaptureContext(monitor, topInsetPx)) {
        Exit("context-changed");
        return false;
    }
    if (!model_.ScrollBy(columns)) return false;
    transactionId_ = nextWindowId_++;
    return PrepareViewport(columns > 0 ? "scroll-right" : "scroll-left",
                           GetTickCount64(), true, detail);
}

bool ScrollTilingController::ResizeActiveColumn(
    int steps, const MonitorGeometry& monitor, int topInsetPx,
    std::wstring* detail) {
    if (detail) detail->clear();
    if (!active_ || steps == 0 || model_.Count() <= 0) return false;
    if (!CaptureContext(monitor, topInsetPx)) {
        Exit("context-changed");
        return false;
    }
    static constexpr double presets[] = {1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0};
    const int count = static_cast<int>(sizeof(presets) / sizeof(presets[0]));
    const double current =
        model_.ColumnAt(model_.ActiveIndex()).width.value;
    int nearest = 0;
    double distance = std::abs(presets[0] - current);
    for (int i = 1; i < count; ++i) {
        const double candidate = std::abs(presets[i] - current);
        if (candidate < distance) {
            nearest = i;
            distance = candidate;
        }
    }
    int next = nearest + (steps > 0 ? 1 : -1);
    next = std::clamp(next, 0, count - 1);
    if (next == nearest) return false;
    if (!model_.SetWidth(model_.ActiveIndex(),
                         {ScrollTilingWidthMode::Proportion, presets[next]})) {
        return false;
    }
    transactionId_ = nextWindowId_++;
    return PrepareViewport("column-width", GetTickCount64(), true, detail);
}

bool ScrollTilingController::BeginGrab(POINT screenPoint,
                                       const MonitorGeometry& monitor,
                                       int topInsetPx, std::wstring* detail) {
    if (detail) detail->clear();
    if (!active_ || grabActive_ || model_.Count() <= 0) return false;
    if (!CaptureContext(monitor, topInsetPx)) {
        Exit("context-changed");
        return false;
    }

    grabStartPoint_ = screenPoint;
    grabLastPoint_ = screenPoint;
    grabStartOffset_ = viewport_.Current();
    grabCurrentOffset_ = grabStartOffset_;
    viewport_.Reset(grabStartOffset_);
    layoutSettleUntilMs_ = 0;
    layoutFailures_.clear();
    grabLogPending_ = false;
    grabLogReason_.clear();
    grabActive_ = true;
    transactionId_ = nextWindowId_++;

    for (ScrollTilingWindowRecord& record : records_) {
        RECT current = {};
        if (GetWindowRect(record.hwnd, &current)) {
            record.previousAppliedRect = current;
            record.lastAppliedRect = current;
        }
    }
    if (!SaveJournal()) {
        grabActive_ = false;
        if (detail) *detail = L"无法保存拖动恢复记录";
        return false;
    }
    LogAndPrint("tiling: event=grab_start tx=%llx columns=%d offset=%d x=%ld",
                static_cast<unsigned long long>(transactionId_), model_.Count(),
                grabStartOffset_, screenPoint.x);
    return true;
}

bool ScrollTilingController::UpdateGrab(POINT screenPoint,
                                        std::wstring* detail) {
    if (detail) detail->clear();
    if (!active_ || !grabActive_) return false;
    const int viewportWidth = workArea_.right - workArea_.left;
    const int requested =
        grabStartOffset_ + (grabStartPoint_.x - screenPoint.x);
    const int offset = model_.ClampViewportOffset(
        requested, viewportWidth, kGapPx, kPaddingPx);
    grabLastPoint_ = screenPoint;
    if (offset == grabCurrentOffset_) return true;
    grabCurrentOffset_ = offset;
    viewport_.Reset(offset);
    if (!ApplyViewportOffset("grab-drag", offset, detail)) {
        needsFrame_ = true;
    }
    return true;
}

bool ScrollTilingController::EndGrab(const char* reason,
                                     const MonitorGeometry& monitor,
                                     int topInsetPx, std::wstring* detail) {
    if (detail) detail->clear();
    if (!active_ || !grabActive_) return false;
    if (!CaptureContext(monitor, topInsetPx)) {
        Exit("context-changed");
        return false;
    }

    const int viewportWidth = workArea_.right - workArea_.left;
    const int current = model_.ClampViewportOffset(
        viewport_.Current(), viewportWidth, kGapPx, kPaddingPx);
    const int nearest = model_.NearestColumnToOffset(
        current, viewportWidth, kGapPx, kPaddingPx);
    if (nearest < 0 || !model_.FocusIndex(nearest)) {
        grabActive_ = false;
        return false;
    }

    grabCurrentOffset_ = current;
    viewport_.Reset(current);
    layoutSettleUntilMs_ = 0;
    layoutFailures_.clear();
    grabActive_ = false;
    transactionId_ = nextWindowId_++;
    grabLogPending_ = true;
    grabLogReason_ = reason ? reason : "unknown";
    grabLogPanPx_ = grabStartPoint_.x - grabLastPoint_.x;
    grabLogNearest_ = nearest;
    grabLogTarget_ = model_.TargetViewportOffset(
        viewportWidth, kGapPx, kPaddingPx);
    if (!PrepareViewport("grab-end", GetTickCount64(), true, detail)) {
        grabLogPending_ = false;
        return false;
    }
    return true;
}

void ScrollTilingController::OnEvents(std::span<const WindowEvent> events,
                                      WindowList& windows,
                                      const MonitorGeometry& monitor,
                                      int topInsetPx, unsigned long long nowMs) {
    if (!active_ || events.empty()) return;
    if (!CaptureContext(monitor, topInsetPx)) {
        Exit("context-changed");
        return;
    }

    for (const WindowEvent& event : events) {
        if (!event.hwnd) continue;
        switch (event.kind) {
            case WindowEventKind::Foreground:
                if (grabActive_) {
                    if (!Find(event.hwnd) &&
                        TaskListContains(windows, event.hwnd)) {
                        QueueAdmission(event.hwnd, nowMs, 0);
                    }
                    break;
                }
                if (Find(event.hwnd)) {
                    transactionId_ = nextWindowId_++;
                    PrepareViewport("foreground", nowMs, true, nullptr);
                } else if (TaskListContains(windows, event.hwnd)) {
                    QueueAdmission(event.hwnd, nowMs, 0);
                }
                break;
            case WindowEventKind::Created:
                QueueAdmission(event.hwnd, nowMs, kAdmissionDelayMs);
                break;
            case WindowEventKind::Shown:
                if (Find(event.hwnd)) {
                    transactionId_ = nextWindowId_++;
                    PrepareViewport("window-shown", nowMs, true, nullptr);
                } else {
                    const auto rejected = std::remove(
                        rejected_.begin(), rejected_.end(), event.hwnd);
                    if (rejected != rejected_.end()) {
                        rejected_.erase(rejected, rejected_.end());
                        LogAndPrint("tiling: event=window_revalidate hwnd=%p",
                                    reinterpret_cast<void*>(event.hwnd));
                    }
                    QueueAdmission(event.hwnd, nowMs, kAdmissionDelayMs);
                }
                break;
            case WindowEventKind::Destroyed:
                rejected_.erase(
                    std::remove(rejected_.begin(), rejected_.end(), event.hwnd),
                    rejected_.end());
                pending_.erase(
                    std::remove_if(pending_.begin(), pending_.end(),
                                   [&](const PendingAdmission& pending) {
                                       return pending.hwnd == event.hwnd;
                                   }),
                    pending_.end());
                if (RemoveWindow(event.hwnd)) {
                    if (records_.empty()) {
                        EndEmptyStrip();
                    } else if (grabActive_) {
                        const int viewportWidth =
                            workArea_.right - workArea_.left;
                        const int offset = model_.ClampViewportOffset(
                            viewport_.Current(), viewportWidth, kGapPx,
                            kPaddingPx);
                        grabCurrentOffset_ = offset;
                        viewport_.Reset(offset);
                        ApplyViewportOffset("grab-window-destroyed", offset,
                                            nullptr);
                    } else {
                        transactionId_ = nextWindowId_++;
                        PrepareViewport("window-destroyed", nowMs, true, nullptr);
                    }
                }
                break;
            case WindowEventKind::Hidden:
                break;
        }
    }
}

bool ScrollTilingController::Tick(unsigned long long nowMs,
                                  const MonitorGeometry& monitor,
                                  int topInsetPx, bool* animating,
                                  std::wstring* detail) {
    if (animating) *animating = false;
    if (!active_) return false;
    if (nowMs - lastContextCheckMs_ >= 500) {
        lastContextCheckMs_ = nowMs;
        if (!CaptureContext(monitor, topInsetPx)) {
            Exit("context-changed");
            return false;
        }
    }
    if (grabActive_) {
        if (animating) *animating = false;
        return false;
    }

    bool changed = false;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->dueMs <= nowMs) {
            const HWND hwnd = it->hwnd;
            it = pending_.erase(it);
            if (AdmitNow(hwnd, monitor, ScrollTilingColumnWidth{}, detail)) {
                changed = true;
            }
        } else {
            ++it;
        }
    }

    const int beforeOffset = viewport_.Current();
    const bool stillAnimating = viewport_.Advance(nowMs, nullptr);
    const bool offsetChanged = viewport_.Current() != beforeOffset;
    if (stillAnimating || offsetChanged || needsFrame_) {
        const char* event = pending_.empty() ? "viewport" : "admit";
        if (!ApplyViewportOffset(event, viewport_.Current(), detail)) {
            const int viewportWidth = workArea_.right - workArea_.left;
            const int target = model_.TargetViewportOffset(
                viewportWidth, kGapPx, kPaddingPx);
            const bool atFinalOffset =
                !stillAnimating && viewport_.Current() == target;
            if (!atFinalOffset) {
                needsFrame_ = true;
                changed = true;
            } else {
                if (layoutSettleUntilMs_ == 0) {
                    layoutSettleUntilMs_ = nowMs + 50;
                }
                if (nowMs < layoutSettleUntilMs_) {
                    needsFrame_ = true;
                    changed = true;
                } else if (!IsolateFailedWindows(nowMs, detail)) {
                    AbortAndRestore("settlement-failed");
                    return false;
                }
            }
        } else {
            needsFrame_ = false;
            layoutSettleUntilMs_ = 0;
            layoutFailures_.clear();
            changed = true;
        }
    }
    if (grabLogPending_ && viewport_.Current() == viewport_.Target()) {
        const int error = std::abs(grabLogTarget_ - viewport_.Current());
        LogAndPrint("tiling: event=grab_end tx=%llx reason=%s columns=%d "
                    "pan_px=%d nearest=%d target=%d offset=%d error=%d "
                    "result=ok",
                    static_cast<unsigned long long>(transactionId_),
                    grabLogReason_.c_str(), model_.Count(), grabLogPanPx_,
                    grabLogNearest_, grabLogTarget_, viewport_.Current(), error);
        grabLogPending_ = false;
    }
    if (animating) *animating = stillAnimating;
    return changed;
}

void ScrollTilingController::OnWindowRefresh(WindowList& windows,
                                             const MonitorGeometry& monitor,
                                             int topInsetPx,
                                             unsigned long long nowMs) {
    if (!active_ || nowMs - lastReconcileMs_ < kReconcileMs) return;
    lastReconcileMs_ = nowMs;
    if (!CaptureContext(monitor, topInsetPx)) {
        Exit("context-changed");
        return;
    }
    if (grabActive_) return;

    bool changed = false;
    if (!viewport_.Animating(nowMs)) {
        changed = DropExternallyMovedWindows() > 0;
    }
    for (auto it = records_.begin(); it != records_.end();) {
        if (!TaskListContains(windows, it->hwnd) ||
            !ScrollTilingWindows::StillSameWindow(*it)) {
            model_.Remove(it->id);
            it = records_.erase(it);
            changed = true;
            continue;
        }
        if (ScrollTilingWindows::IsSafeForLayout(*it)) {
            ++it;
            continue;
        }
        if (IsIconic(it->hwnd)) {
            ++it; // the column remains; the window is simply suspended.
            continue;
        }
        Exit("window-state-changed");
        return;
    }

    for (int i = 0; i < windows.Count(); ++i) {
        HWND hwnd = windows.At(i).handle;
        if (!Find(hwnd)) QueueAdmission(hwnd, nowMs, 0);
    }
    if (changed) {
        if (records_.empty()) {
            EndEmptyStrip();
        } else {
            transactionId_ = nextWindowId_++;
            PrepareViewport("reconcile", nowMs, true, nullptr);
        }
    }
}

void ScrollTilingController::OnFullscreenChanged(bool fullscreen) {
    if (active_ && fullscreen) Exit("fullscreen");
}

bool ScrollTilingController::Contains(HWND window) const {
    return Find(window) != nullptr;
}

ScrollTilingWindowRecord* ScrollTilingController::Find(HWND window) {
    for (ScrollTilingWindowRecord& record : records_) {
        if (record.hwnd == window) return &record;
    }
    return nullptr;
}

const ScrollTilingWindowRecord* ScrollTilingController::Find(HWND window) const {
    for (const ScrollTilingWindowRecord& record : records_) {
        if (record.hwnd == window) return &record;
    }
    return nullptr;
}

bool ScrollTilingController::SameContext(const MonitorGeometry& monitor,
                                         int topInsetPx) const {
    return active_ && monitor_.monitor && monitor.monitor == monitor_.monitor &&
           monitor.work.left == workArea_.left && monitor.work.top == workArea_.top &&
           monitor.work.right == workArea_.right &&
           monitor.work.bottom == workArea_.bottom && monitor.dpi == dpi_ &&
           std::max(0, topInsetPx) == topInsetPx_;
}

bool ScrollTilingController::CaptureContext(const MonitorGeometry& monitor,
                                            int topInsetPx) {
    if (!monitor.monitor) return false;
    if (active_) {
        const MonitorGeometry actual = QueryMonitor(monitor_.monitor);
        return SameContext(actual, topInsetPx);
    }
    monitor_ = monitor;
    workArea_ = monitor.work;
    dpi_ = monitor.dpi;
    topInsetPx_ = std::max(0, topInsetPx);
    return true;
}

void ScrollTilingController::QueueAdmission(HWND window,
                                            unsigned long long nowMs,
                                            unsigned long long delayMs) {
    if (!window || Find(window)) return;
    if (std::find(rejected_.begin(), rejected_.end(), window) != rejected_.end()) return;
    for (PendingAdmission& pending : pending_) {
        if (pending.hwnd == window) {
            pending.dueMs = std::min(pending.dueMs, nowMs + delayMs);
            return;
        }
    }
    pending_.push_back({window, nowMs + delayMs});
}

bool ScrollTilingController::AdmitNow(HWND window, const MonitorGeometry& monitor,
                                      ScrollTilingColumnWidth width,
                                      std::wstring* detail) {
    if (!window || Find(window)) return false;
    ScrollTilingWindowRecord record;
    std::wstring reason;
    if (!ScrollTilingWindows::Capture(window, monitor.monitor, &record, &reason)) {
        if (std::find(rejected_.begin(), rejected_.end(), window) == rejected_.end()) {
            rejected_.push_back(window);
        }
        LogAndPrint(L"tiling: event=window_rejected hwnd=%p reason='%ls'",
                    reinterpret_cast<void*>(window), reason.c_str());
        return false;
    }
    record.id = {nextWindowId_++};
    record.previousAppliedRect = record.originalRect;
    record.lastAppliedRect = record.originalRect;
    if (!model_.AddAfterActive(record.id, width)) return false;
    records_.push_back(record);
    transactionId_ = nextWindowId_++;
    LogAndPrint("tiling: event=window_admitted tx=%llx hwnd=%p column=%d total=%d",
                static_cast<unsigned long long>(transactionId_),
                reinterpret_cast<void*>(window), model_.ActiveIndex(),
                model_.Count());
    return PrepareViewport("window-admitted", GetTickCount64(), true, detail);
}

bool ScrollTilingController::RemoveWindow(HWND window) {
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->hwnd != window) continue;
        model_.Remove(it->id);
        records_.erase(it);
        return true;
    }
    return false;
}

bool ScrollTilingController::PrepareViewport(const char* event,
                                             unsigned long long nowMs,
                                             bool animate,
                                             std::wstring* detail) {
    if (!active_ || grabActive_ || !monitor_.monitor) return false;

    const int viewportWidth = workArea_.right - workArea_.left;
    const int target = model_.TargetViewportOffset(
        viewportWidth, kGapPx, kPaddingPx);
    model_.LayoutInto(
        {workArea_.left,
         std::max(workArea_.top, monitor_.screen.top + topInsetPx_),
         workArea_.right, workArea_.bottom},
        target, kGapPx, kPaddingPx, &layout_);
    if (!layout_.valid) return false;

    for (const ScrollTilingTarget& item : layout_.targets) {
        ScrollTilingWindowRecord* record = nullptr;
        for (ScrollTilingWindowRecord& candidate : records_) {
            if (candidate.id == item.id) {
                record = &candidate;
                break;
            }
        }
        if (!record) continue;
        RECT current = {};
        record->previousAppliedRect =
            GetWindowRect(record->hwnd, &current) ? current : record->lastAppliedRect;
        record->lastAppliedRect = {
            item.rect.left, item.rect.top, item.rect.right, item.rect.bottom};
    }
    if (!SaveJournal()) {
        if (detail) *detail = L"无法保存窗口恢复记录";
        return false;
    }

    needsFrame_ = true;
    if (animate) {
        viewport_.SetTarget(target, nowMs, 160);
    } else {
        viewport_.Reset(target);
    }
    transactionId_ = transactionId_ == 0 ? 1 : transactionId_;
    (void)event;
    return true;
}

bool ScrollTilingController::ApplyViewportOffset(const char* event, int offset,
                                                 std::wstring* detail) {
    layoutFailures_.clear();
    const ScrollTilingRectPx work = {
        workArea_.left, std::max(workArea_.top, monitor_.screen.top + topInsetPx_),
        workArea_.right, workArea_.bottom};
    model_.LayoutInto(work, offset, kGapPx, kPaddingPx, &layout_);
    if (!layout_.valid) return false;
    const bool verify = offset == layout_.viewportTarget;

    placements_.clear();
    for (const ScrollTilingTarget& target : layout_.targets) {
        ScrollTilingWindowRecord* record = nullptr;
        for (ScrollTilingWindowRecord& candidate : records_) {
            if (candidate.id == target.id) {
                record = &candidate;
                break;
            }
        }
        if (!record || !IsWindowVisible(record->hwnd) || IsIconic(record->hwnd)) continue;
        RECT expected = {
            target.rect.left, target.rect.top, target.rect.right, target.rect.bottom};
        if (verify &&
            ScrollTilingWindows::CurrentRectMatches(
                record->hwnd, expected, 1)) {
            continue;
        }
        placements_.push_back({record->hwnd, target.rect});
    }

    ScrollTilingApplyResult result;
    const bool ok =
        ScrollTilingWindows::Apply(placements_, verify, &result);
    if (!ok) {
        const std::uint64_t succeededMask =
            verify ? result.verifiedMask : result.placedMask;
        for (size_t i = 0; i < placements_.size(); ++i) {
            if ((succeededMask & (std::uint64_t{1} << i)) == 0) {
                layoutFailures_.push_back(placements_[i].hwnd);
            }
        }
    }
    if (verify) {
        LogAndPrint("tiling: event=layout_commit tx=%llx action=%s columns=%d "
                    "offset=%d target=%d submitted=%d applied=%d verified=%d "
                    "failed=%d duration_ms=%llu result=%s",
                    static_cast<unsigned long long>(transactionId_),
                    event ? event : "layout", model_.Count(), offset,
                    layout_.viewportTarget, result.requested, result.accepted,
                    result.verified, result.failed, result.durationMs,
                    ok ? "ok" : "failed");
    }
    if (!ok) {
        const std::uint64_t succeededMask =
            verify ? result.verifiedMask : result.placedMask;
        if (!verify) return false;
        for (size_t i = 0; i < placements_.size(); ++i) {
            if ((succeededMask & (std::uint64_t{1} << i)) != 0) continue;
            RECT actual = {};
            GetWindowRect(placements_[i].hwnd, &actual);
            LogAndPrint("tiling: event=layout_verify_failed tx=%llx hwnd=%p "
                        "target=(%d,%d,%d,%d) actual=(%ld,%ld,%ld,%ld)",
                        static_cast<unsigned long long>(transactionId_),
                        reinterpret_cast<void*>(placements_[i].hwnd),
                        placements_[i].rect.left, placements_[i].rect.top,
                        placements_[i].rect.right, placements_[i].rect.bottom,
                        actual.left, actual.top, actual.right, actual.bottom);
        }
    }
    if (!ok && detail) *detail = L"有窗口尚未到达目标位置";
    return ok;
}

bool ScrollTilingController::IsolateFailedWindows(unsigned long long nowMs,
                                                  std::wstring* detail) {
    if (layoutFailures_.empty()) return false;
    const int columnsBefore = model_.Count();
    int dropped = 0;
    int restored = 0;
    int external = 0;
    int failed = 0;

    for (HWND hwnd : layoutFailures_) {
        auto it = std::find_if(records_.begin(), records_.end(),
                               [hwnd](const ScrollTilingWindowRecord& record) {
                                   return record.hwnd == hwnd;
                               });
        if (it == records_.end()) continue;
        switch (ScrollTilingWindows::Restore(*it)) {
            case ScrollTilingRestoreResult::Restored: ++restored; break;
            case ScrollTilingRestoreResult::ExternallyMoved: ++external; break;
            case ScrollTilingRestoreResult::InvalidIdentity: ++failed; break;
            case ScrollTilingRestoreResult::Failed: ++failed; break;
        }
        if (std::find(rejected_.begin(), rejected_.end(), hwnd) ==
            rejected_.end()) {
            rejected_.push_back(hwnd);
        }
        pending_.erase(
            std::remove_if(pending_.begin(), pending_.end(),
                           [hwnd](const PendingAdmission& pending) {
                               return pending.hwnd == hwnd;
                           }),
            pending_.end());
        LogAndPrint("tiling: event=window_layout_rejected tx=%llx hwnd=%p "
                    "action=drop",
                    static_cast<unsigned long long>(transactionId_),
                    reinterpret_cast<void*>(hwnd));
        model_.Remove(it->id);
        records_.erase(it);
        ++dropped;
    }
    layoutFailures_.clear();
    layoutSettleUntilMs_ = 0;

    if (records_.empty()) {
        EndEmptyStrip();
        return true;
    }

    const int viewportWidth = workArea_.right - workArea_.left;
    const int offset = model_.ClampViewportOffset(
        viewport_.Current(), viewportWidth, kGapPx, kPaddingPx);
    viewport_.Reset(offset);
    grabCurrentOffset_ = offset;
    transactionId_ = nextWindowId_++;
    if (!PrepareViewport("layout-partial", nowMs, true, detail)) {
        return false;
    }
    LogAndPrint("tiling: event=layout_partial tx=%llx columns=%d->%d "
                "dropped=%d restored=%d external=%d failed=%d focus=%d "
                "offset=%d journal=%zu result=keep",
                static_cast<unsigned long long>(transactionId_), columnsBefore,
                model_.Count(), dropped, restored, external, failed,
                model_.ActiveIndex(), offset, records_.size());
    return true;
}

int ScrollTilingController::DropExternallyMovedWindows() {
    int dropped = 0;
    for (auto it = records_.begin(); it != records_.end();) {
        RECT current = {};
        if (!GetWindowRect(it->hwnd, &current)) {
            model_.Remove(it->id);
            it = records_.erase(it);
            ++dropped;
            continue;
        }
        if (ScrollTilingWindows::CurrentRectMatches(
                it->hwnd, it->lastAppliedRect, 1, &current) ||
            IsTransitionRect(current, it->previousAppliedRect,
                             it->lastAppliedRect)) {
            ++it;
            continue;
        }
        model_.Remove(it->id);
        LogAndPrint("tiling: event=window_external_move tx=%llx hwnd=%p action=drop",
                    static_cast<unsigned long long>(transactionId_),
                    reinterpret_cast<void*>(it->hwnd));
        it = records_.erase(it);
        ++dropped;
    }
    return dropped;
}

void ScrollTilingController::AbortAndRestore(const char* event) {
    int restored = 0;
    int failed = 0;
    for (const ScrollTilingWindowRecord& record : records_) {
        if (ScrollTilingWindows::RestoreOriginal(record)) ++restored;
        else ++failed;
    }
    LogAndPrint("tiling: event=%s tx=%llx restored=%d failed=%d",
                event ? event : "abort",
                static_cast<unsigned long long>(transactionId_), restored, failed);
    ClearActiveState();
    if (failed == 0) DeleteJournal();
}

void ScrollTilingController::EndEmptyStrip() {
    LogAndPrint("tiling: event=empty tx=%llx action=exit",
                static_cast<unsigned long long>(transactionId_));
    ClearActiveState();
    DeleteJournal();
}

void ScrollTilingController::ClearActiveState() {
    active_ = false;
    grabActive_ = false;
    needsFrame_ = false;
    model_.Clear();
    records_.clear();
    pending_.clear();
    rejected_.clear();
    grabStartPoint_ = {};
    grabLastPoint_ = {};
    grabStartOffset_ = 0;
    grabCurrentOffset_ = 0;
    grabLogPending_ = false;
    grabLogReason_.clear();
    grabLogPanPx_ = 0;
    grabLogNearest_ = -1;
    grabLogTarget_ = 0;
    layoutSettleUntilMs_ = 0;
    layoutFailures_.clear();
    viewport_.Reset(0);
    monitor_ = {};
    workArea_ = {};
    dpi_ = 96;
    topInsetPx_ = 0;
    lastContextCheckMs_ = 0;
}

bool ScrollTilingController::SaveJournal() {
    if (journalPath_.empty() ||
        records_.size() > static_cast<size_t>(kScrollTilingMaxColumns)) {
        return false;
    }
    const std::wstring temporary = journalPath_ + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file) return false;

    JournalHeader header;
    header.magic = kJournalMagic;
    header.version = kJournalVersion;
    header.count = static_cast<std::uint16_t>(records_.size());
    header.flags = grabActive_ ? kJournalFlagGrabActive : 0;
    bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1;
    for (size_t i = 0; ok && i < records_.size(); ++i) {
        const ScrollTilingWindowRecord& record = records_[i];
        const std::uint64_t hwnd = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(record.hwnd));
        ok = std::fwrite(&record.id.value, sizeof(record.id.value), 1, file) == 1 &&
             std::fwrite(&hwnd, sizeof(hwnd), 1, file) == 1 &&
             std::fwrite(&record.processId, sizeof(record.processId), 1, file) == 1 &&
             std::fwrite(&record.processCreationTime, sizeof(record.processCreationTime),
                         1, file) == 1 &&
             std::fwrite(&record.originalPlacement, sizeof(record.originalPlacement),
                         1, file) == 1 &&
             std::fwrite(&record.originalRect, sizeof(record.originalRect), 1, file) == 1 &&
             std::fwrite(&record.previousAppliedRect,
                         sizeof(record.previousAppliedRect), 1, file) == 1 &&
             std::fwrite(&record.lastAppliedRect, sizeof(record.lastAppliedRect), 1, file) == 1;
    }
    if (ok) ok = std::fflush(file) == 0 && _commit(_fileno(file)) == 0;
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    const bool replaced = MoveFileExW(
        temporary.c_str(), journalPath_.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!replaced) DeleteFileW(temporary.c_str());
    return replaced;
}

bool ScrollTilingController::LoadJournal(
    std::vector<ScrollTilingWindowRecord>* records, bool* interruptedGrab) {
    if (!records || !interruptedGrab || journalPath_.empty()) return false;
    records->clear();
    *interruptedGrab = false;
    FILE* file = nullptr;
    if (_wfopen_s(&file, journalPath_.c_str(), L"rb") != 0 || !file) return false;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t count = 0;
    std::uint16_t flags = 0;
    bool ok = std::fread(&magic, sizeof(magic), 1, file) == 1 &&
              std::fread(&version, sizeof(version), 1, file) == 1 &&
              std::fread(&count, sizeof(count), 1, file) == 1 &&
              magic == kJournalMagic &&
              (version == 2 || version == kJournalVersion) &&
              count <= kScrollTilingMaxColumns;
    if (ok && version == kJournalVersion) {
        std::uint16_t reserved = 0;
        ok = std::fread(&flags, sizeof(flags), 1, file) == 1 &&
             std::fread(&reserved, sizeof(reserved), 1, file) == 1;
    }
    *interruptedGrab =
        (flags & kJournalFlagGrabActive) != 0;
    for (size_t i = 0; ok && i < count; ++i) {
        ScrollTilingWindowRecord record;
        std::uint64_t hwnd = 0;
        ok = std::fread(&record.id.value, sizeof(record.id.value), 1, file) == 1 &&
             std::fread(&hwnd, sizeof(hwnd), 1, file) == 1 &&
             std::fread(&record.processId, sizeof(record.processId), 1, file) == 1 &&
             std::fread(&record.processCreationTime, sizeof(record.processCreationTime),
                        1, file) == 1 &&
             std::fread(&record.originalPlacement, sizeof(record.originalPlacement),
                        1, file) == 1 &&
             std::fread(&record.originalRect, sizeof(record.originalRect), 1, file) == 1 &&
             std::fread(&record.previousAppliedRect,
                        sizeof(record.previousAppliedRect), 1, file) == 1 &&
             std::fread(&record.lastAppliedRect, sizeof(record.lastAppliedRect), 1, file) == 1;
        record.hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(hwnd));
        record.originalPlacement.length = sizeof(record.originalPlacement);
        if (ok) records->push_back(record);
    }
    std::fclose(file);
    return ok && !records->empty();
}

void ScrollTilingController::DeleteJournal() {
    if (!journalPath_.empty()) DeleteFileW(journalPath_.c_str());
}

bool ScrollTilingController::JournalExists() const {
    if (journalPath_.empty()) return false;
    const DWORD attributes = GetFileAttributesW(journalPath_.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

} // namespace island
