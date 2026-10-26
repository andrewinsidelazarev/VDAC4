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

/* Снимок для экрана (player.c): ZM_OK — есть. */
int player_ui_model(zm_ui_model_t* model);

#ifdef __cplusplus
}
#endif

#endif
