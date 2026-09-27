#include "epaper_ui/journal_page.h"

#include <algorithm>

#include "render_utils.h"

namespace epaper_ui {
namespace {

constexpr int kMargin = design::spacing::k16;
constexpr int kContentTopGap = design::spacing::k16;
constexpr int kHeadingSubtitleGap = design::spacing::k4;
constexpr int kSubtitleSegmentGap = design::spacing::k12;
constexpr int kSegmentTimelineGap = design::spacing::k16;
constexpr int kTimelineFooterGap = design::spacing::k16;
constexpr auto kHeadingRole = design::TypographyRole::kHeadingH1;
constexpr auto kSubtitleRole = design::TypographyRole::kLabelMediumBold;

int PageWidth(int portrait_width)
{
    return std::max(0, portrait_width - (2 * kMargin));
}

int FooterTop(int portrait_height)
{
    return portrait_height - design::global_footer::kBottomPadding -
           design::global_footer::kButtonSize;
}

SegmentControlStyle SegmentStyle(int width)
{
    SegmentControlStyle style = {};
    style.width = width;
    return style;
}

TimelineListStyle TimelineStyle(const UiRect& timeline)
{
    TimelineListStyle style = {};
    style.width = timeline.width;
    style.height = timeline.height;
    return style;
}

struct Layout {
    UiRect heading = {};
    UiRect subtitle = {};
    UiRect segment = {};
    UiRect timeline = {};
};

Layout BuildLayout(int portrait_width, int portrait_height)
{
    const int page_width = PageWidth(portrait_width);
    const int content_top = StatusBarHeight() + kContentTopGap;

    Layout layout = {};
    layout.heading = {kMargin, content_top, page_width, LineHeight(kHeadingRole)};
    layout.subtitle = {kMargin, layout.heading.bottom() + kHeadingSubtitleGap, page_width,
                       LineHeight(kSubtitleRole)};
    layout.segment = SegmentControlBounds(kMargin, layout.subtitle.bottom() + kSubtitleSegmentGap,
                                          SegmentStyle(page_width));

    const int timeline_top = layout.segment.bottom() + kSegmentTimelineGap;
    const int footer_top = FooterTop(portrait_height);
    const int timeline_height = std::max(0, footer_top - kTimelineFooterGap - timeline_top);
    layout.timeline = {kMargin, timeline_top, page_width, timeline_height};
    return layout;
}

}  // namespace

void DrawJournalPage(uint8_t* framebuffer,
                     int raw_width,
                     int raw_height,
                     int portrait_width,
                     int portrait_height,
                     const JournalPageState& state,
                     const StatusBarState& status_bar_state,
                     const GlobalFooterState& footer_state)
{
    if (framebuffer == nullptr) {
        return;
    }

    FillPortraitRect(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                     {0, 0, portrait_width, portrait_height}, design::color::kWhite);
    DrawStatusBar(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                  status_bar_state);

    const Layout layout = BuildLayout(portrait_width, portrait_height);
    if (!state.title_text.empty()) {
        DrawTypographyText(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                           layout.heading.x, layout.heading.y, state.title_text, kHeadingRole,
                           design::color::kBlack);
    }
    if (!state.subtitle_text.empty()) {
        DrawTypographyText(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                           layout.subtitle.x, layout.subtitle.y, state.subtitle_text,
                           kSubtitleRole, design::color::kBlack);
    }

    DrawSegmentControl(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                       layout.segment.x, layout.segment.y, state.segment_control,
                       SegmentStyle(layout.segment.width));

    DrawTimelineList(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                     layout.timeline.x, layout.timeline.y, state.timeline,
                     TimelineStyle(layout.timeline));

    DrawGlobalFooter(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                     footer_state);
}

}  // namespace epaper_ui
