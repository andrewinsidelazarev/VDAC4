#pragma once
#include "../cui_card.h"
namespace cui_art {
using Sprite = cui_card_image_t;
extern const uint16_t palette[24];
extern const Sprite sprites[2];
extern const uint16_t bg_x[640], bg_y[360], bg_color[1456];
}
