#pragma once

#include <stddef.h>
#include <stdint.h>
#include "zm_app.h"

// Модель ABI 1.5 используется непосредственно, без копии и дополнительных полей.
using pui_model_t = zm_ui_model_t;

namespace zifi {
namespace player_view {
constexpr uint16_t kWidth = 640;
constexpr uint16_t kHeight = 360;
constexpr uint16_t kCoverSide = 192;

struct Rect { uint16_t x0, y0, x1, y1; };
using Allocate = void* (*)(void* context, size_t bytes);
using Release = void (*)(void* context, void* pointer);

// Один экземпляр; begin/end вызываются владельцем экрана. Кэш — через allocate.
bool begin(Allocate allocate, Release release, void* context);
void end();
// Холст RGB565, шаг 640. При maxDirty <= 0 холст и состояние не изменяются.
// Если слотов не хватает, оставшиеся области объединяются в последнем слоте.
int render(const zm_ui_model_t& model, const uint16_t* cover, bool coverChanged,
           bool full, uint32_t nowMs, uint16_t* canvas, Rect* dirty, int maxDirty);
}
}
