/* Спектр звука для окна проигрывателя (view.c): 32 полосы по 12 уровней.
 *
 * Задача плеера отдаёт сюда только принятые PCM-выводом отсчёты (view_feed),
 * каждые 50 мс звукового потока сохраняются полосы: БПФ на 512 точек по последним
 * отсчётам (моно, окно Ханна), полосы — от 86 Гц до 22 кГц в логарифмическом
 * масштабе, уровень — по 4 дБ на ступень. Z80 забирает полосы командой
 * AUDIO_VIEW (view_bands) по позиции воспроизведения: принятые отсчёты минус
 * pcm_queued_ms (кольцо и DMA). На паузе снимок неподвижен; смена файла,
 * перемотка и смена частоты сбрасывают историю. Замок спектра не связан с
 * долгим разбором файла декодером.
 */
#ifndef PLAYER_VIEW_H
#define PLAYER_VIEW_H

#include <stdint.h>

#include "zm_app.h"

#define VIEW_BANDS 32
#define VIEW_LEVELS 12

int view_begin(const zm_core_t* core);          /* ZM_OK — таблицы готовы */
void view_reset(void);                          /* новый файл: тишина */
void view_feed(const float* frames, uint32_t count, uint8_t channels, uint32_t rate);
void view_bands(uint8_t* out, int paused);       /* VIEW_BANDS байтов 0..VIEW_LEVELS */

#endif /* PLAYER_VIEW_H */
