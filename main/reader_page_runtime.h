#ifndef READER_PAGE_RUNTIME_H_
#define READER_PAGE_RUNTIME_H_

#include "display_service.h"
#include "epub_reader.h"
#include "esp_err.h"

// Book reader page. Owns the open book, the current chapter's paragraphs and its pagination
// (all PSRAM); only the current page's text is handed to the display. Chapter loads and
// position saves run on the book worker.
namespace reader_page_runtime {

esp_err_t UpdateDisplayState();
esp_err_t UpdateDisplayStateAndRequestRefresh(const display_service::RefreshRequest& request);

// Queue a book for the next ShowReaderScreen: loads it on the worker and resumes at the saved
// position. Called before the screen switch.
void Open(const epub_reader::PString& book_path);

// +1 next page, -1 previous page; crosses chapter boundaries. Ignored while loading.
void TurnPage(int direction);

// Reader menu (font size, chapters, back) as the shared select modal.
bool ShowMenu();
// Returns true if the reader menu was pending (so app_shell's submit chain stops here).
bool HandleMenuSelection(int selected_index);

// Back to the library, deferred like Details' Back (consumed by app_shell after dispatch).
void RequestBack();
bool ConsumePendingBack();

}  // namespace reader_page_runtime

#endif  // READER_PAGE_RUNTIME_H_
