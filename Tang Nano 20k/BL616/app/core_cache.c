/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Кэш ядер ПЛИС во flash BL616 (docs/next-core.md): ядра, которых нет во flash Tang (ZX Next и
 * другие), ESP переносит с SD Tang сюда, а BL616 по команде грузит их в SRAM ПЛИС по JTAG
 * (core_jtag.c). Протокол — core_mailbox.h.
 *
 * Раскладка flash BL616: 0 — FPGA Partner, 0x40000 — эта прошивка (до 256 КБ), 0x80000 и
 * 0x81000 — две копии каталога (пишется старшая по возрасту; годная — с меткой и верным SHA-256,
 * из двух годных — с большим поколением), с 0x90000 — ядра по кругу, каждое с границы блока
 * 64 КБ; последний блок flash не трогается. Новое ядро пишется за последним записанным, не
 * помещается до конца — с начала; ядра на его месте вычёркиваются из каталога до записи. Копия
 * каталога старого поколения может указывать на перезаписанное место — поэтому SHA-256 ядра
 * сверяется перед каждой загрузкой. Каталога нет, а место не чистое (не 0xFF) — там чьи-то
 * данные: кэш выключен, пока ESP не пришлёт FORMAT.
 *
 * Ядро из кэша работает без ящика и сопроцессора: BL616 раз в 10 мс смотрит снимок STATUS
 * (next_cp ядра Next) — просьбу о перезагрузке, бит 7 флагов, — и делает JTAG Reconfig: адрес
 * перехода у ядра из кэша — 0, ПЛИС грузит слот 0. Снимков нет 3 с — ядро BL616 не отвечает,
 * назад в слот 0 тоже. После перезагрузки — ждать ядро VDAC4 (снимок C5 01), до 5 с.
 */
#include <stdio.h>
#include <string.h>

#include "bflb_flash.h"
#include "bflb_mtimer.h"

#include "core_cache.h"
#include "core_jtag.h"
#include "core_mailbox.h"
#include "fpga_jtag.h"
#include "fpga_link.h"
#include "fw_mailbox.h"
#include "sha256.h"

#define DIR_ADDR    0x80000u
#define DIR_SECTOR  4096u
#define DATA_ADDR   0x90000u
#define BLOCK       65536u
#define DIR_MAGIC   0x31434356u    /* "VCC1" */
#define DIR_MAX     63u
#define INFO_ADDR   0x3FFFFCu
#define IDLE_MS     50u            /* как часто без записи смотреть заголовок ящика */
#define TIMEOUT_MS  10000u         /* без новой команды — запись брошена */
#define OWN_MS      3000u          /* после ответа CP_INFO не перебивать обычным словом */
#define POLL_MS     10u            /* снимок STATUS ядра из кэша */
#define LOST_MS     3000u          /* ядро из кэша не отвечает — назад в слот 0 */
#define RELINK_MS   5000u          /* после перезагрузки — ждать ядро VDAC4 */

typedef struct {
    uint32_t off, size, seq, usercode;
    uint8_t sha[32];
    char name[CC_NAME_LEN];
} entry_t;

typedef struct {
    uint32_t magic, gen, count, head, seq, rsv[3];
    uint8_t sha[32];               /* SHA-256 первых 32 байт и записей [0, count) */
    entry_t e[DIR_MAX];
} dir_t;

_Static_assert(sizeof(entry_t) == 64, "запись каталога — 64 байта");
_Static_assert(sizeof(dir_t) == DIR_SECTOR, "каталог — сектор flash");

static ftc_bus_t bus;
static dir_t dir, tmp;
static int dir_slot;               /* годная копия каталога: 0 — A, 1 — B */
static uint32_t flash_mb;          /* для INFO */
static uint32_t data_end;          /* 0 — кэша нет */
static int foreign_data;           /* каталога нет, место занято чужими данными */
static int dir_saved;              /* каталог есть во flash (на чистой flash он сперва — только в ОЗУ) */
static uint32_t payload[FWU_PAYLOAD_MAX / 4];
static uint8_t chunk[2048];
static struct {
    uint32_t seen, t_check, t_cmd, t_ack;
    int acked;
    int active;                    /* идёт запись ядра */
    uint32_t start, size, need, got, erased;
    uint8_t sha[32];
    char name[CC_NAME_LEN];
    int foreign;                   /* в ПЛИС — ядро из кэша */
    int relink;                    /* после перезагрузки ждём ядро VDAC4 */
    uint32_t t_poll, t_alive, t_relink;
    int fail;                      /* загрузка не вышла: ответ — когда ядро VDAC4 ответит */
    uint32_t fail_seq, fail_status;
} u;

/* мс от отметки до now; отметка позже now (часы загрузки ядра ушли вперёд) — меньше нуля: ещё рано */
static int32_t since(uint32_t now, uint32_t mark)
{
    return (int32_t)(now - mark);
}

static uint32_t mb_word(uint32_t i)
{
    uint32_t w;
    bus.rd(bus.ctx, FWU_MB_BASE + 4 * i, &w, 1);
    return w;
}

static void ack(uint32_t seq, uint32_t status, uint32_t now)
{
    uint32_t v = (CC_INFO_MAGIC << 24) | ((status & 0xFF) << 16) | (seq & 0xFFFF);
    bus.wr(bus.ctx, INFO_ADDR, &v, 1);
    u.t_ack = now;
    u.acked = 1;
}

static void dir_hash(const dir_t *d, uint8_t out[32])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, (const uint8_t *)d, 32);
    sha256_update(&s, (const uint8_t *)d->e, (size_t)d->count * sizeof(entry_t));
    sha256_final(&s, out);
}

static int dir_valid(const dir_t *d)
{
    uint8_t h[32];
    if (d->magic != DIR_MAGIC || d->count > DIR_MAX)
        return 0;
    dir_hash(d, h);
    return memcmp(h, d->sha, 32) == 0;
}

static void dir_empty(void)
{
    memset(&dir, 0, sizeof dir);
    dir.magic = DIR_MAGIC;
    dir.head = DATA_ADDR;
    dir.seq = 1;
    dir_slot = 1;                  /* первая запись — в копию A */
}

static int dir_commit(void)
{
    dir.gen++;
    memset(&dir.e[dir.count], 0, (DIR_MAX - dir.count) * sizeof(entry_t));
    dir_hash(&dir, dir.sha);
    int slot = dir_slot ^ 1;
    uint32_t a = DIR_ADDR + (uint32_t)slot * DIR_SECTOR;
    if (bflb_flash_erase(a, DIR_SECTOR) != 0 || bflb_flash_write(a, (uint8_t *)&dir, sizeof dir) != 0)
        return -1;
    dir_slot = slot;
    dir_saved = 1;
    return 0;
}

static void dir_remove(uint32_t i)
{
    memmove(&dir.e[i], &dir.e[i + 1], (dir.count - i - 1) * sizeof(entry_t));
    dir.count--;
}

static int dir_find(const char *name)
{
    for (uint32_t i = 0; i < dir.count; i++)
        if (memcmp(dir.e[i].name, name, CC_NAME_LEN) == 0)
            return (int)i;
    return -1;
}

static int entry_ok(const entry_t *e)
{
    return e->off >= DATA_ADDR && e->off % BLOCK == 0 && e->size >= 4096 && e->off < data_end &&
           e->size <= data_end - e->off && e->name[0] && memchr(e->name, 0, CC_NAME_LEN);
}

/* место flash [a, a + n) чистое (0xFF) */
static int blank(uint32_t a, uint32_t n)
{
    for (uint32_t at = 0; at < n; at += sizeof chunk) {
        uint32_t k = n - at < sizeof chunk ? n - at : (uint32_t)sizeof chunk;
        if (bflb_flash_read(a + at, chunk, k) != 0)
            return 0;
        for (uint32_t i = 0; i < k; i++)
            if (chunk[i] != 0xFF)
                return 0;
    }
    return 1;
}

static void dir_load(void)
{
    int best = -1;
    for (int i = 0; i < 2; i++) {
        if (bflb_flash_read(DIR_ADDR + (uint32_t)i * DIR_SECTOR, (uint8_t *)&tmp, sizeof tmp) != 0 || !dir_valid(&tmp))
            continue;
        if (best < 0 || tmp.gen > dir.gen) {
            dir = tmp;
            best = i;
        }
    }
    if (best < 0) {
        dir_empty();
        /* каталога нет: место должно быть чистым — иначе там чьи-то данные (FPGA Partner?) */
        foreign_data = !blank(DIR_ADDR, data_end - DIR_ADDR);
        return;
    }
    dir_saved = 1;
    dir_slot = best;
    uint32_t n = 0;
    for (uint32_t i = 0; i < dir.count; i++)
        if (entry_ok(&dir.e[i]))
            dir.e[n++] = dir.e[i];
    dir.count = n;
    if (dir.head < DATA_ADDR || dir.head >= data_end || dir.head % BLOCK)
        dir.head = DATA_ADDR;
}

/* имя из 16 байт ящика: не пустое, с нулём; за нулём — нули */
static int name_get(const uint32_t *w, char out[CC_NAME_LEN])
{
    memcpy(out, w, CC_NAME_LEN);
    char *z = memchr(out, 0, CC_NAME_LEN);
    if (!z || out[0] == 0)
        return -1;
    memset(z, 0, (size_t)(out + CC_NAME_LEN - z));
    return 0;
}

/* Заголовок битстрима (первые 128 байт): синхрослово A5 C3, IDCODE GW2AR-18 (06 00 00 00
   00 00 08 1B), команды по 4 байта до записи кадров (3B) — среди них адрес перехода multiboot
   D2 00 FF FF с адресом 0: ядро из кэша перезагружается в слот 0. */
static int check_head(const uint8_t *b, uint32_t n)
{
    static const uint8_t id[8] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B};
    uint32_t i = 0;
    while (i < 64 && !(b[i] == 0xA5 && b[i + 1] == 0xC3))
        i++;
    if (i >= 64 || memcmp(b + i + 2, id, 8) != 0)
        return CC_BAD_IMAGE;
    for (uint32_t j = i + 10; j + 8 <= n && b[j] != 0x3B; j += 4)
        if (b[j] == 0xD2 && b[j + 1] == 0x00 && b[j + 2] == 0xFF && b[j + 3] == 0xFF)
            return (b[j + 4] | b[j + 5] | b[j + 6] | b[j + 7]) ? CC_BAD_IMAGE : CC_OK;
    return CC_BAD_IMAGE;
}

/* Конец битстрима (последние 64 байта): UserCode 0A 00 00 00 56 44 34 xx — метка VDAC4 */
static int check_tail(const uint8_t *t, uint32_t n, uint32_t *uc)
{
    for (uint32_t i = 0; i + 8 <= n; i++)
        if (t[i] == 0x0A && t[i + 1] == 0 && t[i + 2] == 0 && t[i + 3] == 0 &&
            ((uint32_t)t[i + 4] << 16 | (uint32_t)t[i + 5] << 8 | t[i + 6]) == CC_MARK) {
            *uc = CC_MARK << 8 | t[i + 7];
            return CC_OK;
        }
    return CC_NO_MARK;
}

/* ядро во flash [off, off + size): заголовок, метка VDAC4 (uc — UserCode), SHA-256 */
static int verify(uint32_t off, uint32_t size, const uint8_t *want, uint32_t *uc)
{
    if (bflb_flash_read(off, chunk, 128) != 0)
        return CC_FLASH;
    int r = check_head(chunk, 128);
    if (r != CC_OK)
        return r;
    if (bflb_flash_read(off + size - 64, chunk, 64) != 0)
        return CC_FLASH;
    if ((r = check_tail(chunk, 64, uc)) != CC_OK)
        return r;
    sha256_t s;
    sha256_init(&s);
    for (uint32_t at = 0; at < size;) {
        uint32_t n = size - at < sizeof chunk ? size - at : (uint32_t)sizeof chunk;
        if (bflb_flash_read(off + at, chunk, n) != 0)
            return CC_FLASH;
        sha256_update(&s, chunk, n);
        at += n;
    }
    uint8_t h[32];
    sha256_final(&s, h);
    return memcmp(h, want, 32) == 0 ? CC_OK : CC_BAD_SHA;
}

static int flash_src(void *ctx, uint32_t off, uint8_t *buf, uint32_t n)
{
    return bflb_flash_read(*(const uint32_t *)ctx + off, buf, n) == 0 ? 0 : -1;
}

/* JTAG Reconfig: ПЛИС грузит слот 0; дальше — ждать ядро VDAC4 */
static void reconfig(uint32_t now)
{
    fpga_jtag_reconfig();
    u.foreign = 0;
    u.relink = 1;
    u.t_relink = now;
    u.t_poll = now;
}

static void begin(uint32_t seq, uint32_t now)
{
    uint32_t w[13];
    bus.rd(bus.ctx, FWU_MB_BASE + 4, w, 13);
    u.active = 0;
    if (name_get(&w[CC_W_NAME - 1], u.name) != 0) {
        ack(seq, CC_BAD_NAME, now);
        return;
    }
    uint32_t size = w[CC_W_SIZE - 1], need = (size + BLOCK - 1) / BLOCK * BLOCK;
    if (!data_end || size < 4096 || size > data_end - DATA_ADDR || need > data_end - DATA_ADDR) {
        ack(seq, CC_BAD_SIZE, now);
        return;
    }
    uint32_t start = dir.head;
    if (start + need > data_end)
        start = DATA_ADDR;
    int evicted = 0;
    for (uint32_t i = 0; i < dir.count;) {
        const entry_t *e = &dir.e[i];
        uint32_t e_end = e->off + (e->size + BLOCK - 1) / BLOCK * BLOCK;
        if (e->off < start + need && start < e_end) {
            printf("кэш ядер: %s вытеснено\r\n", e->name);
            dir_remove(i);
            evicted = 1;
        } else {
            i++;
        }
    }
    /* На чистой flash каталог пока только в ОЗУ: оборванная запись оставила бы во flash данные
       без каталога, и при следующем запуске место было бы «чужим» (FOREIGN, кэш выключен до
       FORMAT). Поэтому пустой каталог — во flash до первого куска данных. */
    if ((evicted || !dir_saved) && dir_commit() != 0) {
        ack(seq, CC_FLASH, now);
        return;
    }
    u.start = start;
    u.size = size;
    u.need = need;
    u.got = 0;
    u.erased = start;
    memcpy(u.sha, &w[CC_W_SHA - 1], 32);
    u.active = 1;
    ack(seq, CC_OK, now);
}

static void data(uint32_t seq, uint32_t now)
{
    if (!u.active) {
        ack(seq, CC_BAD_CMD, now);
        return;
    }
    uint32_t w[2];
    bus.rd(bus.ctx, FWU_MB_BASE + 4, w, 2);
    uint32_t len = w[0], off = w[1];
    if (len == 0 || len > FWU_PAYLOAD_MAX || off > u.size || len > u.size - off) {
        ack(seq, CC_BAD_OFFSET, now);
        return;
    }
    if (off + len <= u.got) {      /* повтор принятого: ответ ESP потерялся */
        ack(seq, CC_OK, now);
        return;
    }
    if (off != u.got) {
        ack(seq, CC_BAD_OFFSET, now);
        return;
    }
    bus.rd(bus.ctx, FWU_MB_BASE + 4 * FWU_PAYLOAD_WORD, payload, (len + 3) / 4);
    if (off == 0 && len >= 128 && check_head((const uint8_t *)payload, 128) != CC_OK) {
        u.active = 0;              /* не битстрим для этой ПЛИС — дальше не принимать */
        ack(seq, CC_BAD_IMAGE, now);
        return;
    }
    while (u.erased < u.start + off + len) {
        if (bflb_flash_erase(u.erased, BLOCK) != 0) {
            u.active = 0;
            ack(seq, CC_FLASH, now);
            return;
        }
        u.erased += BLOCK;
    }
    if (bflb_flash_write(u.start + off, (uint8_t *)payload, len) != 0) {
        u.active = 0;
        ack(seq, CC_FLASH, now);
        return;
    }
    u.got += len;
    ack(seq, CC_OK, now);
}

static void end(uint32_t seq, uint32_t now)
{
    if (!u.active) {
        ack(seq, CC_BAD_CMD, now);
        return;
    }
    if (u.got != u.size) {
        ack(seq, CC_BAD_OFFSET, now);
        return;
    }
    u.active = 0;
    uint32_t uc = 0;
    int r = verify(u.start, u.size, u.sha, &uc);
    if (r != CC_OK) {
        ack(seq, (uint32_t)r, now);
        return;
    }
    int same = dir_find(u.name);
    if (same >= 0)
        dir_remove((uint32_t)same);
    if (dir.count == DIR_MAX) {    /* каталог полон — вычеркнуть самое давнее */
        uint32_t old = 0;
        for (uint32_t i = 1; i < dir.count; i++)
            if (dir.e[i].seq < dir.e[old].seq)
                old = i;
        dir_remove(old);
    }
    entry_t *e = &dir.e[dir.count++];
    e->off = u.start;
    e->size = u.size;
    e->seq = dir.seq++;
    e->usercode = uc;
    memcpy(e->sha, u.sha, 32);
    memcpy(e->name, u.name, CC_NAME_LEN);
    dir.head = u.start + u.need;
    if (dir.head >= data_end)
        dir.head = DATA_ADDR;
    if (dir_commit() != 0) {
        ack(seq, CC_FLASH, now);
        return;
    }
    printf("кэш ядер: %s, %u байт с 0x%06X\r\n", u.name, (unsigned)u.size, (unsigned)u.start);
    ack(seq, CC_OK, now);
}

static void find(uint32_t seq, uint32_t now)
{
    uint32_t w[13];
    char name[CC_NAME_LEN];
    bus.rd(bus.ctx, FWU_MB_BASE + 4, w, 13);
    if (name_get(&w[CC_W_NAME - 1], name) != 0) {
        ack(seq, CC_BAD_NAME, now);
        return;
    }
    int i = dir_find(name);
    ack(seq, i < 0 ? CC_NONE : memcmp(dir.e[i].sha, &w[CC_W_SHA - 1], 32) == 0 ? CC_FOUND : CC_STALE, now);
}

static void load(uint32_t seq, uint32_t now)
{
    static const uint8_t any[32];
    uint32_t w[13];
    char name[CC_NAME_LEN];
    bus.rd(bus.ctx, FWU_MB_BASE + 4, w, 13);
    if (name_get(&w[CC_W_NAME - 1], name) != 0) {
        ack(seq, CC_BAD_NAME, now);
        return;
    }
    int i = dir_find(name);
    if (i < 0) {
        ack(seq, CC_NONE, now);
        return;
    }
    entry_t e = dir.e[i];
    if (memcmp(&w[CC_W_SHA - 1], any, 32) != 0 && memcmp(&w[CC_W_SHA - 1], e.sha, 32) != 0) {
        ack(seq, CC_STALE, now);
        return;
    }
    uint32_t uc = 0;
    int r = verify(e.off, e.size, e.sha, &uc);
    if (r != CC_OK) {
        printf("кэш ядер: %s испорчено (%d) — вычеркнуто\r\n", e.name, r);
        dir_remove((uint32_t)i);
        (void)dir_commit();
        ack(seq, (uint32_t)r, now);
        return;
    }
    ack(seq, CC_LOADING, now);
    const uint64_t t0 = bflb_mtimer_get_time_ms();
    bflb_mtimer_delay_ms(50);      /* ESP успевает увидеть LOADING, пока ядро VDAC4 ещё есть */
    r = core_jtag_load(flash_src, &e.off, e.size, &uc);
    if (r == CORE_JTAG_OK && uc != e.usercode)
        r = 7;
    const uint32_t took = (uint32_t)(bflb_mtimer_get_time_ms() - t0);
    now += took;                   /* часы главного цикла — вместе с загрузкой */
    if (r == CORE_JTAG_OK) {
        printf("ядро %s из кэша загружено за %u мс\r\n", e.name, (unsigned)took);
        u.foreign = 1;
        u.t_poll = now;
        u.t_alive = now;
        return;
    }
    if (r == CORE_JTAG_NO_FPGA || r == CORE_JTAG_BAD_SIZE) {
        /* до команд настройки: в ПЛИС по-прежнему TS-Config, и перезагружать её нельзя — она взяла
           бы адрес перехода из заголовка TS-Config (слот 1, ядро FT812), а не слот 0 */
        printf("ядро %s из кэша не загрузилось (%d), ПЛИС не тронута\r\n", e.name, r);
        ack(seq, CC_LOAD_FAIL | (uint32_t)r, now);
        return;
    }
    printf("ядро %s из кэша не загрузилось (%d): назад в слот 0\r\n", e.name, r);
    u.fail = 1;
    u.fail_seq = seq;
    u.fail_status = CC_LOAD_FAIL | (uint32_t)r;
    reconfig(now);
}

static void format(uint32_t seq, uint32_t now)
{
    if (!data_end) {
        ack(seq, CC_BAD_SIZE, now);
        return;
    }
    dir_empty();
    if (bflb_flash_erase(DIR_ADDR, 2 * DIR_SECTOR) != 0 || dir_commit() != 0) {
        ack(seq, CC_FLASH, now);
        return;
    }
    foreign_data = 0;
    printf("кэш ядер: каталог создан заново\r\n");
    ack(seq, CC_OK, now);
}

void core_cache_adopt(const ftc_bus_t *b, uint32_t now)
{
    bus = *b;
    memset(&u, 0, sizeof u);
    u.foreign = 1;
    u.t_poll = now;
    u.t_alive = now;
}

void core_cache_init(const ftc_bus_t *b)
{
    bus = *b;
    memset(&u, 0, sizeof u);
    dir_saved = 0;
    uint32_t fs = bflb_flash_get_size();
    flash_mb = fs >> 20;
    data_end = fs >= DATA_ADDR + 2 * BLOCK && fs <= (256u << 20) ? fs - BLOCK : 0;
    foreign_data = 0;
    if (data_end)
        dir_load();
    else
        dir_empty();
    u.seen = mb_word(0);
    printf("кэш ядер: flash BL616 %u КБ, ядер %u%s\r\n", (unsigned)(fs >> 10), (unsigned)dir.count,
           foreign_data ? ", место занято чужими данными — кэш выключен до FORMAT" : "");
}

int core_cache_owns_info(uint32_t now)
{
    return u.active || u.foreign || u.relink || (u.acked && since(now, u.t_ack) < (int32_t)OWN_MS);
}

int core_cache_poll(uint32_t now)
{
    if (u.foreign) {
        if (since(now, u.t_poll) < (int32_t)POLL_MS)
            return 1;
        u.t_poll = now;
        if (fpga_status_poll() == 0)
            u.t_alive = now;
        if (fpga_boot_requested()) {
            printf("ядро из кэша просит перезагрузку: JTAG Reconfig\r\n");
            reconfig(now);
        } else if (since(now, u.t_alive) >= (int32_t)LOST_MS) {
            printf("ядро из кэша не отвечает BL616: назад в слот 0\r\n");
            reconfig(now);
        }
        return 1;
    }
    if (u.relink) {
        if (since(now, u.t_poll) < (int32_t)POLL_MS)
            return 1;
        u.t_poll = now;
        if (fpga_status_poll() == 0 && !fpga_boot_requested()) {
            u.relink = 0;
            u.seen = mb_word(0);   /* ящик нового ядра — уже виденный */
            if (u.fail)
                ack(u.fail_seq, u.fail_status, now);
            u.fail = 0;
            return 0;
        }
        if (since(now, u.t_relink) >= (int32_t)RELINK_MS) {
            u.relink = 0;
            u.fail = 0;
            return 0;
        }
        return 1;
    }
    if (!u.active && now - u.t_check < IDLE_MS)
        return 0;
    u.t_check = now;
    /* ESP пишет ящик своим клоком: верим только двум одинаковым чтениям заголовка подряд */
    uint32_t hdr = mb_word(0);
    if (hdr != mb_word(0))
        return u.active;
    if (hdr == u.seen) {
        if (u.active && now - u.t_cmd > TIMEOUT_MS)
            u.active = 0;
        return u.active;
    }
    u.seen = hdr;
    if ((hdr >> 24) != CC_MAGIC)
        return u.active;
    uint32_t cmd = (hdr >> 16) & 0xFF, seq = hdr & 0xFFFF;
    u.t_cmd = now;
    if (foreign_data && cmd != CC_INFO && cmd != CC_FORMAT && cmd != CC_FIND) {
        ack(seq, CC_FOREIGN, now);
        return 0;
    }
    switch (cmd) {
    case CC_BEGIN: begin(seq, now); break;
    case CC_DATA: data(seq, now); break;
    case CC_END: end(seq, now); break;
    case CC_ABORT:
        u.active = 0;
        ack(seq, CC_OK, now);
        break;
    case CC_FIND: find(seq, now); break;
    case CC_LOAD: load(seq, now); break;
    case CC_INFO: ack(seq, foreign_data ? 0xFF : data_end ? flash_mb : 0, now); break;
    case CC_FORMAT: {
        uint32_t key = mb_word(CC_W_SIZE);
        if (key != CC_FORMAT_KEY)
            ack(seq, CC_BAD_CMD, now);
        else
            format(seq, now);
        break;
    }
    default: ack(seq, CC_BAD_CMD, now); break;
    }
    return u.active || u.foreign || u.relink;
}
