#include "books_page_runtime.h"

#include <algorithm>
#include <mutex>

#include "book_library.h"
#include "book_worker.h"
#include "epaper_ui/books_page.h"
#include "esp_log.h"
#include "page_navigation/navigation_model.h"
#include "page_navigation/page_focus_projection.h"
#include "page_navigation/roving_focus.h"
#include "ui_refresh_runtime.h"
#include "waveshare_board_config.h"

namespace books_page_runtime {
namespace {

using epub_reader::PString;
using epub_reader::PVector;
using page_navigation::NavigationItemRole;
using page_navigation::NavigationItemSection;

constexpr const char* kTag = "BooksPage";
constexpr int kPortraitHeight = WAVESHARE_EPD_WIDTH;  // panel is landscape; UI is portrait

std::mutex s_mutex;
PVector<book_library::Entry> s_books;
page_navigation::NavigationModel s_model = page_navigation::BuildBooksPageNavigationModel(0);
page_navigation::RovingFocus s_focus;
int s_first_visible = 0;
bool s_scanned = false;
bool s_scanning = false;
std::string s_message = "Carregando livros...";
PString s_pending_open;

footer_runtime::FooterFocusItem FooterItemForSelectedIndex(int selected_index)
{
    switch (selected_index) {
        case 0:
            return footer_runtime::FooterFocusItem::kHome;
        case 1:
            return footer_runtime::FooterFocusItem::kSettings;
        case 2:
            return footer_runtime::FooterFocusItem::kWifi;
        case 3:
            return footer_runtime::FooterFocusItem::kTime;
        case 4:
            return footer_runtime::FooterFocusItem::kSticky;
        default:
            return footer_runtime::FooterFocusItem::kNone;
    }
}

NavigationItemRole FooterRoleForItem(footer_runtime::FooterFocusItem item)
{
    switch (item) {
        case footer_runtime::FooterFocusItem::kHome:
            return NavigationItemRole::kFooterHome;
        case footer_runtime::FooterFocusItem::kSettings:
            return NavigationItemRole::kFooterSettings;
        case footer_runtime::FooterFocusItem::kWifi:
            return NavigationItemRole::kFooterWifi;
        case footer_runtime::FooterFocusItem::kTime:
            return NavigationItemRole::kFooterTime;
        case footer_runtime::FooterFocusItem::kSticky:
            return NavigationItemRole::kFooterSticky;
        default:
            return NavigationItemRole::kUnknown;
    }
}

page_navigation::PageFocusProjection ProjectionForIndex(int focus_index)
{
    return page_navigation::ProjectPageFocus(s_model, NavigationItemSection::kBooksPageList,
                                             focus_index, -1, -1);
}

// Focused book index, or -1 when the footer has focus.
int FocusedBookLocked()
{
    const page_navigation::NavigationItemDescriptor* item = s_model.ItemAt(s_focus.index());
    if (item == nullptr || item->role != NavigationItemRole::kBooksPageItem) {
        return -1;
    }
    return item->item_index;
}

void KeepFocusVisibleLocked()
{
    const int visible = epaper_ui::BooksPageVisibleRowCount(kPortraitHeight);
    const int focused = FocusedBookLocked();
    if (focused < 0) {
        return;
    }
    if (focused < s_first_visible) {
        s_first_visible = focused;
    } else if (focused >= s_first_visible + visible) {
        s_first_visible = focused - visible + 1;
    }
    s_first_visible = std::clamp(s_first_visible, 0,
                                 std::max(0, static_cast<int>(s_books.size()) - visible));
}

std::string ProgressText(int percent)
{
    if (percent < 0) {
        return "Novo";
    }
    if (percent >= 100) {
        return "Concluído";
    }
    return std::to_string(percent) + "% lido";
}

epaper_ui::BooksPageState BuildStateLocked()
{
    epaper_ui::BooksPageState state = {};
    state.navigation_focus_index = s_focus.index();
    state.total_books = static_cast<int>(s_books.size());
    state.first_visible_index = s_first_visible;
    const int visible = epaper_ui::BooksPageVisibleRowCount(kPortraitHeight);
    const int focused = FocusedBookLocked();
    for (int i = s_first_visible;
         i < static_cast<int>(s_books.size()) && i < s_first_visible + visible; ++i) {
        const book_library::Entry& book = s_books[static_cast<size_t>(i)];
        state.rows.push_back({
            .title = std::string(book.title.c_str()),
            .author = std::string(book.author.c_str()),
            .progress_text = ProgressText(book.percent_read),
            .cover = book.cover,
            .selected = i == focused,
        });
    }
    if (state.rows.empty()) {
        state.message_text = s_scanning || !s_scanned
                                 ? s_message
                                 : "Nenhum livro encontrado. Copie arquivos .epub para a pasta "
                                   "\"books\" do cartão SD (pelo modo OTG ou por um leitor de "
                                   "cartão).";
    }
    return state;
}

void RebuildModelLocked(int keep_book)
{
    s_model = page_navigation::BuildBooksPageNavigationModel(static_cast<int>(s_books.size()));
    const int initial = s_books.empty() ? s_model.IndexOfRole(NavigationItemRole::kFooterHome)
                                        : std::clamp(keep_book, 0,
                                                     static_cast<int>(s_books.size()) - 1);
    s_focus.Configure(s_model.item_count, std::max(0, initial));
    KeepFocusVisibleLocked();
}

void ScheduleRefresh()
{
    (void)UpdateDisplayStateAndRequestRefresh({
        .refresh_mode = display_service::RefreshMode::kPartial,
        .scope = display_service::RefreshScope::kRegion,
    });
}

void OnScanProgress(int done, int total)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_message = "Preparando livros... " + std::to_string(done) + "/" + std::to_string(total);
    }
    // Progress only matters while the list is still empty (first run with many books).
    if (display_service::GetCurrentScreen() == display_service::ScreenId::kBooks && done < total) {
        ScheduleRefresh();
    }
}

}  // namespace

esp_err_t UpdateDisplayState()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return display_service::SetBooksPageState(BuildStateLocked());
}

esp_err_t UpdateDisplayStateAndRequestRefresh(const display_service::RefreshRequest& request)
{
    return ui_refresh_runtime::Schedule(ui_refresh_runtime::SurfaceKey::kBooksPage,
                                        &UpdateDisplayState, request);
}

page_actions::FocusMoveOutcome MoveFocus(int delta)
{
    page_actions::FocusMoveOutcome result = {};
    std::lock_guard<std::mutex> lock(s_mutex);
    const int old_index = s_focus.index();
    if (!s_focus.Move(delta)) {
        return result;
    }
    KeepFocusVisibleLocked();
    result.handled = true;
    result.play_navigation_cue = true;
    result.apply_page_state = true;
    result.sync_footer_projection =
        FooterItemForSelectedIndex(ProjectionForIndex(old_index).footer_selected_index) !=
        FooterItemForSelectedIndex(ProjectionForIndex(s_focus.index()).footer_selected_index);
    return result;
}

ActivateResult ActivateFocusedItem()
{
    ActivateResult result = {};
    std::lock_guard<std::mutex> lock(s_mutex);
    const int book = FocusedBookLocked();
    if (book >= 0 && book < static_cast<int>(s_books.size())) {
        s_pending_open = s_books[static_cast<size_t>(book)].path;
        result.handled = true;
        result.play_activate_cue = true;
        return result;
    }
    const footer_runtime::FooterFocusItem footer =
        FooterItemForSelectedIndex(ProjectionForIndex(s_focus.index()).footer_selected_index);
    if (footer != footer_runtime::FooterFocusItem::kNone &&
        footer != footer_runtime::FooterFocusItem::kSticky) {
        result.handled = true;
        result.footer_item = footer;
    }
    return result;
}

footer_runtime::ProjectionState BuildFooterProjectionState()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    footer_runtime::ProjectionState state = {};
    state.focused_item =
        FooterItemForSelectedIndex(ProjectionForIndex(s_focus.index()).footer_selected_index);
    return state;
}

page_actions::FocusUpdateOutcome FocusFooterItem(footer_runtime::FooterFocusItem item)
{
    page_actions::FocusUpdateOutcome result = {};
    std::lock_guard<std::mutex> lock(s_mutex);
    const int index = s_model.IndexOfRole(FooterRoleForItem(item));
    if (index < 0 || !s_focus.SetIndex(index)) {
        return result;
    }
    result.handled = true;
    result.apply_page_state = true;
    result.sync_footer_projection = true;
    return result;
}

void ResetFocus()
{
    footer_runtime::ProjectionState projection = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_focus.item_count() != s_model.item_count) {
            RebuildModelLocked(0);
        }
        KeepFocusVisibleLocked();
    }
    projection = BuildFooterProjectionState();
    footer_runtime::SetProjectionState(projection);
}

void RequestScan(bool force)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_scanning || (s_scanned && !force)) {
            return;
        }
        s_scanning = true;
        s_message = "Carregando livros...";
    }
    const bool queued = book_worker::Submit([]() {
        PVector<book_library::Entry> books;
        const bool ok = book_library::Scan(&books, &OnScanProgress);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            const int keep = FocusedBookLocked();
            s_books = std::move(books);
            s_scanning = false;
            s_scanned = ok;
            if (!ok) {
                s_message = "Não foi possível ler o cartão SD.";
            }
            RebuildModelLocked(keep < 0 ? 0 : keep);
        }
        if (display_service::GetCurrentScreen() == display_service::ScreenId::kBooks) {
            footer_runtime::SetProjectionState(BuildFooterProjectionState());
            (void)footer_runtime::UpdateDisplayState();
            ScheduleRefresh();
        }
    });
    if (!queued) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_scanning = false;
        s_message = "Memória insuficiente para abrir a biblioteca.";
    }
}

void InvalidateLibrary()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_scanned = false;
}

PString ConsumePendingOpenBook()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    PString path = std::move(s_pending_open);
    s_pending_open.clear();
    return path;
}

void SetBookProgress(const PString& path, int percent_read)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    for (book_library::Entry& book : s_books) {
        if (book.path == path) {
            book.percent_read = percent_read;
            return;
        }
    }
    ESP_LOGD(kTag, "progress for unknown book %s", path.c_str());
}

}  // namespace books_page_runtime
