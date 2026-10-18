// =====================================================================
//  Weather.h
//  One reading for one city: the temperature and the sky, in the words the
//  status strip prints.
//
//  WHY IT EXISTS. The island's own line is the clock when the bar is idle, and
//  something else -- the lyric, a message -- when it is not. So for as long as
//  music plays or a message is up, the bar shows no time at all, and the request
//  was that the date, the weekday, the time and the weather move into the empty
//  band to the left of the system button while that is the case.
//
//  WHERE THE POSITION COMES FROM, and the order is deliberate:
//
//   1. a line in %USERPROFILE%\IslandBar\weather.txt -- an explicit choice by the
//      user, so it wins outright. Either a city name or "lat,lon"; the file is
//      described at ReadSettings.
//   2. WINDOWS' OWN LOCATION SERVICE, when the file says nothing. This is the
//      primary source, asked for in as many words ("天气的能不能调用定位 而不是根据
//      ip"): Windows uses WiFi access points and satellites, and on this machine
//      it answers with the right city to 187 m.
//   3. nothing, which is reported with a reason and retried.
//
//  AN IP LOOKUP IS DELIBERATELY NOT USED, and the measurement is why: the keyless
//  IP services answer "Tokyo" on this machine while its clock runs on
//  Asia/Shanghai, because the traffic leaves through a proxy. Wrong weather at the
//  right time is worse than no weather.
//
//  FROM COORDINATES TO A READING is two HTTPS GETs to Open-Meteo, both keyless: the
//  forecast API takes coordinates and answers the current block as CSV, and a city
//  name (the file's other form) goes through Open-Meteo's geocoder first. CSV so
//  the numbers arrive on a line of their own and no JSON reader is needed for them.
//
//  NOTHING HERE TOUCHES THE FRAME. The fetch runs on a worker and the frame loop
//  only reads the last published reading -- the same split the lyrics take, and
//  for the same reason: a slow or unreachable service must not be able to stall
//  the animation. A reading that has stopped being refreshed is still shown,
//  which is what a weather widget does.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>

#include "platform/CriticalSection.h"
#include "platform/WorkerThread.h"

namespace island {

// The WMO weather code, as the one word the strip has room for.
//
// A pure function of the code, so the mapping can be asserted: Open-Meteo
// answers in WMO codes (0 clear, 61 rain, 95 thunderstorm ...), and a strip that
// printed "code 61" would be a strip nobody reads. An unknown code prints the
// temperature without a word rather than inventing one.
//
// Chinese characters are \u escapes for the reason TextRenderer's weekday table
// gives: they survive any editor and any code page.
const wchar_t* WeatherWordForCode(int code);

// What the sky is DOING, for the mark the status line draws before the word.
//
// WHY THIS EXISTS: the line drew a cloud with rain under it, always -- whatever the code was -- so a
// clear night was shown as 晴 beside a rain cloud. Reported as "天气这个图标对应错了 现在显示晴但是下雨的".
// The word and the mark are two answers to the same question and they were not even being asked the same
// thing; this makes the mark a function of the same code the word comes from, and the self-test checks the
// two tables against each other.
enum class SkyMark : int {
    None,      // an unknown code: no word, and no mark to contradict it
    Sun,       // 0, 1
    SunCloud,  // 2
    Cloud,     // 3
    Fog,       // 45, 48
    Rain,      // 51-57, 61-67, 80-82
    Snow,      // 71-77, 85, 86
    Thunder,   // 95-99
};

SkyMark SkyMarkForCode(int code);

// The reading as it is printed: the word, then the temperature, e.g. "\u6674 21\u00b0".
//
// A free function over its inputs so the self-test can check the rounding and the
// missing-code case without a network, a worker or a window.
void WeatherLineFor(double temperatureC, int code, wchar_t* out, size_t capacity);

// The same reading in the two pieces the status line draws separately.
//
// The line gives them their own sizes and weights and a gap between them, and puts a glyph before
// the word -- a single joined string could not be laid out that way. Either piece may come back
// empty, which is what the line draws when there is no reading.
void WeatherPiecesFor(double temperatureC, int code, wchar_t* sky, size_t skyCapacity,
                      wchar_t* temperature, size_t temperatureCapacity);

// --- the wire formats -------------------------------------------------

// Percent-encodes a UTF-8 string for use as a query value. Everything outside
// [A-Za-z0-9-._~] is escaped, which is what makes a Chinese city name a legal URL.
// Returns the number of bytes written, excluding the terminator.
size_t PercentEncodeUtf8(const char* text, char* out, size_t capacity);

// The reading out of Open-Meteo's CSV answer to
// `...&current=temperature_2m,weather_code&format=csv`.
//
// The document is three or four lines: a header block with the coordinates, a
// blank line, a header naming the columns, and the reading. The LAST non-empty
// line is the reading, and it is "time,temperature,code" -- by position, not by
// the header text, because the header carries units in parentheses and is not
// worth parsing.
bool ParseCurrentCsv(const char* csv, double* temperatureC, int* weatherCode);

// The sunrise and the sunset out of Open-Meteo's CSV answer to
// `...&daily=sunrise,sunset&format=csv&timezone=auto`: the last data line is
// "date,sunrise,sunset", the two times ISO ("2026-10-18T06:32"). Answers minutes since midnight in
// the site's own time zone, or false (both set to -1) when there is no such line.
bool ParseDailyCsv(const char* csv, int* sunriseMinutes, int* sunsetMinutes);

// The first result's coordinates out of Open-Meteo's geocoding answer.
//
// Scanned rather than parsed: the document is
// `{"results":[{"name":...,"latitude":39.9,"longitude":116.4,...}],...}`, the
// values wanted are the first `latitude`/`longitude` in it, and a nested-object
// JSON reader is a larger thing than this needs. A search that matched nothing
// answers `{}`, which has neither field, and is reported as "no such city".
bool ParseGeocodeJson(const char* json, double* latitude, double* longitude);

// --- the worker -------------------------------------------------------

// Polls for one city's weather and holds the last reading.
//
// Polling rather than being asked: the reading changes on the weather's own
// schedule (Open-Meteo publishes a current block every 15 minutes), so there is
// nothing for a caller to trigger it with.
class WeatherReader {
public:
    WeatherReader() = default;
    ~WeatherReader();

    WeatherReader(const WeatherReader&) = delete;
    WeatherReader& operator=(const WeatherReader&) = delete;

    // Starts the worker. False only when the thread cannot be created; a missing
    // or unreadable settings file is not a failure, it is a city nobody named
    // yet, and it is logged as such.
    bool Start();
    void Stop();

    // The last published reading, or false when there is none. A copy, taken
    // under the worker's lock, because the reading is two numbers and a string.
    // The sunrise/sunset are minutes since local midnight, or -1 until the daily answer arrives.
    bool Get(double* temperatureC, int* code, int* sunriseMinutes = nullptr,
             int* sunsetMinutes = nullptr) const;

    // The reading as the strip prints it, e.g. "\u6674 21\u00b0". Empty when
    // nothing has been fetched yet, which is how the strip knows to show the date
    // and the time without it.
    void Line(wchar_t* out, size_t capacity) const;

    // The reading in the two pieces the status line sets apart: the word, and the temperature.
    void Pieces(wchar_t* sky, size_t skyCapacity, wchar_t* temperature,
                size_t temperatureCapacity) const;

    // Bumped when a reading is published, so the frame loop can redraw once.
    unsigned long long Revision() const;

    // How often the worker asks again. 15 minutes matches the interval
    // Open-Meteo republishes the current block at; a failure is retried sooner,
    // because an unreachable service is usually a moment rather than an hour.
    static constexpr unsigned kPollMs  = 15u * 60u * 1000u;
    static constexpr unsigned kRetryMs = 60u * 1000u;

private:
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();

    // One cycle: read the settings, resolve the city if it changed, fetch, publish.
    // Returns how long to wait before the next one. `resolvedCity`/`latitude`/
    // `longitude` are the worker's own cache of the geocoding step -- a city is
    // geocoded once, not once per poll.
    unsigned Refresh(char* resolvedCity, size_t resolvedCapacity, double* latitude,
                     double* longitude);

    // The stop event, the thread and the rule for ending both. See platform/WorkerThread.h.
    WorkerThread  worker_;
    volatile bool running_ = false;

    mutable CriticalSection lock_;

    bool           haveReading_ = false;
    double         temperatureC_ = 0.0;
    int            code_ = -1;
    int            sunriseMinutes_ = -1;
    int            sunsetMinutes_ = -1;
    wchar_t        line_[64] = {};
    volatile long  revision_ = 0;
};

} // namespace island
