#ifndef EPAPER_UI_TEXT_LAYOUT_IMPL_H_
#define EPAPER_UI_TEXT_LAYOUT_IMPL_H_

#include "epaper_ui/font_renderer.h"

namespace epaper_ui {

template <typename Emit>
void WrapParagraph(design::TypographyRole role, std::string_view text, int max_width, Emit emit)
{
    if (text.empty() || max_width <= 0) {
        return;
    }
    const int space_width = MeasureWord(role, " ");
    size_t line_start = 0;   // byte offset of the current line
    size_t line_end = 0;     // end of the last word placed on the line
    int line_width = 0;
    bool line_has_word = false;
    size_t pos = 0;

    const auto flush = [&]() -> bool {
        const bool keep_going =
            emit(LineSpan{static_cast<uint32_t>(line_start), static_cast<uint32_t>(line_end - line_start)});
        line_has_word = false;
        line_width = 0;
        return keep_going;
    };

    while (pos < text.size()) {
        while (pos < text.size() && text[pos] == ' ') {
            ++pos;
        }
        if (pos >= text.size()) {
            break;
        }
        size_t word_end = text.find(' ', pos);
        if (word_end == std::string_view::npos) {
            word_end = text.size();
        }
        const std::string_view word = text.substr(pos, word_end - pos);
        const int word_width = MeasureWord(role, word);

        if (line_has_word && line_width + space_width + word_width <= max_width) {
            line_width += space_width + word_width;
            line_end = word_end;
            pos = word_end;
            continue;
        }
        if (line_has_word) {
            if (!flush()) {
                return;
            }
        }
        line_start = pos;
        if (word_width <= max_width) {
            line_width = word_width;
            line_end = word_end;
            line_has_word = true;
            pos = word_end;
            continue;
        }
        // Word wider than a whole line: split it by code point.
        size_t cut = pos;
        int cut_width = 0;
        while (cut < word_end) {
            const size_t next = Utf8Next(text, cut);
            const int w = MeasureWord(role, text.substr(cut, next - cut));
            if (cut_width + w > max_width && cut > pos) {
                break;
            }
            cut_width += w;
            cut = next;
        }
        line_end = cut;
        line_width = cut_width;
        line_has_word = true;
        pos = cut;
        if (pos < word_end) {
            if (!flush()) {
                return;
            }
            line_start = pos;
            line_has_word = false;
        }
    }
    if (line_has_word) {
        (void)flush();
    }
}

}  // namespace epaper_ui

#endif  // EPAPER_UI_TEXT_LAYOUT_IMPL_H_
