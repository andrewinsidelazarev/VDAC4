/* Интернет-радио PLAYER.ELF: разбор плейлиста M3U и текст станций для окна Z80. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_TITLE 64                     /* название станции из #EXTINF (байты как в файле) */
#define RADIO_URL 256                      /* адрес потока */

typedef struct {
  char title[RADIO_TITLE];
  char url[RADIO_URL];
} radio_station_t;

/* Плейлист M3U (M3U8, расширенный #EXTM3U или просто адреса по строкам):
 * станции — строки «http://…» и «https://…», название — из предшествующей
 * #EXTINF:длительность,название (скобки вокруг снимаются). Метка BOM UTF-8,
 * пустые строки, прочие комментарии и локальные пути — мимо; адрес длиннее
 * RADIO_URL − 1 — тоже. Возвращает число станций (до max). */
int radio_parse_m3u(const uint8_t* text, uint32_t size, radio_station_t* out, int max);

/* Текст станции или декодера — в CP866 окна Wild Commander: UTF-8 (если
 * строка им правильна; знак, обрезанный в конце, отбрасывается), иначе
 * 8-битная кириллица — CP866 или CP1251, чьих букв больше (названия в MIDI,
 * модулях и LIST INFO пишут и так, и так). Латиница и кириллица; украинские
 * І, Ї, Є, Ґ — близкими буквами (I, I, E, Г), кавычки и тире — ASCII,
 * прочее — '?'. */
void text_cp866(const char* text, char* out, size_t size);

/* StreamTitle='…'; из блока метаданных ICY: в out (байты как пришли). 1 —
 * название было в блоке (может быть пустым), 0 — нет. */
int radio_stream_title(const char* meta, char* out, size_t size);

#ifdef __cplusplus
}
#endif
