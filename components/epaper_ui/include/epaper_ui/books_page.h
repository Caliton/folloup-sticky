#ifndef EPAPER_UI_BOOKS_PAGE_H_
#define EPAPER_UI_BOOKS_PAGE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "epaper_ui/global_footer.h"
#include "epaper_ui/status_bar.h"

namespace epaper_ui {

// 1-bit cover thumbnail, row-major, MSB first, bit set = black. Shared so page-state copies
// (the display snapshot copies every page's state per render) only bump a refcount.
struct BookCover {
    std::shared_ptr<const std::vector<uint8_t>> bits = {};
    uint16_t width = 0;
    uint16_t height = 0;

    bool operator==(const BookCover& other) const = default;
};

struct BookRowState {
    std::string title = {};
    std::string author = {};
    std::string progress_text = {};  // e.g. "34% lido" or "Novo"
    BookCover cover = {};
    bool selected = false;

    bool operator==(const BookRowState& other) const = default;
};

// Only the rows currently on screen are carried here; the runtime owns the full library.
struct BooksPageState {
    int navigation_focus_index = -1;
    std::string title_text = "Livros";
    std::vector<BookRowState> rows = {};
    int total_books = 0;
    int first_visible_index = 0;  // index of rows[0] in the full library (for "3/12")
    std::string message_text = {};  // empty state / loading / error, shown when rows is empty

    bool operator==(const BooksPageState& other) const = default;
};

// Rows that fit between the title and the footer (the runtime windows the library by this).
int BooksPageVisibleRowCount(int portrait_height);
// Cover box the thumbnails are generated for.
inline constexpr int kBookCoverWidth = 84;
inline constexpr int kBookCoverHeight = 126;

void DrawBooksPage(uint8_t* framebuffer,
                   int raw_width,
                   int raw_height,
                   int portrait_width,
                   int portrait_height,
                   const BooksPageState& state,
                   const StatusBarState& status_bar_state,
                   const GlobalFooterState& footer_state);

}  // namespace epaper_ui

#endif  // EPAPER_UI_BOOKS_PAGE_H_
