// =====================================================================
//  SettingsCatalog.h -- the small, curated set of Windows Settings destinations searchable from Start.
//
//  These are stable top-level destinations, not a fabricated index of every page. Each URI is launched
//  through ShellExecute and a failure is reported rather than hidden.
// =====================================================================
#pragma once

#include <vector>

#include "domain/StartSearch.h"

namespace island {

inline void SearchSettingsCatalog(const wchar_t* query, std::vector<StartSearchResult>* out) {
    if (!out) return;
    struct Entry {
        const wchar_t* name;
        const wchar_t* keywords;
        const wchar_t* uri;
    };
    static constexpr Entry entries[] = {
        {L"\u663E\u793A\u8BBE\u7F6E", L"\u663E\u793A \u5C4F\u5E55 \u5206\u8FA8\u7387 display screen",
         L"ms-settings:display"},
        {L"\u58F0\u97F3\u8BBE\u7F6E", L"\u58F0\u97F3 \u97F3\u91CF \u97F3\u9891 sound audio",
         L"ms-settings:sound"},
        {L"\u84DD\u7259\u8BBE\u7F6E", L"\u84DD\u7259 \u8BBE\u5907 bluetooth device",
         L"ms-settings:bluetooth"},
        {L"Wi-Fi \u8BBE\u7F6E", L"Wi-Fi \u65E0\u7EBF\u7F51\u7EDC \u7F51\u7EDC wireless network",
         L"ms-settings:network-wifi"},
        {L"\u4E2A\u6027\u5316", L"\u4E2A\u6027\u5316 \u4E3B\u9898 \u989C\u8272 \u80CC\u666F personalization theme",
         L"ms-settings:personalization"},
        {L"\u4EFB\u52A1\u680F\u8BBE\u7F6E", L"\u4EFB\u52A1\u680F \u5F00\u59CB\u83DC\u5355 taskbar start",
         L"ms-settings:taskbar"},
        {L"\u5E94\u7528\u7BA1\u7406", L"\u5E94\u7528 \u5378\u8F7D \u5B89\u88C5 apps uninstall",
         L"ms-settings:appsfeatures"},
        {L"Windows \u66F4\u65B0", L"Windows \u66F4\u65B0 update",
         L"ms-settings:windowsupdate"},
    };

    for (const Entry& entry : entries) {
        int score = StartSearchScore(query, entry.name);
        const int keywords = StartSearchScore(query, entry.keywords);
        if (keywords >= 0 && (score < 0 || keywords + 10 < score)) score = keywords + 10;
        if (score < 0) continue;
        StartSearchResult result;
        result.kind = SearchResultKind::Setting;
        result.title = entry.name;
        result.subtitle = L"Windows \u8BBE\u7F6E";
        result.target = entry.uri;
        result.score = score;
        out->push_back(std::move(result));
    }
}

} // namespace island
