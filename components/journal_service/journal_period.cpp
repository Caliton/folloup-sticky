#include "journal_period.h"

#include <cstdio>
#include <ctime>

namespace journal_period {
namespace {

constexpr int64_t kMinValidEpoch = 1704067200;  // 2024-01-01 UTC

constexpr const char* kMonthNames[12] = {
    "Janeiro", "Fevereiro", "Março",    "Abril",   "Maio",     "Junho",
    "Julho",   "Agosto",    "Setembro", "Outubro", "Novembro", "Dezembro",
};
constexpr const char* kMonthShortNames[12] = {
    "jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez",
};
constexpr const char* kWeekdayShortNames[7] = {"seg", "ter", "qua", "qui", "sex", "sáb", "dom"};

// Days since 1970-01-01 (H. Hinnant's days_from_civil).
int64_t DaysFromCivil(int year, int month, int day)
{
    year -= month <= 2 ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

Date CivilFromDays(int64_t z)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    return {year, month, day};
}

int64_t ToDays(const Date& date)
{
    return DaysFromCivil(date.year, date.month, date.day);
}

// Parses exactly `count` decimal digits at `text[offset]`.
bool ParseDigits(const std::string& text, size_t offset, size_t count, int* value)
{
    if (offset + count > text.size()) {
        return false;
    }
    int result = 0;
    for (size_t index = offset; index < offset + count; ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') {
            return false;
        }
        result = (result * 10) + (c - '0');
    }
    *value = result;
    return true;
}

Date MondayOfIsoWeek(int week_year, int week)
{
    const Date jan4 = {week_year, 1, 4};
    const Date week1_monday = AddDays(jan4, -Weekday(jan4));
    return AddDays(week1_monday, (week - 1) * 7);
}

int IsoWeeksInYear(int week_year)
{
    int ignored_year = 0;
    int week = 0;
    IsoWeek({week_year, 12, 28}, &ignored_year, &week);
    return week;
}

struct ParsedKey {
    Level level = Level::kNone;
    int year = 0;
    int month = 0;
    int day = 0;
    int week = 0;
};

ParsedKey Parse(const std::string& key)
{
    ParsedKey parsed = {};
    int year = 0;
    if (!ParseDigits(key, 0, 4, &year) || year < 1970) {
        return {};
    }
    parsed.year = year;
    if (key.size() == 4) {
        parsed.level = Level::kYear;
        return parsed;
    }
    if (key.size() == 8 && key[4] == '-' && key[5] == 'W') {
        int week = 0;
        if (!ParseDigits(key, 6, 2, &week) || week < 1 || week > IsoWeeksInYear(year)) {
            return {};
        }
        parsed.level = Level::kWeek;
        parsed.week = week;
        return parsed;
    }
    int month = 0;
    if (key.size() < 7 || key[4] != '-' || !ParseDigits(key, 5, 2, &month) || month < 1 ||
        month > 12) {
        return {};
    }
    parsed.month = month;
    if (key.size() == 7) {
        parsed.level = Level::kMonth;
        return parsed;
    }
    int day = 0;
    if (key.size() != 10 || key[7] != '-' || !ParseDigits(key, 8, 2, &day) || day < 1 ||
        day > DaysInMonth(year, month)) {
        return {};
    }
    parsed.day = day;
    parsed.level = Level::kDay;
    return parsed;
}

// localtime_r is POSIX; the Windows host test build has localtime_s instead.
void ToLocal(time_t value, struct tm* local)
{
#if defined(_WIN32)
    localtime_s(local, &value);
#else
    localtime_r(&value, local);
#endif
}

std::string DayMonthText(const Date& date)
{
    return std::to_string(date.day) + " " + MonthShortName(date.month);
}

}  // namespace

bool Today(Date* today)
{
    const time_t now = time(nullptr);
    if (static_cast<int64_t>(now) < kMinValidEpoch) {
        return false;
    }
    struct tm local = {};
    ToLocal(now, &local);
    if (today != nullptr) {
        *today = {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
    }
    return true;
}

Date FromUnixSeconds(int64_t unix_seconds)
{
    const time_t value = static_cast<time_t>(unix_seconds);
    struct tm local = {};
    ToLocal(value, &local);
    return {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
}

bool IsValid(const Date& date)
{
    return date.year >= 1970 && date.month >= 1 && date.month <= 12 && date.day >= 1 &&
           date.day <= DaysInMonth(date.year, date.month);
}

int DaysInMonth(int year, int month)
{
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[month - 1];
}

Date AddDays(const Date& date, int days)
{
    return CivilFromDays(ToDays(date) + days);
}

int DaysBetween(const Date& from, const Date& to)
{
    return static_cast<int>(ToDays(to) - ToDays(from));
}

int Weekday(const Date& date)
{
    // 1970-01-01 was a Thursday (index 3 with Monday = 0).
    const int64_t days = ToDays(date);
    const int64_t weekday = (days + 3) % 7;
    return static_cast<int>(weekday < 0 ? weekday + 7 : weekday);
}

bool Before(const Date& a, const Date& b)
{
    return ToDays(a) < ToDays(b);
}

void IsoWeek(const Date& date, int* week_year, int* week)
{
    const Date thursday = AddDays(date, 3 - Weekday(date));
    const int day_of_year = DaysBetween({thursday.year, 1, 1}, thursday) + 1;
    if (week_year != nullptr) {
        *week_year = thursday.year;
    }
    if (week != nullptr) {
        *week = ((day_of_year - 1) / 7) + 1;
    }
}

std::string DayKey(const Date& date)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date.year, date.month, date.day);
    return buffer;
}

std::string WeekKey(const Date& date)
{
    int week_year = 0;
    int week = 0;
    IsoWeek(date, &week_year, &week);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-W%02d", week_year, week);
    return buffer;
}

std::string MonthKey(const Date& date)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d", date.year, date.month);
    return buffer;
}

std::string YearKey(const Date& date)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d", date.year);
    return buffer;
}

std::string KeyFor(Level level, const Date& date)
{
    switch (level) {
        case Level::kYear:
            return YearKey(date);
        case Level::kMonth:
            return MonthKey(date);
        case Level::kWeek:
            return WeekKey(date);
        case Level::kDay:
            return DayKey(date);
        case Level::kNone:
        default:
            return {};
    }
}

Level LevelOf(const std::string& key)
{
    return Parse(key).level;
}

bool IsValidKey(const std::string& key)
{
    return LevelOf(key) != Level::kNone;
}

bool Range(const std::string& key, Date* first, Date* last)
{
    const ParsedKey parsed = Parse(key);
    Date start = {};
    Date end = {};
    switch (parsed.level) {
        case Level::kYear:
            start = {parsed.year, 1, 1};
            end = {parsed.year, 12, 31};
            break;
        case Level::kMonth:
            start = {parsed.year, parsed.month, 1};
            end = {parsed.year, parsed.month, DaysInMonth(parsed.year, parsed.month)};
            break;
        case Level::kWeek:
            start = MondayOfIsoWeek(parsed.year, parsed.week);
            end = AddDays(start, 6);
            break;
        case Level::kDay:
            start = {parsed.year, parsed.month, parsed.day};
            end = start;
            break;
        case Level::kNone:
        default:
            return false;
    }
    if (first != nullptr) {
        *first = start;
    }
    if (last != nullptr) {
        *last = end;
    }
    return true;
}

bool Contains(const std::string& outer, const std::string& inner)
{
    Date outer_first = {};
    Date outer_last = {};
    Date inner_first = {};
    Date inner_last = {};
    if (!Range(outer, &outer_first, &outer_last) || !Range(inner, &inner_first, &inner_last)) {
        return false;
    }
    return !Before(inner_first, outer_first) && !Before(outer_last, inner_last);
}

std::string ParentOf(const std::string& key)
{
    Date first = {};
    if (!Range(key, &first, nullptr)) {
        return {};
    }
    switch (LevelOf(key)) {
        case Level::kDay:
            return WeekKey(first);
        case Level::kWeek:
            return MonthKey(AddDays(first, 3));  // the month of its Thursday
        case Level::kMonth:
            return YearKey(first);
        case Level::kYear:
        case Level::kNone:
        default:
            return {};
    }
}

std::string NextOf(const std::string& key)
{
    const ParsedKey parsed = Parse(key);
    switch (parsed.level) {
        case Level::kDay:
            return DayKey(AddDays({parsed.year, parsed.month, parsed.day}, 1));
        case Level::kWeek:
            return WeekKey(AddDays(MondayOfIsoWeek(parsed.year, parsed.week), 7));
        case Level::kMonth:
            return parsed.month == 12 ? MonthKey({parsed.year + 1, 1, 1})
                                      : MonthKey({parsed.year, parsed.month + 1, 1});
        case Level::kYear:
            return YearKey({parsed.year + 1, 1, 1});
        case Level::kNone:
        default:
            return {};
    }
}

bool HasEnded(const std::string& key, const Date& today)
{
    Date last = {};
    return Range(key, nullptr, &last) && Before(last, today);
}

std::string Label(const std::string& key, const Date& today)
{
    const ParsedKey parsed = Parse(key);
    switch (parsed.level) {
        case Level::kDay: {
            const Date date = {parsed.year, parsed.month, parsed.day};
            const int delta = DaysBetween(today, date);
            if (delta == 0) {
                return "Hoje";
            }
            if (delta == 1) {
                return "Amanhã";
            }
            if (delta == -1) {
                return "Ontem";
            }
            std::string label = std::string(WeekdayShortName(Weekday(date))) + ", " +
                                DayMonthText(date);
            if (date.year != today.year) {
                label += " " + std::to_string(date.year);
            }
            return label;
        }
        case Level::kWeek: {
            int today_week_year = 0;
            IsoWeek(today, &today_week_year, nullptr);
            std::string label = "Semana " + std::to_string(parsed.week);
            if (parsed.year != today_week_year) {
                label += "/" + std::to_string(parsed.year);
            }
            return label;
        }
        case Level::kMonth: {
            std::string label = MonthName(parsed.month);
            if (parsed.year != today.year) {
                label += " " + std::to_string(parsed.year);
            }
            return label;
        }
        case Level::kYear:
            return std::to_string(parsed.year);
        case Level::kNone:
        default:
            return key;
    }
}

std::string ShortLabel(const std::string& key, const Date& today)
{
    const ParsedKey parsed = Parse(key);
    switch (parsed.level) {
        case Level::kDay: {
            const Date date = {parsed.year, parsed.month, parsed.day};
            const int delta = DaysBetween(today, date);
            if (delta == 0) {
                return "hoje";
            }
            if (delta == 1) {
                return "amanhã";
            }
            return DayMonthText(date);
        }
        case Level::kWeek:
            return "S" + std::to_string(parsed.week);
        case Level::kMonth: {
            std::string label = MonthShortName(parsed.month);
            if (parsed.year != today.year) {
                label += "/" + std::to_string(parsed.year % 100);
            }
            return label;
        }
        case Level::kYear:
            return std::to_string(parsed.year);
        case Level::kNone:
        default:
            return key;
    }
}

std::string WeekRangeLabel(const std::string& key)
{
    if (LevelOf(key) != Level::kWeek) {
        return {};
    }
    Date first = {};
    Date last = {};
    Range(key, &first, &last);
    if (first.month == last.month) {
        return std::to_string(first.day) + " - " + DayMonthText(last);
    }
    return DayMonthText(first) + " - " + DayMonthText(last);
}

const char* MonthName(int month)
{
    return month >= 1 && month <= 12 ? kMonthNames[month - 1] : "";
}

const char* MonthShortName(int month)
{
    return month >= 1 && month <= 12 ? kMonthShortNames[month - 1] : "";
}

const char* WeekdayShortName(int weekday)
{
    return weekday >= 0 && weekday <= 6 ? kWeekdayShortNames[weekday] : "";
}

}  // namespace journal_period
