#ifndef BOOKS_PAGE_RUNTIME_H_
#define BOOKS_PAGE_RUNTIME_H_

#include "display_service.h"
#include "epub_reader.h"
#include "esp_err.h"
#include "footer_runtime.h"
#include "page_action_result.h"

// "Livros": the library list. Owns the scanned library (PSRAM), the focus over it and the
// windowed page state (only the rows on screen go to the display).
namespace books_page_runtime {

struct ActivateResult {
    bool handled = false;
    bool play_activate_cue = false;
    // Set when a footer button was activated; app_shell routes it.
    footer_runtime::FooterFocusItem footer_item = footer_runtime::FooterFocusItem::kNone;
};

esp_err_t UpdateDisplayState();
esp_err_t UpdateDisplayStateAndRequestRefresh(const display_service::RefreshRequest& request);

page_actions::FocusMoveOutcome MoveFocus(int delta);
ActivateResult ActivateFocusedItem();

footer_runtime::ProjectionState BuildFooterProjectionState();
page_actions::FocusUpdateOutcome FocusFooterItem(footer_runtime::FooterFocusItem item);
void ResetFocus();

// Rescans /sdcard/books on the book worker. The first scan after boot runs on page entry;
// later entries reuse the list unless `force` (SD formatted / USB storage session ended).
void RequestScan(bool force);
// Marks the library stale so the next RequestScan(false) rescans (SD formatted, USB session).
void InvalidateLibrary();

// A book chosen from the list, consumed by app_shell to open the reader (deferred like
// Details' Back so the screen change happens after input dispatch returns).
epub_reader::PString ConsumePendingOpenBook();

// The reader reports progress so the list shows it without a rescan.
void SetBookProgress(const epub_reader::PString& path, int percent_read);

}  // namespace books_page_runtime

#endif  // BOOKS_PAGE_RUNTIME_H_
