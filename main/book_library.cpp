#include "book_library.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "storage_service.h"

namespace book_library {
namespace {

using epub_reader::PString;
using epub_reader::PVector;

constexpr const char* kTag = "BookLibrary";
constexpr const char* kCacheDirectory = "/sdcard/books/.cache";
constexpr uint32_t kCacheMagic = 0x4B425546;  // "FUBK"
constexpr uint32_t kCacheVersion = 1;

// Runs `fn` with the card mounted and the storage lock held. Keep `fn` short: buttons are
// dropped while the card is busy.
template <typename Fn>
esp_err_t WithCard(Fn&& fn)
{
    struct Context {
        Fn* fn;
    };
    Context context = {&fn};
    return storage_service::RunWithMountedFilesystem(
        [](const char*, void* raw) -> esp_err_t { return (*static_cast<Context*>(raw)->fn)(); },
        &context);
}

bool EndsWithIgnoreCase(const char* name, const char* suffix)
{
    const size_t n = std::strlen(name);
    const size_t m = std::strlen(suffix);
    return n >= m && strcasecmp(name + n - m, suffix) == 0;
}

PString FileName(const PString& path)
{
    const size_t slash = path.rfind('/');
    return slash == PString::npos ? path : path.substr(slash + 1);
}

PString CachePath(const PString& book_path, const char* extension)
{
    PString path = kCacheDirectory;
    path += "/";
    path += FileName(book_path);
    path += extension;
    return path;
}

// Same crash-safe replace as the recording archive: fsynced temp file, then swap.
bool WriteFileAtomic(const PString& path, const void* data, size_t size)
{
    PString temp = path + ".tmp";
    FILE* f = std::fopen(temp.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = std::fwrite(data, 1, size, f) == size && std::fflush(f) == 0 &&
              fsync(fileno(f)) == 0;
    std::fclose(f);
    if (!ok) {
        std::remove(temp.c_str());
        return false;
    }
    struct stat st = {};
    if (stat(path.c_str(), &st) == 0) {
        std::remove(path.c_str());
    }
    return std::rename(temp.c_str(), path.c_str()) == 0;
}

bool ReadWholeFile(const PString& path, PVector<uint8_t>* out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bool ok = size >= 0 && size < 4 * 1024 * 1024;
    if (ok) {
        out->resize(static_cast<size_t>(size));
        ok = size == 0 || std::fread(out->data(), 1, out->size(), f) == out->size();
    }
    std::fclose(f);
    return ok;
}

// --- metadata cache ------------------------------------------------------------------------

class Writer {
public:
    void U16(uint16_t v) { Bytes(&v, sizeof(v)); }
    void U32(uint32_t v) { Bytes(&v, sizeof(v)); }
    void I64(int64_t v) { Bytes(&v, sizeof(v)); }
    void Str(const PString& s)
    {
        const uint16_t n = static_cast<uint16_t>(std::min<size_t>(s.size(), 1024));
        U16(n);
        Bytes(s.data(), n);
    }
    void Bytes(const void* p, size_t n)
    {
        const auto* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    }
    PVector<uint8_t> data;
};

class Reader {
public:
    explicit Reader(const PVector<uint8_t>& d) : data_(d) {}
    bool U16(uint16_t* v) { return Bytes(v, sizeof(*v)); }
    bool U32(uint32_t* v) { return Bytes(v, sizeof(*v)); }
    bool I64(int64_t* v) { return Bytes(v, sizeof(*v)); }
    bool Str(PString* s)
    {
        uint16_t n = 0;
        if (!U16(&n) || pos_ + n > data_.size()) {
            return false;
        }
        s->assign(reinterpret_cast<const char*>(&data_[pos_]), n);
        pos_ += n;
        return true;
    }
    bool Bytes(void* out, size_t n)
    {
        if (pos_ + n > data_.size()) {
            return false;
        }
        std::memcpy(out, &data_[pos_], n);
        pos_ += n;
        return true;
    }
    size_t remaining() const { return data_.size() - pos_; }
    const uint8_t* cursor() const { return &data_[pos_]; }

private:
    const PVector<uint8_t>& data_;
    size_t pos_ = 0;
};

epaper_ui::BookCover MakeCover(const uint8_t* bits, uint16_t width, uint16_t height)
{
    epaper_ui::BookCover cover = {};
    const size_t size = static_cast<size_t>((width + 7) / 8) * height;
    if (width == 0 || height == 0) {
        return cover;
    }
    // std::vector of > 256 bytes lands in PSRAM through malloc's size policy.
    cover.bits = std::make_shared<const std::vector<uint8_t>>(bits, bits + size);
    cover.width = width;
    cover.height = height;
    return cover;
}

bool LoadCache(const PString& book_path, const struct stat& book_stat, Entry* entry)
{
    PVector<uint8_t> data;
    if (!ReadWholeFile(CachePath(book_path, ".meta"), &data)) {
        return false;
    }
    Reader r(data);
    uint32_t magic = 0;
    uint32_t version = 0;
    int64_t size = 0;
    int64_t mtime = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    if (!r.U32(&magic) || magic != kCacheMagic || !r.U32(&version) || version != kCacheVersion ||
        !r.I64(&size) || !r.I64(&mtime) || size != static_cast<int64_t>(book_stat.st_size) ||
        mtime != static_cast<int64_t>(book_stat.st_mtime) || !r.Str(&entry->title) ||
        !r.Str(&entry->author) || !r.U16(&w) || !r.U16(&h)) {
        return false;
    }
    const size_t bits = static_cast<size_t>((w + 7) / 8) * h;
    if (r.remaining() < bits) {
        return false;
    }
    entry->cover = MakeCover(r.cursor(), w, h);
    return true;
}

void StoreCache(const PString& book_path, const struct stat& book_stat, const Entry& entry,
                const epub_reader::Bitmap& cover)
{
    Writer w;
    w.U32(kCacheMagic);
    w.U32(kCacheVersion);
    w.I64(static_cast<int64_t>(book_stat.st_size));
    w.I64(static_cast<int64_t>(book_stat.st_mtime));
    w.Str(entry.title);
    w.Str(entry.author);
    w.U16(cover.width);
    w.U16(cover.height);
    w.Bytes(cover.bits.data(), cover.bits.size());
    if (!WriteFileAtomic(CachePath(book_path, ".meta"), w.data.data(), w.data.size())) {
        ESP_LOGW(kTag, "cache write failed for %s", book_path.c_str());
    }
}

bool ReadBookmarkLocked(const PString& book_path, Bookmark* bookmark)
{
    *bookmark = {};
    FILE* f = std::fopen(CachePath(book_path, ".pos").c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    unsigned spine = 0;
    unsigned block = 0;
    unsigned offset = 0;
    int font = 1;
    int percent = 0;
    const int n = std::fscanf(f, "v1 %u %u %u %d %d", &spine, &block, &offset, &font, &percent);
    std::fclose(f);
    if (n != 5) {
        return false;
    }
    bookmark->valid = true;
    bookmark->spine_index = spine;
    bookmark->block_index = block;
    bookmark->char_offset = offset;
    bookmark->font_level = font;
    bookmark->percent_read = percent;
    return true;
}

}  // namespace

bool Scan(PVector<Entry>* entries, void (*on_progress)(int done, int total))
{
    entries->clear();
    PVector<PString> paths;
    const esp_err_t list_err = WithCard([&]() -> esp_err_t {
        mkdir(kBooksDirectory, 0775);
        mkdir(kCacheDirectory, 0775);
        DIR* dir = opendir(kBooksDirectory);
        if (dir == nullptr) {
            return ESP_FAIL;
        }
        while (struct dirent* e = readdir(dir)) {
            // Skip hidden files, including macOS "._name.epub" resource forks.
            if (e->d_name[0] != '.' && EndsWithIgnoreCase(e->d_name, ".epub")) {
                PString path = kBooksDirectory;
                path += "/";
                path += e->d_name;
                paths.push_back(std::move(path));
            }
        }
        closedir(dir);
        return ESP_OK;
    });
    if (list_err != ESP_OK) {
        ESP_LOGW(kTag, "listing %s failed: %s", kBooksDirectory, esp_err_to_name(list_err));
        return false;
    }

    const int total = static_cast<int>(paths.size());
    for (int i = 0; i < total; ++i) {
        const PString& path = paths[static_cast<size_t>(i)];
        Entry entry;
        entry.path = path;
        // One card slice per book: stat + cache hit is cheap; a miss opens the EPUB and
        // decodes its cover, which is the slow first-run path.
        (void)WithCard([&]() -> esp_err_t {
            struct stat st = {};
            if (stat(path.c_str(), &st) != 0) {
                return ESP_FAIL;
            }
            if (!LoadCache(path, st, &entry)) {
                epub_reader::Book book;
                if (epub_reader::OpenBook(path.c_str(), &book, nullptr)) {
                    entry.title = book.title;
                    entry.author = book.author;
                    epub_reader::Bitmap cover;
                    if (epub_reader::DecodeCover(book, epaper_ui::kBookCoverWidth,
                                                 epaper_ui::kBookCoverHeight, &cover)) {
                        entry.cover = MakeCover(cover.bits.data(), cover.width, cover.height);
                    }
                    StoreCache(path, st, entry, cover);
                } else {
                    entry.title = FileName(path);
                    entry.author = "Arquivo não pôde ser aberto";
                }
            }
            Bookmark bookmark;
            if (ReadBookmarkLocked(path, &bookmark)) {
                entry.percent_read = bookmark.percent_read;
            }
            return ESP_OK;
        });
        if (entry.title.empty()) {
            entry.title = FileName(path);
        }
        entries->push_back(std::move(entry));
        if (on_progress != nullptr) {
            on_progress(i + 1, total);
        }
    }

    std::sort(entries->begin(), entries->end(), [](const Entry& a, const Entry& b) {
        return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
    });
    ESP_LOGI(kTag, "library: %d book(s)", total);
    return true;
}

bool LoadBookmark(const PString& book_path, Bookmark* bookmark)
{
    bool found = false;
    (void)WithCard([&]() -> esp_err_t {
        found = ReadBookmarkLocked(book_path, bookmark);
        return ESP_OK;
    });
    return found;
}

bool SaveBookmark(const PString& book_path, const Bookmark& bookmark)
{
    char line[96] = {};
    const int n = std::snprintf(line, sizeof(line), "v1 %u %u %u %d %d\n",
                                static_cast<unsigned>(bookmark.spine_index),
                                static_cast<unsigned>(bookmark.block_index),
                                static_cast<unsigned>(bookmark.char_offset), bookmark.font_level,
                                bookmark.percent_read);
    bool ok = false;
    (void)WithCard([&]() -> esp_err_t {
        mkdir(kCacheDirectory, 0775);
        ok = WriteFileAtomic(CachePath(book_path, ".pos"), line, static_cast<size_t>(n));
        return ok ? ESP_OK : ESP_FAIL;
    });
    return ok;
}

bool OpenBook(const PString& book_path, epub_reader::Book* book, PString* error)
{
    bool ok = false;
    const esp_err_t err = WithCard([&]() -> esp_err_t {
        ok = epub_reader::OpenBook(book_path.c_str(), book, error);
        return ESP_OK;
    });
    if (err != ESP_OK && error != nullptr) {
        *error = "Cartão SD indisponível";
    }
    return err == ESP_OK && ok;
}

bool LoadChapter(const epub_reader::Book& book, size_t spine_index,
                 PVector<epub_reader::Block>* blocks)
{
    bool ok = false;
    (void)WithCard([&]() -> esp_err_t {
        ok = epub_reader::LoadChapter(book, spine_index, blocks);
        return ESP_OK;
    });
    return ok;
}

}  // namespace book_library
