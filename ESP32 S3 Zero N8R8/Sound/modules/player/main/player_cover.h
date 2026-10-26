/* Обложка и метки MP3 в PLAYER.ELF (cover_glue.cpp).
 *
 * Разбор метки ID3 и сведение миниатюры — те же исходники, что у прошивки
 * (src/cover_art.cpp, src/cover_jpeg.cpp); JPEG сводит TJpgDec из ПЗУ ESP32-S3
 * в своей задаче обложки. Команды Z80 — AUDIO_COVER и AUDIO_THUMB (Docs/VDAC4.md,
 * «Обложка MP3»).
 */
#ifndef PLAYER_COVER_H
#define PLAYER_COVER_H

#include <stddef.h>
#include <stdint.h>

#include "zm_app.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { COVER_WANT = 0, COVER_RECEIVED = 1, COVER_NONE = 2 };     /* итог AUDIO_COVER */
enum { THUMB_PENDING = 0, THUMB_READY = 1, THUMB_NONE = 2 };     /* итог AUDIO_THUMB */
#define COVER_THUMB_PART 512
#define COVER_THUMB_PARTS 4
#define COVER_PICTURE_MAX (1024u * 1024u)   /* больше картинку файлом не берём */

/* Метки в CP866; пустая строка — кадра нет. */
typedef struct {
  char title[64];
  char artist[64];
  char album[64];
  char year[8];
  char track[8];
} player_tags_t;

int cover_begin(const zm_core_t* core);          /* ZM_OK — обложка будет */
void cover_end(void);
void cover_reset(void);                          /* новый файл */
/* Кусок файла с offset → итог (COVER_*), откуда слать дальше — в *want. */
uint8_t cover_feed(uint32_t offset, const uint8_t* data, size_t length, uint32_t* want);
/* Картинка файлом (cover.jpg в папке трека, AUDIO_PICTURE): кусок data с
 * offset из total байтов → итог (COVER_WANT — шли дальше, COVER_RECEIVED —
 * собрана и сводится, COVER_NONE — не взята); принято всего — в *received. */
uint8_t cover_picture(uint32_t total, uint32_t offset, const uint8_t* data, size_t length,
                      uint32_t* received);
/* Картинка, которую нашёл в файле декодер (Ogg: METADATA_BLOCK_PICTURE), —
 * своей копией в сведение. 1 — взята (JPEG), 0 — нет (PNG и прочее TJpgDec не
 * сводит). Можно звать из задачи декодера. */
int cover_image(const uint8_t* data, size_t size);
/* То же для картинки из сети (online_job): сводится сразу, в задаче, что её принесла. */
int cover_image_here(const uint8_t* data, size_t size);
/* Часть миниатюры → итог (THUMB_*); готовая — 512 байт в out. */
uint8_t cover_thumb(uint8_t part, uint8_t* out);
void cover_tags(player_tags_t* out);
/* Обложка для экрана оверлеем Tang (ABI 1.5): сводится вместе с миниатюрой, если ядро может
 * показать экран (core->ui_available), — квадрат 256×256 RGB565. cover_ui_serial — номер
 * обложки (меняется с каждой новой; 0 — нет); cover_ui_copy — сведённая к side×side в out:
 * 1 — есть, 0 — нет. */
uint16_t cover_ui_serial(void);
int cover_ui_copy(uint16_t* out, uint16_t side);
/* Для обложки из сети (online_cover.c): номер картинки — меняется с новым файлом и с каждой
 * новой картинкой к сведению (старый номер — результат поиска опоздал); есть ли у трека своя
 * обложка или она в пути (метка досылается, картинка из папки идёт, сводится, готова);
 * убрать обложку экрана (у радио — песня сменилась). */
uint32_t cover_epoch(void);
int cover_present(void);
void cover_ui_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_COVER_H */
