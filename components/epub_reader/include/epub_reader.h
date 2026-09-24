#ifndef EPUB_READER_H_
#define EPUB_READER_H_

// EPUB parsing for the book reader: ZIP container (ROM tinfl), OPF metadata and spine,
// XHTML -> paragraphs, and JPEG cover -> 1-bit thumbnail (ROM TJpgDec).
//
// Internal RAM is the scarce pool on this board (~20 KB free at peak), and an EPUB holds
// hundreds of small names and strings. Every container here therefore allocates from PSRAM
// explicitly (PsramAllocator) instead of relying on malloc's size-based placement, which
// would put each small string in internal RAM. Callers must run this on a task that does
// not also need to touch flash (NVS) while a PSRAM-backed stack is in use.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "esp_heap_caps.h"

namespace epub_reader {

template <typename T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() noexcept = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept
    {
    }
    T* allocate(size_t n)
    {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == nullptr) {
            // -fno-exceptions: there is no bad_alloc to throw; fail loudly.
            abort();
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) noexcept { heap_caps_free(p); }
    template <typename U>
    bool operator==(const PsramAllocator<U>&) const noexcept
    {
        return true;
    }
    template <typename U>
    bool operator!=(const PsramAllocator<U>&) const noexcept
    {
        return false;
    }
};

using PString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;
template <typename T>
using PVector = std::vector<T, PsramAllocator<T>>;

struct ZipEntry {
    PString name;
    uint16_t method = 0;  // 0 = stored, 8 = deflate
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint32_t local_header_offset = 0;
};

enum class BlockKind : uint8_t {
    kParagraph,
    kHeading,
};

// One paragraph of chapter text, UTF-8, whitespace collapsed, entities decoded.
struct Block {
    BlockKind kind = BlockKind::kParagraph;
    PString text;
};

struct Book {
    PString path;       // e.g. /sdcard/books/dom_casmurro.epub
    PString title;      // dc:title (falls back to the file name)
    PString author;     // first dc:creator, may be empty
    PString cover_href; // zip path of the cover image, may be empty
    PVector<PString> spine;  // zip paths of the chapters, reading order
    PVector<ZipEntry> entries;
};

// 1-bit bitmap, row-major, MSB first, rows padded to whole bytes; bit set = black.
struct Bitmap {
    uint16_t width = 0;
    uint16_t height = 0;
    PVector<uint8_t> bits;
    size_t stride() const { return (static_cast<size_t>(width) + 7U) / 8U; }
    bool empty() const { return width == 0 || height == 0; }
};

// Reads the central directory, container.xml and the OPF. False on anything that is not
// a readable EPUB; `error` (optional) gets a short pt-BR reason for the UI.
bool OpenBook(const char* path, Book* book, PString* error = nullptr);

// Extracts and converts spine item `spine_index` to paragraphs.
bool LoadChapter(const Book& book, size_t spine_index, PVector<Block>* blocks);

// Decodes the cover (JPEG only) and scales it to fit max_width x max_height, dithered to
// 1 bit. False when there is no JPEG cover or decoding fails.
bool DecodeCover(const Book& book, int max_width, int max_height, Bitmap* cover);

// Exposed for tests/tools: XHTML -> paragraphs.
void HtmlToBlocks(const char* html, size_t size, PVector<Block>* blocks);

}  // namespace epub_reader

#endif  // EPUB_READER_H_
