// SPDX-License-Identifier: Apache-2.0
// VIDEO.ELF: панель плеера и окна оверлеями Tang (fb_ovl ядра видео; регистры и окно холстов —
// как у TS-Config, но прошивка ESP под ядрами из кэша оверлеи не ведёт — здесь сырой link_write).
//
// Всё повторяет FTVIEW (avi_controls.c, avi_list.c) в масштабе 720p: экран FT812 1024×768 там —
// окно картинки 960×720 здесь (×0,9375).
// ПАНЕЛЬ (оверлей 1, ×1, холст до 960×64) — низ окна картинки (4:3 или 16:9, 100/95/90 % — из
// CTRL настроек ядра): тёмная полоса, полоса хода, время сейчас и всего, куда идёт звук,
// подсказки клавиш. Видна, пока плагин ставит VS_PANEL, и не видна под окном.
// ОКНО (оверлей 0, ×2, холст 390×308 — на выводе 780×616, по центру картинки): список роликов
// папки (страница строк от плагина) или сообщение («Building seek index...»). Сообщение — поверх
// списка. Точка холста ×2 — как крупный шрифт 28 FT812 в окне списка FTVIEW.
// Прозрачность в fb_ovl — на весь оверлей, поэтому фон и текст полупрозрачны вместе (в FTVIEW
// затемнён только фон); значения подобраны, чтобы текст читался и на светлом кадре.
//
// Холсты рисуются в копии в PSRAM; в ядро (окно LINK 0x80000…, слово w региона — 0x80000 + 4·w)
// уходят только изменившиеся куски строк по 16 слов.
#include <stdint.h>
#include <string.h>

#include "video.h"
#include "pui_font.h"

namespace {

constexpr int PANEL_MAX_W = 960, PANEL_H = 64, PANEL_ALPHA = 11;
constexpr int WIN_W = 390, WIN_H = 308, WIN_ALPHA = 12;
constexpr uint32_t PANEL_BASE = 0;          // слово региона (кратно 8)
constexpr uint32_t WIN_BASE = 32768;
constexpr int FONT = 0;                     // шрифт 13 точек (строка 15)

struct Color { int r, g, b; };
constexpr Color BLACK{0, 0, 0}, WHITE{255, 255, 255}, TRACK{0, 32, 64};     // фон полосы хода FT812
constexpr Color ROW{235, 235, 235}, CUR{255, 210, 90}, SEL{40, 110, 220}, GRAY{170, 170, 170};

uint16_t rgb(Color c) { return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3)); }

struct Canvas {
  int w, h, stride;           // stride — слов на строку: ⌈w/16⌉·8 (так считает fb_ovl)
  uint32_t base;
  uint16_t* px;               // рисуется здесь, строка — stride·2 точек
  uint32_t* sent;             // что уже в ядре (слова), для сравнения
  bool valid;                 // sent соответствует ядру
};
Canvas panel{PANEL_MAX_W, PANEL_H, (PANEL_MAX_W + 15) / 16 * 8, PANEL_BASE, nullptr, nullptr, false};
Canvas win{WIN_W, WIN_H, (WIN_W + 15) / 16 * 8, WIN_BASE, nullptr, nullptr, false};

const zm_core_t* core;

bool alloc(Canvas& c)
{
  c.px = (uint16_t*)core->alloc((size_t)c.stride * 2 * c.h * 2, ZM_MEM_PSRAM);
  c.sent = (uint32_t*)core->alloc((size_t)c.stride * c.h * 4, ZM_MEM_PSRAM);
  return c.px && c.sent;
}

void set_width(Canvas& c, int w)
{
  c.w = w;
  c.stride = (w + 15) / 16 * 8;
  c.valid = false;
}

// --- рисование ---------------------------------------------------------------------------------------
void put(Canvas& c, int x, int y, uint16_t v)
{
  if (x >= 0 && x < c.w && y >= 0 && y < c.h) c.px[y * c.stride * 2 + x] = v;
}
void box(Canvas& c, int x, int y, int w, int h, Color col)
{
  const uint16_t v = rgb(col);
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++) put(c, xx, yy, v);
}
// полоса со скруглёнными концами (как виджет Progress FT812, FT_OPT_FLAT)
void pill(Canvas& c, int x, int y, int w, int h, Color col)
{
  const int r2 = h;                                   // радиус ×2
  for (int yy = 0; yy < h; yy++) {
    const int dy = 2 * yy + 1 - h;                    // ×2 от середины
    for (int xx = 0; xx < w; xx++) {
      const int cx = 2 * xx + 1;
      const int dx = cx < r2 ? r2 - cx : cx > 2 * w - r2 ? cx - (2 * w - r2) : 0;
      if (dx * dx + dy * dy <= h * h) put(c, x + xx, y + yy, rgb(col));
    }
  }
}

const pui_font::Glyph& glyph(unsigned char ch)
{
  return pui_font::fonts[FONT].glyphs[(ch < 32 ? '?' : ch) - 32];
}
int width(const char* s)
{
  int w = 0;
  for (; *s; s++) w += glyph((unsigned char)*s).width;
  return w;
}
// текст с 4-битным сглаживанием поверх того, что уже на холсте; right — граница
int text(Canvas& c, int x, int y, const char* s, Color col, int right)
{
  const auto& f = pui_font::fonts[FONT];
  for (; *s; s++) {
    const auto& g = glyph((unsigned char)*s);
    if (x + g.width > right) break;
    for (int yy = 0; yy < g.height; yy++)
      for (int xx = 0; xx < g.width; xx++) {
        const int i = yy * g.width + xx;
        const uint8_t packed = f.pixels[g.offset + i / 2];
        const int a = (i & 1) ? packed & 15 : packed >> 4;
        const int px = x + xx, py = y + yy;
        if (!a || px < 0 || px >= c.w || py < 0 || py >= c.h) continue;
        uint16_t& p = c.px[py * c.stride * 2 + px];
        const int br = (p >> 11) << 3, bg = ((p >> 5) & 63) << 2, bb = (p & 31) << 3;
        p = rgb({(br * (15 - a) + col.r * a + 7) / 15, (bg * (15 - a) + col.g * a + 7) / 15,
                 (bb * (15 - a) + col.b * a + 7) / 15});
      }
    x += g.width;
  }
  return x;
}
void left(Canvas& c, int x, int y, const char* s, Color col) { text(c, x, y, s, col, c.w); }
void right(Canvas& c, int x, int y, const char* s, Color col) { text(c, x - width(s), y, s, col, c.w); }
void center(Canvas& c, int x, int y, const char* s, Color col) { text(c, x - width(s) / 2, y, s, col, c.w); }
// имя с «...», если не влезает до right
void ellipsis(Canvas& c, int x, int y, const char* s, Color col, int right)
{
  if (x + width(s) <= right) { text(c, x, y, s, col, right); return; }
  const int end = text(c, x, y, s, col, right - width("..."));
  text(c, end, y, "...", col, right);
}

char* put_num(char* p, uint32_t v)
{
  char t[12];
  int n = 0;
  do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) *p++ = t[--n];
  *p = 0;
  return p;
}
// время как в FTVIEW (avi_time): м:сс, от часа — ч:мм:сс
void clock_text(char* p, uint32_t s)
{
  const uint32_t h = s / 3600, m = s / 60 % 60;
  s %= 60;
  if (h) {
    p = put_num(p, h);
    *p++ = ':';
    *p++ = (char)('0' + m / 10);
    *p++ = (char)('0' + m % 10);
  } else {
    p = put_num(p, m);
  }
  *p++ = ':';
  *p++ = (char)('0' + s / 10);
  *p++ = (char)('0' + s % 10);
  *p = 0;
}

// --- панель (avi_hud_build FTVIEW: полоса y 700…767, ход 24,708 976×12, текст на 730 и 748) ----------
struct PanelView {
  uint8_t paused, audio;
  uint16_t audio_tag;
  uint32_t now_s, total_s;
  int bar;                    // точек заполненной части полосы
  int w;                      // ширина холста
  char busy[32];              // плагин занят (VS_BUSY): текст вместо подсказок, например «Indexing 37%»
  bool operator==(const PanelView& o) const
  {
    return paused == o.paused && audio == o.audio && audio_tag == o.audio_tag && now_s == o.now_s &&
           total_s == o.total_s && bar == o.bar && w == o.w && !strcmp(busy, o.busy);
  }
};

constexpr int BAR_X = 22, BAR_Y = 8, BAR_H = 11, ROW1 = 28, ROW2 = 45;

int bar_width(int w) { return w - 2 * BAR_X; }

void draw_panel(const PanelView& v)
{
  Canvas& c = panel;
  box(c, 0, 0, c.w, c.h, BLACK);
  pill(c, BAR_X, BAR_Y, bar_width(c.w), BAR_H, TRACK);
  if (v.bar >= BAR_H) pill(c, BAR_X, BAR_Y, v.bar, BAR_H, WHITE);
  char t[16];
  clock_text(t, v.now_s);
  left(c, BAR_X, ROW1, t, WHITE);
  clock_text(t, v.total_s);
  right(c, c.w - BAR_X, ROW1, t, WHITE);
  // куда идёт звук (в FTVIEW — «GS» / «FT812 GS»)
  const char* snd = !v.audio_tag ? "" :
                    v.audio == 2 ? (v.audio_tag == 0x55 ? "MP3 ESP DAC" : "PCM ESP DAC") :
                    v.audio == 3 ? "ESP DAC: no decoder" : "ESP DAC";
  right(c, c.w - BAR_X, ROW2, snd, GRAY);
  center(c, c.w / 2, ROW1, v.busy[0] ? v.busy :
                           v.paused ? "Paused: Space play | Left/Right seek | Enter list | Esc exit"
                                    : "Space pause | Left/Right seek | Home restart | Enter list | Esc exit", WHITE);
}

// --- окно (avi_list.c FTVIEW: 96…928 × 56…712, строки по 32 с 112; здесь ×0,9375/2) ---------------
constexpr int ROW_Y = 26, ROW_H = 15;

void draw_window(const video_list_t& L, const video_msg_t& M)
{
  Canvas& c = win;
  box(c, 0, 0, c.w, c.h, BLACK);
  if (M.shown) {
    const int y = M.line1[0] ? 120 : 146;
    center(c, c.w / 2, y, M.title, M.shown == 2 ? CUR : WHITE);
    center(c, c.w / 2, y + 26, M.line1, ROW);
    center(c, c.w / 2, y + 44, M.line2, GRAY);
    return;
  }
  left(c, 11, 7, "Videos in this folder", WHITE);
  if (L.message[0]) {
    center(c, c.w / 2, 142, L.message, WHITE);
    return;
  }
  char n[24];
  char* p = put_num(n, (uint32_t)L.selected + 1);
  *p++ = '/';
  put_num(p, L.total);
  right(c, c.w - 11, 7, n, WHITE);
  if (L.selected >= L.first && L.selected < L.first + LIST_ROWS)
    box(c, 6, ROW_Y + (L.selected - L.first) * ROW_H - 1, c.w - 12, ROW_H, SEL);
  for (int i = 0; i < LIST_ROWS; i++)
    if (L.rows[i][0]) ellipsis(c, 13, ROW_Y + i * ROW_H, L.rows[i], L.first + i == L.current ? CUR : ROW, c.w - 13);
  center(c, c.w / 2, c.h - 18, "Up/Down select | Enter play | Esc back", GRAY);
}

// --- выгрузка в ядро ---------------------------------------------------------------------------------
// изменившиеся куски строк холста по 16 слов; false — LINK не работает
bool upload(Canvas& c)
{
  uint32_t row[PANEL_MAX_W / 2];
  for (int y = 0; y < c.h; y++) {
    const uint16_t* p = c.px + y * c.stride * 2;
    for (int i = 0; i < c.stride; i++) row[i] = (uint32_t)p[2 * i] | (uint32_t)p[2 * i + 1] << 16;
    uint32_t* s = c.sent + y * c.stride;
    if (c.valid && !memcmp(row, s, (size_t)c.stride * 4)) continue;
    for (int i = 0; i < c.stride; i += 16) {
      const int n = c.stride - i < 16 ? c.stride - i : 16;
      if (c.valid && !memcmp(row + i, s + i, (size_t)n * 4)) continue;
      if (vlink_write(W_CANVAS + 4 * (c.base + (uint32_t)(y * c.stride + i)), row + i, (uint32_t)n) != ZM_OK) {
        c.valid = false;
        return false;
      }
    }
    memcpy(s, row, (size_t)c.stride * 4);
  }
  c.valid = true;
  return true;
}

// регистры оверлея k: положение, размер холста, начало холста, затем CTRL (вкл., ×2, прозрачность)
void overlay(int k, bool on, bool x2, int alpha, int x, int y, const Canvas& c)
{
  const uint32_t regs[3] = {(uint32_t)x | (uint32_t)y << 16, (uint32_t)c.w | (uint32_t)c.h << 16, c.base};
  vlink_write(R_OVL(k) + 4, regs, 3);
  vlink_wr(R_OVL(k), (on ? 1u : 0u) | (x2 ? 2u : 0u) | (uint32_t)alpha << 4);
}

// окно картинки по ASPECT и SIZE слота 0 (как window() в fb_scanout): {x0, x1, y0, y1}. Ядро 1.39
// растягивает любой ролик на это окно, как кадр FT812 (в 1.37–1.38 окно шло по пропорциям холста
// и ASPECT не бралось; размер кадра w, h окну больше не нужен).
void picture_window(uint32_t cfg, uint32_t w, uint32_t h, int out[4])
{
  static const int W[2][3][4] = {{{160, 1120, 0, 720}, {184, 1096, 18, 702}, {208, 1072, 36, 684}},
                                 {{0, 1280, 0, 720}, {32, 1248, 18, 702}, {64, 1216, 36, 684}}};
  (void)w;
  (void)h;
  const int asp = (cfg >> 14) & 1;
  int size = (cfg >> 15) & 3;
  if (size == 3) size = 0;
  memcpy(out, W[asp][size], sizeof W[0][0]);
}

}  // namespace

extern "C" void hud_task(void* arg)
{
  (void)arg;
  core = V.core;
  video_list_t* L = (video_list_t*)core->alloc(sizeof(video_list_t), ZM_MEM_PSRAM);
  const bool ok = L && alloc(panel) && alloc(win);
  if (!ok) core->log("video: no memory for the player panel");
  while (V.running && ok) {
    while (V.running && !V.session_live) core->delay(20);
    const uint32_t session = V.session_id;
    // новое ядро — холсты в нём пустые, оверлеи выключены
    panel.valid = win.valid = false;
    PanelView last{};
    bool panel_drawn = false, panel_on = false, win_on = false, first = true;
    uint32_t win_serial = 0, cfg_last = 0, fw_last = 0, fh_last = 0;
    int pos[4] = {160, 1120, 0, 720};
    while (V.running && V.session_live && V.session_id == session) {
      video_status_t st;
      video_msg_t M;
      core->mutex_lock(V.lock);
      st = V.status;
      *L = V.list;
      M = V.msg;
      const uint32_t serial = V.ui_serial;
      const uint16_t tag = V.audio_fmt.tag;
      const uint32_t fw = V.frame_w, fh = V.frame_h;
      core->mutex_unlock(V.lock);
      uint32_t slots = 0, cfg = 0;
      if (vlink_read(R_SLOTS, &slots, 1) != ZM_OK || vlink_read(R_CFG, &cfg, 1) != ZM_OK) {
        core->delay(40);
        continue;
      }
      bool moved = false;
      if (first || cfg != cfg_last || fw != fw_last || fh != fh_last) {
        first = false;
        cfg_last = cfg;
        fw_last = fw;
        fh_last = fh;
        picture_window(cfg, fw, fh, pos);
        moved = true;
      }

      // окно: список или сообщение
      const bool want_win = M.shown || L->shown;
      if (want_win && (serial != win_serial || !win.valid)) {
        draw_window(*L, M);
        win_serial = serial;
        upload(win);
      }
      const bool win_vis = want_win && win.valid;
      if (win_vis != win_on || (moved && win_on)) {
        overlay(0, win_vis, true, WIN_ALPHA, 640 - WIN_W, (pos[2] + pos[3]) / 2 - WIN_H, win);
        win_on = win_vis;
      }

      // панель — не под окном
      const int pw = (pos[1] - pos[0] < PANEL_MAX_W ? pos[1] - pos[0] : PANEL_MAX_W) & ~1;
      if (pw != panel.w) { set_width(panel, pw); panel_drawn = false; }
      const uint32_t frame = st.base_frame + (slots ? slots - 1 : 0);
      const uint64_t per = st.us_frame ? st.us_frame : 40000;
      PanelView v{};
      v.paused = (st.flags & VS_PAUSED) != 0;
      v.audio = V.audio_state;
      v.audio_tag = tag;
      v.now_s = (uint32_t)((uint64_t)frame * per / 1000000u);
      v.total_s = (uint32_t)((uint64_t)st.total_frames * per / 1000000u);
      const uint32_t shown = frame + 1 > st.total_frames ? st.total_frames : frame + 1;
      v.bar = st.total_frames ? (int)((uint64_t)bar_width(panel.w) * shown / st.total_frames) : 0;
      v.w = panel.w;
      if (st.flags & VS_BUSY) {
        size_t n = 0;
        while (n < sizeof v.busy - 1 && st.name[n]) { v.busy[n] = st.name[n]; ++n; }
        v.busy[n] = 0;
      }
      const bool want_panel = (st.flags & VS_PANEL) && !want_win;
      if (want_panel && (!panel_drawn || !panel.valid || !(v == last))) {
        draw_panel(v);
        last = v;
        panel_drawn = true;
        upload(panel);
      }
      const bool panel_vis = want_panel && panel.valid;
      if (panel_vis != panel_on || (moved && panel_on)) {
        overlay(1, panel_vis, false, PANEL_ALPHA, (pos[0] + pos[1]) / 2 - panel.w / 2, pos[3] - PANEL_H, panel);
        panel_on = panel_vis;
      }
      core->delay(40);
    }
  }
  if (L) core->free(L);
  hud_done = 1;
}
