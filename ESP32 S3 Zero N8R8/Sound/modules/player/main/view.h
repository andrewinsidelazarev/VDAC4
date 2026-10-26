/* Спектр звука (view.c): для окна проигрывателя Z80 — 32 полосы по 12 ступеней, для экрана
 * оверлеем Tang (zm_ui_model_t, ABI 1.5) — 64 полосы по 256 уровней.
 *
 * Задача плеера отдаёт сюда только принятые PCM-выводом отсчёты (view_feed), каждые
 * 40 мс звукового потока (кадр экрана, 25 в секунду) сохраняется снимок: БПФ на 1024
 * точки по последним отсчётам (моно, окно Ханна). Полосы окна Z80 — от 43 Гц до половины
 * частоты в логарифмическом масштабе, уровень — по 4 дБ на ступень; полосы экрана — по
 * логарифму от 40 Гц до 20 кГц (не выше половины частоты), уровень 0..255 = −72..0 дБ
 * (0 дБ — синус полной громкости). Снимок берётся по позиции воспроизведения: принятые
 * отсчёты минус pcm_queued_ms (кольцо и DMA) — и AUDIO_VIEW (view_bands), и экран
 * (view_ui_bands) идут по одной истории, кто бы ни спросил первым. На паузе снимок
 * неподвижен; смена файла, перемотка и смена частоты сбрасывают историю. Замок спектра
 * не связан с долгим разбором файла декодером.
 */
#ifndef PLAYER_VIEW_H
#define PLAYER_VIEW_H

#include <stdint.h>

#include "zm_app.h"

#define VIEW_BANDS 32
#define VIEW_LEVELS 12
#define VIEW_UI_BANDS 64

int view_begin(const zm_core_t* core);          /* ZM_OK — таблицы готовы */
void view_reset(void);                          /* новый файл: тишина */
void view_feed(const float* frames, uint32_t count, uint8_t channels, uint32_t rate);
void view_bands(uint8_t* out, int paused);       /* VIEW_BANDS байтов 0..VIEW_LEVELS */
void view_ui_bands(uint8_t* out, int paused);    /* VIEW_UI_BANDS байтов 0..255 */

#endif /* PLAYER_VIEW_H */
