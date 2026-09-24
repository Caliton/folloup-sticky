#ifndef EPAPER_UI_READER_PAGE_H_
#define EPAPER_UI_READER_PAGE_H_

#include <cstdint>
#include <string>

#include "design_tokens.h"
#include "epaper_ui/overlay_geometry.h"
#include "epaper_ui/status_bar.h"

namespace epaper_ui {

// Book reader: one page of pre-paginated text below the status bar, no footer.
//
// `page_text` holds the page's lines separated by '\n'. A line starting with '\x01' is a
// heading line (drawn bold); an empty line is a paragraph gap of ReaderParagraphGap().
// Pagination (in the reader runtime) must use the same metrics as drawing, so both go
// through the helpers below.
struct ReaderPageState {
    std::string page_text = {};
    int font_level = 1;             // 0 small, 1 medium, 2 large
    std::string footer_text = {};   // progress line, e.g. "Cap. 2/12 · 34%"
    std::string message_text = {};  // shown instead of the page (loading, errors)

    bool operator==(const ReaderPageState& other) const = default;
};

inline constexpr int kReaderFontLevels = 3;
inline constexpr char kReaderHeadingMarker = '\x01';

design::TypographyRole ReaderBodyRole(int font_level);
design::TypographyRole ReaderHeadingRole(int font_level);
int ReaderLineHeight(int font_level);
int ReaderParagraphGap(int font_level);
// Area the text is laid out in.
UiRect ReaderTextArea(int portrait_width, int portrait_height);

void DrawReaderPage(uint8_t* framebuffer,
                    int raw_width,
                    int raw_height,
                    int portrait_width,
                    int portrait_height,
                    const ReaderPageState& state,
                    const StatusBarState& status_bar_state);

}  // namespace epaper_ui

#endif  // EPAPER_UI_READER_PAGE_H_
