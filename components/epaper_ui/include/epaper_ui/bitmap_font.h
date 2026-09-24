#ifndef BITMAP_FONT_H
#define BITMAP_FONT_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace epaper_font {

struct GlyphBitmap {
    uint32_t bitmap_offset;
    uint16_t bitmap_byte_count;
    uint8_t width;
    uint8_t height;
    int16_t bearing_x;
    int16_t bearing_y;
    uint8_t advance;
};

struct BitmapFont {
    const char* name;
    uint8_t first_char;
    uint8_t last_char;
    uint8_t line_height;
    uint8_t ascent;
    const GlyphBitmap* glyphs;
    const uint8_t* bitmaps;
};

// Decodes the UTF-8 code point starting at `*index` and advances `*index` past it.
// Malformed or truncated sequences consume one byte and yield U+FFFD.
uint32_t DecodeUtf8(std::string_view text, size_t* index);

// Returns the byte length of the UTF-8 sequence that starts with `lead`
// (1 for ASCII, continuation bytes, and invalid leads).
size_t Utf8SequenceLength(unsigned char lead);

const GlyphBitmap* FindGlyph(const BitmapFont& font, uint32_t code_point);
int MeasureText(const BitmapFont& font, std::string_view text, int tracking = 0);

}  // namespace epaper_font

#endif  // BITMAP_FONT_H
