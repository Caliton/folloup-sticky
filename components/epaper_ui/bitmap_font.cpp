#include "epaper_ui/bitmap_font.h"

#include <algorithm>

namespace epaper_font {

namespace {

constexpr uint32_t kReplacementChar = 0xFFFD;

// Typographic characters Gemini output commonly contains that have no Latin-1
// glyph. Map them to the closest glyph the fonts do carry.
uint32_t FoldToLatin1(uint32_t cp) {
    switch (cp) {
        case 0x2018:  // left single quote
        case 0x2019:  // right single quote / apostrophe
        case 0x201A:
        case 0x2032:
            return '\'';
        case 0x201C:  // left double quote
        case 0x201D:  // right double quote
        case 0x201E:
        case 0x2033:
            return '"';
        case 0x2010:  // hyphen
        case 0x2011:
        case 0x2012:
        case 0x2013:  // en dash
        case 0x2014:  // em dash
        case 0x2212:  // minus sign
            return '-';
        case 0x2022:  // bullet
        case 0x2219:
            return 0xB7;  // middle dot
        case 0x2026:  // ellipsis; callers draw it as three '.' glyphs
            return '.';
        case 0x00A0:  // no-break space
        case 0x2002:
        case 0x2003:
        case 0x2009:
        case 0x202F:
            return ' ';
        default:
            return cp;
    }
}

uint32_t NormalizeCodePoint(const BitmapFont& font, uint32_t cp) {
    cp = FoldToLatin1(cp);
    if (cp < font.first_char || cp > font.last_char || (cp >= 127 && cp <= 159)) {
        return '?';
    }
    return cp;
}

}  // namespace

size_t Utf8SequenceLength(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

uint32_t DecodeUtf8(std::string_view text, size_t* index) {
    const size_t start = *index;
    const unsigned char lead = static_cast<unsigned char>(text[start]);
    const size_t length = Utf8SequenceLength(lead);
    if (length == 1) {
        *index = start + 1;
        return lead < 0x80 ? lead : kReplacementChar;
    }
    if (start + length > text.size()) {
        *index = start + 1;
        return kReplacementChar;
    }

    uint32_t cp = lead & (0x7F >> length);
    for (size_t i = 1; i < length; ++i) {
        const unsigned char next = static_cast<unsigned char>(text[start + i]);
        if ((next & 0xC0) != 0x80) {
            *index = start + 1;
            return kReplacementChar;
        }
        cp = (cp << 6) | (next & 0x3F);
    }
    *index = start + length;
    return cp;
}

const GlyphBitmap* FindGlyph(const BitmapFont& font, uint32_t code_point) {
    const uint32_t normalized = NormalizeCodePoint(font, code_point);
    if (normalized < font.first_char || normalized > font.last_char) {
        return nullptr;
    }
    return &font.glyphs[normalized - font.first_char];
}

int MeasureText(const BitmapFont& font, std::string_view text, int tracking) {
    int width = 0;
    bool first = true;

    size_t index = 0;
    while (index < text.size()) {
        const uint32_t cp = DecodeUtf8(text, &index);
        if (cp == '\r' || cp == '\n') {
            break;
        }
        const int repeat = cp == 0x2026 ? 3 : 1;
        const GlyphBitmap* glyph = FindGlyph(font, cp);
        if (glyph == nullptr) {
            continue;
        }
        for (int i = 0; i < repeat; ++i) {
            if (!first) {
                width += std::max(tracking, 0);
            }
            width += glyph->advance;
            first = false;
        }
    }

    return width;
}

}  // namespace epaper_font
