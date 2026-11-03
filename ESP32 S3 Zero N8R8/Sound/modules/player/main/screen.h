/* Экран проигрывателя на HDMI Tang (screen.cpp, PLAYER 1.26).
 *
 * Экран — оверлей 1 Tang, который ведёт сам PLAYER через core->link_write: размер, место,
 * масштаб и прозрачность задаёт PLAYER, прошивка ESP о плеере не знает. Шаг экрана — из задачи
 * обложек (cover_glue.cpp): своей задачи нет, внутренней памяти ESP на её стек не хватает.
 */
#ifndef PLAYER_SCREEN_H
#define PLAYER_SCREEN_H

#include <stdint.h>

#include "zm_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Флаги экрана для плагина (шестой байт ответа AUDIO_OPEN, 33-й — AUDIO_STATUS; плагин 1.21):
 * экран сейчас на HDMI (появляется, показан или уходит) и Tang вообще может его показать. */
#define SCREEN_F_SHOWN 1
#define SCREEN_F_TANG 2

int screen_begin(const zm_core_t* core);   /* ZM_OK — экран будет, когда Tang сможет */
/* Шаг из задачи обложек: выход — через сколько мс звать снова. */
uint32_t screen_tick(uint32_t now_ms);
void screen_end(void);                      /* экран — прочь, память — назад */
uint8_t screen_flags(void);                 /* SCREEN_F_* */

/* Память экрана (1.31). Декодер важнее экрана: банк MIDI TimGM6mb (5,8 МБ) оставлял плееру
 * ~0,9 МБ PSRAM, а экрану нужно 0,7 МБ одним куском (03.11.2026: MIDI не играл, а после него и
 * экрана не было ни на одном формате, пока банк лежал в PSRAM).
 * - screen_bytes — сколько экран просит одним куском;
 * - screen_holds — экран держит её сейчас;
 * - screen_yield — из любой задачи, кроме задачи обложек: экран сразу прочь, память — назад
 *   (это делает задача экрана; ждать её не дольше wait_ms), и не показываться до screen_resume.
 *   midi — уступил ради MIDI: следующий MIDI с тем же банком места экрану тоже не даст.
 *   Выход 1 — экран держал память и отдал её;
 * - screen_resume — новый файл (AUDIO_OPEN): экрану снова можно, кроме MIDI после MIDI;
 *   выход 1 — экран будет показываться. */
uint32_t screen_bytes(void);
int screen_holds(void);
int screen_yield(int midi, uint32_t wait_ms);
int screen_resume(int midi);

/* Снимок для экрана (player.c): ZM_OK — есть. */
int player_ui_model(zm_ui_model_t* model);

#ifdef __cplusplus
}
#endif

#endif
