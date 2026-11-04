/* SPDX-License-Identifier: GPL-3.0-or-later
 * Звук Mega Drive (MD.ELF, 03.11.2026): кольцо событий ядра (LINK 1800…) + время YM (LINK 0b) →
 * md_sound (YM2612 двумя половинами на двух ядрах ESP, PSG) → pcm_write.
 *
 * Время ядра — 20 бит тактов синтеза YM (MCLK/42 = 1,2857 МГц, на паузе стоит). Звук отстаёт от
 * ядра на SND_LAT тактов: так все события блока уже прочитаны. Блок — 256 отсчётов YM (4,8 мс):
 * задача ядра 0 считает половину 0 (каналы 0–2 и PSG), помощник на ядре 1 — половину 1 (каналы 3–5,
 * ЦАП). Отстали больше чем на SND_SKIP (ESP была занята) — события применяются без отрисовки, звук
 * догоняет. Очередь вывода больше SND_Q_HI мс — блок короче на отсчёт, меньше SND_Q_LO — длиннее:
 * кварцы ПЛИС и ESP чуть разные.
 *
 * MD.ELF 0.4 (03.11.2026, плата: Jungle Book «хрипит»): синтез не успевал (половина блока до 1,1–1,3 млн тактов
 * при бюджете 1,148 млн), отрисовка отставших блоков шла подряд без опроса ядра — кольцо событий (1024 слова)
 * переполнялось, записи в регистры YM пропадали («lost»: не те ноты и тембры до следующей записи). Теперь
 * кольцо читается и между блоками, не реже раза в SND_POLL_MS. В журнал — средние и наибольшие такты половин
 * по частям: такт ymfm, выход каналов, PSG.
 *
 * MD.ELF 0.5 (плата, b28: потеряно 324 887 событий за 10 минут Jungle Book): игра пишет в звук до 30 тысяч раз в
 * секунду (ЦАП), очередь на 4096 событий переполнялась уже при отставании 136 мс — раньше, чем догон пропуском
 * (SND_SKIP, 200 мс), и новые события выбрасывались. Теперь очередь на 16384 (полсекунды при 30 тысячах в
 * секунду), а заполнение больше трёх четвертей само включает догон: записи в регистры применяются без звука,
 * но не теряются. В журнал — наибольшее заполнение очереди за окно (evq).
 *
 * MD.ELF 0.7 (плата: «звук тормозит»): в 0.5 синтез мог отставать до 200 мс до догона, а очередь вывода
 * держалась у верхнего порога 90 мс (после догона лишнее уходило по одному отсчёту на блок — десятки секунд):
 * звук отставал от картинки до трети секунды. Догон теперь — с отставания 100 мс.
 *
 * MD.ELF 0.8 (плата, 0.7: «заикается, рвётся, хрипит — не всегда»): pcm_queued_ms прошивки включает хвост DMA —
 * 8 буферов по 480 кадров, на 53 571 Гц около 72 мс. Порог 40 мс в 0.7 был меньше одного хвоста: отсчёты
 * выбрасывались (до 16 на блок), хотя своего запаса почти не было, и при паре медленных блоков DMA оставался
 * без данных (играет нули). Порог снова 90 мс (запас ~18 мс сверх DMA), лишнее — по отсчёту на блок, как в
 * 0.6. В журнал — наименьшая и наибольшая очередь за окно и сколько раз она была меньше 5 мс (DMA пуст).
 * Меньше задержка — только с меньшим DMA в прошивке.
 *
 * MD.ELF 1.4 (ядро 0.8, 04.11.2026: HDMI по стандарту 720p60/720p50, Mega Drive раз в кадр стоит под развёртку):
 * время YM идёт медленнее MCLK/42 — ровно столько отсчётов YM, сколько в кадре VDP, на 60 (50) кадров HDMI в
 * секунду: NTSC 262 x 3420 / 1008 x 60 = 53 336 Гц, PAL 313 x 3420 / 1008 x 50 = 53 098 Гц (было 53 571 — очередь
 * вывода пустела бы на 0,44 / 0,89 %, а подгонка — не больше отсчёта на блок, 0,39 %). Режим — из md_ctrl игры;
 * смена NTSC/PAL — вывод открывается заново между блоками.
 */
#include "md_sound.h"

#define R_SND_WP   0x20u          /* {обороты[31:16], указатель в коде Грея[9:0]} */
#define R_CLK_KHZ  0x28u          /* MCLK, кГц */
#define R_SND_TIME 0x2Cu          /* время YM [19:0] */
/* ядро 0.3 (03.11.2026, плата: звук пропадал, игра шла): состояние Z80 — {RESET_N, BUSRQ_N, BUSAK_N, WAIT_N,
 * HALT_N, BR_N, BGACK_N, банк[8:0], PC[15:0]} и {выборки кода[15:0], записи в YM[7:0], записи Z80 в PSG[7:0]} */
#define R_Z80_A    0x150u
#define R_Z80_B    0x154u
#define W_SND      0x6000u        /* кольцо событий, 1024 слова */
#define SND_CELLS  1024u
#define SND_LAT    (1285714u * 25u / 1000u)      /* 25 мс */
#define SND_SKIP   (1285714u / 10u)              /* 100 мс */
#define SND_EVQ    16384u
#define SND_Q_HI   90u
#define SND_Q_LO   12u
#define SND_POLL_MS 8u           /* кольцо 1024 слова: ЦАП 16 кГц заполняет его за 64 мс */

static void *snd_mem, *snd_go, *snd_done;
static volatile int snd_helper_done = 1;
static uint32_t snd_evq_t[SND_EVQ];               /* младшие 32 бита полного времени события */
static uint32_t snd_evq_w[SND_EVQ];
static unsigned snd_evq_head, snd_evq_tail;
static unsigned snd_evq_fill(void) { return (snd_evq_head - snd_evq_tail) & (SND_EVQ - 1); }
static md_snd_write_t snd_blk[MD_SND_MAX_WRITES];

/* Снимок F5/F6 (MD.ELF 1.1, md_snapshot.h): просьба потоку звука, исполняется между блоками. Время YM ядра на
 * снимке стоит (таймеры YM — SNAP_ACT), Z80 уже во вставке: событий больше нет. SAVE — всё до этого времени
 * в синтез (с выводом), состояние — в snd_req_buf (не больше snd_req_len байт; длина — в snd_req_len). LOAD —
 * очередь событий прежней игры выбросить, состояние — из snd_req_buf (snd_req_len байт), звук догоняет время
 * с обычным отставанием SND_LAT. Готово — snd_req = 0. */
#define SND_REQ_SAVE 1
#define SND_REQ_LOAD 2
static volatile int snd_req;
static uint8_t *volatile snd_req_buf;
static volatile size_t snd_req_len;

static void snd_helper(void *arg)
{
    (void)arg;
    while (running) {
        if (core->signal_wait(snd_go, 50) == ZM_OK && snd_mem) {
            md_snd_render_half(snd_mem, 1);
            core->signal_give(snd_done);
        }
    }
    snd_helper_done = 1;
}

typedef struct {
    uint64_t now;                 /* полное время ядра */
    uint32_t last20, rd;          /* rd — прочитано событий всего (по модулю 2^26, как запись) */
    uint64_t t_render;            /* начало следующего блока */
    uint32_t lost, skips, blocks, drops, dups;
    uint32_t win_n, max0, max1;   /* за окно журнала: блоков, наибольшие такты половин */
    uint32_t events;              /* за окно журнала: событий из кольца ядра (0 — игра не пишет в YM и PSG) */
    uint32_t evq_max;             /* за окно журнала: наибольшее заполнение очереди */
    uint32_t q_min, q_max, q_empty; /* за окно журнала: очередь вывода (мс, с хвостом DMA), раз меньше 5 мс */
    uint64_t sum0, sum1, part[5]; /* суммы тактов половин и частей (md_snd_parts) */
} snd_pos_t;

/* время ядра и новые события; 0 — LINK не ответил */
static int snd_poll(snd_pos_t *s)
{
    uint32_t t = 0, v = 0, words[16];
    /* указатель и обороты читаются из домена SCK: на обороте можно поймать одно без другого —
       два одинаковых чтения подряд */
    uint32_t v2 = 1;
    if (live_read(R_SND_TIME, &t, 1) != ZM_OK) return 0;
    for (int i = 0; i < 4 && v != v2; i++)
        if (live_read(R_SND_WP, &v, 1) != ZM_OK || live_read(R_SND_WP, &v2, 1) != ZM_OK) return 0;
    if (v != v2) return 0;
    t &= 0xFFFFFu;
    s->now += (t - s->last20) & 0xFFFFFu;
    s->last20 = t;
    /* записано всего: обороты * 1024 + указатель (26 бит); больше кольца непрочитанного — потеря */
    const uint32_t written = ((v >> 16) << 10 | gray_dec(v & 0x3FFu)) & 0x3FFFFFFu;
    uint32_t have = (written - s->rd) & 0x3FFFFFFu;
    if (have > SND_CELLS) { s->lost++; s->rd = written; have = 0; }
    while (have) {
        const uint32_t rp = s->rd & (SND_CELLS - 1);
        uint32_t n = have > 16 ? 16 : have;
        if (n > SND_CELLS - rp) n = SND_CELLS - rp;
        if (live_read(W_SND + 4 * rp, words, n) != ZM_OK) return 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t w = words[i], stamp = (w >> 8) & 0xFFFFFu;
            const uint64_t full = s->now - ((t - stamp) & 0xFFFFFu);
            const unsigned next = (snd_evq_head + 1) & (SND_EVQ - 1);
            if (next == snd_evq_tail) { s->lost++; continue; }
            snd_evq_t[snd_evq_head] = (uint32_t)full;
            snd_evq_w[snd_evq_head] = w;
            snd_evq_head = next;
            s->events++;
            const unsigned fill = snd_evq_fill();
            if (fill > s->evq_max) s->evq_max = fill;
        }
        s->rd = (s->rd + n) & 0x3FFFFFFu;
        have -= n;
    }
    return 1;
}

/* события до end — в блок (позиции от start); n = 0 — только применить */
static unsigned snd_take(uint64_t start, uint64_t end, unsigned n)
{
    unsigned k = 0;
    const uint32_t s32 = (uint32_t)start, e32 = (uint32_t)end;
    while (snd_evq_tail != snd_evq_head && k < MD_SND_MAX_WRITES) {
        const uint32_t et = snd_evq_t[snd_evq_tail];
        if ((int32_t)(et - e32) >= 0) break;
        const int32_t d = (int32_t)(et - s32);
        unsigned pos = d <= 0 ? 0u : (unsigned)d / MD_SND_TICKS;
        if (n == 0) pos = 0; else if (pos >= n) pos = n - 1;
        const uint32_t w = snd_evq_w[snd_evq_tail];
        snd_blk[k].pos = (uint16_t)pos;
        snd_blk[k].kind = (uint8_t)(w >> 30);
        snd_blk[k].port = (uint8_t)((w >> 28) & 3);
        snd_blk[k].data = (uint8_t)w;
        k++;
        snd_evq_tail = (snd_evq_tail + 1) & (SND_EVQ - 1);
    }
    return k;
}

static void snd_service(snd_pos_t *s, float *out)
{
    if (snd_req == SND_REQ_SAVE) {
        while (s->t_render + MD_SND_TICKS <= s->now) {
            const uint64_t left = (s->now - s->t_render) / MD_SND_TICKS;
            const unsigned n = left > MD_SND_BLOCK ? MD_SND_BLOCK : (unsigned)left;
            const uint64_t end = s->t_render + (uint64_t)n * MD_SND_TICKS;
            const unsigned k = snd_take(s->t_render, end, n);
            md_snd_block_begin(snd_mem, snd_blk, k, n);
            core->signal_give(snd_go);
            md_snd_render_half(snd_mem, 0);
            if (core->signal_wait(snd_done, 100) != ZM_OK) break;
            md_snd_mix(snd_mem, out, 0.9f);
            s->t_render = end;
            for (unsigned done = 0; done < n && running;) {
                const int w = core->pcm_write(out + 2 * done, n - done);
                if (w <= 0) break;
                done += (unsigned)w;
            }
        }
        unsigned k;
        do {                                           /* события в последнем неполном отсчёте */
            k = snd_take(s->t_render, s->now + 1, 0);
            md_snd_block_begin(snd_mem, snd_blk, k, 0);
            md_snd_render_half(snd_mem, 0);
            md_snd_render_half(snd_mem, 1);
        } while (k == MD_SND_MAX_WRITES);
        snd_req_len = md_snd_save(snd_mem, snd_req_buf, snd_req_len);
    } else if (snd_req == SND_REQ_LOAD) {
        snd_evq_tail = snd_evq_head;
        if (!md_snd_load(snd_mem, snd_req_buf, snd_req_len)) snd_req_len = 0;
        s->t_render = s->now - SND_LAT;
    }
    snd_req = 0;
}

#define MD_SYNC_CORE 0x06010008u      /* ядро 0.8: стоянка под стандартную развёртку */
static uint32_t snd_rate(uint32_t khz)
{
    if (core_version < MD_SYNC_CORE) return khz * 1000u / 1008u;
    return (md_ctrl & MD_CTRL_PAL) ? 313u * 3420u * 50u / 1008u : 262u * 3420u * 60u / 1008u;
}

static void snd_session(void)
{
    const uint32_t id = session_id;
    uint32_t khz = 0;
    if (live_read(R_CLK_KHZ, &khz, 1) != ZM_OK || khz < 40000 || khz > 70000) khz = 54000;
    zm_pcm_format_t fmt = {snd_rate(khz), 2};
    if (!snd_mem || core->pcm_open(&fmt) != ZM_OK) {
        while (running && session_live && session_id == id) core->delay(50);
        return;
    }
    core->log("md: audio %u Hz, YM2612 on two ESP cores", (unsigned)fmt.rate);
    md_snd_reset(snd_mem);
    snd_evq_head = snd_evq_tail = 0;
    snd_pos_t s;
    memset(&s, 0, sizeof s);
    s.q_min = 0xFFFFFFFFu;
    uint32_t t = 0, v = 0;
    if (live_read(R_SND_TIME, &t, 1) == ZM_OK) s.last20 = t & 0xFFFFFu;
    if (live_read(R_SND_WP, &v, 1) == ZM_OK) s.rd = ((v >> 16) << 10 | gray_dec(v & 0x3FFu)) & 0x3FFFFFFu;
    s.now = 1ull << 32;                                /* запас от нуля */
    s.t_render = s.now - SND_LAT;
    static float out[(MD_SND_BLOCK + 1) * 2];
    uint32_t last_poll = core->millis();
    /* Уступать процессор (03.11.2026, плата: Jungle Book): при настоящей музыке синтез занимает до 94 % ядра 0,
     * блоки рисуются подряд, задача (приоритет 4) не блокируется — задача простоя ядра 0 не получала времени,
     * через 5 с сторож задач перезапускал ESP. Тик сна — не реже раза в 40 мс работы (около 2 % синтеза).
     * Счёт «работы без сна» — через все проходы, сбрасывается только сном (MD.ELF 0.4: в 0.3 он начинался заново
     * на каждом проходе, проход рисовал меньше 40 мс — сна не было вовсе, сторож снова сработал на плате). */
    uint32_t busy_ms = last_poll;
    while (running && session_live && session_id == id) {
        if (!snd_poll(&s)) { core->delay(5); busy_ms = core->millis(); continue; }
        last_poll = core->millis();
        if (snd_req) { snd_service(&s, out); continue; }
        if (snd_rate(khz) != fmt.rate) {                 /* NTSC <-> PAL: вывод заново на новой частоте */
            core->pcm_close();
            fmt.rate = snd_rate(khz);
            if (core->pcm_open(&fmt) != ZM_OK) break;
        }
        const uint64_t target = s.now - SND_LAT;
        if (target > s.t_render + (uint64_t)MD_SND_BLOCK * MD_SND_TICKS &&
            (target > s.t_render + SND_SKIP || snd_evq_fill() > SND_EVQ / 4u * 3u)) {   /* отстали: применить без звука */
            const uint64_t to = target - (uint64_t)MD_SND_BLOCK * MD_SND_TICKS;
            unsigned k;
            do {                                       /* за раз — не больше MD_SND_MAX_WRITES записей */
                k = snd_take(s.t_render, to, 0);
                md_snd_block_begin(snd_mem, snd_blk, k, 0);
                md_snd_render_half(snd_mem, 0);
                md_snd_render_half(snd_mem, 1);
            } while (k == MD_SND_MAX_WRITES);
            s.t_render = to;
            s.skips++;
        }
        int worked = 0;
        while (running && session_live && s.t_render + (uint64_t)MD_SND_BLOCK * MD_SND_TICKS <= target) {
            if (core->millis() - busy_ms >= 40u) { core->delay(1); busy_ms = core->millis(); }
            if (core->millis() - last_poll >= SND_POLL_MS) {     /* события — из кольца в очередь, цель та же */
                if (!snd_poll(&s)) break;
                last_poll = core->millis();
                if (snd_evq_fill() > SND_EVQ / 4u * 3u) break;      /* очередь почти полна — догон пропуском */
            }
            const uint64_t end = s.t_render + (uint64_t)MD_SND_BLOCK * MD_SND_TICKS;
            const unsigned k = snd_take(s.t_render, end, MD_SND_BLOCK);
            md_snd_block_begin(snd_mem, snd_blk, k, MD_SND_BLOCK);
            core->signal_give(snd_go);                  /* половина 1 — помощник на ядре 1 */
            md_snd_render_half(snd_mem, 0);
            if (core->signal_wait(snd_done, 100) != ZM_OK) { s.lost++; break; }
            md_snd_mix(snd_mem, out, 0.9f);
            s.t_render = end;
            s.blocks++;
            {
                uint32_t c0, c1, pt[5]; int ph;
                md_snd_cycles(snd_mem, &c0, &c1);
                md_snd_parts(snd_mem, pt, &ph);
                (void)ph;
                s.win_n++; s.sum0 += c0; s.sum1 += c1;
                if (c0 > s.max0) s.max0 = c0;
                if (c1 > s.max1) s.max1 = c1;
                for (int i = 0; i < 5; i++) s.part[i] += pt[i];
            }
            unsigned n = MD_SND_BLOCK;
            const uint32_t q = core->pcm_queued_ms();
            if (q < s.q_min) s.q_min = q;
            if (q > s.q_max) s.q_max = q;
            if (q < 5u) s.q_empty++;
            if (q > SND_Q_HI) { n--; s.drops++; }
            else if (q < SND_Q_LO) { out[2 * n] = out[2 * n - 2]; out[2 * n + 1] = out[2 * n - 1]; n++; s.dups++; }
            unsigned done = 0;
            while (done < n && running && session_live) {
                const int w = core->pcm_write(out + 2 * done, n - done);
                if (w <= 0) break;
                done += (unsigned)w;
            }
            worked = 1;
        }
        if (!worked) { core->delay(2); busy_ms = core->millis(); }
    }
    core->pcm_close();
    /* итог сеанса — после игры: в игре журнал не пишется (MD.ELF 1.0, плата: журнал прошивки — во флэше, а работа с
       флэшем останавливает кэш обоих ядер, и код модуля из PSRAM стоит — чтение журнала во время игры давало
       рывок звука 82 мс); строка — до 127 байт */
    const uint64_t nb = s.blocks ? s.blocks : 1;
    core->log("md: audio end: blocks %u, lost %u, skips %u, drop %u, dup %u, evq max %u, queue %u..%u ms, empty %u",
              (unsigned)s.blocks, (unsigned)s.lost, (unsigned)s.skips, (unsigned)s.drops, (unsigned)s.dups,
              (unsigned)s.evq_max, (unsigned)(s.q_min == 0xFFFFFFFFu ? 0u : s.q_min), (unsigned)s.q_max, (unsigned)s.q_empty);
    core->log("md: synth kcyc/block (budget %u): h0 %u/%u, h1 %u/%u, psg %u",
              (unsigned)(240000000u / (fmt.rate / MD_SND_BLOCK) / 1000u), (unsigned)(s.sum0 / nb / 1000u),
              (unsigned)(s.max0 / 1000u), (unsigned)(s.sum1 / nb / 1000u), (unsigned)(s.max1 / 1000u),
              (unsigned)(s.part[4] / nb / 1000u));
}

static void audio_task(void *arg)
{
    (void)arg;
    while (running) {
        while (running && !session_live) core->delay(20);
        if (running) snd_session();
    }
    audio_done = 1;
}
