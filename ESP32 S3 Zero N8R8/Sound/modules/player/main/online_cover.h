/* Обложка из сети (PLAYER 1.21): трек без своей обложки (нет ни в метке, ни в папке, ни у
 * декодера) и песня радио — по исполнителю и названию в Deezer API (api.deezer.com/search, без
 * ключа, HTTPS). Берётся обложка альбома первой найденной записи (cover_big, 500×500 JPEG),
 * если исполнитель записи совпал с исполнителем трека, — иначе обложки нет: чужая хуже
 * никакой. Наружу уходят только исполнитель и название.
 */
#ifndef PLAYER_ONLINE_COVER_H
#define PLAYER_ONLINE_COVER_H

#include <stddef.h>
#include <stdint.h>

#include "zm_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_COVER_MAX (1024u * 1024u)   /* больше картинку не берём */

/* Адрес поиска: исполнитель и название (CP866) — в UTF-8 и в запрос. 0 — не вышло. */
size_t online_cover_search_url(const char* artist, const char* title, char* url, size_t size);
/* Ответ поиска (JSON): адрес обложки альбома первой записи, если её исполнитель совпал с
 * artist (CP866; без регистра, пробелов и знаков) — 1; иначе 0. */
int online_cover_pick(const char* json, size_t length, const char* artist, char* url, size_t size);
/* Найти и скачать: картинка (core->alloc, PSRAM; освобождать core->free) и её длина, или
 * NULL. Ждёт сеть до ~20 с; *cancel ≠ 0 — бросить. Звать из своей задачи (net_*). */
uint8_t* online_cover_fetch(const zm_core_t* core, const char* artist, const char* title,
                            size_t* size, const volatile int* cancel);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_ONLINE_COVER_H */
