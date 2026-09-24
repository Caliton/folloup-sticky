#ifndef BOOK_LIBRARY_H_
#define BOOK_LIBRARY_H_

#include <cstdint>

#include "epaper_ui/books_page.h"
#include "epub_reader.h"

// SD-backed book library: lists /sdcard/books/*.epub, caches per-book metadata and a cover
// thumbnail in /sdcard/books/.cache, and stores reading positions there. Every call blocks on
// SD I/O and runs its card access through storage_service::RunWithMountedFilesystem in short
// slices (buttons are ignored while the card is busy), so call it from the book worker, never
// from the input or display task.
namespace book_library {

inline constexpr const char* kBooksDirectory = "/sdcard/books";

struct Entry {
    epub_reader::PString path;
    epub_reader::PString title;
    epub_reader::PString author;
    epaper_ui::BookCover cover = {};
    int percent_read = -1;  // -1 = never opened
};

// Where the reader is in a book. Positions are font-size independent: a spine item, a
// paragraph within it and a byte offset within that paragraph.
struct Bookmark {
    bool valid = false;
    uint32_t spine_index = 0;
    uint32_t block_index = 0;
    uint32_t char_offset = 0;
    int font_level = 1;
    int percent_read = 0;
};

// Lists the library, opening (and caching) books that have no valid cache yet.
// `on_progress(done, total)` is called between books so the UI can show progress.
bool Scan(epub_reader::PVector<Entry>* entries, void (*on_progress)(int done, int total));

bool LoadBookmark(const epub_reader::PString& book_path, Bookmark* bookmark);
bool SaveBookmark(const epub_reader::PString& book_path, const Bookmark& bookmark);

bool OpenBook(const epub_reader::PString& book_path, epub_reader::Book* book,
              epub_reader::PString* error);
bool LoadChapter(const epub_reader::Book& book, size_t spine_index,
                 epub_reader::PVector<epub_reader::Block>* blocks);

}  // namespace book_library

#endif  // BOOK_LIBRARY_H_
