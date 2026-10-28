/* SPDX-License-Identifier: Apache-2.0
 * VIDEO.ELF: команды плагина FTVIEW4, загрузка ядра видео и сеанс.
 *
 * Запуск (как у слоёв ядер VDAC4 Cores): плагин шлёт 2D CORE_APP "VIDEO" — прошивка ESP берёт
 * CORES/VIDEO.ELF с microSD Tang и запускает это приложение; затем 70 START — рабочая задача
 * грузит ядро (core_load "VIDEO": с ESP 0.1.101 и BL616 35 — из слота 3 flash Tang за доли
 * секунды; слот пуст — CORES/VIDEO.BIN через кэш BL616) и ждёт плагин.
 * Плагин шлёт STATUS не реже раза в SILENCE_MS (тело — что рисовать), EXIT — назад в слот 0.
 *
 * Команды (ответ — команда | 0x80):
 *   70 START [имя ядра,0]           → F0 [1 — грузим / 0 — нельзя сейчас]
 *   72 STATUS [тело панели]         → F2 [состояние][VERSION LE32][слотов LE32][отменено LE16]
 *                                       [звук: 0 нет, 1 ждёт, 2 играет, 3 нет декодера][ошибка,0]
 *        тело: [1][флаги VS_*][кадр начала LE32][кадров LE32][мкс на кадр LE32]
 *              [байтов звука до начала потока LE32][имя,0]
 *        Новый поток (ролик, перемотка): REG_CPURESET 1 (новая эпоха звука), HDR (новый ролик),
 *        STATUS с новыми кадром начала и байтами звука, REG_CPURESET 0, подача.
 *   73 EXIT                         → F3 [1]: ядро — назад в слот 0, затем состояние 3
 *   76 HDR [смещение LE32][последний][байты]  → F6 [1]: заголовок AVI (hdrl) для звука
 *   77 LIST [показ][всего LE16][первая LE16][выбор LE16][текущий LE16][сообщение,0][строки,0…]
 *                                   → F7 [1]: окно списка роликов (показ 0 — убрать)
 *   78 MSG [вид: 0 убрать, 1 сведения, 2 ошибка][заголовок,0][строка,0][строка,0] → F8 [1]
 * Состояния: 0 — ждёт START, 1 — грузит ядро, 2 — ядро работает, 3 — вернулись в слот 0,
 *            255 — ошибка (текст в STATUS).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "video.h"

#define SILENCE_MS 2000u          /* плагин молчит дольше — Evo сброшена или плагин упал: назад */
#define SILENCE_LONG_MS 60000u    /* … а с окном на экране: таблица перемотки, чтение каталога */

video_app_t V;
static volatile int state, exit_req, worker_done = 1;
volatile int audio_done = 1, hud_done = 1;
static char core_name[16] = "VIDEO";
static char error_text[64];
static void* wake;
static volatile uint32_t status_ms;

static void wr32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t rd32(const uint8_t* p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

/* --- LINK под замком сеанса --------------------------------------------------------------------- */
int vlink_read(uint32_t a, uint32_t* words, uint32_t n)
{
  V.core->mutex_lock(V.link_lock);
  const int r = V.session_live ? V.core->link_read(a, words, n) : ZM_ERROR;
  V.core->mutex_unlock(V.link_lock);
  return r;
}
int vlink_write(uint32_t a, const uint32_t* words, uint32_t n)
{
  V.core->mutex_lock(V.link_lock);
  const int r = V.session_live ? V.core->link_write(a, words, n) : ZM_ERROR;
  V.core->mutex_unlock(V.link_lock);
  return r;
}
int vlink_wr(uint32_t a, uint32_t v) { return vlink_write(a, &v, 1); }

static void fail(const char* text)
{
  strncpy(error_text, text, sizeof error_text - 1);
  error_text[sizeof error_text - 1] = 0;
  V.core->log("video: %s", text);
  state = 255;
}

/* копия строки с нулём на конце из данных команды; вернёт, сколько байтов занято (с нулём) */
static uint16_t take_str(char* out, size_t cap, const uint8_t* p, uint16_t n)
{
  uint16_t i = 0;
  while (i < n && p[i]) i++;
  const size_t len = i < cap - 1 ? i : cap - 1;
  memcpy(out, p, len);
  out[len] = 0;
  return (uint16_t)(i < n ? i + 1 : i);
}

/* --- ядро --------------------------------------------------------------------------------------- */
static int load_core(void)
{
  const zm_core_t* core = V.core;
  core->log("video: loading core %s", core_name);
  const int r = core->core_load(core_name, &V.core_version);
  if (r == ZM_NOT_FOUND) { fail("core not found: Tang flash slot 3 empty, not on the Tang SD (CORES)"); return 0; }
  if (r != ZM_OK) { fail("core did not start (Tang is back in TS-Config)"); return 0; }
  if ((V.core_version >> 24) != VIDEO_PERSONALITY) {
    fail("loaded core is not the video core (VERSION personality)");
    core->core_back(10000);
    return 0;
  }
  /* приём звука ядром — до «работает»: плагин пускает поток сразу, а задача звука просыпается
     позже; пока приём выключен, ядро звуковые чанки пропускает (audio.c, set_ctl) */
  uint32_t wp = 0;
  if (core->link_read(R_AU_WP, &wp, 1) == ZM_OK) core->link_write(R_AU_CTL, (const uint32_t[]){1u | (wp >> 24) << 8}, 1);
  core->mutex_lock(V.link_lock);
  V.session_id = V.session_id + 1 == 0 ? 1 : V.session_id + 1;
  V.session_live = 1;
  core->mutex_unlock(V.link_lock);
  status_ms = core->millis();
  state = 2;
  core->log("video: core runs, VERSION %08X", (unsigned)V.core_version);
  return 1;
}

static void end_session(void)
{
  V.core->mutex_lock(V.link_lock);
  V.session_live = 0;
  V.core->mutex_unlock(V.link_lock);
}

static void leave_core(void)
{
  end_session();                    /* звук и панель отпускают LINK до возврата в слот 0 */
  if (V.core->core_back(10000) != ZM_OK) { fail("could not return the Tang to TS-Config (slot 0)"); return; }
  state = 3;
}

/* START → ядро → сеанс (STATUS плагина) → EXIT или тишина → ждать START снова */
static void worker(void* arg)
{
  (void)arg;
  const zm_core_t* core = V.core;
  while (V.running) {
    if (state != 1) { core->signal_wait(wake, 50); continue; }
    if (!load_core()) continue;
    while (V.running && state == 2) {
      const uint32_t quiet = core->millis() - status_ms;
      /* долгие дела плагина (таблица перемотки по idx1, чтение каталога) идут без STATUS — при
         окне сообщения или списка */
      const uint32_t limit = (V.msg.shown || V.list.shown) ? SILENCE_LONG_MS : SILENCE_MS;
      if (exit_req || quiet > limit) {
        core->log(exit_req ? "video: exit — back to slot 0" : "video: the plugin is silent — back to slot 0");
        leave_core();
        break;
      }
      core->signal_wait(wake, 20);
    }
  }
  end_session();                    /* выгрузка: ядро вернёт прошивка ESP */
  worker_done = 1;
}

/* --- команды ------------------------------------------------------------------------------------ */
static int cmd_status(const uint8_t* p, uint16_t n)
{
  const zm_core_t* core = V.core;
  status_ms = core->millis();
  if (n >= 18 && p[0] == 1) {
    core->mutex_lock(V.lock);
    V.status.flags = p[1];
    V.status.base_frame = rd32(p + 2);
    V.status.total_frames = rd32(p + 6);
    V.status.us_frame = rd32(p + 10);
    V.status.audio_base = rd32(p + 14);
    take_str(V.status.name, sizeof V.status.name, p + 18, (uint16_t)(n - 18));
    V.ui_serial++;
    core->mutex_unlock(V.lock);
  }
  uint32_t slots = 0, aborts = 0;
  if (state == 2) {
    vlink_read(R_SLOTS, &slots, 1);
    vlink_read(R_ABORTS, &aborts, 1);
  }
  uint8_t st[16 + sizeof error_text];
  st[0] = (uint8_t)state;
  wr32(st + 1, V.core_version);
  wr32(st + 5, slots);
  st[9] = (uint8_t)aborts; st[10] = (uint8_t)(aborts >> 8);
  st[11] = V.audio_state;
  const size_t len = strlen(error_text);
  memcpy(st + 12, error_text, len + 1);
  return core->reply(0xF2, st, (uint16_t)(12 + len + 1));
}

static int cmd_list(const uint8_t* p, uint16_t n)
{
  uint8_t ok = 0;
  if (n >= 9) {
    V.core->mutex_lock(V.lock);
    video_list_t* L = &V.list;
    L->shown = p[0];
    L->total = rd16(p + 1);
    L->first = rd16(p + 3);
    L->selected = rd16(p + 5);
    L->current = rd16(p + 7);
    uint16_t at = 9;
    at += take_str(L->message, sizeof L->message, p + at, (uint16_t)(n - at));
    for (int i = 0; i < LIST_ROWS; i++) {
      L->rows[i][0] = 0;
      if (at < n) at += take_str(L->rows[i], sizeof L->rows[i], p + at, (uint16_t)(n - at));
    }
    V.ui_serial++;
    V.core->mutex_unlock(V.lock);
    ok = 1;
  }
  return V.core->reply(0xF7, &ok, 1);
}

static int cmd_msg(const uint8_t* p, uint16_t n)
{
  uint8_t ok = 0;
  if (n >= 1) {
    V.core->mutex_lock(V.lock);
    video_msg_t* M = &V.msg;
    M->shown = p[0];
    uint16_t at = 1;
    M->title[0] = M->line1[0] = M->line2[0] = 0;
    if (at < n) at += take_str(M->title, sizeof M->title, p + at, (uint16_t)(n - at));
    if (at < n) at += take_str(M->line1, sizeof M->line1, p + at, (uint16_t)(n - at));
    if (at < n) at += take_str(M->line2, sizeof M->line2, p + at, (uint16_t)(n - at));
    V.ui_serial++;
    V.core->mutex_unlock(V.lock);
    ok = 1;
  }
  return V.core->reply(0xF8, &ok, 1);
}

static int command(uint8_t cmd, const uint8_t* p, uint16_t n)
{
  const zm_core_t* core = V.core;
  uint8_t ok = 0;
  switch (cmd) {
  case 0x70:                                    /* START */
    if ((state == 0 || state == 3 || state == 255) && n <= sizeof core_name && (n == 0 || p[n - 1] == 0)) {
      if (n > 1) memcpy(core_name, p, n);
      error_text[0] = 0;
      exit_req = 0;
      status_ms = core->millis();
      ok = 1;
      state = 1;
    }
    core->reply(0xF0, &ok, 1);
    if (ok) core->signal_give(wake);
    return ZM_OK;
  case 0x72:
    cmd_status(p, n);
    return ZM_OK;
  case 0x73:                                    /* EXIT */
    exit_req = 1;
    if (state == 0 || state == 255) state = 3;
    ok = 1;
    core->reply(0xF3, &ok, 1);
    core->signal_give(wake);
    return ZM_OK;
  case 0x76:                                    /* HDR: заголовок AVI для звука */
    if (n >= 5) {
      status_ms = core->millis();
      avi_info_add(rd32(p), p + 5, (uint32_t)(n - 5), p[4]);
      ok = 1;
    }
    core->reply(0xF6, &ok, 1);
    return ZM_OK;
  case 0x77:
    status_ms = core->millis();
    cmd_list(p, n);
    return ZM_OK;
  case 0x78:
    status_ms = core->millis();
    cmd_msg(p, n);
    return ZM_OK;
  default:
    return ZM_ERROR;
  }
}

static int start(const zm_core_t* c)
{
  memset(&V, 0, sizeof V);
  V.core = c;
  if (c->abi_major != 1 || c->abi_minor < 6 ||
      c->size < offsetof(zm_core_t, app_done) + sizeof c->app_done || !c->core_load || !c->link_read)
    return ZM_ERROR;
  V.lock = c->mutex_create();
  V.link_lock = c->mutex_create();
  wake = c->signal_create();
  if (!V.lock || !V.link_lock || !wake)
    return ZM_NO_MEMORY;
  avi_info_reset();
  state = 0;
  exit_req = 0;
  error_text[0] = 0;
  V.running = 1;
  worker_done = 0;
  int r = c->task_start(worker, NULL, 6144, 3, 1);
  if (r != ZM_OK) { worker_done = 1; V.running = 0; return r; }
  audio_done = 0;
  r = c->task_start(audio_task, NULL, 8192, 4, 0);         /* звук — ядро 0, выше панели */
  if (r != ZM_OK) { audio_done = 1; V.running = 0; return r; }
  hud_done = 0;
  r = c->task_start(hud_task, NULL, 6144, 1, 0);           /* панель — ядро 0, ниже всех */
  if (r != ZM_OK) { hud_done = 1; V.running = 0; return r; }
  c->log("video: VIDEO.ELF " APP_VERSION " ready");
  return ZM_OK;
}

static void stop(void)
{
  V.running = 0;
  if (wake) V.core->signal_give(wake);
  for (unsigned i = 0; i < 300 && !(worker_done && audio_done && hud_done); i++)
    V.core->delay(10);
}

#ifndef VIDEO_APP_HOST_TEST
int main(int argc, char** argv)
{
  static const zm_app_t app = {sizeof(zm_app_t), ZM_APP_ABI_MAJOR, ZM_APP_ABI_MINOR, "video", APP_VERSION,
                               0x6E, 0x7E, start, stop, command, NULL, NULL, NULL};
  if (argc != 3)
    return 0;
  *(const zm_app_t**)argv[2] = &app;
  return ZM_APP_ABI_MAJOR;
}
#else
/* стенд на ПК: таблица приложения напрямую */
const zm_app_t* video_app_table(void)
{
  static const zm_app_t app = {sizeof(zm_app_t), ZM_APP_ABI_MAJOR, ZM_APP_ABI_MINOR, "video", APP_VERSION,
                               0x6E, 0x7E, start, stop, command, NULL, NULL, NULL};
  return &app;
}
#endif
