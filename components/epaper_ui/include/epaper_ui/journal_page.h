#ifndef EPAPER_UI_JOURNAL_PAGE_H_
#define EPAPER_UI_JOURNAL_PAGE_H_

#include <cstdint>
#include <string>

#include "epaper_ui/global_footer.h"
#include "epaper_ui/segment_control.h"
#include "epaper_ui/status_bar.h"
#include "epaper_ui/timeline_list.h"

namespace epaper_ui {

// The bullet-journal page ("Diário"): a level switcher (Ano / Mês / Semana / Dia) over a
// timeline whose groups are the periods of the selected level.
struct JournalPageState {
    int navigation_focus_index = -1;
    std::string title_text = "Diário";
    // The period on screen, e.g. "Semana 40 · 28 set - 4 out".
    std::string subtitle_text = {};
    SegmentControlState segment_control = {};
    TimelineListState timeline = {};

    bool operator==(const JournalPageState& other) const = default;
};

void DrawJournalPage(uint8_t* framebuffer,
                     int raw_width,
                     int raw_height,
                     int portrait_width,
                     int portrait_height,
                     const JournalPageState& state,
                     const StatusBarState& status_bar_state,
                     const GlobalFooterState& footer_state);

}  // namespace epaper_ui

#endif  // EPAPER_UI_JOURNAL_PAGE_H_
