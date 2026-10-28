// Банк ядра видео VDAC4 (страница 11 WMF). AVI играет ядро видео Tang (личность 05,
// docs/video-core.md проекта VDAC4): MJPEG декодирует ПЛИС, кадр — на HDMI 720p; звук,
// панель плеера и окна — приложение ESP VIDEO.ELF (Cores/VIDEO/esp): ЦАП ESP и оверлеи Tang.
// Плагин шлёт файл в ядро по SPI FT812 (медиа-FIFO, как у FT812: окно #E0000,
// REG_MEDIAFIFO_READ/WRITE), ведёт клавиши, перемотку и список роликов, а в ESP — что
// показывать на панели и в окнах.
//
// Вход — core_try банка изображений (crt0.s), для AVI до GS и FT812: far_entry ниже. Не тот
// ролик (не MJPEG, кадр больше 2048×1536), нет ESP, VIDEO.ELF или ядра — возврат, и банк
// изображений играет ролик прежним путём (FT812 и BL616).
//
// Порядок:
//   разбор заголовка (avi_probe — тот же код, что у банка изображений, с любым звуком);
//   ESP: связь (esp_link.asm); VIDEO.ELF из /WC/ESP на томе WC — в кэш ESP (MOD_QUERY #69, если
//   его там нет или он изменился — MOD_PUT #6A по сектору, MOD_COMMIT #6B), APP_START (#6C);
//   START (#70) — ядро из слота 3 flash Tang (или VIDEO.BIN из кэша BL616) в ПЛИС; STATUS (#72),
//   пока не «работает» (до 60 с: ядро с SD Tang сначала идёт в кэш);
//   M_SEL = 1 (режим FT812 у Evo): ядро без обмена с M_SEL = 0 через 0,5 с уходит в слот 0;
//   ядро: REG_ID #7C и блок состояния «VID1» (размер FIFO);
//   HDR (#76) — заголовок до movi: по нему ESP знает формат звука;
//   поток: REG_CPURESET 1 (новая эпоха звука), STATUS с кадром начала и байтами звука до
//   него, REG_CPURESET 0, подача файла в FIFO;
//   STATUS раз в 100 мс: флаги панели и паузы (молчание плагина > 2 с ESP считает выходом);
//   Esc — EXIT (#73), ждать «вернулись в слот 0», APP_STOP (#6D), текст WC, выход.
// Пауза — бит 2 REG_CPURESET: ядро держит кадр, ESP по нему же ставит паузу ЦАП.
// Перемотка — таблица по idx1 (avi_index.c, в страницах памяти Evo 12…15): поток с чанка
// записи — заголовок до movi, выравнивающий JUNK, файл с записи; ESP получает кадр начала и
// число байтов звука до записи. Список роликов (Enter, конец ролика) читает банк списка
// (LIST_OP_READ), рисует ESP; выбор — новый ролик тем же ядром.

#include "avi_shared.h"
#include "avi_list.h"

// --- обмен с ESP (esp_link.asm) ------------------------------------------------------------
#define ESP_LINK 0xB680     // = esp_link в build.py
typedef struct
{
  u8 *buf;            // ProtoBuf: принятый ответ
  u16 *rxlen;         // ProtoRxLen
  u8 cmd, resp;
  void *data;
  u16 len, polls;
  u8 repeat;
} ESP_ARGS;
#define ESP ((ESP_ARGS*)(ESP_LINK + 6))
#define ESP_POLLS 6000      // опросов на быстрый ответ (как у VDAC4 Cores)

u8 esp_init() __naked
{
  __asm
    jp ESP_LINK
  __endasm;
}

u8 esp_go() __naked
{
  __asm
    jp ESP_LINK + 3
  __endasm;
}

// Кадр cmd и ответ cmd | #80 не дольше repeat × polls опросов (repeat 0 — только послать).
bool esp_call(u8 cmd, void *data, u16 len, u8 repeat, u16 polls)
{
  ESP->cmd = cmd;
  ESP->resp = cmd | 0x80;
  ESP->data = data;
  ESP->len = len;
  ESP->polls = polls;
  ESP->repeat = repeat;
  return !esp_go();
}

#define CMD_MOD_QUERY 0x69  // [флаги][длина LE32][время FAT LE32][имя, 0] -> [0 есть / 1 шли / 2 нет
                            //   места / 3 плохое имя][свободно LE32]
#define CMD_MOD_PUT 0x6A    // [смещение LE32][данные] -> [1 принято][принято всего LE32]
#define CMD_MOD_COMMIT 0x6B // -> [1 положен в кэш]
#define CMD_APP_START 0x6C  // [имя, 0] -> [итог int8][приложение, 0][версия/текст, 0]
#define CMD_APP_STOP 0x6D
#define VC_START  0x70      // [имя ядра, 0] -> [1 — грузим]
#define VC_STATUS 0x72      // [тело панели] -> [состояние][VERSION][слотов][отменено][звук][ошибка]
#define VC_EXIT   0x73
#define VC_HDR    0x76      // [смещение LE32][последний][байты]
#define VC_LIST   0x77      // [показ][всего][первая][выбор][текущий LE16][сообщение,0][строки,0…]
#define VC_MSG    0x78      // [вид][заголовок,0][строка,0][строка,0]
#define VC_ROWS   0x79      // [первая][строки,0…] — строки страницы списка до VC_LIST (VIDEO.ELF 0.9)
#define VC_RUNNING 2
#define VC_BACK 3
#define VC_ERROR 255
#define VS_PAUSED 1
#define VS_PANEL 2
#define VS_SEEKING 8
#define VS_BUSY 16          // достраивается таблица перемотки: в панели ESP — текст имени

// --- ядро видео (SPI FT812) ----------------------------------------------------------------
#define CORE_FIFO 0xE0000UL
#define CR_RESET 1          // REG_CPURESET: сброс потока
#define CR_PAUSE 4          //   пауза: темп стоит

u32 core_word(u8 n)
{
  u32 v;
  ft_read(&v, (u32)n * 4, 4);   // блок состояния: слово защёлкивается по младшему байту
  return v;
}

// --- общие с avi_index.c ------------------------------------------------------------------
u32 core_avg;               // nAvgBytesPerSec звука (avi_probe) — шаг звука таблицы
u32 header_left;
u16 header_pos, header_size;

u8 header_byte()
{
  if (view_failed) return 0;
  if (header_pos == header_size)
  {
    if (!header_left || !read_chunk(header_left))
    { view_failed = 1; return 0; }
    header_size = min(header_left, FS_BUF_SIZE);
    header_pos = 0;
    header_left -= header_size;
  }
  return fs_buf[header_pos++];
}

u32 avi_word()
{
  u32 value = header_byte();
  value |= (u32)header_byte() << 8;
  value |= (u32)header_byte() << 16;
  value |= (u32)header_byte() << 24;
  return value;
}

// Таблица перемотки (avi_index.c) — в страницах плагина 12…15: см. index_copy ниже.
void index_write(void *src, u32 addr, u16 n) __sdcccall(0);
void index_read(void *dst, u32 addr, u16 n) __sdcccall(0);

#include "avi_index.c"

// #D0000 + k*12 → страница 12 + смещение/16 КиБ. Источник записи бывает в окне #C000 (готовая
// таблица FTViewConvert в fs_buf): через промежуточный буфер. Во время таблицы в #C000 —
// страница 1 (fs_buf).
u8 ix_bounce[64];
void index_copy(u8 *buf, u32 addr, u16 n, bool write)
{
  addr -= AVI_INDEX;
  while (n)
  {
    u16 off = (u16)addr & 0x3FFF, part = 0x4000 - off;
    if (part > n) part = n;
    if (part > sizeof ix_bounce) part = sizeof ix_bounce;
    if (write) memcpy(ix_bounce, buf, part);
    wc_map_buffer(12 + (u8)(addr >> 14));
    if (write) memcpy((u8*)0xC000 + off, ix_bounce, part);
    else memcpy(ix_bounce, (u8*)0xC000 + off, part);
    wc_map_buffer(1);
    if (!write) memcpy(buf, ix_bounce, part);
    buf += part; addr += part; n -= part;
  }
}
void index_write(void *src, u32 addr, u16 n) __sdcccall(0) { index_copy(src, addr, n, true); }
void index_read(void *dst, u32 addr, u16 n) __sdcccall(0) { index_copy(dst, addr, n, false); }

// --- подача в FIFO --------------------------------------------------------------------------
// Как у видеобанка (avi_feed): страницы буфера EVO 1…7 по 31 сектору — кэш SD, из него DMA
// в окно FIFO ядра (страница 8 — секторы idx1 фоновой таблицы перемотки, core_bg.c). Поток —
// [заголовок до movi + JUNK] (после перемотки) и файл с позиции.
#define CACHE_BYTES 0x3E00
#define CACHE_PAGES 7
u32 cr_fifo;                 // байтов FIFO ядра (степень 2)
u32 cr_write;                // позиция записи FIFO
u32 cr_room;                 // свободно в FIFO при последней подаче (core_feed)
// В FIFO ядра больше стольких байтов — шаг фоновой таблицы не помешает картинке (ядро 1.38:
// FIFO 2 МБ, у прежних 32 КБ — порога не достичь)
#define BG_LEVEL 0x40000UL
u32 cr_end, cr_left;         // конец подачи в файле; сколько до него осталось
u32 cr_disk_left;
u16 cr_cache_pos;
u8 cr_head, cr_tail, cr_count;

bool cache_fill()
{
  u16 count;
  u8 sectors;
  if (!cr_disk_left || cr_count == CACHE_PAGES) return true;
  count = min(cr_disk_left, CACHE_BYTES);
  sectors = (count + 511) >> 9;
  wc_map_buffer(cr_tail);
  if (*(volatile u8*)_ABT ||
      wc_load512(fs_buf, sectors) != 0xC000 + ((u16)sectors << 9))
  { fail(3); return false; }
  cr_disk_left -= count;
  if (++cr_tail == CACHE_PAGES + 1) cr_tail = 1;
  ++cr_count;
  return true;
}

// Свободно в FIFO (минус 4: полный не отличить от пустого).
u32 fifo_free()
{
  return (ft_rreg32(REG_MEDIAFIFO_READ) - cr_write - 4) & (cr_fifo - 1);
}

// Блок data (до 16 КиБ, в одной странице) в FIFO: DMA в окно и новая позиция записи.
void fifo_put(u8 *data, u16 count)
{
  u16 padded = (count + 3) & 0xFFFC;
  while (count < padded) data[count++] = 0;
  ft_load_ram_dma(data, *(u8*)((u16)data >= 0xC000 ? _PAGE3 : _PAGE2), CORE_FIFO + cr_write, padded);
  cr_write = (cr_write + padded) & (cr_fifo - 1);
  ft_wreg32(REG_MEDIAFIFO_WRITE, cr_write);
}

// Одно продвижение потока: true — блок ушёл. false без view_failed — FIFO полон или поток
// кончился: пауза идёт на чтение следующей страницы с SD.
bool core_feed()
{
  u16 count;
  if (view_failed || !cr_left) return false;
  count = min(cr_left, min(FS_BUF_SIZE, CACHE_BYTES - cr_cache_pos));
  if (count > cr_fifo - cr_write) count = cr_fifo - cr_write;
  if (!cr_count && !cache_fill()) return false;
  cr_room = fifo_free();
  if (cr_room < ((count + 3) & 0xFFFC)) { cache_fill(); return false; }
  wc_map_buffer(cr_head);
  fifo_put(fs_buf + cr_cache_pos, count);
  cr_left -= count;
  cr_cache_pos += count;
  if (cr_cache_pos == CACHE_BYTES)
  {
    cr_cache_pos = 0;
    if (++cr_head == CACHE_PAGES + 1) cr_head = 1;
    --cr_count;
  }
  return true;
}

// Ждать места в FIFO под count байтов (заголовок перед потоком); Esc прерывает.
bool fifo_room(u16 count)
{
  u8 tick = *(volatile u8*)_TMN;
  while (fifo_free() < ((count + 3) & 0xFFFC))
    if (*(volatile u8*)_ABT || (u8)(*(volatile u8*)_TMN - tick) > 150) { fail(20); return false; }
  return true;
}

// --- ролик ---------------------------------------------------------------------------------
u8 paused, keys, edges, key_tick, ended;
u16 panel_left;              // тики WC, пока видна панель (15 с после клавиши)
#define PANEL_TICKS 750
u32 cr_base;                 // кадр ролика, с которого идёт поток
u32 cr_audio;                // байтов звука до начала потока
u32 cr_slots;                // слотов темпа с начала потока (блок состояния, слово 2)
u8 st_buf[96];               // тело STATUS / HDR / MSG

void avi_first()
{
  wc_rewind();
  avi.cluster = *(u32*)0x38A8;
  avi.size = filesize;
}

// STATUS: тело для панели ESP. Ответ — состояние слоя; false — ESP молчит. bg_busy — перемотка
// ждёт фоновую таблицу (core_bg.c): процент + 1, в панели вместо подсказок «Indexing NN%».
u8 core_state, bg_busy;
bool core_status(bool body)
{
  u8 *p = st_buf;
  if (body)
  {
    *p++ = 1;
    *p++ = (paused ? VS_PAUSED : 0) | (paused || panel_left ? VS_PANEL : 0) | (bg_busy ? VS_BUSY : 0);
    memcpy(p, &cr_base, 4); p += 4;
    memcpy(p, &avi.total, 4); p += 4;
    memcpy(p, &avi.period, 4); p += 4;
    memcpy(p, &cr_audio, 4); p += 4;
    if (bg_busy)
    {
      u8 n = bg_busy - 1;
      memcpy(p, "Indexing ", 9); p += 9;
      if (n >= 100) *p++ = '1';
      if (n >= 10) *p++ = '0' + n / 10 % 10;
      *p++ = '0' + n % 10;
      *p++ = '%';
    }
    *p++ = 0;               // имя ролика ESP не нужно
  }
  if (!esp_call(VC_STATUS, st_buf, p - st_buf, 1, ESP_POLLS)) return false;
  core_state = ESP->buf[0];
  return true;
}

// Заголовок ролика до movi — в ESP (формат звука): кадры HDR по 512 байтов данных. Чтение —
// в fs_buf (#C000…#CFFF), кадр собирается за ним (#D000): кэш потока ещё не начат.
#define HDR_FRAME ((u8*)0xD000)
bool core_hdr()
{
  u32 left = avi.movi_start, off = 0;
  if (left > 0x10000UL) left = 0x10000UL;
  wc_map_buffer(1);
  if (!avi_rewind()) return false;
  while (left)
  {
    u16 count = min(left, FS_BUF_SIZE), i;
    if (!read_chunk(left)) return false;
    for (i = 0; i < count; i += 512)
    {
      u16 part = min(count - i, 512);
      memcpy(HDR_FRAME, &off, 4);
      HDR_FRAME[4] = left == count && i + part == count;
      memcpy(HDR_FRAME + 5, fs_buf + i, part);
      if (!esp_call(VC_HDR, HDR_FRAME, part + 5, 1, ESP_POLLS)) return false;
      off += part;
    }
    left -= count;
  }
  return true;
}

// Поток с кадра записи таблицы (base > 0: заголовок + JUNK + файл с avi.seek_pos) или с
// начала файла. Перед подачей — новая эпоха звука и STATUS с началом потока.
bool core_restart()
{
  u32 base = avi.seek_pos & ~3UL;
  cr_base = avi.seek_frame;
  cr_audio = avi.seek_audio;
  ft_wreg8(FT_REG_CPURESET, CR_RESET);
  core_status(true);
  ft_wreg8(FT_REG_CPURESET, 0);
  paused = 0;
  cr_write = 0;
  cr_room = cr_fifo;
  cr_slots = 0;
  ended = 0;
  cr_end = avi.movi_end;
  if (cr_end > avi.size) cr_end = avi.size;
  if (base)
  {
    // Заголовок до 'movi' и JUNK до позиции записи (как index_header банка изображений).
    u32 left = avi.movi_start;
    wc_map_buffer(1);
    if (!avi_rewind()) { fail(5); return false; }
    while (left)
    {
      u16 count = min(left, FS_BUF_SIZE);
      if (!read_chunk(left)) return false;
      left -= count;
      if (!left)
      {
        u8 fill = -(u8)(avi.movi_start + 8) & 3;
        memcpy(fs_buf + count, "JUNK\0\0\0\0\0\0", 10);
        fs_buf[count + 4] = fill + (avi.seek_pos & 3);
        count += 8 + fill;
      }
      if (!fifo_room(count)) return false;
      fifo_put(fs_buf, count);
    }
    if (!avi_rewind() || !disk_skip(base >> 9)) { fail(5); return false; }
  }
  else if (!avi_rewind()) { fail(5); return false; }
  cr_left = cr_end - base;
  cr_disk_left = cr_end - (base & ~511UL);
  cr_cache_pos = base & 511;
  cr_head = cr_tail = 1;
  cr_count = 0;
  return !view_failed;
}

// --- клавиши -------------------------------------------------------------------------------
// Восемь клавиш WC (номера функций API) — биты 0…7.
u8 keys_read(const u8 *codes)
{
  u8 k = 0, bit = 1;
  for (; bit; bit <<= 1, ++codes)
    if (wc_api__bool(*codes)) k |= bit;
  return k;
}

// Плеер: Space, Left, Right, Home, PgDn, PgUp, Enter, любая (панель на 15 с); Esc — ABT.
const u8 play_keys[8] = {_SPKE, _LFFF, _RGGG, _HOME, _PGD, _PGU, _ENKE, _ANYK};
// Список: вверх, вниз, Enter, Esc, PgUp, PgDn, Home, End.
const u8 list_keys[8] = {_UPPP, _DWWW, _ENKE, _ESC, _PGU, _PGD, _HOME, _END};

// Раз в тик WC: нажатия копятся в edges, пока их не заберёт цикл плеера.
void keys_poll()
{
  u8 k;
  if (*(volatile u8*)_TMN == key_tick) return;
  key_tick = *(volatile u8*)_TMN;
  k = keys_read(play_keys);
  edges |= k & ~keys;
  keys = k;
}

// --- выход ---------------------------------------------------------------------------------
// Ядро — назад в слот 0 (EXIT, ждать состояния 3 до 15 с), приложение ESP — выгрузить, текст WC.
// Любой wc_exit идёт через view_cleanup: ядро не остаётся на HDMI после выхода в WC.
u8 core_live;
void core_leave()
{
  u16 n;
  if (!core_live) return;
  core_live = 0;
  esp_call(VC_EXIT, 0, 0, 1, ESP_POLLS);
  for (n = 0; n < 75; ++n)
  {
    u8 t = *(volatile u8*)_TMN;
    // Разность — в u8: без приведения она в int, и при t 246…255 никогда не дорастала до 10 —
    // плагин навсегда вставал после VC_EXIT (Tang уже в слоте 0, APP_STOP не уходил): так висли
    // выход по PgUp/PgDn и Esc (28.10.2026, примерно каждый четвёртый).
    while ((u8)(*(volatile u8*)_TMN - t) < 10) ;
    if (core_status(false) && core_state != VC_RUNNING) break;
  }
  esp_call(CMD_APP_STOP, 0, 0, 4, 0xFFFF);
  ft_spi_unsel();
  __asm
    push ix
    xor a
    ex af, af
    ld a, #80
    call _WCAPI
    xor a
    ex af, af
    ld a, #_MNGV_PL
    call _WCAPI
    pop ix
  __endasm;
}

void view_cleanup()
{
  core_leave();
}

// Ошибка после старта ядра: назад в слот 0, окно сообщения банка списка, выход в WC.
void core_fail(u8 op)
{
  core_leave();
  if (!*(volatile u8*)_ABT)
  {
    wc_map_buffer(1);
    LIST_FAR->op = op;
    LIST_FAR->code = fail_at;
    far_call_core(LIST_PAGE, LIST_FAR);
  }
  wc_exit(WC_EXIT);
}

// --- список роликов ------------------------------------------------------------------------
// Имя — целиком (до NAME_MAX букв, длиннее — с «...» банка списка): по ширине строки его обрезает
// ESP. Прежде плагин резал его на 56 буквах — так 17 строк входили в кадр 1024 байтов, и в окне
// VIDEO.ELF 0.8 (шрифт 24, строка 728 точек) имя обрывалось, хотя место в строке было
// (пользователь, 28.10.2026). Теперь строки с ROWS_FIRST-й уходят раньше командой VC_ROWS (окно не
// меняется), а VC_LIST с флагом #80 в «показ» их оставляет.
#define ROWS 17
#define ROWS_FIRST 9
u8 ls_count, ls_sel, ls_top, ls_cur;

u8 *row_put(u8 *q, u8 i)
{
  const char *s = LIST_E[ls_top + i].name;
  while ((*q++ = *s++)) ;
  return q;
}

// Окно списка в ESP: показ, всего, первая, выбор, текущий, сообщение, строки страницы.
void list_send(u8 shown, const char *msg)
{
  u8 *p = fs_buf + 0x200, *q = p, i;   // за сектором каталога, до таблицы (#CC00)
  bool rows = shown && !*msg;
  bool split = rows && ls_count > ls_top + ROWS_FIRST;
  wc_map_buffer(1);
  if (split)
  {
    *q++ = ROWS_FIRST;
    for (i = ROWS_FIRST; i < ROWS && ls_top + i < ls_count; ++i) q = row_put(q, i);
    esp_call(VC_ROWS, p, q - p, 1, ESP_POLLS);
    q = p;
  }
  *q++ = shown | (split ? 0x80 : 0);
  *q++ = ls_count; *q++ = 0;
  *q++ = ls_top; *q++ = 0;
  *q++ = ls_sel; *q++ = 0;
  *q++ = ls_cur; *q++ = ls_cur == 0xFF ? 0xFF : 0;
  while ((*q++ = *msg++)) ;
  if (rows)
    for (i = 0; i < (split ? ROWS_FIRST : ROWS) && ls_top + i < ls_count; ++i) q = row_put(q, i);
  esp_call(VC_LIST, p, q - p, 1, ESP_POLLS);
}

// Соседний ролик папки для PgDn/PgUp: список — тот же, что по Enter (банк списка читает каталог, без
// окна: окно «Reading folder...» пользователь отверг, 28.10.2026; STATUS перед чтением — ESP считает
// молчание плагина от него, а чтение каталога — доли секунды). true — avi.cluster/size — его; false —
// каталог не прочитан или за краем списка.
bool core_neighbour(bool next)
{
  u8 n, i;
  core_status(true);
  wc_map_buffer(1);
  *AVI_FAR = avi;
  LIST_FAR->op = LIST_OP_READ;
  far_call_core(LIST_PAGE, LIST_FAR);
  wc_map_buffer(1);
  n = LIST_FAR->count;
  i = LIST_FAR->sel;
  if (!LIST_FAR->ok || !n || (next ? i + 1 >= n : !i)) return false;
  i = next ? i + 1 : i - 1;
  avi.cluster = LIST_E[i].cluster;
  avi.size = LIST_E[i].size;
  return true;
}

// Список поверх кадра: true — выбран ролик (avi.cluster/size — его), false — Esc. end — конец
// ролика (Esc оставляет кадр на паузе, Space начнёт сначала).
bool core_list()
{
  u8 tick = *(volatile u8*)_TMN, prev = 0xFF, hold = 0, k, edge, old;
  bool chosen = false;
  ls_count = ls_sel = ls_top = 0;
  ls_cur = 0xFF;
  list_send(1, "Reading folder...");
  wc_map_buffer(1);
  *AVI_FAR = avi;
  LIST_FAR->op = LIST_OP_READ;
  far_call_core(LIST_PAGE, LIST_FAR);
  wc_map_buffer(1);
  ls_count = LIST_FAR->count;
  ls_sel = ls_cur = LIST_FAR->sel;
  if (!ls_count) ls_cur = 0xFF;
  ls_top = ls_sel > ROWS / 2 ? ls_sel - ROWS / 2 : 0;
  if (ls_count > ROWS && ls_top > ls_count - ROWS) ls_top = ls_count - ROWS;
  list_send(1, !LIST_FAR->ok ? "Cannot read the folder" : !ls_count ? "No MJPEG AVI files here" : "");
  // Esc, нажатый, пока читался каталог, — уже взведённый ABT: окно закрывается сразу (в
  // цикле ниже он не новое нажатие).
  while (!*(volatile u8*)_ABT)
  {
    // ESP ждёт знака жизни, ядро — обмена: STATUS раз в 5 тиков.
    while (*(volatile u8*)_TMN == tick) ;
    tick = *(volatile u8*)_TMN;
    if (!(tick % 5)) core_status(true);
    k = keys_read(list_keys);
    if (*(volatile u8*)_ABT) k |= 8;
    edge = k & ~prev; prev = k;
    if (k & 0x33)
    {
      if (hold < 255) ++hold;
      if (hold > 20 && !(hold % 3)) edge |= k & 0x33;
    }
    else hold = 0;
    if (edge & 8) break;
    if (edge & 4)
    {
      if (ls_count)
      {
        chosen = true;
        avi.cluster = LIST_E[ls_sel].cluster;
        avi.size = LIST_E[ls_sel].size;
      }
      break;
    }
    if (!ls_count) continue;
    old = ls_sel;
    if ((edge & 1) && ls_sel) --ls_sel;
    if ((edge & 2) && ls_sel + 1 < ls_count) ++ls_sel;
    if (edge & 16) ls_sel = ls_sel > ROWS ? ls_sel - ROWS : 0;
    if (edge & 32) ls_sel = ls_sel + ROWS < ls_count ? ls_sel + ROWS : ls_count - 1;
    if (edge & 64) ls_sel = 0;
    if (edge & 128) ls_sel = ls_count - 1;
    if (ls_sel != old)
    {
      if (ls_sel < ls_top) ls_top = ls_sel;
      if (ls_sel >= ls_top + ROWS) ls_top = ls_sel - ROWS + 1;
      list_send(1, "");
    }
  }
  list_send(0, "");
  // Клавиши, закрывшие окно, не должны дойти до плеера.
  do { while (*(volatile u8*)_TMN == tick) ; tick = *(volatile u8*)_TMN; } while (wc_api__bool(_ANYK));
  *(volatile u8*)_ABT = 0;
  keys = 0xFF; edges = 0;
  return chosen;
}

// --- плеер ---------------------------------------------------------------------------------
// Фоновая таблица перемотки (core_bg.c, банк списка): начать с нового ролика; шаг — короткий.
u8 bg_run;                   // таблица строится (копия BG_FAR->state: страница 1 не всегда в #C000)
void bg_call(u8 op)
{
  wc_map_buffer(1);
  LIST_FAR->op = op;
  far_call_core(LIST_PAGE, LIST_FAR);
  wc_map_buffer(1);
  bg_run = BG_FAR->state == BG_RUN;
}

void bg_begin()
{
  wc_map_buffer(1);
  *AVI_FAR = avi;
  BG_FAR->avg = core_avg;
  bg_call(LIST_OP_BG_START);
}

// Цель за готовой частью таблицы: достроить её подряд (поток стоит), в панели — проценты.
// Esc — выход в WC (как в плеере).
void bg_wait(u32 target)
{
  u8 tick = *(volatile u8*)_TMN;
  for (;;)
  {
    wc_map_buffer(1);
    if (BG_FAR->state != BG_RUN || (BG_FAR->step && target / BG_FAR->step < BG_FAR->count)) break;
    bg_busy = BG_FAR->percent + 1;
    if (*(volatile u8*)_ABT) { core_leave(); wc_exit(WC_EXIT); }
    bg_call(LIST_OP_BG_STEP);
    if ((u8)(*(volatile u8*)_TMN - tick) >= 5)
    {
      tick = *(volatile u8*)_TMN;
      core_status(true);
    }
  }
  if (bg_busy) { bg_busy = 0; core_status(true); }
}

// Перейти к кадру target: с записи таблицы или с начала.
bool core_seek(u32 target)
{
  ft_wreg8(FT_REG_CPURESET, CR_PAUSE);      // кадр стоит, пока достраивается таблица
  paused = 1;
  if (target) bg_wait(target);
  wc_map_buffer(1);
  avi.target = target;
  view_failed = 0;
  index_seek(&avi);
  if (view_failed) return false;
  return core_restart();
}

// Новый ролик (avi.cluster/size): разбор, заголовок в ESP, поток с начала.
bool core_open()
{
  wc_map_buffer(1);
  view_failed = 0;
  index_state = 0;
  core_avg = 0;
  filesize = avi.size;
  if (!avi_rewind()) { fail(6); return false; }
  if (!avi_probe()) return false;
  avi.seek_pos = avi.seek_frame = avi.seek_audio = 0;
  ft_wreg8(FT_REG_CPURESET, CR_RESET);      // HDR — при сброшенном потоке (audio.c VIDEO.ELF)
  if (!core_hdr()) return false;
  bg_begin();
  return core_restart();
}

void core_play()
{
  u8 tick = *(volatile u8*)_TMN, idle = 0;
  u32 last_slots = 0;
  panel_left = PANEL_TICKS;
  keys = 0xFF; edges = 0;
  key_tick = tick;
  if (!core_open()) core_fail(LIST_OP_FILE);
  for (;;)
  {
    u8 edge;
    u32 target = 0;
    bool seek = false, fed;
    // Поток: блок за круг, пока есть место; между блоками — клавиши и тик. Шаг фоновой таблицы
    // перемотки (~20 мс): на паузе; когда в FIFO ядра больше BG_LEVEL (ядро 1.38: FIFO 2 МБ,
    // запас — секунды); и когда FIFO не берёт, а кэш полон (или файл дочитан) — у ядра до 1.38
    // (FIFO 32 КБ, кэш — 0,15 с) только так, и картинка на нём вставала.
    fed = !paused && core_feed();
    if (bg_run && !view_failed &&
        (paused || cr_fifo - cr_room > BG_LEVEL ||
         (!fed && (cr_count == CACHE_PAGES || !cr_disk_left))))
      bg_call(LIST_OP_BG_STEP);
    if (view_failed) core_fail(LIST_OP_FILE);
    keys_poll();
    if (*(volatile u8*)_TMN == tick) continue;
    tick = *(volatile u8*)_TMN;
    if (panel_left) --panel_left;
    edge = edges; edges = 0;
    if (edge) panel_left = PANEL_TICKS;
    if (*(volatile u8*)_ABT) { core_leave(); wc_exit(WC_EXIT); }
    // PgDn/PgUp — соседний ролик папки тем же ядром. Прежде — выход в WC с соседним файлом: ядро
    // уходило в слот 0, WC запускал плагин снова, и ядро грузилось из слота 3 заново (пользователь,
    // 28.10.2026: «зачем плеер по pg up/dn каждый раз переходит в Slot0?»). За краем списка роликов —
    // как прежде, соседний файл WC.
    if (edge & 48)
    {
      bool next = (edge & 16) != 0;
      ft_wreg8(FT_REG_CPURESET, CR_PAUSE);
      paused = 1;
      if (core_neighbour(next))
      {
        if (!core_open()) core_fail(LIST_OP_FILE);
        keys = 0xFF; edges = 0;              // держат PgDn — следующий только после отпускания
        continue;
      }
      core_leave();
      wc_exit(next ? WC_NEXT_FILE : WC_PREV_FILE);
    }
    // Слоты темпа — время ролика: конец, шаг перемотки.
    cr_slots = core_word(2);
    if (edge & 1)
    {
      // Space в конце ролика — сначала, как Home.
      if (ended) { target = 0; seek = true; }
      else
      {
        paused ^= 1;
        ft_wreg8(FT_REG_CPURESET, paused ? CR_PAUSE : 0);
      }
    }
    if (edge & 14)
    {
      u32 step = 10000000UL / avi.period, now = cr_base + (cr_slots ? cr_slots - 1 : 0);
      if (!step) step = 1;
      target = now;
      if (edge & 2) target = target > step ? target - step : 0;
      if (edge & 4) target = target + step < avi.total ? target + step : avi.total - 1;
      if (edge & 8) target = 0;
      seek = true;
    }
    if (edge & 64)
    {
      // Enter — список: ролик на паузе; выбран — он с начала, Esc — дальше как был.
      u8 was = paused;
      ft_wreg8(FT_REG_CPURESET, CR_PAUSE);
      paused = 1;
      if (core_list())
      {
        if (!core_open()) core_fail(LIST_OP_FILE);
        continue;
      }
      paused = was;
      if (!ended) ft_wreg8(FT_REG_CPURESET, paused ? CR_PAUSE : 0);
    }
    if (seek)
    {
      bool was = paused;
      keys &= 1;
      if (!core_seek(target)) core_fail(LIST_OP_FILE);
      if (was && !ended) { paused = 1; ft_wreg8(FT_REG_CPURESET, CR_PAUSE); }
      ended = 0;
      continue;
    }
    // STATUS раз в 5 тиков (100 мс): панель, пауза; ESP жива — иначе выход.
    if (!(tick % 5) && !core_status(true)) core_fail(LIST_OP_FILE);
    if (core_state != VC_RUNNING && core_state != 0) { core_leave(); wc_exit(WC_EXIT); }
    // Конец ролика: поток подан, FIFO пуст, кадры не идут секунду — список (как FTView).
    if (!paused && !cr_left && cr_slots == last_slots && (core_word(7) & 4))
    {
      if (++idle == 50)
      {
        idle = 0;
        ended = 1;
        paused = 1;
        panel_left = PANEL_TICKS;
        if (core_list())
        {
          if (!core_open()) core_fail(LIST_OP_FILE);
          continue;
        }
      }
    }
    else idle = 0;
    last_slots = cr_slots;
  }
}

// --- старт ---------------------------------------------------------------------------------
const WC_TX_WINDOW core_fail_wnd =
{
  WC_WIND_HDR_TXT, 0, 19, 10, 42, 6, 0x2F, 0, 0, 0, 0,
  "\x0E" " FTView ", "",
  "\x0E\x0C\x01" "Video core did not start.\r"
  "\x0E\x0C\x02" "Playing through FT812."
};

// Слой ESP ядра видео — VIDEO.ELF в /WC/ESP на томе WC, как модули плеера (PLAYER.ELF, MP3.ELF):
// битстрим ядра — во flash Tang (слот 3), на SD Tang его нет, и ELF при нём не лежит (пользователь,
// 28.10.2026; прежде — CORE_APP: CORES/VIDEO.ELF с SD Tang). Каталог — служебным потоком WC (#FD,
// WC Improved), иначе от корня тома панели (как у плеера). В кэше ESP его нет или он изменился
// (имя, длина, время записи — MOD_QUERY) — по сектору MOD_PUT и MOD_COMMIT (ESP пишет его во flash
// — секунды). Затем поток — снова тот, с которым WC запустил плагин (см. back). false — файла нет
// или ESP его не взяла.
#define MOD_BUF ((u8*)0xD000)   // кадр MOD_PUT: смещение, сектор — в окне #C000 (страница 1)
static const u8 fe_wc[] = "\x10" "wc", fe_esp[] = "\x10" "esp", fe_elf[] = "\0" "VIDEO.ELF";
u8 mod_entry[32];

bool esp_module()
{
  u32 size, off;
  u16 n;
  u8 *q = MOD_BUF;
  u8 dos = *(volatile u8*)0x600B, cloned = 0;
  bool ok = false;
  wc_map_buffer(1);
  if (!wc_stream(0xFD, 0))
  {
    cloned = 1;
    wc_stream(0xFE, 0);
    wc_stream(0, 0xFFFF);
    wc_stream(0xFF, 0);
    wc_stream(1, 0xFFFF);
    wc_stream(0xFF, 0);
    wc_stream(0, 0xFFFF);
    if (!wc_fentry(fe_wc, &size)) goto back;
    wc_api_ix(_GDIR);
  }
  if (!wc_fentry(fe_esp, &size)) goto back;
  wc_api_ix(_GDIR);
  if (!wc_fentry(fe_elf, &size)) goto back;
  wc_tentry(mod_entry);
  q[0] = 0;                                 // флаги 0 — модуль (не ресурс)
  memcpy(q + 1, &size, 4);
  memcpy(q + 5, mod_entry + 22, 4);         // время и дата записи FAT
  memcpy(q + 9, fe_elf + 1, sizeof fe_elf - 1);
  if (!esp_call(CMD_MOD_QUERY, q, 9 + sizeof fe_elf - 1, 32, 0xFFFF)) goto back;
  if (ESP->buf[0] == 1)
  {
    wc_api_ix(_GFILE);
    for (off = 0; off < size; off += n)
    {
      n = size - off > 512 ? 512 : (u16)(size - off);
      if (!wc_load512(q + 4, 1)) goto back;
      memcpy(q, &off, 4);
      if (!esp_call(CMD_MOD_PUT, q, 4 + n, 4, ESP_POLLS) || ESP->buf[0] != 1) goto back;
    }
    if (!esp_call(CMD_MOD_COMMIT, 0, 0, 32, 0xFFFF) || ESP->buf[0] != 1) goto back;
  }
  else if (ESP->buf[0]) goto back;
  ok = true;
back:
  // Поток WC — страница WildDOS: вызовы API берут её из #600B, прямой вход GIPAG (avi_rewind) — из
  // окна #0000 (#6000), и API после вызова возвращает в окно прежнюю. STREAM меняет #600B: прежде
  // здесь клон панели (#FE) выбирал поток 0, и у ролика из правой панели (поток 1) GIPAG ставил одну
  // страницу, а LOAD512 читал другую — чужие данные, «Reason 00» (28.10.2026 на плате). Теперь
  // #600B — прежняя. Служебный поток WC (#FD) — своя страница: поток ролика он не трогает; поиск от
  // корня (старый WC) уводил клон в /WC/ESP — его снова клонирует #FE (в потоки 0 и 1).
  if (cloned) wc_stream(0xFE, 0);
  *(volatile u8*)0x600B = dos;
  return ok;
}

// ESP, VIDEO.ELF и ядро. false — их нет или ядро не встало: путь FT812.
bool core_start()
{
  u16 n;
  bool ok = false;
  static const char name[] = "VIDEO";
  if (esp_init()) return false;
  if (!esp_module()) return false;
  if (!esp_call(CMD_APP_START, (void*)(fe_elf + 1), sizeof fe_elf - 1, 32, 0xFFFF) || ESP->buf[0])
    return false;
  // Под этим именем в кэше ESP оказалось другое приложение — ядру видео оно не хозяин.
  if (memcmp(ESP->buf + 1, "video", 6))
  {
    esp_call(CMD_APP_STOP, 0, 0, 4, 0xFFFF);
    return false;
  }
  // Окна на время загрузки нет (пользователь, 27.10.2026): ядро из слота 3 flash Tang встаёт за
  // доли секунды.
  if (esp_call(VC_START, (void*)name, sizeof name, 1, ESP_POLLS) && ESP->buf[0])
    for (n = 0; n < 300; ++n)                  // до 60 с: STATUS раз в 10 тиков
    {
      u8 t = *(volatile u8*)_TMN;
      while ((u8)(*(volatile u8*)_TMN - t) < 10) ;
      if (!core_status(false)) continue;
      if (core_state == VC_RUNNING) { ok = true; break; }
      if (core_state == VC_ERROR || core_state == VC_BACK) break;
    }
  if (ok)
  {
    // Монитор — ядро (M_SEL = 1), затем само ядро: REG_ID и блок состояния.
    wc_vmode(0x87);
    graphics_active = 1;
    cr_fifo = core_word(1);
    core_live = 1;
    if (ft_rreg8(FT_REG_ID) == FT_ID && core_word(0) == 0x31444956UL &&
        cr_fifo >= 0x1000 && !(cr_fifo & (cr_fifo - 1))) return true;
    core_leave();
    return false;
  }
  esp_call(CMD_APP_STOP, 0, 0, 4, 0xFFFF);
  // Ядро не встало (нет VIDEO.BIN, сбой загрузки): окно на 2 с — дальше путь FT812, но молча
  // он сошёл бы за «ядро видео тормозит».
  wc_api_u16(_PRWOW, (u16)&core_fail_wnd);
  for (n = 0; n < 100; ++n) { u8 t = *(volatile u8*)_TMN; while (*(volatile u8*)_TMN == t) ; }
  wc_api_u16(_RRESB, (u16)&core_fail_wnd);
  return false;
}

// Вход из банка изображений (core_try): играет ролик и выходит в WC сам; вернулся — путь FT812.
void far_entry(void *p)
{
  p;
  wc_map_buffer(1);
  view_failed = 0;
  fail_at = 0;
  graphics_active = 0;
  avi_first();
  // Ролик для ядра: MJPEG до 2048×1536 (больше 1024×768 ядро 1.38 кладёт вдвое меньше).
  // Иначе — путь FT812 (его отказ скажет сам).
  if (!avi_rewind() || !avi_probe()) { view_failed = 0; return; }
  if (!core_start()) return;
  core_play();
}

// WC входит только в банк изображений (#8000); эти имена нужны шлюзам crt0.s.
void main() __naked
{
  __asm
    ret
  __endasm;
}

void main_dispatch()
{
}
