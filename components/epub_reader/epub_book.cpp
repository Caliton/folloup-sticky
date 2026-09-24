#include "epub_internal.h"

#include <cstring>

#include "esp_log.h"

namespace epub_reader {
namespace {

constexpr const char* kTag = "EpubBook";

// --- a deliberately small XML scanner --------------------------------------------------
// container.xml and the OPF are machine-written and shallow; we only need element names
// and attributes, never a DOM.

struct Tag {
    PString name;  // lowercase, namespace prefix stripped ("dc:title" -> "title")
    bool closing = false;
    const char* attrs_begin = nullptr;
    const char* attrs_end = nullptr;
    const char* after = nullptr;  // first byte after '>'
};

bool NextTag(const char* p, const char* end, Tag* tag, const char** tag_start)
{
    while (p < end) {
        const char* lt = static_cast<const char*>(std::memchr(p, '<', end - p));
        if (lt == nullptr) {
            return false;
        }
        const char* gt = static_cast<const char*>(std::memchr(lt, '>', end - lt));
        if (gt == nullptr) {
            return false;
        }
        const char* q = lt + 1;
        if (q < gt && (*q == '?' || *q == '!')) {
            p = gt + 1;
            continue;
        }
        tag->closing = q < gt && *q == '/';
        if (tag->closing) {
            ++q;
        }
        const char* name_begin = q;
        while (q < gt && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && *q != '/') {
            ++q;
        }
        const char* colon = static_cast<const char*>(std::memchr(name_begin, ':', q - name_begin));
        if (colon != nullptr) {
            name_begin = colon + 1;
        }
        tag->name.assign(name_begin, q - name_begin);
        for (char& c : tag->name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        tag->attrs_begin = q;
        tag->attrs_end = gt;
        tag->after = gt + 1;
        if (tag_start != nullptr) {
            *tag_start = lt;
        }
        return true;
    }
    return false;
}

// Value of attribute `name` (case-insensitive, prefix-insensitive) in [begin, end).
PString Attr(const char* begin, const char* end, const char* name)
{
    const size_t name_len = std::strlen(name);
    const char* p = begin;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '/')) {
            ++p;
        }
        const char* key_begin = p;
        while (p < end && *p != '=' && *p != ' ' && *p != '>') {
            ++p;
        }
        const char* key_end = p;
        const char* colon =
            static_cast<const char*>(std::memchr(key_begin, ':', key_end - key_begin));
        if (colon != nullptr) {
            key_begin = colon + 1;
        }
        if (p >= end || *p != '=') {
            continue;
        }
        ++p;
        if (p >= end || (*p != '"' && *p != '\'')) {
            continue;
        }
        const char quote = *p++;
        const char* value_begin = p;
        while (p < end && *p != quote) {
            ++p;
        }
        if (static_cast<size_t>(key_end - key_begin) == name_len &&
            strncasecmp(key_begin, name, name_len) == 0) {
            PString value;
            // Attribute values in OPF can carry entities (&amp; in hrefs).
            for (const char* v = value_begin; v < p; ++v) {
                if (*v == '&') {
                    const char* semi = static_cast<const char*>(std::memchr(v, ';', p - v));
                    if (semi != nullptr && DecodeEntity(v + 1, semi, &value)) {
                        v = semi;
                        continue;
                    }
                }
                value.push_back(*v);
            }
            return value;
        }
        ++p;
    }
    return {};
}

// Text content from `from` up to the next '<', entities decoded, trimmed.
PString InnerText(const char* from, const char* end)
{
    const char* lt = static_cast<const char*>(std::memchr(from, '<', end - from));
    const char* stop = lt != nullptr ? lt : end;
    PString out;
    for (const char* p = from; p < stop; ++p) {
        if (*p == '&') {
            const char* semi = static_cast<const char*>(std::memchr(p, ';', stop - p));
            if (semi != nullptr && DecodeEntity(p + 1, semi, &out)) {
                p = semi;
                continue;
            }
        }
        out.push_back(*p == '\n' || *p == '\r' || *p == '\t' ? ' ' : *p);
    }
    const size_t b = out.find_first_not_of(' ');
    if (b == PString::npos) {
        return {};
    }
    return out.substr(b, out.find_last_not_of(' ') - b + 1);
}

PString UrlDecode(const PString& in)
{
    PString out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            const char hex[3] = {in[i + 1], in[i + 2], 0};
            out.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// Resolves `href` (relative to the OPF's directory) into a normalized zip path.
PString ResolvePath(const PString& base_dir, const PString& href)
{
    PString raw = UrlDecode(href);
    const size_t hash = raw.find('#');
    if (hash != PString::npos) {
        raw.resize(hash);
    }
    PString joined = raw.empty() || raw[0] == '/' ? raw.substr(raw.empty() ? 0 : 1) : base_dir + raw;
    PVector<PString> parts;
    size_t start = 0;
    while (start <= joined.size()) {
        const size_t slash = joined.find('/', start);
        const PString part = joined.substr(start, slash == PString::npos ? PString::npos : slash - start);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == PString::npos) {
            break;
        }
        start = slash + 1;
    }
    PString out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out.push_back('/');
        }
        out += parts[i];
    }
    return out;
}

PString FileStem(const char* path)
{
    const char* slash = std::strrchr(path, '/');
    PString name = slash != nullptr ? slash + 1 : path;
    const size_t dot = name.rfind('.');
    if (dot != PString::npos) {
        name.resize(dot);
    }
    for (char& c : name) {
        if (c == '_' || c == '-') {
            c = ' ';
        }
    }
    return name;
}

bool ExtractText(const Book& book, const PString& name, PVector<uint8_t>* out)
{
    const ZipEntry* entry = FindEntry(book.entries, name);
    return entry != nullptr && ExtractEntry(book.path.c_str(), *entry, out);
}

struct ManifestItem {
    PString id;
    PString href;
    PString media_type;
    PString properties;
};

}  // namespace

bool OpenBook(const char* path, Book* book, PString* error)
{
    *book = {};
    book->path = path;
    const auto fail = [&](const char* reason) {
        if (error != nullptr) {
            *error = reason;
        }
        ESP_LOGW(kTag, "%s: %s", path, reason);
        return false;
    };

    if (!ReadZipDirectory(path, &book->entries)) {
        return fail("Arquivo EPUB inválido");
    }

    PVector<uint8_t> xml;
    if (!ExtractText(*book, "META-INF/container.xml", &xml)) {
        return fail("EPUB sem container.xml");
    }
    PString opf_path;
    {
        const char* p = reinterpret_cast<const char*>(xml.data());
        const char* end = p + xml.size();
        Tag tag;
        while (NextTag(p, end, &tag, nullptr)) {
            if (!tag.closing && tag.name == "rootfile") {
                opf_path = Attr(tag.attrs_begin, tag.attrs_end, "full-path");
                break;
            }
            p = tag.after;
        }
    }
    if (opf_path.empty() || !ExtractText(*book, opf_path, &xml)) {
        return fail("EPUB sem índice (OPF)");
    }
    const size_t last_slash = opf_path.rfind('/');
    const PString base_dir =
        last_slash == PString::npos ? PString{} : opf_path.substr(0, last_slash + 1);

    PVector<ManifestItem> manifest;
    PVector<PString> spine_ids;
    PString cover_id;
    const char* p = reinterpret_cast<const char*>(xml.data());
    const char* end = p + xml.size();
    Tag tag;
    while (NextTag(p, end, &tag, nullptr)) {
        p = tag.after;
        if (tag.closing) {
            continue;
        }
        if (tag.name == "title" && book->title.empty()) {
            book->title = InnerText(tag.after, end);
        } else if (tag.name == "creator" && book->author.empty()) {
            book->author = InnerText(tag.after, end);
        } else if (tag.name == "meta") {
            if (Attr(tag.attrs_begin, tag.attrs_end, "name") == "cover") {
                cover_id = Attr(tag.attrs_begin, tag.attrs_end, "content");
            }
        } else if (tag.name == "item") {
            manifest.push_back({
                .id = Attr(tag.attrs_begin, tag.attrs_end, "id"),
                .href = Attr(tag.attrs_begin, tag.attrs_end, "href"),
                .media_type = Attr(tag.attrs_begin, tag.attrs_end, "media-type"),
                .properties = Attr(tag.attrs_begin, tag.attrs_end, "properties"),
            });
        } else if (tag.name == "itemref") {
            if (Attr(tag.attrs_begin, tag.attrs_end, "linear") != "no") {
                spine_ids.push_back(Attr(tag.attrs_begin, tag.attrs_end, "idref"));
            }
        }
    }

    for (const PString& id : spine_ids) {
        for (const ManifestItem& item : manifest) {
            if (item.id == id) {
                book->spine.push_back(ResolvePath(base_dir, item.href));
                break;
            }
        }
    }
    if (book->spine.empty()) {
        return fail("EPUB sem capítulos");
    }

    // EPUB 3 marks the cover with properties="cover-image"; EPUB 2 with <meta name="cover">.
    for (const ManifestItem& item : manifest) {
        const bool is_cover = item.properties.find("cover-image") != PString::npos ||
                              (!cover_id.empty() && item.id == cover_id);
        if (is_cover && item.media_type.rfind("image/", 0) == 0) {
            book->cover_href = ResolvePath(base_dir, item.href);
            break;
        }
    }
    if (book->title.empty()) {
        book->title = FileStem(path);
    }
    ESP_LOGI(kTag, "opened %s: \"%s\" by \"%s\", %u chapters, cover=%s", path,
             book->title.c_str(), book->author.c_str(), static_cast<unsigned>(book->spine.size()),
             book->cover_href.empty() ? "-" : book->cover_href.c_str());
    return true;
}

bool LoadChapter(const Book& book, size_t spine_index, PVector<Block>* blocks)
{
    blocks->clear();
    if (spine_index >= book.spine.size()) {
        return false;
    }
    PVector<uint8_t> html;
    if (!ExtractText(book, book.spine[spine_index], &html)) {
        return false;
    }
    HtmlToBlocks(reinterpret_cast<const char*>(html.data()), html.size(), blocks);
    return true;
}

}  // namespace epub_reader
