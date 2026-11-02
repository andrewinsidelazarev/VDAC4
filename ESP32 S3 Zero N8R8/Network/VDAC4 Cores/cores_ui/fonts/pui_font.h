#pragma once
#include <stdint.h>
namespace pui_font {
struct Glyph { uint32_t offset; uint8_t width, height; };
struct Font { const uint8_t* pixels; const Glyph* glyphs; uint8_t height; };
extern const Font fonts[3];
}
