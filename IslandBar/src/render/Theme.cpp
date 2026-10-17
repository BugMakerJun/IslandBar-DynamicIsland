// =====================================================================
//  Theme.cpp -- see the header for what a theme is and what it is not.
// =====================================================================
#include "render/Theme.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace island {
namespace {

// --- the built-in themes -----------------------------------------------------------------------
//
// 默认 is EXACTLY what the bar looked like before themes existed -- the same numbers that were in
// Renderer.h and TextRenderer.cpp -- so adding this cannot change anyone's screen until they choose
// something else, and the screenshots taken before it stay valid.
//
// 冰蓝 is the brief's own palette: black at the top of the pill, deep blue under it, an ice-blue
// accent, and an ink that is very slightly blue rather than white.
//
// 石墨 is the high-contrast end: a neutral dark grey surface and pure-ish white ink, for a bright
// room or a light wallpaper, where the near-black pill reads as a hole.
const Theme kThemes[] = {
    // name          pillTop                          pillBottom                       gloss
    //               cardFill                         edge   accent                            ink
    //               shadow grain
    {L"\u9ED8\u8BA4",   {0.05f, 0.05f, 0.07f, 0.92f}, {0.04f, 0.05f, 0.08f, 0.98f}, 0.06f,
     {0.05f, 0.05f, 0.07f, 0.97f}, 0.13f, {0.0f, 0.47f, 0.83f, 0.95f}, {0.93f, 0.93f, 0.93f, 1.0f},
     0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    // 冰蓝
    {L"\u51B0\u84DD", {0.06f, 0.09f, 0.16f, 0.93f}, {0.02f, 0.03f, 0.07f, 0.97f}, 0.10f,
     {0.04f, 0.06f, 0.12f, 0.97f}, 0.16f, {0.20f, 0.62f, 0.98f, 0.95f}, {0.90f, 0.94f, 1.00f, 1.0f},
     0.05f, 0.02f, 0.10f, 0.18f, 0.12f},
    // 石墨
    {L"\u77F3\u58A8", {0.13f, 0.13f, 0.14f, 0.94f}, {0.08f, 0.08f, 0.09f, 0.98f}, 0.05f,
     {0.11f, 0.11f, 0.12f, 0.97f}, 0.18f, {0.24f, 0.56f, 0.90f, 0.95f}, {0.99f, 0.99f, 0.99f, 1.0f},
     0.05f, 0.0f, 0.06f, 0.16f, 0.10f},
    // 玻璃 -- asked for in these words: "加一个玻璃效果的".
    //
    // IT IS PAINTED GLASS, and the difference from real glass is measured rather than assumed. Three
    // APIs were tried on this window (scripts/probe-glass-blur.ps1, probe-glass-region.ps1):
    //
    //   * ACCENT_ENABLE_ACRYLICBLURBEHIND does blur what is behind the window -- edge energy of a strip
    //     behind it fell 16.0 -> 3.4 -- but across the WHOLE window rectangle, and a window region does
    //     NOT clip it (outside the region: 7.8 -> 3.4 with acrylic on). The window is a 360 DIP strip, so
    //     that would paint a blurred band across the top of the screen.
    //   * DWMWA_SYSTEMBACKDROP_TYPE = acrylic: accepted the call, changed nothing visible.
    //   * DwmEnableBlurBehindWindow WITH a region -- the one API documented to honour one -- returns
    //     S_OK and does nothing on Windows 11: the classic Aero glass it drove was removed.
    //
    // THE ROUTE THAT WAS LEFT IS NOW TAKEN (HANDOFF 8.166-8.168): `backdrop = true` gives this theme a
    // second, island-sized window that carries the system's acrylic, so the desktop behind the island IS
    // blurred. The painted material stays, and it is not decoration: the second window can fail to exist
    // (no export, a refused material), and then this theme is exactly what it used to be. Its fill is
    // deliberately much more transparent than the other three, because a 0.92 alpha would hide the
    // backdrop entirely; the gloss, the double rim, the grain and the wallpaper tint are what make the
    // blurred content read as a THICK piece of glass rather than as frosted plastic.
    // The numbers moved toward Apple's material on 2026-10-17, after the first real backdrop was judged
    // "still not liquid glass": it was a flat, nearly opaque grey slab. The backdrop is really blurred
    // now, so the fill can be much more transparent -- the colour behind comes through -- and the rim,
    // gloss and sweep carry the thickness that a photograph cannot (HANDOFF 8.170).
    {L"\u73BB\u7483", {0.14f, 0.18f, 0.26f, 0.18f}, {0.04f, 0.06f, 0.12f, 0.28f}, 0.30f,
     {0.08f, 0.10f, 0.16f, 0.60f}, 0.40f, {0.45f, 0.80f, 1.00f, 0.95f}, {1.00f, 1.00f, 1.00f, 1.0f},
     0.0f, 0.03f, 0.45f, 0.65f, 0.30f},
};
constexpr int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));

// The list the tile walks: the built-ins, then the user's own.
std::vector<Theme> g_themes(kThemes, kThemes + kThemeCount);
int g_current = 0;

// --- helpers -----------------------------------------------------------------------------------

float Clamp01(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }

// The relative luminance of a colour, on the sRGB curve the WCAG contrast rule is written in.
float Luminance(const D2D1_COLOR_F& colour) {
    const auto channel = [](float c) {
        c = Clamp01(c);
        return c <= 0.03928f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * channel(colour.r) + 0.7152f * channel(colour.g) + 0.0722f * channel(colour.b);
}

// "0.05" or "0.05,0.05,0.07" or "0.05,0.05,0.07,0.97".
bool ParseColour(const std::string& text, D2D1_COLOR_F* out) {
    if (!out) return false;
    const char* cursor = text.c_str();
    float parts[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int count = 0;
    while (count < 4) {
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor) break;
        parts[count++] = value;
        if (*end != ',') break;
        cursor = end + 1;
    }
    if (count < 3) {
        // One component is the shorthand for a GREY, which is what a theme file written by hand
        // wants half the time, and it costs one line here.
        if (count == 1) {
            parts[1] = parts[2] = parts[0];
        } else {
            return false;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (parts[i] < 0.0f || parts[i] > 1.0f) return false;
    }
    *out = D2D1::ColorF(parts[0], parts[1], parts[2], parts[3]);
    return true;
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// A UTF-8 name into the fixed field: the tile shows it, so it has to survive here as text rather than
// as bytes -- and the theme file is UTF-8, like weather.txt.
void NameFromUtf8(const std::string& utf8, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (utf8.empty()) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out, static_cast<int>(capacity));
}

std::string NameToUtf8(const wchar_t* name) {
    if (!name || !name[0]) return std::string();
    char buffer[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, buffer, sizeof(buffer), nullptr, nullptr);
    return std::string(buffer);
}

// `<exe dir>\<file>`: the same place weather.txt lives, so everything the user can edit is in one
// directory next to the program.
bool DataFilePath(const wchar_t* file, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0 || !file) return false;
    out[0] = L'\0';
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(module, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';
    swprintf(out, capacity, L"%ls%ls", module, file);
    return true;
}

void LogTheme(const Theme& theme, const wchar_t* how) {
    const float contrast = ThemeInkContrast(theme);
    LogLine(L"  theme: %ls (%ls) -- ink contrast %.1f:1%ls", theme.name, how, contrast,
            contrast < kInkContrastFloor ? L"  BELOW THE 4.5 FLOOR: text may be hard to read" : L"");
}

} // namespace

// --- the theme in force ------------------------------------------------------------------------

const Theme& CurrentTheme() {
    if (g_themes.empty()) return kThemes[0];
    if (g_current < 0 || g_current >= static_cast<int>(g_themes.size())) g_current = 0;
    return g_themes[static_cast<size_t>(g_current)];
}

int ThemeCount() { return static_cast<int>(g_themes.size()); }

const Theme& ThemeAt(int index) {
    if (index < 0 || index >= ThemeCount()) return g_themes[0];
    return g_themes[static_cast<size_t>(index)];
}

int CurrentThemeIndex() { return g_current; }

const Theme& CycleTheme() {
    if (g_themes.empty()) return kThemes[0];
    g_current = (g_current + 1) % ThemeCount();
    LogTheme(CurrentTheme(), L"switched");
    SaveThemeChoice(CurrentTheme().name);
    return CurrentTheme();
}

int BuiltInThemeCount() { return kThemeCount; }

const Theme& BuiltInTheme(int index) {
    if (index < 0 || index >= kThemeCount) return kThemes[0];
    return kThemes[index];
}

D2D1_COLOR_F ThemeInk(float alpha) {
    const D2D1_COLOR_F ink = CurrentTheme().ink;
    return D2D1::ColorF(ink.r, ink.g, ink.b, Clamp01(alpha) * Clamp01(ink.a));
}

float ThemeInkContrast(const Theme& theme) {
    // The card's fill over a BRIGHT desktop (0.9 grey): the card is translucent, so this is what the
    // text is actually read against on the wallpaper that gives it the least help.
    const float a = Clamp01(theme.cardFill.a);
    const float backdrop = 0.9f;
    const D2D1_COLOR_F effective = D2D1::ColorF(theme.cardFill.r * a + backdrop * (1.0f - a),
                                               theme.cardFill.g * a + backdrop * (1.0f - a),
                                               theme.cardFill.b * a + backdrop * (1.0f - a), 1.0f);
    const float first = Luminance(theme.ink);
    const float second = Luminance(effective);
    const float lighter = first > second ? first : second;
    const float darker = first > second ? second : first;
    return (lighter + 0.05f) / (darker + 0.05f);
}

// --- the liquid-glass layers: the wallpaper's colour, and the clamp that keeps the text readable -------

namespace {

// What the ink would be read against if a surface of this colour were the card: the same rule
// ThemeInkContrast uses, so the clamp below and the floor the self-test asserts are the same question.
float ContrastAgainst(const D2D1_COLOR_F& surface, const D2D1_COLOR_F& ink) {
    const float a = Clamp01(surface.a);
    const float backdrop = 0.9f;   // the bright desktop the floor is defined against
    const D2D1_COLOR_F effective = D2D1::ColorF(surface.r * a + backdrop * (1.0f - a),
                                              surface.g * a + backdrop * (1.0f - a),
                                              surface.b * a + backdrop * (1.0f - a), 1.0f);
    const float first = Luminance(ink);
    const float second = Luminance(effective);
    const float lighter = first > second ? first : second;
    const float darker = first > second ? second : first;
    return (lighter + 0.05f) / (darker + 0.05f);
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& from, const D2D1_COLOR_F& to, float amount) {
    const float t = Clamp01(amount);
    return D2D1::ColorF(from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
                        from.b + (to.b - from.b) * t, from.a + (to.a - from.a) * t);
}

}  // namespace

D2D1_COLOR_F TintedFill(const Theme& theme, const D2D1_COLOR_F& fill, const D2D1_COLOR_F& wallpaper,
                        float amount) {
    const float wanted = Clamp01(amount) * Clamp01(theme.tint);
    if (wanted <= 0.001f) return fill;

    // WALKED BACK, not clamped in luminance: the strength of the mix is what gives way, so a bright
    // wallpaper tints the pill a little instead of being refused outright. Four halvings are enough for
    // any wallpaper this side of white, and the loop is bounded rather than a bisection because the
    // answer only has to be good enough to pass a floor -- not exact.
    float applied = wanted;
    for (int step = 0; step < 4; ++step) {
        const D2D1_COLOR_F candidate = Mix(fill, wallpaper, applied);
        if (ContrastAgainst(candidate, theme.ink) >= kInkContrastFloor) return candidate;
        applied *= 0.5f;
    }
    return fill;
}

namespace {
D2D1_COLOR_F g_wallpaperTint = D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f);
bool g_wallpaperKnown = false;
}  // namespace

void SetWallpaperTint(const D2D1_COLOR_F& tint, bool fromWallpaper) {
    g_wallpaperTint = tint;
    g_wallpaperKnown = fromWallpaper;
}

const D2D1_COLOR_F& WallpaperTint() { return g_wallpaperTint; }
bool WallpaperTintIsKnown() { return g_wallpaperKnown; }

// --- reading a theme from text -------------------------------------------------------------------

bool ThemeFromText(const char* utf8, Theme* out, std::string* why) {
    if (!utf8 || !out) {
        if (why) *why = "no text";
        return false;
    }

    // Every field has a default, so a file may state only what it changes -- which is what a user
    // writing "the same as 默认 but with an ice-blue accent" wants to write.
    Theme theme = kThemes[0];
    theme.name[0] = L'\0';
    bool haveName = false;
    int applied = 0;

    const char* line = utf8;
    while (*line) {
        const char* end = line;
        while (*end && *end != '\n') ++end;
        const std::string raw(line, static_cast<size_t>(end - line));
        line = *end ? end + 1 : end;

        std::string text = Trim(raw);
        if (text.empty() || text[0] == '#') continue;

        const size_t equals = text.find('=');
        if (equals == std::string::npos) {
            if (why) *why = "a line without '=' : " + text;
            return false;
        }
        const std::string key = Trim(text.substr(0, equals));
        const std::string value = Trim(text.substr(equals + 1));
        if (key.empty() || value.empty()) {
            if (why) *why = "an empty key or value";
            return false;
        }

        const auto colour = [&](D2D1_COLOR_F* target) {
            if (!ParseColour(value, target)) {
                if (why) *why = "not a colour (0..1 per channel): " + key + "=" + value;
                return false;
            }
            ++applied;
            return true;
        };
        const auto scalarf = [&](float* target) {
            char* stop = nullptr;
            const float parsed = std::strtof(value.c_str(), &stop);
            if (stop == value.c_str() || parsed < 0.0f || parsed > 1.0f) {
                if (why) *why = "not a 0..1 number: " + key + "=" + value;
                return false;
            }
            *target = parsed;
            ++applied;
            return true;
        };

        if (key == "name") {
            NameFromUtf8(value, theme.name, 32);
            haveName = theme.name[0] != L'\0';
            ++applied;
        } else if (key == "pill-top") {
            if (!colour(&theme.pillTop)) return false;
        } else if (key == "pill-bottom") {
            if (!colour(&theme.pillBottom)) return false;
        } else if (key == "gloss") {
            if (!scalarf(&theme.gloss)) return false;
        } else if (key == "card") {
            if (!colour(&theme.cardFill)) return false;
        } else if (key == "card-edge") {
            if (!scalarf(&theme.cardEdge)) return false;
        } else if (key == "accent") {
            if (!colour(&theme.accent)) return false;
        } else if (key == "ink") {
            if (!colour(&theme.ink)) return false;
        } else if (key == "shadow") {
            if (!scalarf(&theme.shadow)) return false;
        } else if (key == "grain") {
            if (!scalarf(&theme.grain)) return false;
        } else if (key == "sweep") {
            if (!scalarf(&theme.sweep)) return false;
        } else if (key == "rim") {
            if (!scalarf(&theme.rim)) return false;
        } else if (key == "tint") {
            if (!scalarf(&theme.tint)) return false;
        } else {
            if (why) *why = "unknown key: " + key;
            return false;
        }
    }

    if (!haveName) {
        if (why) *why = "no name= line";
        return false;
    }
    if (applied <= 1) {
        if (why) *why = "nothing but a name";
        return false;
    }
    *out = theme;
    return true;
}

// --- loading, remembering ------------------------------------------------------------------------

namespace {

// The `themes\` directory beside the program, and every *.txt in it. A file that does not parse is
// REPORTED and skipped: one bad file must not cost the user the themes that do work.
void LoadUserThemes() {
    wchar_t directory[MAX_PATH] = {};
    if (!DataFilePath(L"themes\\", directory, MAX_PATH)) return;

    wchar_t pattern[MAX_PATH] = {};
    swprintf(pattern, MAX_PATH, L"%ls*.txt", directory);

    WIN32_FIND_DATAW found = {};
    const HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        LogLine(L"  theme: no themes\\ directory (that is the ordinary case)");
        return;
    }
    int loaded = 0;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t path[MAX_PATH] = {};
        swprintf(path, MAX_PATH, L"%ls%ls", directory, found.cFileName);

        std::string text;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"rb") != 0 || !file) {
            LogLine(L"  theme: could not open %ls", found.cFileName);
            continue;
        }
        char buffer[2048] = {};
        const size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
        fclose(file);
        text.assign(buffer, read);

        // A UTF-8 BOM is what Notepad writes on an older Windows, and it would otherwise become part
        // of the first line's key.
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            text.erase(0, 3);
        }

        Theme theme;
        std::string why;
        if (!ThemeFromText(text.c_str(), &theme, &why)) {
            LogLine(L"  theme: %ls was skipped -- %hs", found.cFileName, why.c_str());
            continue;
        }
        g_themes.push_back(theme);
        LogTheme(theme, L"from themes\\");
        ++loaded;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    LogLine(L"  theme: %d theme(s) loaded from themes\\", loaded);
}

} // namespace

bool SaveThemeChoice(const wchar_t* name) {
    wchar_t path[MAX_PATH] = {};
    if (!DataFilePath(L"theme.txt", path, MAX_PATH)) return false;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) return false;
    std::fprintf(file, "# The bar's material. One line, the name of a theme: the built-in ones are\n");
    std::fprintf(file, "# 默认 / 冰蓝 / 石墨, and any file in themes\\ adds one more.\n");
    const std::string utf8 = NameToUtf8(name);
    std::fwrite(utf8.c_str(), 1, utf8.size(), file);
    std::fprintf(file, "\n");
    std::fclose(file);
    return true;
}

void ApplyRememberedTheme() {
    LoadUserThemes();

    wchar_t path[MAX_PATH] = {};
    if (!DataFilePath(L"theme.txt", path, MAX_PATH)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") != 0 || !file) {
        LogTheme(CurrentTheme(), L"built-in default; no theme.txt");
        return;
    }
    char buffer[256] = {};
    // The return value is deliberately not kept: the buffer is what matters, and a short read
    // simply leaves the rest of the buffer zero-filled.
    (void)fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);

    std::string wanted;
    const char* line = buffer;
    while (*line) {
        const char* end = line;
        while (*end && *end != '\n') ++end;
        std::string text = Trim(std::string(line, static_cast<size_t>(end - line)));
        line = *end ? end + 1 : end;
        if (text.empty() || text[0] == '#') continue;
        wanted = text;
        break;
    }
    if (wanted.empty()) {
        LogTheme(CurrentTheme(), L"built-in default; theme.txt names nothing");
        return;
    }

    wchar_t wide[32] = {};
    NameFromUtf8(wanted, wide, 32);
    for (int i = 0; i < ThemeCount(); ++i) {
        if (_wcsicmp(ThemeAt(i).name, wide) == 0) {
            g_current = i;
            LogTheme(CurrentTheme(), L"remembered by theme.txt");
            return;
        }
    }
    LogLine(L"  theme: theme.txt names '%ls', which is not one of the %d themes; staying on %ls",
            wide, ThemeCount(), CurrentTheme().name);
}

} // namespace island
