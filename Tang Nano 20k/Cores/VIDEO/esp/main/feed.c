/* SPDX-License-Identifier: Apache-2.0
 * VIDEO.ELF: ролик с SD2 WC (VDAC4 VFS) в медиа-FIFO ядра видео 1.47 — без Evo (прошивка ESP 0.1.126,
 * ABI 1.15: sd2_*, link_burst). Плагин FTVIEW4 по-прежнему разбирает файл (читает его через WC),
 * ведёт клавиши, перемотку и список, а поток идёт отсюда:
 *   SD2 — сама карта Tang (WC держит её посекторно): целые секторы — мостом microSD ядра прямо в
 *     медиа-FIFO (sd2_stream: CMD18, данные мимо ESP), начало и хвост не на границе сектора —
 *     чтением ESP и окном MF_DATA;
 *   SD2 — том VFS (флешка USB, карта Tang через FatFs ESP): файл читает ESP (sd2_read), в ядро — окном
 *     MF_DATA (link_burst).
 * Поток — тот же, что слал бы плагин: файл с начала или [заголовок до movi][JUNK][файл с base]
 * (перемотка), до end (конец movi). Темп подачи — по заполнению кольца (MF_STAT): порция уходит,
 * когда для неё есть место; на паузе кольцо полно — подача ждёт.
 *
 * Команды (ответ — команда | 0x80):
 *   7A FEED [кластер LE32][длина файла LE32][заголовок LE32][база LE32][конец LE32][JUNK 0…12 байт]
 *        → FA [1 — подаём / 0 — нет: не SD2 VDAC4 VFS, другой файл, ядро старше 1.47 или прошивка
 *        старше 0.1.126]. Перед FEED плагин сбрасывает поток (REG_CPURESET 1, затем 0) и для нового
 *        ролика шлёт HDR — начало файла на SD2 сверяется с ним (тот ли это файл).
 *   7B STOP → FB [1]: подача остановлена (ответ — когда задача подачи встала).
 * Ход подачи — в ответе STATUS (байт 12): 0 нет, 1 идёт, 2 подано всё, 3 ошибка.
 */
#include <stddef.h>
#include <string.h>

#include "video.h"

#define R_MF_STAT  0x124u       /* [14:0] слов кольца не разобрано, по 16; [23] слово MF_DATA потеряно;
                                   [31:24] эпоха потока; запись — сбросить [23] */
#define W_MF_DATA  0x40000u     /* окно: слова потока по порядку, первый байт — [7:0] */
#define MF_WORDS   (1u << 19)   /* кольцо 2 МБ */
#define MF_MARGIN  1024u        /* слов запаса: заполнение — по 16 вниз, ящик, выборка наперёд */
#define MIN_CORE   0x012Fu      /* ядро видео 1.47: второй вход медиа-FIFO и мост microSD */
#define CHUNK      16384u       /* байтов файла за чтение ESP */
#define BURST      1024u        /* слов за link_burst (~3 мс LINK: звук и панель не ждут дольше) */
#define STREAM_MAX 65536u       /* байтов за порцию моста (карту она держит ~30 мс) */

volatile uint8_t feed_state;
volatile int feed_done = 1;
static volatile int feed_stop, feed_go, feed_busy;
static void* feed_wake;
static void* feed_file;
static int feed_raw;
static uint32_t f_hdr, f_base, f_end, f_junk_len;
static uint8_t f_junk[12];
static uint8_t* bbuf;           /* чтение файла (PSRAM) */
static uint32_t* wbuf;          /* слова для MF_DATA */
static uint32_t wn, acc, accn;  /* слов в wbuf; неполное слово */
static uint8_t epoch;

static uint32_t rd32(const uint8_t* p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int feed_available(void)
{
  const zm_core_t* c = V.core;
  return c->size >= offsetof(zm_core_t, link_burst) + sizeof c->link_burst && c->sd2_open && c->link_burst;
}

static int vlink_burst(const uint32_t* words, uint32_t count)
{
  V.core->mutex_lock(V.link_lock);
  const int r = V.session_live ? V.core->link_burst(W_MF_DATA, words, count) : ZM_ERROR;
  V.core->mutex_unlock(V.link_lock);
  return r;
}

/* место в кольце под words слов; 0 — стоп, сброс потока, ядро ушло или слово потеряно */
static int wait_room(uint32_t words)
{
  for (;;) {
    uint32_t s = 0;
    if (feed_stop || !V.running) return 0;
    if (vlink_read(R_MF_STAT, &s, 1) != ZM_OK) return 0;
    if ((uint8_t)(s >> 24) != epoch) { V.core->log("video: feed: stream reset by the player"); return 0; }
    if (s & 0x800000u) { V.core->log("video: feed: MF_DATA word lost (mailbox busy)"); return 0; }
    if (((s & 0x7FFFu) + 1u) * 16u + MF_MARGIN + words <= MF_WORDS) return 1;
    V.core->delay(5);
  }
}

static int flush_words(void)
{
  if (!wn) return 1;
  if (!wait_room(wn) || vlink_burst(wbuf, wn) != ZM_OK) return 0;
  wn = 0;
  return 1;
}

static int put_bytes(const uint8_t* p, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) {
    acc |= (uint32_t)p[i] << (8 * accn);
    if (++accn == 4) {
      wbuf[wn++] = acc;
      acc = accn = 0;
      if (wn == BURST && !flush_words()) return 0;
    }
  }
  return 1;
}

/* файл [off, off + n) — чтением ESP в MF_DATA */
static int read_put(uint32_t off, uint32_t n)
{
  while (n) {
    const uint32_t part = n > CHUNK ? CHUNK : n;
    uint32_t got = 0;
    if (feed_stop) return 0;
    if (V.core->sd2_read(feed_file, off, bbuf, part, &got) != ZM_OK || got != part) {
      V.core->log("video: feed: SD2 read failed at %u", (unsigned)off);
      return 0;
    }
    if (!put_bytes(bbuf, part)) return 0;
    off += part;
    n -= part;
  }
  return 1;
}

static void run_feed(void)
{
  const zm_core_t* core = V.core;
  const uint32_t t0 = core->millis();
  uint32_t pos = f_base, streamed = 0, crc_bad = 0, portion_max = 0;
  wn = acc = accn = 0;
  int ok = 1;
  /* перемотка: заголовок до movi и JUNK до позиции (как core_restart FTVIEW4) */
  if (f_base) ok = read_put(0, f_hdr) && put_bytes(f_junk, f_junk_len);
  if (ok && feed_raw) {
    /* карта Tang: до границы сектора — чтением, дальше — целыми секторами мостом */
    if (pos & 511u) {
      uint32_t n = 512u - (pos & 511u);
      if (n > f_end - pos) n = f_end - pos;
      ok = read_put(pos, n);
      pos += n;
    }
    ok = ok && flush_words();               /* слова до потока — в ядро раньше него */
    while (ok && accn == 0 && f_end - pos >= 512u) {
      uint32_t n = (f_end - pos) & ~511u, crc = 0;
      if (n > STREAM_MAX) n = STREAM_MAX;
      ok = wait_room(n / 4);
      if (!ok) break;
      const uint32_t p0 = core->millis();
      const int r = core->sd2_stream(feed_file, pos, n, &crc);   /* карту держит всю порцию */
      if (core->millis() - p0 > portion_max) portion_max = core->millis() - p0;
      if (r == ZM_NOT_FOUND) break;         /* поток мостом не годится — дальше чтением */
      if (r != ZM_OK) { core->log("video: feed: Tang card stream failed at %u", (unsigned)pos); ok = 0; break; }
      crc_bad += crc;
      streamed += n;
      pos += n;
    }
  }
  if (ok && pos < f_end) ok = read_put(pos, f_end - pos);
  if (ok && accn) { wbuf[wn++] = acc; acc = accn = 0; }   /* хвост — словом с нулями */
  ok = ok && flush_words();
  const uint32_t ms = core->millis() - t0;
  core->log("video: feed %s: %u KB (%u KB by the Tang bridge, longest portion %u ms), %u ms%s",
            ok ? "done" : feed_stop ? "stopped" : "FAILED", (unsigned)((f_end - f_base + f_hdr) >> 10),
            (unsigned)(streamed >> 10), (unsigned)portion_max, (unsigned)ms, crc_bad ? ", CRC errors" : "");
  feed_state = ok ? 2 : feed_stop ? 0 : 3;
}

void feed_task(void* arg)
{
  (void)arg;
  while (V.running) {
    V.core->signal_wait(feed_wake, 50);
    if (!feed_go) continue;
    feed_go = 0;
    run_feed();
    V.core->sd2_close(feed_file);
    feed_file = NULL;
    feed_busy = 0;
  }
  feed_done = 1;
}

int feed_init(void)
{
  feed_state = 0;
  feed_go = feed_busy = feed_stop = 0;
  if (!feed_available()) return ZM_OK;      /* прошивка старше 0.1.126: подачи нет, плеер — как прежде */
  feed_wake = V.core->signal_create();
  bbuf = (uint8_t*)V.core->alloc(CHUNK, ZM_MEM_PSRAM);
  wbuf = (uint32_t*)V.core->alloc(BURST * 4, ZM_MEM_PSRAM);
  if (!feed_wake || !bbuf || !wbuf) return ZM_NO_MEMORY;
  feed_done = 0;
  return V.core->task_start(feed_task, NULL, 4096, 2, 0);
}

/* остановить подачу и дождаться задачи (порция моста — до ~50 мс) */
void feed_halt(void)
{
  feed_stop = 1;
  feed_go = 0;
  for (unsigned i = 0; i < 200 && feed_busy; i++) V.core->delay(5);
  if (feed_state == 1) feed_state = 0;
}

int cmd_feed(const uint8_t* p, uint16_t n)
{
  const zm_core_t* core = V.core;
  uint8_t ok = 0;
  if (n >= 20 && n <= 32 && feed_available() && V.session_live && (V.core_version >> 24) == VIDEO_PERSONALITY &&
      (V.core_version & 0xFFFF) >= MIN_CORE) {
    feed_halt();
    if (!feed_busy) {
      const uint32_t cluster = rd32(p), size = rd32(p + 4);
      f_hdr = rd32(p + 8);
      f_base = rd32(p + 12);
      f_end = rd32(p + 16);
      f_junk_len = (uint32_t)(n - 20);
      memcpy(f_junk, p + 20, f_junk_len);
      void* file = NULL;
      int raw = 0;
      const uint8_t* hdr = NULL;
      const uint32_t hdr_len = avi_info_bytes(&hdr);
      uint32_t check = hdr_len < 512 ? hdr_len : 512, got = 0;
      if (check > size) check = size;
      if (f_end <= size && f_base <= f_end && f_hdr <= size && (f_base & 3) == 0 && check >= 12 &&
          core->sd2_open(cluster, size, &file, &raw) == ZM_OK) {
        /* тот ли файл: начало на SD2 — как заголовок, присланный плагином (HDR) */
        if (core->sd2_read(file, 0, bbuf, check, &got) == ZM_OK && got == check && !memcmp(bbuf, hdr, check)) {
          uint32_t s = 0;
          vlink_wr(R_MF_STAT, 0);           /* сбросить флаг потерянного слова */
          if (vlink_read(R_MF_STAT, &s, 1) == ZM_OK) {
            epoch = (uint8_t)(s >> 24);
            feed_file = file;
            feed_raw = raw;
            file = NULL;
            feed_stop = 0;
            feed_state = 1;
            feed_busy = 1;
            feed_go = 1;
            core->signal_give(feed_wake);
            ok = 1;
            core->log("video: feed from SD2 (%s), cluster %u, %u bytes, %u..%u", raw ? "Tang card" : "VFS volume",
                      (unsigned)cluster, (unsigned)size, (unsigned)f_base, (unsigned)f_end);
          }
        } else {
          core->log("video: feed: the SD2 file at cluster %u differs from the player's file", (unsigned)cluster);
        }
        if (file) core->sd2_close(file);
      }
    }
  }
  return core->reply(0xFA, &ok, 1);
}

int cmd_stop(void)
{
  uint8_t ok = 1;
  feed_halt();
  return V.core->reply(0xFB, &ok, 1);
}
