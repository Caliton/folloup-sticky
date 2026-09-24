#ifndef EPAPER_UI_FONT_RENDERER_H_
#define EPAPER_UI_FONT_RENDERER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "design_tokens.h"

namespace epaper_ui {

using DrawPixelFn = std::function<void(int x, int y, uint8_t color)>;

int MeasureText(design::TypographyRole role, std::string_view text);
int LineHeight(design::TypographyRole role);

// Largest position <= `pos` that does not fall inside a UTF-8 multi-byte sequence.
size_t Utf8Boundary(std::string_view text, size_t pos);
// Byte position of the code point after the one starting at `pos`.
size_t Utf8Next(std::string_view text, size_t pos);

// Returns `text` if it fits `max_width`; otherwise the longest UTF-8-safe prefix that
// fits, followed by "..." when `ellipsis` is set. Empty if not even "..." fits.
std::string FitText(design::TypographyRole role,
                    std::string_view text,
                    int max_width,
                    bool ellipsis = true);
void DrawText(const DrawPixelFn& draw_pixel,
              int x,
              int y,
              std::string_view text,
              uint8_t color,
              design::TypographyRole role);

}  // namespace epaper_ui

#endif  // EPAPER_UI_FONT_RENDERER_H_
