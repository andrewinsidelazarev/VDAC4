/* SPDX-License-Identifier: Apache-2.0
 * VIDEO.ELF — слой ESP ядра видео VDAC4 (личность 05, docs/video-core.md): загрузка ядра из кэша
 * BL616, звук ролика (FIFO звука ядра → ЦАП ESP), часы звука для темпа кадров, панель плеера и окна
 * оверлеями Tang. Плеер — плагин FTVIEW4 на Evo: он шлёт файл в ядро по SPI FT812, ведёт клавиши,
 * перемотку и список, а сюда — что рисовать.
 *
 * Общие объявления частей приложения: app.c (команды, загрузка ядра, сеанс), audio.c (звук),
 * avi_info.c (заголовок AVI), hud.cpp (панель и окна).
 */
#ifndef VIDEO_H
#define VIDEO_H

#include <stddef.h>
#include <stdint.h>

#include "zm_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_VERSION "0.5"
#define VIDEO_PERSONALITY 0x05u

/* --- регистры LINK ядра видео (байтовые адреса; ts_fb_core, HAS_VIDEO) ------------------------ */
#define R_VERSION   0x004u      /* {личность 05, протокол 01, версия} */
#define R_SLOTS     0x028u      /* слотов темпа с сброса потока (кадр ролика с повторами) */
#define R_ABORTS    0x02Cu      /* {ширина холста, отменённых кадров} */
#define R_CFG       0x040u      /* CTRL настроек: [14] 16:9, [16:15] размер 100/95/90 % */
#define R_AU_WP     0x100u      /* {эпоха[31:24], пауза[23], слов FIFO звука записано[10:0]} / запись: AU_RP */
#define R_AU_CTL    0x104u      /* [0] приём звука, [1] темп по часам звука, [15:8] эпоха ESP */
#define R_AU_CLK    0x108u      /* время проигранного звука с начала потока, мкс */
#define W_AUDIO     0x2000u     /* окно FIFO звука: 1024 слова, младший байт — первый */
#define R_OVL(k)    (0x0C0u + 16u * (k))   /* оверлей k: CTRL, POS, SIZE, BASE */
#define W_CANVAS    0x80000u    /* окно холстов оверлеев: слово w региона — 0x80000 + 4·w */
#define AUDIO_WORDS 1024u

/* --- состояние, которое шлёт плагин (STATUS) ---------------------------------------------------- */
enum {
  VS_PAUSED = 1,        /* пауза */
  VS_PANEL  = 2,        /* панель видна (15 с после клавиши, на паузе — всегда) */
  VS_END    = 4,        /* ролик кончился */
  VS_SEEKING = 8,       /* идёт перемотка (поток перезапускается) */
  VS_BUSY   = 16,       /* плагин занят (достраивает таблицу перемотки): в панели — текст name */
};

typedef struct {
  uint8_t flags;            /* VS_* */
  uint32_t base_frame;      /* кадр ролика, с которого начат поток (перемотка) */
  uint32_t total_frames;    /* кадров в ролике (0 — неизвестно) */
  uint32_t us_frame;        /* мкс на кадр (avih) */
  uint32_t audio_base;      /* байтов звука в файле до начала потока (индекс перемотки; с начала — 0) */
  char name[64];            /* имя ролика (CP866), для окна списка — текущий; при VS_BUSY — текст панели */
} video_status_t;

/* окно списка: страница строк, выбор, «текущий» */
#define LIST_ROWS 17
typedef struct {
  uint8_t shown;
  uint16_t total, first, selected, current;   /* current — играющий ролик (0xFFFF — нет) */
  char rows[LIST_ROWS][64];                    /* CP866 */
  char message[64];                            /* «Reading folder...» и т. п. (пусто — строки) */
} video_list_t;

/* окно сообщения: ошибка или ход работы */
typedef struct {
  uint8_t shown;            /* 0 — нет, 1 — сведения, 2 — ошибка */
  char title[48];
  char line1[64];
  char line2[64];
} video_msg_t;

/* --- общее состояние приложения ------------------------------------------------------------------ */
typedef struct {
  const zm_core_t* core;
  void* lock;               /* status, list, msg, audio_fmt */
  void* link_lock;          /* LINK ядра: только пока session_live */
  volatile int running;
  volatile int session_live;
  volatile uint32_t session_id;
  uint32_t core_version;

  video_status_t status;
  video_list_t list;
  video_msg_t msg;
  volatile uint32_t ui_serial;      /* растёт при каждой смене status/list/msg */

  /* формат звука (avi_info.c) */
  struct {
    uint16_t tag;           /* 1 — PCM, 0x55 — MP3, 0 — звука нет */
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;
    uint16_t block_align;
    uint32_t avg_bytes;     /* байтов в секунду (nAvgBytesPerSec): сколько звука в выброшенном */
    volatile uint32_t serial;   /* растёт при новом заголовке */
  } audio_fmt;
  /* размер кадра (avih): широкий кадр ядро 1.36 кладёт на холст 16:9 и выводит окном 16:9 */
  uint32_t frame_w, frame_h;
  /* для панели: что со звуком */
  volatile uint8_t audio_state;     /* 0 — нет, 1 — ждёт данных, 2 — играет, 3 — нет декодера */
} video_app_t;

extern video_app_t V;

/* LINK под замком сеанса: ZM_ERROR, если ядра уже нет */
int vlink_read(uint32_t address, uint32_t* words, uint32_t count);
int vlink_write(uint32_t address, const uint32_t* words, uint32_t count);
int vlink_wr(uint32_t address, uint32_t value);

/* avi_info.c: заголовок AVI (hdrl) — частями, по мере прихода */
void avi_info_reset(void);
void avi_info_add(uint32_t offset, const uint8_t* data, uint32_t length, int last);

/* audio.c */
void audio_task(void* arg);
extern volatile int audio_done;

/* hud.cpp */
void hud_task(void* arg);
extern volatile int hud_done;

#ifdef __cplusplus
}
#endif
#endif
