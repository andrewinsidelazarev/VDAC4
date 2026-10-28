// Фоновая таблица перемотки банка ядра видео VDAC4 (банк списка, страница 10; вызовы —
// LIST_OP_BG_START и LIST_OP_BG_STEP из core.c). Прежде таблица строилась при первой перемотке
// целиком, с окном «Building seek index...» на полэкрана (пользователь, 27.10.2026: «кто мешает
// построить в фоне?»). Теперь банк ядра зовёт короткий шаг, пока ролик идёт и кэш потока полон;
// перемотка берёт готовую часть таблицы, а за неё — ждёт (в панели ESP — «Indexing NN%»).
//
// Сам ролик лежит на карте Evo: ESP его не видит, idx1 читает Evo. Читателей два — поток ролика
// (банк ядра, LOAD512 по 31 сектору в кэш страниц 1…7) и idx1 (здесь) — на одном потоке WildDOS.
// Его позиция — NSDC/EOC (#3848), LLHL и LTHL/LTDE (#3860…#3867), CUHL/CUDE (#38A8): LOAD512
// продолжает с них (NSDC = EOC = 0 — с начала кластера CUHL через GIPAG), драйвер ставится по LTHL
// заново на каждую порцию (CORE32.ASM, LOAD512/LPREX/NXTC). Поэтому шаг сохраняет эти 14 байт
// потока ролика, ставит свои, читает и возвращает их обратно — без нового прохода цепочки FAT.
//
// Шаг (не дольше ~20 мс: кэш потока банка ядра — 7 страниц, ~0,15 с ролика):
//   WALK — к сектору заголовка чанка: целые кластеры — по цепочке (fat_walk, до 255 кластеров,
//          ~3 сектора FAT), назад — с начала файла;
//   HEAD — до 16 секторов до заголовка; idx1 — к записям, JUNK и прочее — дальше (до 8 чанков);
//   SCAN — до 16 секторов idx1 в страницу 8 и записи из них — index_scan (как у банка
//          изображений, avi_scan.c); запись, разрезанная границей порции, — в начало следующей.
// Звук — в байтах (BG_FAR->avg — nAvgBytesPerSec), частота кадров — дробью rate/scale, как
// прежде у банка ядра (index_build). Таблица — в страницах 12…15 (#D0000 + k·12).

#define BG_PAGE 8                      // буфер секторов idx1 (страница буфера 8; кэш потока — 1…7)
#define BG_BUF ((u8*)0xC010)           // перед ним — место под недочитанную запись (до 15 байт)
#define BG_SECTORS 16                  // секторов за шаг
#define BG_CLUSTERS 255                // кластеров цепочки за шаг
#define WD_NSDC 0x3848                 // NSDC, EOC (KERNEL.ASM: VALS = DRVR + 24·3)
#define WD_LLHL 0x3860                 // LLHL, LTHL, LTDE
#ifndef AVI_INDEX
#define AVI_INDEX 0xD0000UL
#define AVI_INDEX_MAX 5460
#endif
#define INDEX_WRITE _index_write

enum { PH_OFF, PH_WALK, PH_HEAD, PH_SCAN };
u8 bg_ph, bg_result, bg_tries, bg_carry;
u32 bg_sec;                            // сектор файла, который читатель idx1 прочтёт следующим
u32 bg_pos;                            // WALK/HEAD — заголовок чанка; SCAN — следующая запись idx1
u32 bg_left, bg_all;                   // записей idx1 осталось / было
u32 bg_size, bg_cl;                    // размер файла и его первый кластер
u32 bg_p[2];                           // «AVI_SHARED» для index_scan: ему нужен лишь movi_start (+4)
u8 bg_wd[14];                          // позиция читателя idx1 на потоке WildDOS
u8 *bg_ptr;

void wd_get(u8 *s)
{
  memcpy(s, (u8*)WD_NSDC, 2);
  memcpy(s + 2, (u8*)WD_LLHL, 8);
  memcpy(s + 10, (u8*)0x38A8, 4);      // CUHL/CUDE
}

void wd_put(const u8 *s)
{
  memcpy((u8*)WD_NSDC, s, 2);
  memcpy((u8*)WD_LLHL, s + 2, 8);
  memcpy((u8*)0x38A8, s + 10, 4);
}

// Запись таблицы (ABI 0, как index_write банка ядра): страницы 12…15, затем снова буфер idx1.
// Источник — ib_entry в памяти банка (#BD00…), окно #C000 его не задевает.
void index_write(void *src, u32 addr, u16 n) __sdcccall(0)
{
  u8 *s = src;
  addr -= AVI_INDEX;
  while (n)
  {
    u16 off = (u16)addr & 0x3FFF, part = 0x4000 - off;
    if (part > n) part = n;
    wc_map_buffer(12 + (u8)(addr >> 14));
    memcpy((u8*)0xC000 + off, s, part);
    s += part; addr += part; n -= part;
  }
  wc_map_buffer(BG_PAGE);
}

// Записи idx1 подряд из буфера (index_scan берёт их через idx_entry)
u8 *idx_entry()
{
  u8 *e = bg_ptr;
  bg_ptr += 16;
  return e;
}

#include "avi_walk.c"
#include "avi_scan.c"

void bg_end(u8 result)
{
  bg_ph = PH_OFF;
  bg_result = result;
}

// n секторов с bg_sec в BG_BUF. Esc (флаг WC) — шаг отложен, LOAD512 его не трогает.
bool bg_read(u8 n)
{
  if (*(volatile u8*)_ABT) return false;
  if (wc_load512(BG_BUF, n) != (u16)BG_BUF + ((u16)n << 9)) { bg_end(BG_NONE); return false; }
  bg_sec += n;
  return true;
}

// Читатель — с начала файла: первый кластер, NSDC = EOC = 0 (LOAD512 встанет на него сам)
void bg_rewind()
{
  memset(bg_wd, 0, 10);
  memcpy(bg_wd + 10, &bg_cl, 4);
  wd_put(bg_wd);
  bg_sec = 0;
}

void bg_walk()
{
  u8 spc = *(u8*)0x3894;               // BSECPC
  u32 to = bg_pos >> 9, at, n;
  if (to < bg_sec) bg_rewind();        // цель позади читателя (короткий JUNK в прочитанном)
  at = bg_sec - *(u8*)WD_NSDC;         // начало текущего кластера, в секторах файла
  n = (to - at) / spc;
  if (!n) { bg_ph = PH_HEAD; return; }
  if (n > BG_CLUSTERS) n = BG_CLUSTERS;
  fw_count = n * spc;
  if (!fat_walk()) { bg_end(BG_NONE); return; }
  // fat_walk поставил поток GIPAG на начало итогового кластера; NSDC — тоже с начала
  *(u8*)WD_NSDC = 0;
  *(u8*)(WD_NSDC + 1) = 0;
  bg_sec = at + n * spc;
}

// Записи в [p, end): сколько их целых — index_scan; остаток — в начало буфера к следующему шагу
void bg_entries(u8 *p, u8 *end)
{
  u32 m = (u16)(end - p) >> 4, done;
  if (m > bg_left) m = bg_left;
  bg_ptr = p;
  if (m)
  {
    ib_n = m;
    index_scan((AVI_SHARED*)bg_p);
    if (view_failed) { bg_end(BG_NONE); return; }
    done = m - ib_n;                   // index_scan выходит и раньше: таблица полна, Esc
    bg_left -= done;
    bg_pos += done << 4;
  }
  if (!bg_left || ib_k >= ib_count) { bg_end(BG_DONE); return; }
  // index_scan вышел посреди порции по Esc: плагин и так уходит в WC
  if (end - bg_ptr >= 16) { bg_end(BG_NONE); return; }
  bg_carry = end - bg_ptr;
  memcpy(BG_BUF - bg_carry, bg_ptr, bg_carry);
}

void bg_head()
{
  u32 first = bg_sec, need = ((bg_pos + 8 + 511) >> 9) - bg_sec, size;
  u8 n = need > BG_SECTORS ? BG_SECTORS : (u8)need, *h;
  if (!bg_read(n)) return;
  if (bg_sec < ((bg_pos + 8 + 511) >> 9)) return;     // до заголовка ещё не дочитали
  for (;;)
  {
    h = BG_BUF + (u16)(bg_pos - (first << 9));
    memcpy(&size, h + 4, 4);
    if (!memcmp(h, "idx1", 4))
    {
      if (size > bg_size - bg_pos - 8) { bg_end(BG_NONE); return; }
      bg_all = bg_left = size >> 4;
      bg_pos += 8;
      bg_ph = PH_SCAN;
      bg_entries(h + 8, BG_BUF + ((u16)n << 9));
      return;
    }
    // JUNK (в том числе с готовой таблицей FTViewConvert: idx1 за ней тоже есть) — дальше;
    // следующий заголовок целиком в прочитанном — сразу он
    bg_pos += 8 + size + (size & 1);
    if (++bg_tries == 8 || bg_pos + 8 > bg_size) { bg_end(BG_NONE); return; }
    if (bg_pos + 8 > bg_sec << 9) break;
  }
  bg_ph = PH_WALK;
}

void bg_scan()
{
  u32 end = bg_pos + (bg_left << 4), left;
  u8 n;
  left = ((end + 511) >> 9) - bg_sec;
  if (!left) { bg_end(BG_DONE); return; }              // idx1 обрезан концом файла
  n = left > BG_SECTORS ? BG_SECTORS : (u8)left;
  if (!bg_read(n)) return;
  bg_entries(BG_BUF - bg_carry, BG_BUF + ((u16)n << 9));
}

// START (окно #C000 — страница 1): ролик из AVI_FAR, BG_FAR->avg — байтов звука в секунду
void bg_start()
{
  AVI_SHARED *a = AVI_FAR;
  u8 spc = *(u8*)0x3894;
  u32 avg = BG_FAR->avg;
  bg_ph = PH_OFF;
  bg_result = BG_NONE;
  ib_fps = a->rate;
  // Как index_build: заголовок до 'movi' (его подаёт перемотка) — до 64 КиБ; частота кадров —
  // дробью, звук — в байтах. WildDOS — в окне #0000 (fat_walk читает его FAT напрямую).
  if (*(u8*)0x6000 == *(u8*)0x600B && spc && !(spc & (spc - 1)) && a->total &&
      a->movi_start <= 0x10000UL && (!a->pcm_rate || ib_fps) && a->rate <= 0xFFFF)
  {
    ib_frame = ib_sample = ib_video = ib_audio = ib_base = ib_sample_step = 0;
    ib_k = ib_rem = ib_sample_rem = 0;
    ib_pcm_total = a->pcm_rate && avg ? a->pcm_total : 0;
    ib_step = a->total / AVI_INDEX_MAX + 1;
    ib_count = (a->total + ib_step - 1) / ib_step;
    if (ib_pcm_total)
    {
      // S(k+1) = S(k) + step*avg*scale/rate с остатком по rate (ib_fps). avg*scale не влезает в
      // 32 бита — звук в таблице не считается (позиция звука после перемотки — 0).
      u32 q, r;
      if (a->scale > 0xFFFFFFFFUL / avg) ib_pcm_total = 0;
      else
      {
        q = avg * a->scale;
        r = q % a->rate;
        q /= a->rate;
        ib_sample_step = (u32)ib_step * q + (u32)ib_step * r / a->rate;
        ib_sample_rem = (u32)ib_step * r % a->rate;
      }
    }
    bg_p[1] = a->movi_start;
    bg_size = a->size;
    bg_cl = a->cluster;
    bg_pos = a->movi_end + (a->movi_end & 1);
    bg_tries = bg_carry = 0;
    bg_left = bg_all = 0;
    memset(bg_wd, 0, 10);
    memcpy(bg_wd + 10, &bg_cl, 4);
    bg_sec = 0;
    bg_ph = PH_WALK;
  }
  BG_FAR->state = bg_ph ? BG_RUN : BG_NONE;
  BG_FAR->percent = 0;
  BG_FAR->step = ib_step;
  BG_FAR->count = 0;
}

// Шаг (окно #C000 — страница 1 на входе и на выходе)
void bg_step()
{
  u8 st[14];
  if (bg_ph != PH_OFF && !*(volatile u8*)_ABT)
  {
    wd_get(st);
    wd_put(bg_wd);
    wc_map_buffer(BG_PAGE);
    if (bg_ph == PH_WALK) bg_walk();
    else if (bg_ph == PH_HEAD) bg_head();
    else bg_scan();
    wd_get(bg_wd);
    wd_put(st);
    wc_map_buffer(1);
  }
  BG_FAR->state = bg_ph ? BG_RUN : bg_result;
  BG_FAR->percent = bg_all ? (u8)((bg_all - bg_left) * 100 / bg_all) : 0;
  BG_FAR->count = ib_k;
}
