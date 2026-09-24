#include "reader_page_runtime.h"

#include <algorithm>
#include <mutex>

#include "book_library.h"
#include "book_worker.h"
#include "books_page_runtime.h"
#include "epaper_ui/reader_page.h"
#include "epaper_ui/select_modal.h"
#include "epaper_ui/text_layout.h"
#include "esp_log.h"
#include "overlay_runtime.h"
#include "ui_refresh_runtime.h"
#include "waveshare_board_config.h"

namespace reader_page_runtime {
namespace {

using epub_reader::Block;
using epub_reader::BlockKind;
using epub_reader::PString;
using epub_reader::PVector;

constexpr const char* kTag = "ReaderPage";
constexpr int kPortraitWidth = WAVESHARE_EPD_HEIGHT;  // panel is landscape; UI is portrait
constexpr int kPortraitHeight = WAVESHARE_EPD_WIDTH;

// One wrapped line of the current chapter: a byte range of one paragraph.
struct Line {
    uint32_t block = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    bool heading = false;
    bool paragraph_start = false;  // gets a paragraph gap above it unless it tops the page
};

enum class MenuAction : uint8_t {
    kFontSmall,
    kFontMedium,
    kFontLarge,
    kNextChapter,
    kPreviousChapter,
    kBack,
    kClose,
};

// Where to land after a chapter load.
enum class Landing : uint8_t {
    kBookmark,   // page containing (block, offset)
    kFirstPage,
    kLastPage,
};

std::mutex s_mutex;
PString s_path;
epub_reader::Book s_book;
bool s_book_open = false;
PVector<Block> s_blocks;
PVector<Line> s_lines;
PVector<uint32_t> s_pages;  // index of each page's first line
size_t s_chapter = 0;
size_t s_page = 0;
int s_font_level = 1;
bool s_loading = false;
std::string s_message;
bool s_pending_back = false;
bool s_menu_pending = false;
PVector<MenuAction> s_menu_actions;

void ScheduleRefresh()
{
    (void)UpdateDisplayStateAndRequestRefresh({
        .refresh_mode = display_service::RefreshMode::kPartial,
        .scope = display_service::RefreshScope::kRegion,
    });
}

void PaginateLocked()
{
    s_lines.clear();
    s_pages.clear();
    const epaper_ui::UiRect area = epaper_ui::ReaderTextArea(kPortraitWidth, kPortraitHeight);
    const design::TypographyRole body = epaper_ui::ReaderBodyRole(s_font_level);
    const design::TypographyRole heading = epaper_ui::ReaderHeadingRole(s_font_level);
    for (uint32_t b = 0; b < s_blocks.size(); ++b) {
        const Block& block = s_blocks[b];
        const bool is_heading = block.kind == BlockKind::kHeading;
        bool first = true;
        epaper_ui::WrapParagraph(
            is_heading ? heading : body, std::string_view(block.text.data(), block.text.size()),
            area.width, [&](const epaper_ui::LineSpan& span) {
                s_lines.push_back({b, span.offset, span.length, is_heading, first});
                first = false;
                return true;
            });
    }
    const int line_height = epaper_ui::ReaderLineHeight(s_font_level);
    const int gap = epaper_ui::ReaderParagraphGap(s_font_level);
    int used = area.height;  // force a new page on the first line
    for (uint32_t i = 0; i < s_lines.size(); ++i) {
        const int cost = line_height + (s_lines[i].paragraph_start && used > 0 ? gap : 0);
        if (used + cost > area.height) {
            s_pages.push_back(i);
            used = line_height;
        } else {
            used += cost;
        }
    }
}

size_t PageForPositionLocked(uint32_t block, uint32_t offset)
{
    size_t page = 0;
    for (size_t p = 0; p < s_pages.size(); ++p) {
        const Line& first = s_lines[s_pages[p]];
        if (first.block > block || (first.block == block && first.offset > offset)) {
            break;
        }
        page = p;
    }
    return page;
}

int PercentReadLocked()
{
    if (!s_book_open || s_book.spine.empty()) {
        return 0;
    }
    const double chapter_fraction =
        s_pages.empty() ? 1.0 : static_cast<double>(s_page + 1) / s_pages.size();
    const double overall = (s_chapter + chapter_fraction) / s_book.spine.size();
    return std::clamp(static_cast<int>(overall * 100.0), 0, 100);
}

book_library::Bookmark BookmarkLocked()
{
    book_library::Bookmark bookmark = {};
    bookmark.valid = true;
    bookmark.spine_index = static_cast<uint32_t>(s_chapter);
    bookmark.font_level = s_font_level;
    bookmark.percent_read = PercentReadLocked();
    if (s_page < s_pages.size()) {
        const Line& first = s_lines[s_pages[s_page]];
        bookmark.block_index = first.block;
        bookmark.char_offset = first.offset;
    }
    return bookmark;
}

epaper_ui::ReaderPageState BuildStateLocked()
{
    epaper_ui::ReaderPageState state = {};
    state.font_level = s_font_level;
    if (!s_message.empty()) {
        state.message_text = s_message;
        return state;
    }
    if (s_page >= s_pages.size()) {
        state.message_text = "Capítulo sem texto.";
    } else {
        const uint32_t begin = s_pages[s_page];
        const uint32_t end = s_page + 1 < s_pages.size() ? s_pages[s_page + 1]
                                                         : static_cast<uint32_t>(s_lines.size());
        // One string with '\n'-separated lines: > 256 bytes, so malloc puts it in PSRAM
        // instead of internal RAM (the display snapshot copies it on every render).
        std::string text;
        text.reserve(2048);
        for (uint32_t i = begin; i < end; ++i) {
            const Line& line = s_lines[i];
            if (i > begin) {
                text.push_back('\n');
                if (line.paragraph_start) {
                    text.push_back('\n');  // empty line = paragraph gap
                }
            }
            if (line.heading) {
                text.push_back(epaper_ui::kReaderHeadingMarker);
            }
            text.append(s_blocks[line.block].text.data() + line.offset, line.length);
        }
        state.page_text = std::move(text);
    }
    state.footer_text = "Cap. " + std::to_string(s_chapter + 1) + "/" +
                        std::to_string(s_book.spine.size()) + "  ·  Pág. " +
                        std::to_string(std::min(s_page + 1, s_pages.size())) + "/" +
                        std::to_string(s_pages.size()) + "  ·  " +
                        std::to_string(PercentReadLocked()) + "%";
    return state;
}

// Worker: load `chapter` and land per `landing`; walks past empty chapters in the travel
// direction (cover-only XHTML files are common). Saves the position when done.
void LoadChapterJob(const PString& expected_path, size_t chapter, Landing landing,
                    uint32_t block, uint32_t offset)
{
    const int step = landing == Landing::kLastPage ? -1 : 1;
    PVector<Block> blocks;
    size_t spine_count = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_path != expected_path || !s_book_open) {
            return;  // a different book was opened after this job was queued
        }
        spine_count = s_book.spine.size();
    }
    bool loaded = false;
    for (size_t tries = 0; tries < spine_count; ++tries) {
        // s_book is only replaced on this same worker (Open's job), so reading it without
        // holding the lock across SD I/O is safe.
        const bool ok = book_library::LoadChapter(s_book, chapter, &blocks);
        if (ok && !blocks.empty()) {
            loaded = true;
            break;
        }
        if ((step < 0 && chapter == 0) || (step > 0 && chapter + 1 >= spine_count)) {
            break;
        }
        chapter = static_cast<size_t>(static_cast<int>(chapter) + step);
        landing = step < 0 ? Landing::kLastPage : Landing::kFirstPage;
    }

    book_library::Bookmark bookmark;
    PString path;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_path != expected_path) {
            return;
        }
        s_loading = false;
        if (!loaded) {
            // Keep showing the current page if there is one; otherwise explain.
            if (s_blocks.empty()) {
                s_message = "Não foi possível abrir este capítulo.";
            }
        }
    }
    if (!loaded) {
        ScheduleRefresh();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_chapter = chapter;
        s_blocks = std::move(blocks);
        PaginateLocked();
        switch (landing) {
            case Landing::kBookmark:
                s_page = PageForPositionLocked(block, offset);
                break;
            case Landing::kLastPage:
                s_page = s_pages.empty() ? 0 : s_pages.size() - 1;
                break;
            case Landing::kFirstPage:
            default:
                s_page = 0;
                break;
        }
        s_message.clear();
        bookmark = BookmarkLocked();
        path = s_path;
    }
    ScheduleRefresh();
    (void)book_library::SaveBookmark(path, bookmark);
    books_page_runtime::SetBookProgress(path, bookmark.percent_read);
}

// Call WITHOUT s_mutex held (a failed submit takes it to clear the loading flag).
void SubmitChapterLoad(const PString& path, size_t chapter, Landing landing)
{
    if (!book_worker::Submit([path, chapter, landing]() {
            LoadChapterJob(path, chapter, landing, 0, 0);
        })) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_loading = false;
        ESP_LOGW(kTag, "chapter load not queued (worker unavailable)");
    }
}

void SavePositionAsync()
{
    book_library::Bookmark bookmark;
    PString path;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        bookmark = BookmarkLocked();
        path = s_path;
    }
    // Latest wins in the worker, which is right for rapid page turns.
    (void)book_worker::Submit([bookmark, path]() {
        (void)book_library::SaveBookmark(path, bookmark);
        books_page_runtime::SetBookProgress(path, bookmark.percent_read);
    });
}

void RepaginateJob(int font_level)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        uint32_t block = 0;
        uint32_t offset = 0;
        if (s_page < s_pages.size()) {
            block = s_lines[s_pages[s_page]].block;
            offset = s_lines[s_pages[s_page]].offset;
        }
        s_font_level = font_level;
        PaginateLocked();
        s_page = PageForPositionLocked(block, offset);
        s_loading = false;
    }
    ScheduleRefresh();
    SavePositionAsync();
}

}  // namespace

esp_err_t UpdateDisplayState()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return display_service::SetReaderPageState(BuildStateLocked());
}

esp_err_t UpdateDisplayStateAndRequestRefresh(const display_service::RefreshRequest& request)
{
    return ui_refresh_runtime::Schedule(ui_refresh_runtime::SurfaceKey::kReaderPage,
                                        &UpdateDisplayState, request);
}

void Open(const PString& book_path)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_book_open && s_path == book_path && !s_loading) {
            return;  // same book, keep the position in memory
        }
        s_path = book_path;
        s_book_open = false;
        s_blocks.clear();
        s_lines.clear();
        s_pages.clear();
        s_page = 0;
        s_chapter = 0;
        s_loading = true;
        s_message = "Abrindo livro...";
    }
    const bool queued = book_worker::Submit([path = PString(book_path)]() {
        epub_reader::Book book;
        PString error;
        if (!book_library::OpenBook(path, &book, &error)) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_loading = false;
            s_message = error.empty() ? "Não foi possível abrir o livro."
                                      : std::string(error.c_str());
            ScheduleRefresh();
            return;
        }
        book_library::Bookmark bookmark;
        const bool has_bookmark = book_library::LoadBookmark(path, &bookmark);
        size_t chapter = 0;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_path != path) {
                return;  // another book was opened meanwhile
            }
            s_book = std::move(book);
            s_book_open = true;
            if (has_bookmark) {
                s_font_level = std::clamp(bookmark.font_level, 0, epaper_ui::kReaderFontLevels - 1);
                chapter = std::min<size_t>(bookmark.spine_index, s_book.spine.size() - 1);
            }
        }
        LoadChapterJob(path, chapter, has_bookmark ? Landing::kBookmark : Landing::kFirstPage,
                       has_bookmark ? bookmark.block_index : 0,
                       has_bookmark ? bookmark.char_offset : 0);
    });
    if (!queued) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_loading = false;
        s_message = "Memória insuficiente para abrir o livro.";
    }
}

void TurnPage(int direction)
{
    bool moved = false;
    bool load = false;
    size_t chapter = 0;
    Landing landing = Landing::kFirstPage;
    PString path;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_loading || !s_book_open) {
            return;
        }
        if (direction > 0) {
            if (s_page + 1 < s_pages.size()) {
                ++s_page;
                moved = true;
            } else if (s_chapter + 1 < s_book.spine.size()) {
                load = true;
                chapter = s_chapter + 1;
                landing = Landing::kFirstPage;
            }
        } else if (direction < 0) {
            if (s_page > 0) {
                --s_page;
                moved = true;
            } else if (s_chapter > 0) {
                load = true;
                chapter = s_chapter - 1;
                landing = Landing::kLastPage;
            }
        }
        s_loading = load;
        path = s_path;
    }
    if (moved) {
        ScheduleRefresh();
        SavePositionAsync();
    } else if (load) {
        SubmitChapterLoad(path, chapter, landing);
    }
}

bool ShowMenu()
{
    epaper_ui::SelectModalState modal = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_book_open) {
            return false;
        }
        s_menu_actions.clear();
        modal.title_text = std::string(s_book.title.c_str());
        const auto add = [&](const char* label, MenuAction action) {
            modal.items.push_back({label});
            s_menu_actions.push_back(action);
        };
        static constexpr const char* kFontLabels[] = {"Letra pequena", "Letra média",
                                                      "Letra grande"};
        static constexpr MenuAction kFontActions[] = {MenuAction::kFontSmall,
                                                      MenuAction::kFontMedium,
                                                      MenuAction::kFontLarge};
        for (int level = 0; level < epaper_ui::kReaderFontLevels; ++level) {
            if (level != s_font_level) {
                add(kFontLabels[level], kFontActions[level]);
            }
        }
        if (s_chapter + 1 < s_book.spine.size()) {
            add("Próximo capítulo", MenuAction::kNextChapter);
        }
        if (s_chapter > 0) {
            add("Capítulo anterior", MenuAction::kPreviousChapter);
        }
        add("Voltar aos livros", MenuAction::kBack);
        add("Fechar", MenuAction::kClose);
        modal.selected_index = 0;
        s_menu_pending = true;
    }
    const esp_err_t err = overlay_runtime::ShowSelectModal(modal);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_menu_pending = false;
        ESP_LOGW(kTag, "reader menu failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool HandleMenuSelection(int selected_index)
{
    MenuAction action = MenuAction::kClose;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_menu_pending) {
            return false;
        }
        s_menu_pending = false;
        if (selected_index < 0 || selected_index >= static_cast<int>(s_menu_actions.size())) {
            return true;
        }
        action = s_menu_actions[static_cast<size_t>(selected_index)];
        if (s_loading) {
            return true;
        }
    }
    switch (action) {
        case MenuAction::kFontSmall:
        case MenuAction::kFontMedium:
        case MenuAction::kFontLarge: {
            const int level = static_cast<int>(action) - static_cast<int>(MenuAction::kFontSmall);
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_loading = true;
            }
            if (!book_worker::Submit([level]() { RepaginateJob(level); })) {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_loading = false;
            }
            break;
        }
        case MenuAction::kNextChapter:
        case MenuAction::kPreviousChapter: {
            size_t chapter = 0;
            PString path;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_loading = true;
                chapter = action == MenuAction::kNextChapter ? s_chapter + 1 : s_chapter - 1;
                path = s_path;
            }
            SubmitChapterLoad(path, chapter, Landing::kFirstPage);
            break;
        }
        case MenuAction::kBack:
            RequestBack();
            break;
        case MenuAction::kClose:
        default:
            break;
    }
    return true;
}

void RequestBack()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_pending_back = true;
}

bool ConsumePendingBack()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    const bool pending = s_pending_back;
    s_pending_back = false;
    return pending;
}

}  // namespace reader_page_runtime
