// =====================================================================
//  SystemLoad.h -- cheap, rate-limited system CPU and physical-memory readings.
// =====================================================================
#pragma once

#include <windows.h>

namespace island {

class SystemLoadReader {
public:
    struct Sample {
        float cpuPercent = 0.0f;
        float memoryPercent = 0.0f;
        unsigned long long memoryUsedBytes = 0;
        unsigned long long memoryTotalBytes = 0;
    };

    // Samples at most once per second. Returns true only when a new sample was published.
    bool Refresh();
    const Sample& Current() const { return sample_; }
    unsigned long long Revision() const { return revision_; }

private:
    static unsigned long long FileTimeValue(const FILETIME& value);

    Sample sample_;
    unsigned long long revision_ = 1;
    unsigned long long lastRefreshMs_ = 0;
    unsigned long long lastIdle_ = 0;
    unsigned long long lastKernel_ = 0;
    unsigned long long lastUser_ = 0;
    bool haveCpuBaseline_ = false;
};

} // namespace island
