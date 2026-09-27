// Host-side checks for journal_period (no ESP-IDF). Build and run with scripts/test_journal_host.py.
#include <cstdio>
#include <string>

#include "journal_period.h"

using namespace journal_period;

namespace {

int s_failures = 0;

void Expect(bool condition, const char* what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++s_failures;
    }
}

void ExpectEq(const std::string& actual, const std::string& expected, const char* what)
{
    if (actual != expected) {
        std::printf("FAIL: %s: got \"%s\", want \"%s\"\n", what, actual.c_str(), expected.c_str());
        ++s_failures;
    }
}

}  // namespace

int main()
{
    // ISO weeks around year boundaries.
    ExpectEq(WeekKey({2026, 12, 31}), "2026-W53", "2026-12-31");
    ExpectEq(WeekKey({2027, 1, 1}), "2026-W53", "2027-01-01");
    ExpectEq(WeekKey({2027, 1, 4}), "2027-W01", "2027-01-04");
    ExpectEq(WeekKey({2025, 12, 29}), "2026-W01", "2025-12-29");
    ExpectEq(WeekKey({2026, 9, 26}), "2026-W39", "2026-09-26 (Saturday)");
    ExpectEq(WeekKey({2026, 9, 28}), "2026-W40", "2026-09-28 (Monday)");
    Expect(Weekday({2026, 9, 26}) == 5, "2026-09-26 is a Saturday");

    // Parsing / levels.
    Expect(LevelOf("2026") == Level::kYear, "year level");
    Expect(LevelOf("2026-10") == Level::kMonth, "month level");
    Expect(LevelOf("2026-W40") == Level::kWeek, "week level");
    Expect(LevelOf("2026-10-03") == Level::kDay, "day level");
    Expect(LevelOf("2026-W54") == Level::kNone, "week 54 invalid");
    Expect(LevelOf("2027-W53") == Level::kNone, "2027 has 52 weeks");
    Expect(LevelOf("2026-W53") == Level::kWeek, "2026 has 53 weeks");
    Expect(LevelOf("2026-02-29") == Level::kNone, "2026-02-29 invalid");
    Expect(LevelOf("2028-02-29") == Level::kDay, "2028-02-29 valid");
    Expect(LevelOf("") == Level::kNone, "empty invalid");
    Expect(LevelOf("abcd") == Level::kNone, "garbage invalid");

    // Ranges.
    Date first = {};
    Date last = {};
    Expect(Range("2026-W53", &first, &last), "range W53");
    Expect(first == Date{2026, 12, 28} && last == Date{2027, 1, 3}, "W53 spans 28 dec - 3 jan");
    Expect(Range("2026-W01", &first, &last), "range W01");
    Expect(first == Date{2025, 12, 29}, "2026-W01 starts 2025-12-29");

    // Parents / next.
    ExpectEq(ParentOf("2026-10-03"), "2026-W40", "parent of day");
    ExpectEq(ParentOf("2026-W40"), "2026-10", "parent of week (Thursday rule)");
    ExpectEq(ParentOf("2026-W53"), "2026-12", "parent of W53");
    ExpectEq(ParentOf("2026-10"), "2026", "parent of month");
    ExpectEq(ParentOf("2026"), "", "parent of year");
    ExpectEq(NextOf("2026-12-31"), "2027-01-01", "next day");
    ExpectEq(NextOf("2026-W53"), "2027-W01", "next week");
    ExpectEq(NextOf("2026-12"), "2027-01", "next month");
    ExpectEq(NextOf("2026"), "2027", "next year");

    // Containment / ended.
    Expect(Contains("2026-10", "2026-10-03"), "month contains day");
    Expect(Contains("2026", "2026-W40"), "year contains week");
    Expect(!Contains("2026-10", "2026-W40"), "week 40 starts in September");
    const Date today = {2026, 9, 26};
    Expect(HasEnded("2026-09-25", today), "yesterday ended");
    Expect(!HasEnded("2026-09-26", today), "today not ended");
    Expect(HasEnded("2026-W38", today), "last week ended");
    Expect(!HasEnded("2026-W39", today), "this week not ended");
    Expect(HasEnded("2026-08", today), "last month ended");
    Expect(!HasEnded("2026", today), "this year not ended");

    // Labels.
    ExpectEq(Label("2026-09-26", today), "Hoje", "label today");
    ExpectEq(Label("2026-09-27", today), "Amanhã", "label tomorrow");
    ExpectEq(Label("2026-10-03", today), "sáb, 3 out", "label day");
    ExpectEq(Label("2026-W40", today), "Semana 40", "label week");
    ExpectEq(Label("2026-10", today), "Outubro", "label month");
    ExpectEq(Label("2027-01", today), "Janeiro 2027", "label month other year");
    ExpectEq(ShortLabel("2026-W40", today), "S40", "short week");
    ExpectEq(ShortLabel("2026-10-03", today), "3 out", "short day");
    ExpectEq(WeekRangeLabel("2026-W40"), "28 set - 4 out", "week range across months");
    ExpectEq(WeekRangeLabel("2026-W41"), "5 - 11 out", "week range same month");

    if (s_failures == 0) {
        std::printf("journal_period: all checks passed\n");
        return 0;
    }
    std::printf("journal_period: %d failure(s)\n", s_failures);
    return 1;
}
