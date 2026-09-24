#include "epaper_ui/reader_page.h"

#include <algorithm>
#include <string_view>

#include "epaper_ui/text_layout.h"
#include "render_utils.h"

namespace epaper_ui {
namespace {

constexpr int kSideMargin = design::spacing::k20;
constexpr int kTopGap = design::spacing::k16;
constexpr int kFooterGap = design::spacing::k12;
constexpr int kBottomMargin = design::spacing::k12;
constexpr auto kFooterRole = design::TypographyRole::kLabelSmall;
constexpr auto kMessageRole = design::TypographyRole::kBodyLarge;

int FooterTextY(int portrait_height)
{
    return portrait_height - kBottomMargin - LineHeight(kFooterRole);
}

}  // namespace

design::TypographyRole ReaderBodyRole(int font_level)
{
    switch (std::clamp(font_level, 0, kReaderFontLevels - 1)) {
        case 0:
            return design::TypographyRole::kBody;
        case 2:
            return design::TypographyRole::kLabelLarge;
        case 1:
        default:
            return design::TypographyRole::kBodyLarge;
    }
}

design::TypographyRole ReaderHeadingRole(int font_level)
{
    switch (std::clamp(font_level, 0, kReaderFontLevels - 1)) {
        case 0:
            return design::TypographyRole::kLabelSmallBold;
        case 2:
            return design::TypographyRole::kLabelLargeBold;
        case 1:
        default:
            return design::TypographyRole::kLabelMediumBold;
    }
}

int ReaderLineHeight(int font_level)
{
    return std::max(LineHeight(ReaderBodyRole(font_level)),
                    LineHeight(ReaderHeadingRole(font_level)));
}

int ReaderParagraphGap(int font_level)
{
    return ReaderLineHeight(font_level) / 2;
}

UiRect ReaderTextArea(int portrait_width, int portrait_height)
{
    const int top = StatusBarHeight() + kTopGap;
    const int bottom = FooterTextY(portrait_height) - kFooterGap;
    return {kSideMargin, top, std::max(0, portrait_width - (2 * kSideMargin)),
            std::max(0, bottom - top)};
}

void DrawReaderPage(uint8_t* framebuffer,
                    int raw_width,
                    int raw_height,
                    int portrait_width,
                    int portrait_height,
                    const ReaderPageState& state,
                    const StatusBarState& status_bar_state)
{
    if (framebuffer == nullptr) {
        return;
    }
    uint8_t* fb = framebuffer;
    const int rw = raw_width;
    const int rh = raw_height;
    const int pw = portrait_width;
    const int ph = portrait_height;

    FillPortraitRect(fb, rw, rh, pw, ph, {0, 0, pw, ph}, design::color::kWhite);
    DrawStatusBar(fb, rw, rh, pw, ph, status_bar_state);

    const UiRect area = ReaderTextArea(pw, ph);
    if (!state.message_text.empty()) {
        int y = area.y + (area.height / 3);
        WrapParagraph(kMessageRole, state.message_text, area.width, [&](const LineSpan& span) {
            const std::string_view line =
                std::string_view(state.message_text).substr(span.offset, span.length);
            const int x = area.x + (area.width - MeasureText(kMessageRole, line)) / 2;
            DrawTypographyText(fb, rw, rh, pw, ph, x, y, line, kMessageRole, design::color::kBlack);
            y += LineHeight(kMessageRole);
            return true;
        });
    } else {
        const int line_height = ReaderLineHeight(state.font_level);
        const int gap = ReaderParagraphGap(state.font_level);
        const design::TypographyRole body = ReaderBodyRole(state.font_level);
        const design::TypographyRole heading = ReaderHeadingRole(state.font_level);
        const std::string_view text = state.page_text;
        int y = area.y;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string_view::npos) {
                end = text.size();
            }
            std::string_view line = text.substr(pos, end - pos);
            if (line.empty()) {
                y += gap;
            } else {
                const bool is_heading = line.front() == kReaderHeadingMarker;
                if (is_heading) {
                    line.remove_prefix(1);
                }
                DrawTypographyText(fb, rw, rh, pw, ph, area.x, y, line, is_heading ? heading : body,
                                   design::color::kBlack);
                y += line_height;
            }
            if (end == text.size()) {
                break;
            }
            pos = end + 1;
        }
    }

    if (!state.footer_text.empty()) {
        const std::string footer = FitText(kFooterRole, state.footer_text, area.width);
        const int x = (pw - MeasureText(kFooterRole, footer)) / 2;
        FillPortraitRect(fb, rw, rh, pw, ph,
                         {area.x, FooterTextY(ph) - (kFooterGap / 2), area.width, 1},
                         design::color::kBlack);
        DrawTypographyText(fb, rw, rh, pw, ph, x, FooterTextY(ph), footer, kFooterRole,
                           design::color::kBlack);
    }
}

}  // namespace epaper_ui
