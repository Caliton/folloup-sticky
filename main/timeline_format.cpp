#include "timeline_format.h"

#include <cstdio>
#include <ctime>

#include "timezone_service.h"

namespace timeline_format {

using recording_archive_service::RecordingTag;

namespace {

constexpr const char* kWeekdayShort[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"};
constexpr const char* kWeekdayLong[] = {"Domingo",      "Segunda-feira", "Terça-feira",
                                        "Quarta-feira", "Quinta-feira",  "Sexta-feira",
                                        "Sábado"};
constexpr const char* kMonthShort[] = {"jan", "fev", "mar", "abr", "mai", "jun",
                                       "jul", "ago", "set", "out", "nov", "dez"};
constexpr const char* kMonthLong[] = {"janeiro", "fevereiro", "março",    "abril",
                                      "maio",    "junho",     "julho",    "agosto",
                                      "setembro", "outubro",  "novembro", "dezembro"};

template <size_t N>
const char* Pick(const char* const (&names)[N], int index)
{
    return (index >= 0 && static_cast<size_t>(index) < N) ? names[index] : "";
}

// "YYYY-MM-DD" for the local day `offset_days` from now; empty when the clock is unset.
std::string LocalDayKey(int offset_days)
{
    const std::string today = timezone_service::GetSnapshot().runtime.current_date;
    if (offset_days == 0 || today.empty()) {
        return offset_days == 0 ? today : std::string{};
    }
    int year = 0;
    int month = 0;
    int day = 0;
    if (std::sscanf(today.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
        return {};
    }
    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day + offset_days;
    tm.tm_hour = 12;  // midday keeps DST transitions from shifting the day
    if (std::mktime(&tm) == static_cast<std::time_t>(-1)) {
        return {};
    }
    char buffer[40] = {};  // sized for any int, which -Wformat-truncation assumes
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday);
    return buffer;
}

}  // namespace

const char* WeekdayShort(int tm_wday)
{
    return Pick(kWeekdayShort, tm_wday);
}

const char* WeekdayLong(int tm_wday)
{
    return Pick(kWeekdayLong, tm_wday);
}

const char* MonthShort(int tm_mon)
{
    return Pick(kMonthShort, tm_mon);
}

const char* MonthLong(int tm_mon)
{
    return Pick(kMonthLong, tm_mon);
}

std::string FormatShortDate(const std::tm& local)
{
    return std::string(WeekdayShort(local.tm_wday)) + ", " + std::to_string(local.tm_mday) + " " +
           MonthShort(local.tm_mon);
}

std::string FormatLongDate(const std::tm& local)
{
    return std::to_string(local.tm_mday) + " de " + MonthLong(local.tm_mon) + " de " +
           std::to_string(local.tm_year + 1900);
}

std::string FormatClock(const std::tm& local)
{
    char buffer[24] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", local.tm_hour, local.tm_min);
    return buffer;
}

std::string DateKey(const std::string& created_local_date)
{
    const auto space = created_local_date.find(' ');
    return space == std::string::npos ? created_local_date : created_local_date.substr(0, space);
}

std::string FormatDateLabel(const std::string& created_local_date)
{
    const std::string day_key = DateKey(created_local_date);
    if (day_key.empty()) {
        return "Hoje";
    }

    // GetSnapshot().current_date is the live "YYYY-MM-DD", so these relabel at midnight.
    if (day_key == LocalDayKey(0)) {
        return "Hoje";
    }
    if (day_key == LocalDayKey(-1)) {
        return "Ontem";
    }

    int year = 0;
    int month = 0;
    int day = 0;
    if (std::sscanf(day_key.c_str(), "%d-%d-%d", &year, &month, &day) == 3) {
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = 12;
        if (std::mktime(&tm) != static_cast<std::time_t>(-1)) {
            return FormatShortDate(tm);
        }
    }
    return created_local_date;
}

std::string FormatTimeLabel(bool time_valid, int64_t created_unix_seconds)
{
    if (time_valid && created_unix_seconds > 0) {
        std::time_t stamp = static_cast<std::time_t>(created_unix_seconds);
        std::tm local = {};
        localtime_r(&stamp, &local);
        return FormatClock(local);
    }
    return "--:--";
}

std::string FormatDurationLabel(uint32_t duration_ms)
{
    const uint32_t seconds = duration_ms / 1000U;
    if (seconds < 60U) {
        return std::to_string(seconds) + "s";
    }
    return std::to_string(seconds / 60U) + "min";
}

std::string TrimTranscript(const std::string& text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string TagText(RecordingTag tag)
{
    switch (tag) {
        case RecordingTag::kTask:
            return "Tarefa";
        case RecordingTag::kIdea:
            return "Ideia";
        case RecordingTag::kNote:
        default:
            return "Nota";
    }
}

}  // namespace timeline_format
