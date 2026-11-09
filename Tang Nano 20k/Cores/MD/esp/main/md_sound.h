/* SPDX-License-Identifier: GPL-3.0-or-later
 * Звук Mega Drive на ESP (MD.ELF, 03.11.2026): YM2612 (ymfm) и SN76489 по записям в их порты, которые
 * ядро VDAC4 отдаёт словами событий (md_snd_events.sv):
 *   [31:30] вид: 0 — порт YM, 1 — PSG, 2 — сброс YM (байт 1 — сброс снят)
 *   [29:28] порт YM (A1, A0), [27:8] время в тактах синтеза YM (MCLK/42), [7:0] байт.
 * Отсчёт YM — 24 такта синтеза (MCLK/1008: 53 571 Гц при MCLK 54 МГц). Шесть каналов YM считают два
 * экземпляра ym2612 (каналы 0–2 и 3–5 с ЦАП) — на двух ядрах ESP; у обоих все записи в регистры.
 *
 * Порядок работы (один блок — до MD_SND_BLOCK отсчётов):
 *   md_snd_block_begin  — записи блока: событие с временем t попадает в отсчёт (t − начало блока)/24;
 *   md_snd_render_half  — половина YM (0 — каналы 0–2 и PSG, 1 — каналы 3–5); половины независимы;
 *   md_snd_mix          — смешать в стерео float.
 * Без ESP (ПК) — те же функции: tests/snd_render.cpp делает WAV из событий модели.
 */
#ifndef MD_SOUND_H
#define MD_SOUND_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MD_SND_BLOCK      256u          /* отсчётов YM в блоке (4,8 мс) */
#define MD_SND_TICKS      24u           /* тактов синтеза на отсчёт YM */
#define MD_SND_MAX_WRITES 1024u         /* записей в блоке не больше (ЦАП — до ~1 на отсчёт) */

typedef struct {
    uint16_t pos;                       /* отсчёт в блоке */
    uint8_t kind;                       /* 0 — порт YM, 1 — PSG, 2 — сброс YM */
    uint8_t port;                       /* порт YM 0..3 */
    uint8_t data;
} md_snd_write_t;

/* Память движка (два ym2612, PSG, буферы блока) — от вызывающего: alloc(size) раз при запуске.
 * ymfm сам берёт память под каналы и операторы (new): на ESP — через md_snd_set_alloc до md_snd_init. */
void md_snd_set_alloc(void *(*alloc)(size_t), void (*release)(void *));
size_t md_snd_size(void);
void md_snd_init(void *mem);
void md_snd_reset(void *mem);
/* Записи блока (по времени), n отсчётов. Вернёт 0, если записей больше MD_SND_MAX_WRITES. */
int md_snd_block_begin(void *mem, const md_snd_write_t *w, unsigned nw, unsigned n);
void md_snd_render_half(void *mem, int half);
/* Стерео float −1..1, n кадров; gain — общее усиление. */
void md_snd_mix(void *mem, float *out, float gain);
/* MD.ELF 1.6: каналы YM 0..split-1 считает половина 0, остальные — половина 1; граница сдвигается между блоками
 * по тактам (ESP). Ответ — граница сейчас, moves — смен за сеанс (может быть NULL). md_snd_test_split(mem, 1) —
 * проверка на ПК: граница меняется каждый блок, звук обязан остаться тем же. */
unsigned md_snd_split(const void *mem, unsigned *moves);
void md_snd_test_split(void *mem, int on);
/* Такты процессора на половины прошлого блока (ESP — CCOUNT; ПК — 0). */
void md_snd_cycles(const void *mem, uint32_t *half0, uint32_t *half1);
/* Они же по частям: такт ymfm и выход каналов половины 0, то же половины 1, PSG; psg_half — чья была PSG. */
void md_snd_parts(const void *mem, uint32_t parts[5], int *psg_half);
/* Снимок F5/F6: состояние синтеза (оба ymfm, PSG) — в buf (не больше cap байт, 0 — не влезло), длина — ответ;
 * загрузка — из того же (1 — принято). Звать между блоками (поток синтеза стоит). */
#define MD_SND_STATE_MAX  6144u
size_t md_snd_save(void *mem, uint8_t *buf, size_t cap);
int md_snd_load(void *mem, const uint8_t *buf, size_t n);
/* Замер (ESP): три блока половины 0 на своей нагрузке — всего, такт, выход, PSG по каждому; движок сбрасывается. */
void md_snd_bench(void *mem, uint32_t out[12]);
/* Микрозамеры (ESP): такты на 4096 итераций — регистры, чтение внутренней памяти, PSRAM, l32r, вызов; out[5] — МГц. */
void md_cpu_bench(void *mem, uint32_t out[6], uint32_t (*ms)(void));

#ifdef __cplusplus
}
#endif
#endif
