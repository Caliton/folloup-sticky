#include "epub_internal.h"

#include <cstring>

#include "esp32s3/rom/tjpgd.h"
#include "esp_log.h"

namespace epub_reader {
namespace {

constexpr const char* kTag = "EpubCover";
constexpr size_t kJpegWorkBytes = 8192;  // TJpgDec needs ~3.1 KB; headroom for odd tables
constexpr int kMaxJpegDimension = 8192;

struct DecodeContext {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    int width = 0;   // decoded (descaled) size
    int height = 0;
    PVector<uint8_t> gray;  // width * height luminance
};

UINT JpegInput(JDEC* jd, BYTE* buf, UINT n)
{
    auto* ctx = static_cast<DecodeContext*>(jd->device);
    const size_t take = std::min<size_t>(n, ctx->size - ctx->pos);
    if (buf != nullptr) {
        std::memcpy(buf, ctx->data + ctx->pos, take);
    }
    ctx->pos += take;
    return static_cast<UINT>(take);
}

UINT JpegOutput(JDEC* jd, void* bitmap, JRECT* rect)
{
    auto* ctx = static_cast<DecodeContext*>(jd->device);
    const auto* rgb = static_cast<const uint8_t*>(bitmap);
    for (int y = rect->top; y <= rect->bottom; ++y) {
        for (int x = rect->left; x <= rect->right; ++x) {
            if (x < ctx->width && y < ctx->height) {
                ctx->gray[static_cast<size_t>(y) * ctx->width + x] =
                    static_cast<uint8_t>((rgb[0] * 30 + rgb[1] * 59 + rgb[2] * 11) / 100);
            }
            rgb += 3;
        }
    }
    return 1;
}

bool IsJpeg(const PVector<uint8_t>& data)
{
    return data.size() > 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

}  // namespace

bool DecodeCover(const Book& book, int max_width, int max_height, Bitmap* cover)
{
    *cover = {};
    if (book.cover_href.empty() || max_width <= 0 || max_height <= 0) {
        return false;
    }
    const ZipEntry* entry = FindEntry(book.entries, book.cover_href);
    DecodeContext ctx;
    PVector<uint8_t> jpeg;
    if (entry == nullptr || !ExtractEntry(book.path.c_str(), *entry, &jpeg)) {
        return false;
    }
    if (!IsJpeg(jpeg)) {
        ESP_LOGI(kTag, "cover is not JPEG, skipping: %s", book.cover_href.c_str());
        return false;
    }
    ctx.data = jpeg.data();
    ctx.size = jpeg.size();

    void* work = heap_caps_malloc(kJpegWorkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (work == nullptr) {
        return false;
    }
    JDEC jd = {};
    JRESULT res = jd_prepare(&jd, JpegInput, work, kJpegWorkBytes, &ctx);
    if (res != JDR_OK || jd.width == 0 || jd.height == 0 ||
        static_cast<int>(jd.width) > kMaxJpegDimension ||
        static_cast<int>(jd.height) > kMaxJpegDimension) {
        heap_caps_free(work);
        ESP_LOGW(kTag, "jd_prepare failed (%d) for %s", static_cast<int>(res),
                 book.cover_href.c_str());
        return false;
    }

    // Largest descale (1/2^s, s<=3) that still leaves at least the target size.
    uint8_t scale = 0;
    while (scale < 3 && static_cast<int>(jd.width >> (scale + 1)) >= max_width &&
           static_cast<int>(jd.height >> (scale + 1)) >= max_height) {
        ++scale;
    }
    ctx.width = static_cast<int>((jd.width + (1U << scale) - 1) >> scale);
    ctx.height = static_cast<int>((jd.height + (1U << scale) - 1) >> scale);
    ctx.gray.assign(static_cast<size_t>(ctx.width) * ctx.height, 0xFF);
    res = jd_decomp(&jd, JpegOutput, scale);
    heap_caps_free(work);
    jpeg.clear();
    jpeg.shrink_to_fit();
    if (res != JDR_OK) {
        ESP_LOGW(kTag, "jd_decomp failed (%d) for %s", static_cast<int>(res),
                 book.cover_href.c_str());
        return false;
    }

    // Fit inside max_width x max_height, keeping the aspect ratio.
    int out_w = max_width;
    int out_h = static_cast<int>(static_cast<int64_t>(ctx.height) * out_w / ctx.width);
    if (out_h > max_height) {
        out_h = max_height;
        out_w = static_cast<int>(static_cast<int64_t>(ctx.width) * out_h / ctx.height);
    }
    out_w = std::max(1, out_w);
    out_h = std::max(1, out_h);

    // Area-average downsample into a signed working buffer for error diffusion.
    PVector<int16_t> level(static_cast<size_t>(out_w) * out_h);
    for (int y = 0; y < out_h; ++y) {
        const int sy0 = y * ctx.height / out_h;
        const int sy1 = std::max(sy0 + 1, (y + 1) * ctx.height / out_h);
        for (int x = 0; x < out_w; ++x) {
            const int sx0 = x * ctx.width / out_w;
            const int sx1 = std::max(sx0 + 1, (x + 1) * ctx.width / out_w);
            uint32_t sum = 0;
            uint32_t count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t* row = &ctx.gray[static_cast<size_t>(sy) * ctx.width];
                for (int sx = sx0; sx < sx1; ++sx) {
                    sum += row[sx];
                    ++count;
                }
            }
            level[static_cast<size_t>(y) * out_w + x] = static_cast<int16_t>(sum / count);
        }
    }
    ctx.gray.clear();
    ctx.gray.shrink_to_fit();

    // Floyd-Steinberg to 1 bit.
    cover->width = static_cast<uint16_t>(out_w);
    cover->height = static_cast<uint16_t>(out_h);
    cover->bits.assign(cover->stride() * out_h, 0);
    for (int y = 0; y < out_h; ++y) {
        for (int x = 0; x < out_w; ++x) {
            int16_t& px = level[static_cast<size_t>(y) * out_w + x];
            const int old_value = px;
            const int new_value = old_value < 128 ? 0 : 255;
            const int err = old_value - new_value;
            if (new_value == 0) {
                cover->bits[static_cast<size_t>(y) * cover->stride() + (x >> 3)] |=
                    static_cast<uint8_t>(0x80 >> (x & 7));
            }
            const auto spread = [&](int dx, int dy, int weight) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx >= 0 && nx < out_w && ny < out_h) {
                    int16_t& n = level[static_cast<size_t>(ny) * out_w + nx];
                    n = static_cast<int16_t>(n + err * weight / 16);
                }
            };
            spread(1, 0, 7);
            spread(-1, 1, 3);
            spread(0, 1, 5);
            spread(1, 1, 1);
        }
    }
    ESP_LOGI(kTag, "cover %s: %ux%u -> %dx%d (descale 1/%d)", book.cover_href.c_str(),
             static_cast<unsigned>(jd.width), static_cast<unsigned>(jd.height), out_w, out_h,
             1 << scale);
    return true;
}

}  // namespace epub_reader
