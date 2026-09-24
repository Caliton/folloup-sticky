#include "epaper_ui/text_layout.h"

namespace epaper_ui {

int MeasureWord(design::TypographyRole role, std::string_view word)
{
    return MeasureText(role, word);
}

}  // namespace epaper_ui
