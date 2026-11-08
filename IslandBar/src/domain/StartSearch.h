// =====================================================================
//  StartSearch.h
//  Pure name matching for the Start card's application search.
// =====================================================================
#pragma once

#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace island {

enum class SearchResultKind {
    Application = 0,
    Setting,
    File,
};

struct StartSearchResult {
    SearchResultKind kind = SearchResultKind::Application;
    int sourceIndex = -1;       // StartApps::ListCount()-relative for applications, otherwise -1
    int score = 0;
    std::wstring title;
    std::wstring subtitle;
    std::wstring target;        // AppsFolder name, ms-settings URI, or a file path
    std::vector<uint8_t> iconPng;
};

// Lower is better. -1 means the name does not contain the query.
inline int StartSearchScore(std::wstring_view query, std::wstring_view name) {
    size_t first = 0;
    size_t last = query.size();
    while (first < last && std::iswspace(query[first])) ++first;
    while (last > first && std::iswspace(query[last - 1])) --last;
    if (first == last) return 0;

    std::wstring foldedQuery;
    std::wstring foldedName;
    foldedQuery.reserve(last - first);
    foldedName.reserve(name.size());
    for (size_t i = first; i < last; ++i) foldedQuery.push_back(std::towlower(query[i]));
    for (const wchar_t c : name) foldedName.push_back(std::towlower(c));

    if (foldedName == foldedQuery) return 0;
    if (foldedName.size() >= foldedQuery.size() &&
        foldedName.compare(0, foldedQuery.size(), foldedQuery) == 0) {
        return 1;
    }
    return foldedName.find(foldedQuery) == std::wstring::npos ? -1 : 2;
}

inline bool SearchResultBefore(const StartSearchResult& a, const StartSearchResult& b) {
    const int aKind = static_cast<int>(a.kind);
    const int bKind = static_cast<int>(b.kind);
    if (aKind != bKind) return aKind < bKind;
    if (a.score != b.score) return a.score < b.score;
    return a.title < b.title;
}

// Move the keyboard selection through a result list. A fresh list starts at row zero; Up and Down clamp at
// the ends instead of wrapping, which is what a search list should do when Enter is the active action.
inline int MoveSearchSelection(int current, int movement, int count) {
    if (count <= 0) return -1;
    if (current < 0 || current >= count) return movement > 0 ? 0 : count - 1;
    int selected = current + movement;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    return selected;
}

}  // namespace island
