#ifndef CORES_UI_H
#define CORES_UI_H

#include <stddef.h>
#include <stdint.h>
#include "cui_card.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { CUI_WIDTH = 640, CUI_HEIGHT = 360, CUI_MAX_CORES = 32 };
typedef struct {
    uint16_t *px;
    uint16_t w, h, stride;
} cui_canvas_t;
/* Полуоткрытые границы, оба x чётные. */
typedef struct {
    uint16_t x0, y0, x1, y1;
} cui_rect_t;
typedef struct {
    char name[16];
    uint32_t size_bytes;
    uint8_t own_esp_layer;
    /* Проверенная карточка в arena владельца; NULL — общий рисунок и тексты. */
    const cui_card_t *card;
} cui_core_t;

typedef enum {
    CUI_CONNECTING,
    CUI_READING,
    CUI_MENU,
    CUI_STARTING_ESP,
    CUI_COPYING_CACHE,
    CUI_LOADING,
    CUI_PROGRAMMING,
    CUI_ERROR
} cui_state_t;

typedef enum {
    CUI_ERR_NO_CORES,
    CUI_ERR_NO_SD,
    CUI_ERR_ESP_LAYER,
    CUI_ERR_TIMEOUT,
    CUI_ERR_CORE,
    CUI_ERR_ESP_LOST,
    CUI_ERR_ESP_TEXT,
    CUI_ERR_UPDATE_NES,
    CUI_ERR_NO_ESP,
    CUI_ERR_NO_ANSWER
} cui_error_t;

typedef struct {
    cui_core_t cores[CUI_MAX_CORES];
    uint8_t count, selected;
    cui_state_t state;
    cui_error_t error;
    /* -1 — неизвестно, 0…100 — только реальный процент из обвязки. */
    int16_t progress_percent;
    /* Текст ASCII; 60 знаков + нуль, используется при CUI_ERR_ESP_TEXT. */
    char error_text[61];
    /* Увеличивать при изменении списка, строк и других статических данных. */
    uint32_t revision;
} cui_model_t;

/* Один экземпляр, последовательные вызовы. Единственный кэш выделяется в init.
   Повторный init переиспользует кэш; память остаётся у владельца alloc. */
int cui_init(cui_canvas_t *canvas, void *(*alloc)(size_t size, int psram, void *ctx), void *ctx);
/* При ошибочном холсте/модели/dirty/max_dirty возвращает 0 без изменения.
   full — восстановить весь холст. Не отправляет данные и не читает клавиши. */
int cui_render(const cui_model_t *model, uint32_t now_ms, int full, cui_canvas_t *canvas,
               cui_rect_t *dirty, int max_dirty);
/* Забыть кэш без освобождения: владелец затем освобождает сохранённый указатель. */
void cui_reset(void);

#ifdef __cplusplus
}
#endif
#endif
