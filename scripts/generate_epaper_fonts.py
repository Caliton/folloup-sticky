#!/usr/bin/env python3
"""Generate packed 1-bit bitmap fonts for the e-paper UI from TTF files.

Cross-platform (FreeType via freetype-py): `python -m pip install freetype-py`.

Each font covers a contiguous code point range starting at 32. Text fonts go up
to 255 so Latin-1 accented letters (pt-BR: á â ã à ç é ê í ó ô õ ú ü) render;
the renderer decodes UTF-8 and indexes glyphs by code point.

Spec format: <font-path>:<symbol>:<size>[:<last-code-point>]  (default last=255)
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import freetype
except ImportError:  # pragma: no cover - environment guard
    raise SystemExit("freetype-py is required: python -m pip install freetype-py")


kFirstChar = 32
kDefaultLastChar = 255


@dataclass(frozen=True)
class FontSpec:
    path: str
    symbol: str
    size: float
    last_char: int


@dataclass(frozen=True)
class GlyphRecord:
    width: int
    height: int
    bearing_x: int
    bearing_y: int
    advance: int
    bitmap: list[int]


class FontGenError(RuntimeError):
    pass


def _is_number(text: str) -> bool:
    try:
        float(text)
    except ValueError:
        return False
    return True


def parse_args(argv: list[str]) -> tuple[Path, list[FontSpec]]:
    if len(argv) < 4 or argv[1] != "--output":
        raise FontGenError(
            "usage: generate_epaper_fonts.py --output <file> "
            "<font-path:symbol:size[:last]>..."
        )

    output_path = Path(argv[2])
    specs: list[FontSpec] = []
    for raw in argv[3:]:
        # Split from the right so Windows drive letters (C:\...) survive.
        last_char = kDefaultLastChar
        parts = raw.rsplit(":", 3)
        if len(parts) == 4 and parts[3].isdigit() and _is_number(parts[2]):
            path, symbol, size_text, last_text = parts
            last_char = int(last_text)
        else:
            parts = raw.rsplit(":", 2)
            if len(parts) != 3:
                raise FontGenError(f"invalid font spec: {raw}")
            path, symbol, size_text = parts
        if not _is_number(size_text):
            raise FontGenError(f"invalid font size in spec: {raw}")
        size = float(size_text)
        if not kFirstChar <= last_char <= 255:
            raise FontGenError(f"last code point must be in 32..255: {raw}")
        specs.append(FontSpec(path=path, symbol=symbol, size=size, last_char=last_char))

    return output_path, specs


def load_face(spec: FontSpec) -> freetype.Face:
    try:
        face = freetype.Face(spec.path)
    except freetype.FT_Exception as exc:
        raise FontGenError(f"unable to load font: {spec.path}") from exc
    # 72 dpi so the size is in pixels, matching the original CoreText generator.
    face.set_char_size(int(round(spec.size * 64)), 0, 72, 72)
    return face


def pack_bitmap(buffer: bytes, width: int, height: int, pitch: int) -> list[int]:
    packed: list[int] = []
    current_byte = 0
    bit_index = 0

    for row in range(height):
        row_base = row * pitch
        for col in range(width):
            byte = buffer[row_base + (col >> 3)]
            if byte & (0x80 >> (col & 7)):
                current_byte |= 0x80 >> bit_index
            bit_index += 1
            if bit_index == 8:
                packed.append(current_byte)
                current_byte = 0
                bit_index = 0

    if bit_index:
        packed.append(current_byte)
    return packed


def render_glyph(face: freetype.Face, code: int) -> GlyphRecord:
    # C1 control range has no glyphs; keep the slot so indexing stays contiguous.
    if 127 <= code <= 159:
        return GlyphRecord(0, 0, 0, 0, 0, [])

    glyph_index = face.get_char_index(code)
    if glyph_index == 0:
        glyph_index = face.get_char_index(ord("?"))
        if glyph_index == 0:
            raise FontGenError("font is missing fallback glyph '?'")

    face.load_glyph(
        glyph_index,
        freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO | freetype.FT_LOAD_MONOCHROME,
    )
    slot = face.glyph
    bitmap = slot.bitmap
    advance = int(round(slot.advance.x / 64.0))

    width = bitmap.width
    height = bitmap.rows
    if width == 0 or height == 0:
        return GlyphRecord(0, 0, slot.bitmap_left, slot.bitmap_top, advance, [])

    if width > 255 or height > 255 or advance > 255:
        raise FontGenError(f"glyph U+{code:04X} exceeds uint8 metrics")

    packed = pack_bitmap(bytes(bitmap.buffer), width, height, bitmap.pitch)
    return GlyphRecord(
        width=width,
        height=height,
        bearing_x=slot.bitmap_left,
        bearing_y=slot.bitmap_top,
        advance=advance,
        bitmap=packed,
    )


def emit_byte_array(values: list[int], indent: str) -> str:
    if not values:
        return ""

    lines: list[str] = []
    line = indent
    for index, value in enumerate(values):
        entry = f"0x{value:02X}"
        if index == 0:
            line += entry
        elif len(line) + len(entry) + 2 > 100:
            lines.append(line + ",")
            line = indent + entry
        else:
            line += ", " + entry
    lines.append(line)
    return "\n".join(lines)


def generate_source(specs: list[FontSpec]) -> str:
    out = [
        '#include "epaper_ui/generated_epaper_fonts.h"',
        "",
        "namespace epaper_fonts {",
        "",
    ]

    for spec in specs:
        face = load_face(spec)
        metrics = face.size
        ascent = max(1, (metrics.ascender + 63) // 64)
        descent = max(0, (-metrics.descender + 63) // 64)
        line_height = max(1, (metrics.height + 63) // 64, ascent + descent)

        all_bitmap_bytes: list[int] = []
        glyph_lines: list[str] = []
        for code in range(kFirstChar, spec.last_char + 1):
            glyph = render_glyph(face, code)
            offset = len(all_bitmap_bytes)
            all_bitmap_bytes.extend(glyph.bitmap)
            glyph_lines.append(
                "    { %5u, %4u, %3u, %3u, %4d, %4d, %3u },"
                % (
                    offset,
                    len(glyph.bitmap),
                    glyph.width,
                    glyph.height,
                    glyph.bearing_x,
                    glyph.bearing_y,
                    glyph.advance,
                )
            )

        out.extend(
            [
                "namespace {",
                f"const epaper_font::GlyphBitmap {spec.symbol}_glyphs[] = {{",
                *glyph_lines,
                "};",
                "",
                f"const uint8_t {spec.symbol}_bitmaps[] = {{",
            ]
        )
        if all_bitmap_bytes:
            out.append(emit_byte_array(all_bitmap_bytes, indent="    "))
        out.extend(
            [
                "};",
                "}  // namespace",
                "",
                f"const epaper_font::BitmapFont {spec.symbol} = {{",
                f'    "{spec.symbol}",',
                f"    {kFirstChar},",
                f"    {spec.last_char},",
                f"    {line_height},",
                f"    {ascent},",
                f"    {spec.symbol}_glyphs,",
                f"    {spec.symbol}_bitmaps,",
                "};",
                "",
            ]
        )

    out.append("}  // namespace epaper_fonts")
    out.append("")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    try:
        output_path, specs = parse_args(argv)
        source = generate_source(specs)
        output_path.write_text(source, encoding="utf-8", newline="\n")
        return 0
    except FontGenError as exc:
        sys.stderr.write(f"font generation failed: {exc}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
