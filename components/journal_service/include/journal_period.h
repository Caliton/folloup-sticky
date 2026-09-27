#ifndef JOURNAL_PERIOD_H_
#define JOURNAL_PERIOD_H_

#include <cstdint>
#include <string>

// Bullet-journal period keys shared with the web app (see docs/journal.md):
//   year "2026", month "2026-10", ISO week "2026-W40", day "2026-10-03".
// Pure calendar math with no ESP-IDF dependency, so it also builds on the host for tests.
namespace journal_period {

enum class Level : uint8_t {
    kNone = 0,
    kYear,
    kMonth,
    kWeek,
    kDay,
};

struct Date {
    int year = 0;
    int month = 0;  // 1..12
    int day = 0;    // 1..31

    bool operator==(const Date& other) const = default;
};

// Today's local date (TZ already applied by the timezone service). False while the clock has
// never been set, so callers can avoid filing items under 1970.
bool Today(Date* today);
Date FromUnixSeconds(int64_t unix_seconds);

bool IsValid(const Date& date);
int DaysInMonth(int year, int month);
Date AddDays(const Date& date, int days);
int DaysBetween(const Date& from, const Date& to);  // to - from
// 0 = Monday .. 6 = Sunday.
int Weekday(const Date& date);
bool Before(const Date& a, const Date& b);

std::string DayKey(const Date& date);
std::string WeekKey(const Date& date);
std::string MonthKey(const Date& date);
std::string YearKey(const Date& date);
std::string KeyFor(Level level, const Date& date);

Level LevelOf(const std::string& key);
bool IsValidKey(const std::string& key);
// First and last day covered by `key`.
bool Range(const std::string& key, Date* first, Date* last);
bool Contains(const std::string& outer, const std::string& inner);
// day -> its ISO week, week -> month of its Thursday, month -> year, year -> "".
std::string ParentOf(const std::string& key);
// The same-level period right after `key` (tomorrow, next week, ...).
std::string NextOf(const std::string& key);
// The period's last day is before `today`.
bool HasEnded(const std::string& key, const Date& today);
// ISO week number of `date` (1..53) and its week-based year.
void IsoWeek(const Date& date, int* week_year, int* week);

// pt-BR labels for the e-paper UI.
// "Hoje", "Amanhã", "Ontem", "sáb, 3 out", "Semana 40", "Outubro", "Outubro 2027", "2026".
std::string Label(const std::string& key, const Date& today);
// Compact form for migration marks: "hoje", "3 out", "S40", "out", "2027".
std::string ShortLabel(const std::string& key, const Date& today);
// "28 set - 4 out" for a week key (the e-paper fonts are Latin-1 only: no en dash), "" otherwise.
std::string WeekRangeLabel(const std::string& key);
const char* MonthName(int month);       // "Outubro"
const char* MonthShortName(int month);  // "out"
const char* WeekdayShortName(int weekday);  // "seg".."dom"

}  // namespace journal_period

#endif  // JOURNAL_PERIOD_H_
