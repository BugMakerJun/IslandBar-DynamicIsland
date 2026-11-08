// =====================================================================
//  WindowsFileSearch.h -- an on-demand worker over the Windows Search index.
//
//  Only file NAMES are matched. The index supplies the scope and metadata; this class never scans the
//  disk, reads file contents, or leaves a worker running after the Start search closes.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "domain/StartSearch.h"
#include "platform/WorkerThread.h"

namespace island {

class WindowsFileSearch {
public:
    WindowsFileSearch() = default;
    ~WindowsFileSearch();
    WindowsFileSearch(const WindowsFileSearch&) = delete;
    WindowsFileSearch& operator=(const WindowsFileSearch&) = delete;

    bool Request(const wchar_t* query, std::uint64_t sequence);
    bool Take(std::uint64_t* sequence, std::vector<StartSearchResult>* results, bool* completed);
    void Stop();

private:
    bool Start();
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();

    WorkerThread worker_;
    HANDLE wakeEvent_ = nullptr;
    std::mutex mutex_;
    std::wstring pendingQuery_;
    std::uint64_t pendingSequence_ = 0;
    std::uint64_t requestedSequence_ = 0;
    std::uint64_t publishedSequence_ = 0;
    std::uint64_t consumedSequence_ = 0;
    std::vector<StartSearchResult> publishedResults_;
    bool published_ = false;
    volatile bool running_ = false;
};

} // namespace island
