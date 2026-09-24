#ifndef EPAPER_UI_TEXT_LAYOUT_H_
#define EPAPER_UI_TEXT_LAYOUT_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "design_tokens.h"

namespace epaper_ui {

// Byte span of one wrapped line inside the paragraph passed to WrapParagraph.
struct LineSpan {
    uint32_t offset = 0;
    uint32_t length = 0;
};

// Greedy word wrap of a single paragraph (no '\n' handling) to `max_width` pixels in `role`.
// Words wider than a line are split at UTF-8 boundaries. Runs in O(n): glyph advances are
// additive (no kerning, zero tracking), so each word is measured once. `emit` is called for
// every line in order and returns false to stop early.
template <typename Emit>
void WrapParagraph(design::TypographyRole role, std::string_view text, int max_width, Emit emit);

// Implementation detail of WrapParagraph, exposed for the template.
int MeasureWord(design::TypographyRole role, std::string_view word);

}  // namespace epaper_ui

#include "epaper_ui/text_layout_impl.h"

#endif  // EPAPER_UI_TEXT_LAYOUT_H_
