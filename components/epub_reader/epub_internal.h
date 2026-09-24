#ifndef EPUB_INTERNAL_H_
#define EPUB_INTERNAL_H_

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <strings.h>

#include "epub_reader.h"

namespace epub_reader {

bool ReadZipDirectory(const char* path, PVector<ZipEntry>* entries);
const ZipEntry* FindEntry(const PVector<ZipEntry>& entries, const PString& name);
bool ExtractEntry(const char* path, const ZipEntry& entry, PVector<uint8_t>* out);

// Appends the UTF-8 encoding of `cp` to `out`.
void AppendUtf8(uint32_t cp, PString* out);
// Decodes the entity whose name/number sits between '&' and ';' (exclusive). Returns false
// for unknown names so the caller can keep the raw text.
bool DecodeEntity(const char* begin, const char* end, PString* out);

}  // namespace epub_reader

#endif  // EPUB_INTERNAL_H_
