// =====================================================================
//  Weather.cpp
//
//  NOTE ON CHINESE IN THIS FILE: every non-ASCII character is written as a
//  \u escape, and the comments are English. A PowerShell round trip through
//  `Get-Content -Raw` / `Set-Content` rewrote this file in the console's code
//  page once, which produced invalid UTF-8 (MSVC C4828) and swallowed line
//  breaks inside comments -- one of them inside the code table below. Escapes
//  and ASCII comments cannot be damaged that way. See the encoding rules in
//  START-HERE.txt and HANDOFF 6.1.
// =====================================================================
#include "shell/Weather.h"

#include "core/Log.h"
#include "platform/Http.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>

// RoInitialize / RoUninitialize: the apartment entry points, which the media session's worker
// uses too. Not pulled in by the projection headers, and the error when it is missing names a
// function rather than the header.
#include <roapi.h>

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace island {
namespace {

// The two services, both keyless. Named here rather than at the call sites so that
// "which service answered" is one thing to look at when the reading stops.
constexpr wchar_t kGeocodeHost[] = L"geocoding-api.open-meteo.com";
constexpr wchar_t kForecastHost[] = L"api.open-meteo.com";

// Ten seconds: long enough for a slow connection, short enough that a poll cannot
// sit in one request for the rest of the session.
constexpr int kTimeoutMs = 10000;


// Where the city is read from: a file beside the executable, which is where the bar
// itself lives (%USERPROFILE%\IslandBar). Beside the executable rather than in the
// working tree, for the reason the log gives -- running the bar must not dirty the
// repository -- and rather than in the registry, because a text file is something the
// user can open, read and correct without a tool.
//
// It is an OVERRIDE now rather than a requirement: with no line in it the bar asks Windows
// for the machine's position (see LocationFromWindows), and a line is how the user pins a
// city instead -- which is what the file is for, and why "no file" is the ordinary case.
void SettingsPath(wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';

    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;

    wchar_t* lastSlash = wcsrchr(module, L'\\');
    if (!lastSlash) return;
    *(lastSlash + 1) = L'\0';

    swprintf(out, capacity, L"%lsweather.txt", module);
}

// Reads the settings file and answers the one line that names the city.
//
// The format, in full:
//
//   * the file is UTF-8, and the first line that is neither empty nor a comment is
//     the city;
//   * a line beginning with '#' is a comment, so the file can explain itself;
//   * the line is either a CITY NAME (e.g. "\u5317\u4eac" for Beijing) or a pair of
//     coordinates ("39.9042,116.4074"), which skips the geocoding step.
//
// A missing (or empty, or all-comment) file is the ORDINARY case and not an error: it means
// "use where the machine says it is", and only when Windows cannot answer either does the
// status line go without a reading -- with the reason in the log.
bool ReadSettings(wchar_t* out, size_t capacity, bool* outIsCoordinates) {
    if (outIsCoordinates) *outIsCoordinates = false;
    if (!out || capacity == 0) return false;
    out[0] = L'\0';

    wchar_t path[MAX_PATH] = {};
    SettingsPath(path, MAX_PATH);
    if (path[0] == L'\0') return false;

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") != 0 || !file) return false;

    char buffer[1024] = {};
    const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[read] = '\0';

    // UTF-8 -> UTF-16 once, and then the lines are walked as wide text: the city can be
    // Chinese, and a byte-wise trim would cut a multi-byte character in half.
    wchar_t wide[1024] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, buffer, -1, wide, 1024) <= 0) return false;

    wchar_t* cursor = wide;
    if (cursor[0] == L'\ufeff') ++cursor;   // a BOM is not part of the first line

    while (*cursor != L'\0') {
        wchar_t* lineEnd = wcschr(cursor, L'\n');
        if (lineEnd) *lineEnd = L'\0';

        // Trimmed, so a line written with trailing spaces -- or with CRLF line endings --
        // still reads.
        wchar_t* start = cursor;
        while (*start == L' ' || *start == L'\t' || *start == L'\r') ++start;
        wchar_t* end = start + wcslen(start);
        while (end > start && (end[-1] == L' ' || end[-1] == L'\t' || end[-1] == L'\r')) --end;
        *end = L'\0';

        if (start[0] != L'\0' && start[0] != L'#') {
            wcsncpy_s(out, capacity, start, _TRUNCATE);

            // Coordinates, when the line holds exactly two numbers around a comma.
            double latitude = 0.0;
            double longitude = 0.0;
            const wchar_t* comma = wcschr(out, L',');
            if (comma) {
                wchar_t first[64] = {};
                wcsncpy_s(first, 64, out, static_cast<size_t>(comma - out));
                wchar_t* stopA = nullptr;
                wchar_t* stopB = nullptr;
                latitude = wcstod(first, &stopA);
                longitude = wcstod(comma + 1, &stopB);
                const bool bothNumbers = (stopA != first) && (stopB != comma + 1);
                if (bothNumbers && *stopA == L'\0' && std::fabs(latitude) <= 90.0 &&
                    std::fabs(longitude) <= 180.0) {
                    if (outIsCoordinates) *outIsCoordinates = true;
                }
            }
            return true;
        }

        if (!lineEnd) break;
        cursor = lineEnd + 1;
    }

    return false;
}

// One field of a comma-separated line, trimmed. False when the field is empty or the
// line has no such field.
bool CsvField(const char* line, int index, char* out, size_t capacity) {
    if (!line || !out || capacity == 0) return false;
    out[0] = '\0';

    const char* cursor = line;
    for (int i = 0; i < index; ++i) {
        const char* comma = std::strchr(cursor, ',');
        if (!comma) return false;
        cursor = comma + 1;
    }

    const char* end = std::strchr(cursor, ',');
    size_t length = end ? static_cast<size_t>(end - cursor) : std::strlen(cursor);
    while (length > 0 && (cursor[0] == ' ' || cursor[0] == '\t')) {
        ++cursor;
        --length;
    }
    while (length > 0 && (cursor[length - 1] == ' ' || cursor[length - 1] == '\t' ||
                          cursor[length - 1] == '\r' || cursor[length - 1] == '\n')) {
        --length;
    }
    if (length == 0 || length + 1 > capacity) return false;

    std::memcpy(out, cursor, length);
    out[length] = '\0';
    return true;
}

// Whether a line is worth reading as a reading: not blank, and not a comment.
bool IsDataLine(const char* line) {
    return line != nullptr && line[0] != '\0' && line[0] != '#';
}

// The machine's own position, from Windows.
//
// WHY THIS AND NOT AN IP LOOKUP. An IP lookup answers for the network the traffic leaves from,
// and on this machine that is a proxy: `ip-api.com` says Tokyo while the clock runs on
// Asia/Shanghai. Windows' own provider uses WiFi access points and satellites, and was measured
// on this machine answering WiFi, 187 m, in the right city. Asked for in these words:
// "天气的能不能调用定位 而不是根据ip".
//
// False with a reason in `reason` when the machine cannot say: the service is off, this program
// is refused, or no provider answers inside the timeout. A reason rather than a bare failure
// because the three have three different fixes, and the log is where they are told apart.
//
// Must be called from a thread that has entered an apartment; see WeatherReader::Run.
bool LocationFromWindows(double* latitude, double* longitude, char* reason, size_t reasonCapacity) {
    if (reason && reasonCapacity) reason[0] = '\0';
    if (!latitude || !longitude) return false;

    using namespace winrt::Windows::Devices::Geolocation;

    // May raise the Windows consent prompt the first time an unpackaged program asks. A denial is
    // a normal answer, not an error: the user may simply keep location off, and the bar falls back
    // to weather.txt without complaining about it every fifteen minutes.
    GeolocationAccessStatus access = GeolocationAccessStatus::Unspecified;
    try {
        access = Geolocator::RequestAccessAsync().get();
    } catch (const winrt::hresult_error& e) {
        if (reason && reasonCapacity) {
            std::snprintf(reason, reasonCapacity, "the consent call failed (0x%08lX)",
                          static_cast<unsigned long>(e.code().value));
        }
        return false;
    }
    if (access != GeolocationAccessStatus::Allowed) {
        if (reason && reasonCapacity) {
            std::snprintf(reason, reasonCapacity, "location is %s for this program",
                          access == GeolocationAccessStatus::Denied ? "denied" : "unspecified");
        }
        return false;
    }

    try {
        Geolocator locator;
        locator.DesiredAccuracy(PositionAccuracy::Default);

        // A fix up to an hour old is accepted -- the bar is not navigating -- but an older one is
        // not, and the whole call gives up after ten seconds.
        const winrt::Windows::Foundation::TimeSpan maximumAge{std::chrono::seconds(3600)};
        const winrt::Windows::Foundation::TimeSpan timeout{std::chrono::seconds(10)};
        const Geoposition position = locator.GetGeopositionAsync(maximumAge, timeout).get();
        if (!position) {
            if (reason && reasonCapacity) std::snprintf(reason, reasonCapacity, "no position");
            return false;
        }

        const Geocoordinate coordinate = position.Coordinate();
        const auto point = coordinate.Point().Position();
        *latitude = point.Latitude;
        *longitude = point.Longitude;
        return true;
    } catch (const winrt::hresult_error& e) {
        // The status is worth naming: Disabled (the service is off), NoData (no provider answered)
        // and NotAvailable (no provider exists) need three different answers from the user.
        if (reason && reasonCapacity) {
            std::snprintf(reason, reasonCapacity, "no fix (0x%08lX)",
                          static_cast<unsigned long>(e.code().value));
        }
        return false;
    }
}

} // namespace

// --- the pure pieces --------------------------------------------------

const wchar_t* WeatherWordForCode(int code) {
    // The WMO codes Open-Meteo answers with, in the one word the strip has room for.
    // The Chinese is \u-escaped, one whole statement per line, for the reason the file's
    // header gives. The comment after each line says what the code means.
    switch (code) {
        case 0:  return L"\u6674";              // clear
        case 1:  return L"\u6674";              // mainly clear
        case 2:  return L"\u591a\u4e91";        // partly cloudy
        case 3:  return L"\u9634";              // overcast
        case 45: return L"\u96fe";              // fog
        case 48: return L"\u96fe";              // depositing rime fog
        case 51: return L"\u5c0f\u96e8";        // drizzle, light
        case 53: return L"\u5c0f\u96e8";        // drizzle, moderate
        case 55: return L"\u4e2d\u96e8";        // drizzle, dense
        case 56: return L"\u51bb\u96e8";        // freezing drizzle, light
        case 57: return L"\u51bb\u96e8";        // freezing drizzle, dense
        case 61: return L"\u5c0f\u96e8";        // rain, slight
        case 63: return L"\u4e2d\u96e8";        // rain, moderate
        case 65: return L"\u5927\u96e8";        // rain, heavy
        case 66: return L"\u51bb\u96e8";        // freezing rain, light
        case 67: return L"\u51bb\u96e8";        // freezing rain, heavy
        case 71: return L"\u5c0f\u96ea";        // snow, slight
        case 73: return L"\u4e2d\u96ea";        // snow, moderate
        case 75: return L"\u5927\u96ea";        // snow, heavy
        case 77: return L"\u7c73\u96ea";        // snow grains
        case 80: return L"\u9635\u96e8";        // rain showers, slight
        case 81: return L"\u9635\u96e8";        // rain showers, moderate
        case 82: return L"\u5f3a\u9635\u96e8";  // rain showers, violent
        case 85: return L"\u9635\u96ea";        // snow showers, slight
        case 86: return L"\u9635\u96ea";        // snow showers, heavy
        case 95: return L"\u96f7\u9635\u96e8";  // thunderstorm
        case 96: return L"\u51b0\u96f9";        // thunderstorm with slight hail
        case 99: return L"\u51b0\u96f9";        // thunderstorm with heavy hail
        default: return nullptr;
    }
}

SkyMark SkyMarkForCode(int code) {
    // The same WMO table as WeatherWordForCode above, and deliberately listed code by code rather than as
    // ranges: the two tables are read side by side, and a code that is in one and not the other is what the
    // self-test looks for.
    switch (code) {
        case 0:  return SkyMark::Sun;       // clear
        case 1:  return SkyMark::Sun;       // mainly clear
        case 2:  return SkyMark::SunCloud;  // partly cloudy
        case 3:  return SkyMark::Cloud;     // overcast
        case 45: return SkyMark::Fog;
        case 48: return SkyMark::Fog;
        case 51: return SkyMark::Rain;      // drizzle, light
        case 53: return SkyMark::Rain;      // drizzle, moderate
        case 55: return SkyMark::Rain;      // drizzle, dense
        case 56: return SkyMark::Rain;      // freezing drizzle
        case 57: return SkyMark::Rain;
        case 61: return SkyMark::Rain;      // rain, slight
        case 63: return SkyMark::Rain;      // rain, moderate
        case 65: return SkyMark::Rain;      // rain, heavy
        case 66: return SkyMark::Rain;      // freezing rain
        case 67: return SkyMark::Rain;
        case 71: return SkyMark::Snow;      // snow, slight
        case 73: return SkyMark::Snow;
        case 75: return SkyMark::Snow;
        case 77: return SkyMark::Snow;      // snow grains
        case 80: return SkyMark::Rain;      // rain showers
        case 81: return SkyMark::Rain;
        case 82: return SkyMark::Rain;      // violent rain showers
        case 85: return SkyMark::Snow;      // snow showers
        case 86: return SkyMark::Snow;
        case 95: return SkyMark::Thunder;   // thunderstorm
        case 96: return SkyMark::Thunder;   // with slight hail
        case 99: return SkyMark::Thunder;   // with heavy hail
        default: return SkyMark::None;
    }
}

void WeatherLineFor(double temperatureC, int code, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';

    // Rounded to the nearest degree: that is what the strip's few characters can hold, and
    // what every weather readout on this desktop shows.
    const int degrees = static_cast<int>(std::lround(temperatureC));
    const wchar_t* word = WeatherWordForCode(code);
    if (word) {
        swprintf(out, capacity, L"%ls %d\u00b0", word, degrees);
    } else {
        // An unknown code prints the temperature on its own rather than inventing a sky.
        swprintf(out, capacity, L"%d\u00b0", degrees);
    }
}

void WeatherPiecesFor(double temperatureC, int code, wchar_t* sky, size_t skyCapacity,
                      wchar_t* temperature, size_t temperatureCapacity) {
    if (sky && skyCapacity) sky[0] = L'\0';
    if (temperature && temperatureCapacity) temperature[0] = L'\0';

    const int degrees = static_cast<int>(std::lround(temperatureC));
    const wchar_t* word = WeatherWordForCode(code);
    // An unknown code leaves the word empty rather than inventing a sky; the line then draws the
    // temperature on its own, which is what the joined form does too (see WeatherLineFor).
    if (word && sky && skyCapacity) wcsncpy_s(sky, skyCapacity, word, _TRUNCATE);
    if (temperature && temperatureCapacity) {
        swprintf(temperature, temperatureCapacity, L"%d\u00b0", degrees);
    }
}

size_t PercentEncodeUtf8(const char* text, char* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    out[0] = '\0';
    if (!text) return 0;

    size_t written = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                                c == '~';
        if (written + 4 >= capacity) break;   // room for the escape and the terminator

        if (unreserved) {
            out[written++] = static_cast<char>(c);
        } else {
            std::snprintf(out + written, 4, "%%%02X", static_cast<unsigned>(c));
            written += 3;
        }
    }
    out[written] = '\0';
    return written;
}

bool ParseCurrentCsv(const char* csv, double* temperatureC, int* weatherCode) {
    if (!csv) return false;

    // The LAST data line, walked line by line rather than taken as "line two": a service
    // that adds a header line must not turn the reading into the columns' names.
    char lastLine[256] = {};
    bool haveLastLine = false;
    const char* cursor = csv;
    while (*cursor != '\0') {
        const char* lineEnd = std::strchr(cursor, '\n');
        const size_t length = lineEnd ? static_cast<size_t>(lineEnd - cursor) : std::strlen(cursor);

        bool blank = true;
        for (size_t i = 0; i < length; ++i) {
            if (cursor[i] != ' ' && cursor[i] != '\t' && cursor[i] != '\r') {
                blank = false;
                break;
            }
        }
        // COPIED, not pointed at: CsvField reads a field up to the next comma, and the last
        // field of the document's last line has no comma after it -- it has the end of the
        // buffer, so the line has to end here as well.
        if (!blank && length < sizeof(lastLine)) {
            std::memcpy(lastLine, cursor, length);
            lastLine[length] = '\0';
            haveLastLine = true;
        }

        if (!lineEnd) break;
        cursor = lineEnd + 1;
    }

    if (!haveLastLine || !IsDataLine(lastLine)) return false;

    char temperature[32] = {};
    char code[32] = {};
    if (!CsvField(lastLine, 1, temperature, sizeof(temperature))) return false;
    if (!CsvField(lastLine, 2, code, sizeof(code))) return false;

    // Field 0 is the reading's own timestamp and is not needed: the value is whatever the
    // service last published, and the run's log carries the time it arrived.
    char* stopTemp = nullptr;
    char* stopCode = nullptr;
    const double t = std::strtod(temperature, &stopTemp);
    const long c = std::strtol(code, &stopCode, 10);
    if (stopTemp == temperature || stopCode == code) return false;

    if (temperatureC) *temperatureC = t;
    if (weatherCode) *weatherCode = static_cast<int>(c);
    return true;
}

bool ParseDailyCsv(const char* csv, int* sunriseMinutes, int* sunsetMinutes) {
    if (sunriseMinutes) *sunriseMinutes = -1;
    if (sunsetMinutes) *sunsetMinutes = -1;
    if (!csv) return false;

    // The LAST non-empty line is today's row: date,sunrise,sunset.
    char lastLine[256] = {};
    bool haveLastLine = false;
    const char* cursor = csv;
    while (*cursor != '\0') {
        const char* lineEnd = std::strchr(cursor, '\n');
        const size_t length = lineEnd ? static_cast<size_t>(lineEnd - cursor) : std::strlen(cursor);

        bool blank = true;
        for (size_t i = 0; i < length; ++i) {
            if (cursor[i] != ' ' && cursor[i] != '\t' && cursor[i] != '\r') {
                blank = false;
                break;
            }
        }
        if (!blank && length < sizeof(lastLine)) {
            std::memcpy(lastLine, cursor, length);
            lastLine[length] = '\0';
            haveLastLine = true;
        }

        if (!lineEnd) break;
        cursor = lineEnd + 1;
    }

    if (!haveLastLine || !IsDataLine(lastLine)) return false;

    char sunrise[48] = {};
    char sunset[48] = {};
    if (!CsvField(lastLine, 1, sunrise, sizeof(sunrise))) return false;
    if (!CsvField(lastLine, 2, sunset, sizeof(sunset))) return false;

    const auto minutesSinceMidnight = [](const char* iso) -> int {
        if (!iso) return -1;
        const char* t = std::strchr(iso, 'T');
        if (!t) return -1;
        int hour = -1;
        int minute = -1;
        if (std::sscanf(t + 1, "%d:%d", &hour, &minute) != 2) return -1;
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
        return hour * 60 + minute;
    };
    const int rise = minutesSinceMidnight(sunrise);
    const int set = minutesSinceMidnight(sunset);
    if (rise < 0 || set < 0) return false;

    if (sunriseMinutes) *sunriseMinutes = rise;
    if (sunsetMinutes) *sunsetMinutes = set;
    return true;
}

bool ParseGeocodeJson(const char* json, double* latitude, double* longitude) {
    if (!json) return false;

    // "results" first: a search that matched nothing answers "{}", and this must not read a
    // latitude out of some other part of a document that has none.
    const char* results = std::strstr(json, "\"results\"");
    if (!results) return false;

    const char* latKey = std::strstr(results, "\"latitude\"");
    const char* lonKey = std::strstr(results, "\"longitude\"");
    if (!latKey || !lonKey) return false;

    const char* latValue = std::strchr(latKey, ':');
    const char* lonValue = std::strchr(lonKey, ':');
    if (!latValue || !lonValue) return false;

    char* stopLat = nullptr;
    char* stopLon = nullptr;
    const double lat = std::strtod(latValue + 1, &stopLat);
    const double lon = std::strtod(lonValue + 1, &stopLon);
    if (stopLat == latValue + 1 || stopLon == lonValue + 1) return false;

    if (latitude) *latitude = lat;
    if (longitude) *longitude = lon;
    return true;
}

// --- the worker -------------------------------------------------------

WeatherReader::~WeatherReader() {
    Stop();
}

bool WeatherReader::Start() {
    if (running_) return true;

    // This Start() was the one that unwound correctly by hand -- Stop() now owns that unwinding for every
    // reader, which is why the hand-written version above it is gone (see platform/WorkerThread.h).
    if (!worker_.Prepare()) return false;

    HANDLE thread = CreateThread(nullptr, 0, &WeatherReader::ThreadEntry, this, 0, nullptr);
    if (!thread) {
        worker_.Stop();
        return false;
    }
    worker_.Attach(thread);

    running_ = true;
    return true;
}

void WeatherReader::Stop() {
    // The worker may be inside a network call, so the timeout is the 5000 ms the reader chose for itself.
    worker_.Stop(5000);
    running_ = false;
}

bool WeatherReader::Get(double* temperatureC, int* code, int* sunriseMinutes,
                        int* sunsetMinutes) const {
    if (!worker_.Prepared() || !haveReading_) return false;

    lock_.Enter();
    const bool have = haveReading_;
    if (have) {
        if (temperatureC) *temperatureC = temperatureC_;
        if (code) *code = code_;
        if (sunriseMinutes) *sunriseMinutes = sunriseMinutes_;
        if (sunsetMinutes) *sunsetMinutes = sunsetMinutes_;
    }
    lock_.Leave();
    return have;
}

void WeatherReader::Line(wchar_t* out, size_t capacity) const {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (!worker_.Prepared()) return;

    lock_.Enter();
    if (haveReading_) wcsncpy_s(out, capacity, line_, _TRUNCATE);
    lock_.Leave();
}

void WeatherReader::Pieces(wchar_t* sky, size_t skyCapacity, wchar_t* temperature,
                           size_t temperatureCapacity) const {
    if (sky && skyCapacity) sky[0] = L'\0';
    if (temperature && temperatureCapacity) temperature[0] = L'\0';
    if (!worker_.Prepared()) return;

    lock_.Enter();
    if (haveReading_) {
        WeatherPiecesFor(temperatureC_, code_, sky, skyCapacity, temperature, temperatureCapacity);
    }
    lock_.Leave();
}

unsigned long long WeatherReader::Revision() const {
    return static_cast<unsigned long long>(revision_);
}

unsigned long __stdcall WeatherReader::ThreadEntry(void* self) {
    static_cast<WeatherReader*>(self)->Run();
    return 0;
}

void WeatherReader::Run() {
    // The worker owns its own apartment, exactly as the media session's does and for the same
    // reason: a WinRT call has to be made from inside one, and the frame loop's apartment is not
    // this thread's. Entering MTA keeps the UI thread out of the location call entirely -- which
    // matters here, because asking can block for seconds while a provider is found.
    const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    const bool haveApartment = SUCCEEDED(apartment);

    char resolvedCity[128] = {};
    double latitude = 0.0;
    double longitude = 0.0;

    // The first pass runs immediately -- starting the bar must not mean waiting fifteen
    // minutes for the weather to appear -- and each pass says how long to wait for the next.
    unsigned waitMs = 0;
    while (true) {
        if (waitMs > 0) {
            if (WaitForSingleObject(worker_.StopEvent(), static_cast<DWORD>(waitMs)) == WAIT_OBJECT_0) break;
        }
        if (WaitForSingleObject(worker_.StopEvent(), 0) == WAIT_OBJECT_0) break;

        if (!haveApartment) {
            LogAndPrint(L"  weather: no Windows Runtime apartment -- location cannot be asked for; "
                        L"a city in weather.txt still works\n");
            std::fflush(stdout);
            return;
        }
        waitMs = Refresh(resolvedCity, sizeof(resolvedCity), &latitude, &longitude);
    }

    if (haveApartment) RoUninitialize();
}

unsigned WeatherReader::Refresh(char* resolvedCity, size_t resolvedCapacity, double* latitude,
                                double* longitude) {
    wchar_t city[128] = {};
    bool coordinatesGiven = false;
    const bool haveSetting = ReadSettings(city, 128, &coordinatesGiven);

    if (!haveSetting) {
        // No city in the file, so the machine's own position is the primary source -- which is
        // what the user asked for: "天气的能不能调用定位 而不是根据ip". Windows answers with WiFi or
        // satellite fixes, not with an IP lookup (measured on this machine: WiFi, 187 m).
        char reason[192] = {};
        if (!LocationFromWindows(latitude, longitude, reason, sizeof(reason))) {
            // Reported once per REASON rather than once per poll: a machine with location turned
            // off is a fact about the session, and a line every minute about it is noise in a log
            // that is read for the faults.
            static char reportedReason[192] = {};
            if (std::strcmp(reason, reportedReason) != 0) {
                strncpy_s(reportedReason, sizeof(reportedReason), reason, _TRUNCATE);
                wchar_t path[MAX_PATH] = {};
                SettingsPath(path, MAX_PATH);
                LogAndPrint(L"  weather: no position from Windows (%hs) -- %ls would pin one\n",
                            reason, path);
                std::fflush(stdout);
            }
            return kRetryMs;
        }

        // The reading is logged when the POSITION changes, because that is the event worth
        // knowing about: the machine moved (or the fix changed provider), and the weather that
        // follows belongs to somewhere else. Fifteen identical lines an hour say nothing.
        static double reportedLatitude = 0.0;
        static double reportedLongitude = 0.0;
        const bool moved = std::fabs(*latitude - reportedLatitude) > 0.0005 ||
                           std::fabs(*longitude - reportedLongitude) > 0.0005;
        if (moved) {
            reportedLatitude = *latitude;
            reportedLongitude = *longitude;
            LogAndPrint(L"  weather: position from Windows -> %.6f, %.6f\n", *latitude,
                        *longitude);
            std::fflush(stdout);
        }
        // Remembered so the forecast below is not asked for the same place twice in a row: the
        // poll interval is fifteen minutes and the position is stable to a few tens of metres.
        strncpy_s(resolvedCity, resolvedCapacity, "<windows-location>", _TRUNCATE);
    }

    // Reported once per CHANGE of city rather than once per poll: a city is a fact about the
    // session, and a line every fifteen minutes would be noise.
    static wchar_t reportedCity[128] = {};
    if (haveSetting && wcscmp(city, reportedCity) != 0) {
        wcsncpy_s(reportedCity, 128, city, _TRUNCATE);
        wchar_t path[MAX_PATH] = {};
        SettingsPath(path, MAX_PATH);
        LogAndPrint(L"  weather: city '%ls' from %ls\n", city, path);
        std::fflush(stdout);
    }

    // WHERE THE COORDINATES COME FROM, and there are exactly three answers.
    //
    // 1. A "lat,lon" line in weather.txt: an explicit choice, already parsed.
    // 2. A city name in weather.txt: geocoded, once per city rather than once per poll.
    // 3. Neither, and the position came from Windows -- already in hand, so nothing to do.
    char cityUtf8[256] = {};
    if (haveSetting && !coordinatesGiven) {
        if (WideCharToMultiByte(CP_UTF8, 0, city, -1, cityUtf8, sizeof(cityUtf8), nullptr,
                                nullptr) <= 0) {
            return kRetryMs;
        }
        cityUtf8[sizeof(cityUtf8) - 1] = '\0';

        if (std::strcmp(cityUtf8, resolvedCity) != 0) {
            char encoded[768] = {};
            PercentEncodeUtf8(cityUtf8, encoded, sizeof(encoded));

            wchar_t path[900] = {};
            swprintf(path, 900, L"/v1/search?name=%hs&count=1&language=zh&format=json", encoded);

            char body[8192] = {};
            size_t length = 0;
            DWORD status = 0;
            // No Referer for this service (null), and the reader's own ten-second timeout.
            if (!HttpGet(kGeocodeHost, path, nullptr, kTimeoutMs, body, sizeof(body), &length, &status)) {
                LogAndPrint(L"  weather: geocode failed (http %lu, %zu bytes)\n",
                            static_cast<unsigned long>(status), length);
                std::fflush(stdout);
                return kRetryMs;
            }
            if (!ParseGeocodeJson(body, latitude, longitude)) {
                LogAndPrint(L"  weather: no place called '%ls'\n", city);
                std::fflush(stdout);
                // Remembered as attempted, so a misspelt city is not asked for once a minute
                // for the rest of the session.
                strncpy_s(resolvedCity, resolvedCapacity, cityUtf8, _TRUNCATE);
                return kPollMs;
            }

            strncpy_s(resolvedCity, resolvedCapacity, cityUtf8, _TRUNCATE);
            LogAndPrint(L"  weather: %ls -> %.4f, %.4f\n", city, *latitude, *longitude);
            std::fflush(stdout);
        }
    } else if (haveSetting) {
        // "lat,lon" in the file. ReadSettings has already established that both halves are
        // numbers and that both are in range.
        const wchar_t* comma = wcschr(city, L',');
        if (!comma) return kRetryMs;
        *latitude = wcstod(city, nullptr);
        *longitude = wcstod(comma + 1, nullptr);
        strncpy_s(resolvedCity, resolvedCapacity, "<lat,lon>", _TRUNCATE);
    }

    wchar_t path[512] = {};
    swprintf(path, 512,
             L"/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code"
             L"&format=csv&timezone=auto",
             *latitude, *longitude);

    char body[4096] = {};
    size_t length = 0;
    DWORD status = 0;
    if (!HttpGet(kForecastHost, path, nullptr, kTimeoutMs, body, sizeof(body), &length, &status)) {
        LogAndPrint(L"  weather: forecast failed (http %lu, %zu bytes)\n",
                    static_cast<unsigned long>(status), length);
        std::fflush(stdout);
        return kRetryMs;
    }

    double temperature = 0.0;
    int code = -1;
    if (!ParseCurrentCsv(body, &temperature, &code)) {
        LogAndPrint(L"  weather: the forecast answer held no reading (%zu bytes)\n", length);
        std::fflush(stdout);
        return kRetryMs;
    }

    // THE SUNRISE AND THE SUNSET, for the island's ambient light. A second call, because Open-Meteo's
    // CSV puts the current block and the daily block on different time axes and only the daily line
    // carries the two times. A failure here leaves them at -1 and the ambient falls back to 06:00/18:00.
    int sunriseMinutes = -1;
    int sunsetMinutes = -1;
    {
        wchar_t dailyPath[512] = {};
        swprintf(dailyPath, 512,
                 L"/v1/forecast?latitude=%.4f&longitude=%.4f&daily=sunrise,sunset"
                 L"&format=csv&timezone=auto",
                 *latitude, *longitude);
        char dailyBody[4096] = {};
        size_t dailyLength = 0;
        DWORD dailyStatus = 0;
        if (HttpGet(kForecastHost, dailyPath, nullptr, kTimeoutMs, dailyBody, sizeof(dailyBody),
                    &dailyLength, &dailyStatus)) {
            ParseDailyCsv(dailyBody, &sunriseMinutes, &sunsetMinutes);
        }
    }

    wchar_t line[64] = {};
    WeatherLineFor(temperature, code, line, 64);

    lock_.Enter();
    temperatureC_ = temperature;
    code_ = code;
    sunriseMinutes_ = sunriseMinutes;
    sunsetMinutes_ = sunsetMinutes;
    wcsncpy_s(line_, 64, line, _TRUNCATE);
    haveReading_ = true;
    lock_.Leave();
    InterlockedIncrement(&revision_);

    LogAndPrint(L"  weather: %.1f C, code %d, sunrise %d, sunset %d -> %ls\n", temperature, code,
                sunriseMinutes, sunsetMinutes, line);
    std::fflush(stdout);
    return kPollMs;
}

} // namespace island
