// =====================================================================
//  SystemLoad.cpp -- see the header.
// =====================================================================
#include "platform/SystemLoad.h"

#include <algorithm>

namespace island {

unsigned long long SystemLoadReader::FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER ticks = {};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

bool SystemLoadReader::Refresh() {
    const unsigned long long now = GetTickCount64();
    if (lastRefreshMs_ != 0 && now - lastRefreshMs_ < 1000) return false;
    lastRefreshMs_ = now;

    FILETIME idleTime = {};
    FILETIME kernelTime = {};
    FILETIME userTime = {};
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        const unsigned long long idle = FileTimeValue(idleTime);
        const unsigned long long kernel = FileTimeValue(kernelTime);
        const unsigned long long user = FileTimeValue(userTime);
        if (haveCpuBaseline_ && kernel >= lastKernel_ && user >= lastUser_ && idle >= lastIdle_) {
            const unsigned long long kernelDelta = kernel - lastKernel_;
            const unsigned long long userDelta = user - lastUser_;
            const unsigned long long idleDelta = idle - lastIdle_;
            const unsigned long long totalDelta = kernelDelta + userDelta;
            if (totalDelta > 0) {
                const unsigned long long busyDelta = idleDelta < totalDelta ? totalDelta - idleDelta : 0;
                sample_.cpuPercent =
                    std::min(100.0f, static_cast<float>(busyDelta) * 100.0f /
                                         static_cast<float>(totalDelta));
            }
        }
        lastIdle_ = idle;
        lastKernel_ = kernel;
        lastUser_ = user;
        haveCpuBaseline_ = true;
    }

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys > 0) {
        sample_.memoryTotalBytes = memory.ullTotalPhys;
        sample_.memoryUsedBytes = memory.ullTotalPhys - memory.ullAvailPhys;
        sample_.memoryPercent =
            std::min(100.0f, static_cast<float>(sample_.memoryUsedBytes) * 100.0f /
                                 static_cast<float>(sample_.memoryTotalBytes));
    }

    ++revision_;
    return true;
}

} // namespace island
