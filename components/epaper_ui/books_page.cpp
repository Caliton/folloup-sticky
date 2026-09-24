#include "epaper_ui/books_page.h"

#include <algorithm>

#include "epaper_ui/text_layout.h"
#include "render_utils.h"

namespace epaper_ui {
namespace {

constexpr auto kTitleRole = design::TypographyRole::kHeadingH1;
constexpr auto kBookTitleRole = design::TypographyRole::kLabelMediumBold;
constexpr auto kAuthorRole = design::TypographyRole::kBody;
constexpr auto kProgressRole = design::TypographyRole::kLabelSmallBold;
constexpr auto kMessageRole = design::TypographyRole::kBodyLarge;
constexpr auto kCounterRole = design::TypographyRole::kLabelSmall;

constexpr int kMargin = design::spacing::k16;
constexpr int kTitleTopGap = design::spacing::k24;
constexpr int kTitleListGap = design::spacing::k16;
constexpr int kRowPadding = design::spacing::k8;
constexpr int kRowGap = design::spacing::k8;
constexpr int kCoverTextGap = design::spacing::k16;
constexpr int kFocusBorder = 3;
constexpr int kFocusRadius = design::spacing::k12;
constexpr int kRowHeight = kBookCoverHeight + (2 * kRowPadding);
constexpr int kMaxTitleLines = 2;

int ListTop()
{
    return StatusBarHeight() + kTitleTopGap + LineHeight(kTitleRole) + kTitleListGap;
}

int FooterTop(int portrait_height)
{
    return portrait_height - design::global_footer::kBottomPadding -
           design::global_footer::kButtonSize - design::spacing::k8;
}

void DrawCover(uint8_t* fb, int rw, int rh, int pw, int ph, int x, int y, const BookCover& cover)
{
    const UiRect box = {x, y, kBookCoverWidth, kBookCoverHeight};
    if (cover.bits == nullptr || cover.width == 0 || cover.height == 0) {
        // No usable cover: a plain book-shaped frame with a spine line.
        DrawPortraitBorder(fb, rw, rh, pw, ph, box, 2, design::color::kBlack);
        FillPortraitRect(fb, rw, rh, pw, ph, {x + 10, y, 3, kBookCoverHeight},
                         design::color::kBlack);
        return;
    }
    const int cx = x + (kBookCoverWidth - cover.width) / 2;
    const int cy = y + (kBookCoverHeight - cover.height) / 2;
    const EmbeddedImageAsset asset = {
        cover.bits->data(),
        cover.width,
        cover.height,
        static_cast<uint16_t>((cover.width + 7) / 8),
        ImageFormat::kMono1,
    };
    DrawPortraitMonoAsset(fb, rw, rh, pw, ph, cx, cy, &asset, design::color::kBlack);
    DrawPortraitBorder(fb, rw, rh, pw, ph, {cx, cy, cover.width, cover.height}, 1,
                       design::color::kBlack);
}

void DrawRow(uint8_t* fb, int rw, int rh, int pw, int ph, const UiRect& row,
             const BookRowState& state)
{
    if (state.selected) {
        DrawRoundedPortraitBorder(fb, rw, rh, pw, ph, row, kFocusRadius, kFocusBorder,
                                  design::color::kBlack);
    }
    const int cover_x = row.x + kRowPadding + 4;
    DrawCover(fb, rw, rh, pw, ph, cover_x, row.y + kRowPadding, state.cover);

    const int text_x = cover_x + kBookCoverWidth + kCoverTextGap;
    const int text_width = std::max(0, row.right() - kRowPadding - 4 - text_x);
    int y = row.y + kRowPadding + 2;

    // Title: up to two lines, the second ellipsized.
    int lines = 0;
    WrapParagraph(kBookTitleRole, state.title, text_width, [&](const LineSpan& span) {
        const std::string_view rest = std::string_view(state.title).substr(span.offset);
        const bool last = lines + 1 == kMaxTitleLines;
        const std::string line = last ? FitText(kBookTitleRole, rest, text_width)
                                      : std::string(rest.substr(0, span.length));
        DrawTypographyText(fb, rw, rh, pw, ph, text_x, y, line, kBookTitleRole,
                           design::color::kBlack);
        y += LineHeight(kBookTitleRole);
        return ++lines < kMaxTitleLines;
    });
    if (!state.author.empty()) {
        DrawTypographyText(fb, rw, rh, pw, ph, text_x, y,
                           FitText(kAuthorRole, state.author, text_width), kAuthorRole,
                           design::color::kBlack);
    }
    if (!state.progress_text.empty()) {
        const int progress_y = row.bottom() - kRowPadding - LineHeight(kProgressRole);
        DrawTypographyText(fb, rw, rh, pw, ph, text_x, progress_y, state.progress_text,
                           kProgressRole, design::color::kBlack);
    }
}

}  // namespace

int BooksPageVisibleRowCount(int portrait_height)
{
    const int available = FooterTop(portrait_height) - ListTop();
    return std::max(1, (available + kRowGap) / (kRowHeight + kRowGap));
}

void DrawBooksPage(uint8_t* framebuffer,
                   int raw_width,
                   int raw_height,
                   int portrait_width,
                   int portrait_height,
                   const BooksPageState& state,
                   const StatusBarState& status_bar_state,
                   const GlobalFooterState& footer_state)
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

    const int title_y = StatusBarHeight() + kTitleTopGap;
    DrawTypographyText(fb, rw, rh, pw, ph, kMargin, title_y, state.title_text, kTitleRole,
                       design::color::kBlack);
    if (state.total_books > 0) {
        const std::string counter = std::to_string(state.total_books) +
                                    (state.total_books == 1 ? " livro" : " livros");
        const int counter_x = pw - kMargin - MeasureText(kCounterRole, counter);
        const int counter_y =
            title_y + LineHeight(kTitleRole) - LineHeight(kCounterRole) - design::spacing::k4;
        DrawTypographyText(fb, rw, rh, pw, ph, counter_x, counter_y, counter, kCounterRole,
                           design::color::kBlack);
    }

    const int list_top = ListTop();
    const int row_width = pw - (2 * kMargin);
    if (state.rows.empty()) {
        int y = list_top + design::spacing::k32;
        WrapParagraph(kMessageRole, state.message_text, row_width, [&](const LineSpan& span) {
            DrawTypographyText(fb, rw, rh, pw, ph, kMargin, y,
                               std::string_view(state.message_text).substr(span.offset, span.length),
                               kMessageRole, design::color::kBlack);
            y += LineHeight(kMessageRole);
            return true;
        });
    } else {
        int y = list_top;
        for (const BookRowState& row : state.rows) {
            DrawRow(fb, rw, rh, pw, ph, {kMargin, y, row_width, kRowHeight}, row);
            y += kRowHeight + kRowGap;
        }
    }

    DrawGlobalFooter(fb, rw, rh, pw, ph, footer_state);
}

}  // namespace epaper_ui
