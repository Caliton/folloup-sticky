#include "epub_internal.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "miniz.h"

namespace epub_reader {
namespace {

constexpr const char* kTag = "EpubZip";
constexpr uint32_t kEocdSignature = 0x06054b50;
constexpr uint32_t kCentralSignature = 0x02014b50;
constexpr uint32_t kLocalSignature = 0x04034b50;
constexpr size_t kEocdMinSize = 22;
constexpr size_t kEocdSearchWindow = 0xFFFF + kEocdMinSize;
// Chapters and covers are small; this only guards against a hostile or broken archive
// exhausting PSRAM.
constexpr uint32_t kMaxEntryBytes = 4U * 1024U * 1024U;

uint16_t Le16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct FileCloser {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            std::fclose(f);
        }
    }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

bool ReadAt(FILE* f, long offset, void* out, size_t size)
{
    return std::fseek(f, offset, SEEK_SET) == 0 && std::fread(out, 1, size, f) == size;
}

}  // namespace

bool ReadZipDirectory(const char* path, PVector<ZipEntry>* entries)
{
    entries->clear();
    FilePtr file(std::fopen(path, "rb"));
    if (!file) {
        ESP_LOGW(kTag, "open failed: %s", path);
        return false;
    }
    if (std::fseek(file.get(), 0, SEEK_END) != 0) {
        return false;
    }
    const long file_size = std::ftell(file.get());
    if (file_size < static_cast<long>(kEocdMinSize)) {
        return false;
    }

    // The end-of-central-directory record sits in the last 64 KiB + 22 bytes (the
    // comment can be up to 64 KiB). Scan backwards for its signature.
    const size_t window = static_cast<size_t>(
        std::min<long>(file_size, static_cast<long>(kEocdSearchWindow)));
    PVector<uint8_t> tail(window);
    if (!ReadAt(file.get(), file_size - static_cast<long>(window), tail.data(), window)) {
        return false;
    }
    long eocd = -1;
    for (size_t i = window - kEocdMinSize + 1; i-- > 0;) {
        if (Le32(&tail[i]) == kEocdSignature) {
            eocd = static_cast<long>(i);
            break;
        }
    }
    if (eocd < 0) {
        ESP_LOGW(kTag, "no end-of-central-directory: %s", path);
        return false;
    }
    const uint8_t* e = &tail[static_cast<size_t>(eocd)];
    const uint16_t count = Le16(e + 10);
    const uint32_t cd_size = Le32(e + 12);
    const uint32_t cd_offset = Le32(e + 16);
    if (static_cast<long>(cd_offset) + static_cast<long>(cd_size) > file_size ||
        cd_size > kMaxEntryBytes) {
        return false;
    }

    PVector<uint8_t> cd(cd_size);
    if (!ReadAt(file.get(), static_cast<long>(cd_offset), cd.data(), cd_size)) {
        return false;
    }
    entries->reserve(count);
    size_t pos = 0;
    for (uint16_t n = 0; n < count; ++n) {
        if (pos + 46 > cd.size() || Le32(&cd[pos]) != kCentralSignature) {
            return false;
        }
        const uint8_t* h = &cd[pos];
        const uint16_t name_len = Le16(h + 28);
        const uint16_t extra_len = Le16(h + 30);
        const uint16_t comment_len = Le16(h + 32);
        if (pos + 46 + name_len > cd.size()) {
            return false;
        }
        ZipEntry entry;
        entry.method = Le16(h + 10);
        entry.compressed_size = Le32(h + 20);
        entry.uncompressed_size = Le32(h + 24);
        entry.local_header_offset = Le32(h + 42);
        entry.name.assign(reinterpret_cast<const char*>(h + 46), name_len);
        entries->push_back(std::move(entry));
        pos += 46U + name_len + extra_len + comment_len;
    }
    return true;
}

const ZipEntry* FindEntry(const PVector<ZipEntry>& entries, const PString& name)
{
    for (const ZipEntry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    // Some producers differ in case from the OPF hrefs.
    for (const ZipEntry& entry : entries) {
        if (entry.name.size() == name.size() &&
            strncasecmp(entry.name.c_str(), name.c_str(), name.size()) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

bool ExtractEntry(const char* path, const ZipEntry& entry, PVector<uint8_t>* out)
{
    out->clear();
    if (entry.uncompressed_size > kMaxEntryBytes || entry.compressed_size > kMaxEntryBytes) {
        ESP_LOGW(kTag, "entry too large: %s (%u bytes)", entry.name.c_str(),
                 static_cast<unsigned>(entry.uncompressed_size));
        return false;
    }
    FilePtr file(std::fopen(path, "rb"));
    if (!file) {
        return false;
    }
    uint8_t local[30];
    if (!ReadAt(file.get(), static_cast<long>(entry.local_header_offset), local, sizeof(local)) ||
        Le32(local) != kLocalSignature) {
        return false;
    }
    const long data_offset = static_cast<long>(entry.local_header_offset) + 30 +
                             Le16(local + 26) + Le16(local + 28);

    PVector<uint8_t> compressed(entry.compressed_size);
    if (entry.compressed_size > 0 &&
        !ReadAt(file.get(), data_offset, compressed.data(), compressed.size())) {
        return false;
    }
    file.reset();

    if (entry.method == 0) {
        *out = std::move(compressed);
        out->resize(entry.uncompressed_size);
        return true;
    }
    if (entry.method != 8) {
        ESP_LOGW(kTag, "unsupported compression %u: %s", entry.method, entry.name.c_str());
        return false;
    }

    // ~11 KB of decoder state: keep it in PSRAM, never on a task stack.
    auto* decomp = static_cast<tinfl_decompressor*>(
        heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (decomp == nullptr) {
        return false;
    }
    tinfl_init(decomp);
    out->resize(entry.uncompressed_size);
    size_t in_size = compressed.size();
    size_t out_size = out->size();
    const tinfl_status status =
        tinfl_decompress(decomp, compressed.data(), &in_size, out->data(), out->data(),
                         &out_size, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(decomp);
    if (status != TINFL_STATUS_DONE) {
        ESP_LOGW(kTag, "inflate failed (%d): %s", static_cast<int>(status), entry.name.c_str());
        out->clear();
        return false;
    }
    out->resize(out_size);
    return true;
}

}  // namespace epub_reader
