#include "epub_internal.h"

#include <cstdlib>
#include <cstring>

namespace epub_reader {
namespace {

struct NamedEntity {
    const char* name;
    uint16_t code_point;
};

// XML's five plus the HTML names EPUB producers commonly emit (Latin-1 letters and
// typographic punctuation). Numeric references cover everything else.
constexpr NamedEntity kEntities[] = {
    {"amp", '&'},       {"lt", '<'},         {"gt", '>'},        {"quot", '"'},
    {"apos", '\''},     {"nbsp", 0xA0},      {"shy", 0xAD},      {"copy", 0xA9},
    {"reg", 0xAE},      {"deg", 0xB0},       {"laquo", 0xAB},    {"raquo", 0xBB},
    {"middot", 0xB7},   {"iexcl", 0xA1},     {"iquest", 0xBF},   {"ordf", 0xAA},
    {"ordm", 0xBA},     {"sect", 0xA7},      {"para", 0xB6},     {"euro", 0x20AC},
    {"ndash", 0x2013},  {"mdash", 0x2014},   {"hellip", 0x2026}, {"lsquo", 0x2018},
    {"rsquo", 0x2019},  {"ldquo", 0x201C},   {"rdquo", 0x201D},  {"bdquo", 0x201E},
    {"bull", 0x2022},   {"thinsp", 0x2009},  {"ensp", 0x2002},   {"emsp", 0x2003},
    {"Agrave", 0xC0},   {"Aacute", 0xC1},    {"Acirc", 0xC2},    {"Atilde", 0xC3},
    {"Auml", 0xC4},     {"Ccedil", 0xC7},    {"Egrave", 0xC8},   {"Eacute", 0xC9},
    {"Ecirc", 0xCA},    {"Euml", 0xCB},      {"Igrave", 0xCC},   {"Iacute", 0xCD},
    {"Icirc", 0xCE},    {"Iuml", 0xCF},      {"Ntilde", 0xD1},   {"Ograve", 0xD2},
    {"Oacute", 0xD3},   {"Ocirc", 0xD4},     {"Otilde", 0xD5},   {"Ouml", 0xD6},
    {"Ugrave", 0xD9},   {"Uacute", 0xDA},    {"Ucirc", 0xDB},    {"Uuml", 0xDC},
    {"agrave", 0xE0},   {"aacute", 0xE1},    {"acirc", 0xE2},    {"atilde", 0xE3},
    {"auml", 0xE4},     {"ccedil", 0xE7},    {"egrave", 0xE8},   {"eacute", 0xE9},
    {"ecirc", 0xEA},    {"euml", 0xEB},      {"igrave", 0xEC},   {"iacute", 0xED},
    {"icirc", 0xEE},    {"iuml", 0xEF},      {"ntilde", 0xF1},   {"ograve", 0xF2},
    {"oacute", 0xF3},   {"ocirc", 0xF4},     {"otilde", 0xF5},   {"ouml", 0xF6},
    {"ugrave", 0xF9},   {"uacute", 0xFA},    {"ucirc", 0xFB},    {"uuml", 0xFC},
};

bool NameIs(const char* begin, size_t len, const char* name)
{
    return std::strlen(name) == len && std::strncmp(begin, name, len) == 0;
}

// Tags whose content is never reading text.
bool IsSkippedElement(const PString& name)
{
    return name == "head" || name == "style" || name == "script" || name == "svg" ||
           name == "title" || name == "math";
}

// Tags that end the current paragraph.
bool IsBlockElement(const PString& name)
{
    static constexpr const char* kBlocks[] = {
        "p",  "div", "br", "li", "blockquote", "section", "article", "tr", "hr",
        "dd", "dt",  "pre", "figcaption", "aside", "header", "footer", "td", "table",
    };
    for (const char* b : kBlocks) {
        if (name == b) {
            return true;
        }
    }
    return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

bool IsHeading(const PString& name)
{
    return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

class BlockBuilder {
public:
    explicit BlockBuilder(PVector<Block>* blocks) : blocks_(blocks) {}

    void AddText(const char* p, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            AddChar(p[i]);
        }
    }

    void AddChar(char c)
    {
        const bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
        if (space) {
            pending_space_ = !current_.empty();
            return;
        }
        if (pending_space_) {
            current_.push_back(' ');
            pending_space_ = false;
        }
        current_.push_back(c);
    }

    void AddUtf8(const PString& s)
    {
        for (char c : s) {
            AddChar(c);
        }
    }

    void Flush()
    {
        pending_space_ = false;
        if (!current_.empty()) {
            blocks_->push_back({.kind = heading_ ? BlockKind::kHeading : BlockKind::kParagraph,
                                .text = std::move(current_)});
            current_.clear();
        }
        heading_ = false;
    }

    void SetHeading(bool heading) { heading_ = heading_ || heading; }

private:
    PVector<Block>* blocks_;
    PString current_;
    bool pending_space_ = false;
    bool heading_ = false;
};

}  // namespace

void AppendUtf8(uint32_t cp, PString* out)
{
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool DecodeEntity(const char* begin, const char* end, PString* out)
{
    const size_t len = static_cast<size_t>(end - begin);
    if (len == 0 || len > 10) {
        return false;
    }
    if (*begin == '#') {
        char buf[12] = {};
        std::memcpy(buf, begin + 1, len - 1);
        const bool hex = buf[0] == 'x' || buf[0] == 'X';
        char* parse_end = nullptr;
        const unsigned long cp = std::strtoul(hex ? buf + 1 : buf, &parse_end, hex ? 16 : 10);
        if (parse_end == buf || (parse_end != nullptr && *parse_end != '\0') || cp == 0) {
            return false;
        }
        AppendUtf8(static_cast<uint32_t>(cp), out);
        return true;
    }
    for (const NamedEntity& e : kEntities) {
        if (NameIs(begin, len, e.name)) {
            AppendUtf8(e.code_point, out);
            return true;
        }
    }
    return false;
}

void HtmlToBlocks(const char* html, size_t size, PVector<Block>* blocks)
{
    blocks->clear();
    BlockBuilder builder(blocks);
    const char* p = html;
    const char* end = html + size;
    int skip_depth = 0;
    PString name;
    PString entity;

    // Start at <body> when there is one; everything before it is metadata.
    const char* body = nullptr;
    for (const char* s = p; s + 5 < end; ++s) {
        if (s[0] == '<' && (s[1] == 'b' || s[1] == 'B') && strncasecmp(s + 1, "body", 4) == 0) {
            body = s;
            break;
        }
    }
    if (body != nullptr) {
        p = body;
    }

    while (p < end) {
        if (*p == '<') {
            if (end - p >= 4 && std::strncmp(p, "<!--", 4) == 0) {
                const char* close = nullptr;
                for (const char* s = p + 4; s + 2 < end; ++s) {
                    if (s[0] == '-' && s[1] == '-' && s[2] == '>') {
                        close = s + 3;
                        break;
                    }
                }
                p = close != nullptr ? close : end;
                continue;
            }
            const char* gt = static_cast<const char*>(std::memchr(p, '>', end - p));
            if (gt == nullptr) {
                break;
            }
            const char* q = p + 1;
            const bool closing = q < gt && *q == '/';
            if (closing) {
                ++q;
            }
            const char* name_begin = q;
            while (q < gt && std::isalnum(static_cast<unsigned char>(*q))) {
                ++q;
            }
            name.assign(name_begin, q - name_begin);
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const bool self_closing = gt > p && gt[-1] == '/';

            if (IsSkippedElement(name)) {
                if (!closing && !self_closing) {
                    ++skip_depth;
                } else if (closing && skip_depth > 0) {
                    --skip_depth;
                }
            } else if (skip_depth == 0) {
                if (IsBlockElement(name)) {
                    builder.Flush();
                    if (!closing && IsHeading(name)) {
                        builder.SetHeading(true);
                    }
                    if (!closing && name == "li") {
                        builder.AddUtf8(PString("\xE2\x80\xA2 "));  // bullet
                    }
                } else if (name == "img" && !closing) {
                    // Images cannot be shown; leave no gap in the text flow.
                }
            }
            p = gt + 1;
            continue;
        }
        if (skip_depth > 0) {
            ++p;
            continue;
        }
        if (*p == '&') {
            const char* semi = static_cast<const char*>(std::memchr(p, ';', std::min<ptrdiff_t>(end - p, 12)));
            if (semi != nullptr) {
                entity.clear();
                if (DecodeEntity(p + 1, semi, &entity)) {
                    // A decoded no-break space still separates words for wrapping.
                    if (entity == "\xC2\xA0") {
                        builder.AddChar(' ');
                    } else if (entity != "\xC2\xAD") {  // drop soft hyphens
                        builder.AddUtf8(entity);
                    }
                    p = semi + 1;
                    continue;
                }
            }
        }
        builder.AddChar(*p);
        ++p;
    }
    builder.Flush();
}

}  // namespace epub_reader
