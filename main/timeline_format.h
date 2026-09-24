#ifndef TIMELINE_FORMAT_H_
#define TIMELINE_FORMAT_H_

#include <cstdint>
#include <ctime>
#include <string>

#include "recording_archive_service.h"

// Shared formatters for the recording timeline shown on the Notes, Todos, and Follow-up pages (and
// the sticky-note overlay, which reuses the same content shape).
namespace timeline_format {

// Extract the "YYYY-MM-DD" day key from a created_local_date that may be either "YYYY-MM-DD" or
// "YYYY-MM-DD HH:MM:SS", so recordings from the same day group under one date chip. This is the
// stable grouping identity -- it never changes as the calendar rolls over.
std::string DateKey(const std::string& created_local_date);

// pt-BR calendar names. newlib's strftime has no locale support, so %a/%A/%b always come out
// in English; every user-visible date goes through these instead. Indices follow std::tm.
const char* WeekdayShort(int tm_wday);  // "Seg"
const char* WeekdayLong(int tm_wday);   // "Segunda-feira"
const char* MonthShort(int tm_mon);     // "jul"
const char* MonthLong(int tm_mon);      // "julho"

// "Sex, 10 jul"
std::string FormatShortDate(const std::tm& local);
// "10 de julho de 2026"
std::string FormatLongDate(const std::tm& local);
// 24-hour "14:05"
std::string FormatClock(const std::tm& local);

// Human label for a recording's day: "Hoje" while its day matches the current local date,
// "Ontem" for the day before, otherwise FormatShortDate (e.g. "Sex, 10 jul"). Because the
// comparison is against the live current date, labels roll over at midnight. Empty dates fall
// back to "Hoje"; unparseable ones are returned as-is.
std::string FormatDateLabel(const std::string& created_local_date);

// 24-hour clock time for a recording's header when the timestamp is valid, otherwise "--:--".
std::string FormatTimeLabel(bool time_valid, int64_t created_unix_seconds);

// Compact duration: "<N>s" under a minute, otherwise "<N>min".
std::string FormatDurationLabel(uint32_t duration_ms);

// Trim leading/trailing whitespace from a transcript (empty when all whitespace).
std::string TrimTranscript(const std::string& text);

// Human tag label: kTask -> "Tarefa", kIdea -> "Ideia", otherwise "Nota".
std::string TagText(recording_archive_service::RecordingTag tag);

}  // namespace timeline_format

#endif  // TIMELINE_FORMAT_H_
