/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Проверка core_cache.c на ПК: flash BL616 — массив (стирание секторами 4 КБ в 0xFF, запись
 * только сбрасывает биты — запись поверх нестёртого считается ошибкой), почтовый ящик и CP_INFO
 * — массив слов, ПЛИС — снимок STATUS, просьба о перезагрузке, JTAG Reconfig и загрузка по JTAG
 * (принимает образ целиком через источник core_cache). Ядра — свои образы с заголовком и
 * концом как у NEXT.BIN; первое — сам NEXT.BIN, если передан.
 * Ожидается: запись, поиск, загрузка и возврат в слот 0; круг вытесняет самое давнее; каталог
 * переживает перезагрузку BL616 и порчу новой копии; ошибки образа, порядка, SHA-256 и flash —
 * свои коды; чужие данные на месте кэша не трогаются до FORMAT.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("ОШИБКА %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- flash ---- */
static uint8_t *fl;
static uint32_t fl_size = 4u << 20;
static unsigned fl_overwrite, fl_bad_erase, fl_erases;
uint32_t bflb_flash_get_size(void) { return fl_size; }
int bflb_flash_erase(uint32_t a, uint32_t n)
{
    if (a % 4096 || n % 4096 || a + n > fl_size) { fl_bad_erase++; return -1; }
    memset(fl + a, 0xFF, n);
    fl_erases++;
    return 0;
}
int bflb_flash_write(uint32_t a, uint8_t *d, uint32_t n)
{
    if (a + n > fl_size) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (d[i] & ~fl[a + i]) fl_overwrite++;
        fl[a + i] &= d[i];
    }
    return 0;
}
int bflb_flash_read(uint32_t a, uint8_t *d, uint32_t n)
{
    if (a + n > fl_size) return -1;
    memcpy(d, fl + a, n);
    return 0;
}

/* ---- время ---- */
static uint64_t t_ms = 1000;
uint64_t bflb_mtimer_get_time_ms(void) { return t_ms; }
uint64_t bflb_mtimer_get_time_us(void) { return t_ms * 1000; }
void bflb_mtimer_delay_ms(uint32_t ms) { t_ms += ms; }
void bflb_mtimer_delay_us(uint32_t us) { t_ms += us / 1000; }

/* ---- ПЛИС: ящик, CP_INFO, STATUS, JTAG ---- */
static uint32_t mb[FWU_MB_WORDS], info;
static int fpga_kind = 0;          /* 0 — ядро VDAC4, 1 — ядро из кэша или слота, 2 — грузится,
                                      3 — не настроена (битстрим слота испорчен) */
static int loading_polls;          /* «грузится» — столько снимков */
static int load_to;                /* чем станет ПЛИС после загрузки */
static int boot_flag, foreign_alive = 1;
static unsigned reconfigs, jtag_loads, boots;
static uint32_t boot_addr;
static int slot_kind[8] = {0, 0, 0, 1, 3, 0, 0, 0};   /* что грузится из слота flash Tang */
static int jtag_result = CORE_JTAG_OK;
static uint8_t *jtag_img;
static uint32_t jtag_n;

static void b_rd(void *ctx, uint32_t a, uint32_t *w, uint32_t n)
{
    (void)ctx;
    for (uint32_t i = 0; i < n; i++, a += 4) {
        CHECK(fpga_kind == 0 || a < FWU_MB_BASE, "чтение ящика 0x%06X, когда в ПЛИС не ядро VDAC4", (unsigned)a);
        w[i] = fpga_kind == 0 && a >= FWU_MB_BASE && a < FWU_MB_BASE + 4 * FWU_MB_WORDS ? mb[(a - FWU_MB_BASE) / 4] : 0;
    }
}
static void b_wr(void *ctx, uint32_t a, const uint32_t *w, uint32_t n)
{
    (void)ctx;
    CHECK(fpga_kind == 0, "запись в ПЛИС 0x%06X, когда в ПЛИС не ядро VDAC4", (unsigned)a);
    if (a == 0x3FFFFCu && n == 1 && fpga_kind == 0) info = w[0];
}
int fpga_status_poll(void)
{
    if (fpga_kind == 2) {
        if (--loading_polls <= 0) { fpga_kind = load_to; boot_flag = 0; }
        return -1;
    }
    if (fpga_kind == 3 || (fpga_kind == 1 && !foreign_alive)) return -1;
    return 0;
}
int fpga_boot_requested(void) { return boot_flag; }
/* Reconfig: адрес перехода у ядер из кэша и слотов — 0 */
void fpga_jtag_reconfig(void) { reconfigs++; fpga_kind = 2; load_to = 0; loading_polls = 20; }
/* переход в слот (прошивка 35): ПЛИС грузит то, что лежит в слоте */
int core_jtag_boot(uint32_t address)
{
    boots++;
    boot_addr = address;
    if (jtag_result == CORE_JTAG_NO_FPGA) return jtag_result;
    fpga_kind = 2;
    load_to = slot_kind[(address >> 20) & 7];
    loading_polls = 20;
    return CORE_JTAG_OK;
}
int core_jtag_load(core_jtag_read_fn read, void *ctx, uint32_t size, uint32_t *usercode)
{
    jtag_loads++;
    free(jtag_img);
    jtag_img = malloc(size);
    jtag_n = size;
    for (uint32_t at = 0; at < size; at += 512) {
        uint32_t n = size - at < 512 ? size - at : 512;
        if (read(ctx, at, jtag_img + at, n) != 0) return CORE_JTAG_READ;
    }
    /* после неудачи ПЛИС пустая; отказ до команд настройки (IDCODE, размер) её не трогает */
    fpga_kind = jtag_result == CORE_JTAG_OK ? 1
              : jtag_result == CORE_JTAG_NO_FPGA || jtag_result == CORE_JTAG_BAD_SIZE ? 0 : 2;
    loading_polls = 1000000;
    load_to = 0;
    if (usercode) *usercode = 0x56443403u;
    return jtag_result;
}

static ftc_bus_t bus = {.rd = b_rd, .wr = b_wr};

/* ---- образы ---- */
typedef struct { uint8_t *b; uint32_t n; uint8_t sha[32]; } img_t;

static void img_make(img_t *im, uint32_t n, uint32_t seed, int kind)
{
    static const uint8_t head[68] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xA5, 0xC3, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B,
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x51, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x0B, 0x00, 0x00, 0x00, 0xD2, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
        0x3B, 0x80, 0x08, 0x3E};
    static const uint8_t tail[30] = {
        0x34, 0x73, 0x0A, 0x00, 0x00, 0x00, 0x56, 0x44, 0x34, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x08, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    im->b = malloc(n);
    im->n = n;
    uint32_t x = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; im->b[i] = (uint8_t)(x >> 16); }
    memcpy(im->b, head, sizeof head);
    memcpy(im->b + n - sizeof tail, tail, sizeof tail);
    if (kind == 1) im->b[31] = 0x1C;            /* IDCODE не GW2AR-18 */
    if (kind == 2) im->b[n - 30 + 8] = 0x00;     /* нет метки VDAC4 */
    if (kind == 3) im->b[59] = 0x10;            /* адрес перехода не 0 */
    sha256(im->b, n, im->sha);
}

/* ---- ESP: команды ящика ---- */
static uint32_t seq = 0x100;

static uint32_t run(int max_ms)
{
    uint32_t want = seq & 0xFFFF;
    for (int i = 0; i < max_ms / 10; i++) {
        core_cache_poll((uint32_t)t_ms);
        t_ms += 10;
        if ((info >> 24) == CC_INFO_MAGIC && (info & 0xFFFF) == want) return (info >> 16) & 0xFF;
    }
    return 0x1FF;                                /* ответа нет */
}

static void put_name_sha(const char *name, const uint8_t *sha)
{
    memset(&mb[1], 0, 13 * 4);
    if (sha) memcpy(&mb[CC_W_SHA], sha, 32);
    if (name) memcpy(&mb[CC_W_NAME], name, strlen(name) < 16 ? strlen(name) : 16);
}

static uint32_t cmd(uint32_t c)
{
    seq++;
    mb[0] = CC_MAGIC << 24 | c << 16 | (seq & 0xFFFF);
    return run(3000);
}

static uint32_t c_find(const char *name, const uint8_t *sha) { put_name_sha(name, sha); return cmd(CC_FIND); }
static uint32_t c_info(void) { return cmd(CC_INFO); }

static uint32_t c_data(const img_t *im, uint32_t off, uint32_t len)
{
    mb[1] = len;
    mb[2] = off;
    memcpy(&mb[FWU_PAYLOAD_WORD], im->b + off, len);
    return cmd(CC_DATA);
}

/* вся запись: первый не OK ответ (или OK) */
static uint32_t upload(const char *name, const img_t *im, const uint8_t *sha)
{
    put_name_sha(name, sha ? sha : im->sha);
    mb[1] = im->n;
    uint32_t r = cmd(CC_BEGIN);
    if (r) return r;
    for (uint32_t off = 0; off < im->n; off += FWU_PAYLOAD_MAX) {
        uint32_t len = im->n - off < FWU_PAYLOAD_MAX ? im->n - off : FWU_PAYLOAD_MAX;
        if ((r = c_data(im, off, len)) != 0) return r;
    }
    return cmd(CC_END);
}

static uint32_t c_load_cmd(uint32_t c)
{
    seq++;
    mb[0] = CC_MAGIC << 24 | c << 16 | (seq & 0xFFFF);
    /* LOADING пишется до загрузки; итог — по состоянию модели */
    uint32_t want = seq & 0xFFFF;
    for (int i = 0; i < 300; i++) {
        core_cache_poll((uint32_t)t_ms);
        t_ms += 10;
        if ((info >> 24) == CC_INFO_MAGIC && (info & 0xFFFF) == want) return (info >> 16) & 0xFF;
    }
    return 0x1FF;
}

static uint32_t c_load(const char *name, const uint8_t *sha)
{
    put_name_sha(name, sha);
    return c_load_cmd(CC_LOAD);
}

static void reboot_bl616(void)
{
    memset(mb, 0, sizeof mb);
    info = 0;
    core_cache_init(&bus);
}

/* круг шагов главного цикла, пока кэш держит цикл */
static int steps_while_busy(int max)
{
    int n = 0;
    while (n < max && core_cache_poll((uint32_t)t_ms)) { t_ms += 10; n++; }
    return n;
}

int main(int argc, char **argv)
{
    fl = malloc(16u << 20);
    memset(fl, 0xFF, 16u << 20);
    img_t next, b, c, d, e, c2;
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        fseek(f, 0, SEEK_END);
        next.n = (uint32_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        next.b = malloc(next.n);
        if (fread(next.b, 1, next.n, f) != next.n) return 2;
        fclose(f);
        sha256(next.b, next.n, next.sha);
    } else {
        img_make(&next, 907418, 1, 0);
    }
    img_make(&b, 900000, 2, 0);
    img_make(&c, 880000, 3, 0);
    img_make(&d, 917504, 4, 0);
    img_make(&e, 600000, 5, 0);
    img_make(&c2, 870000, 6, 0);

    /* 1. Чистая flash 4 МБ */
    reboot_bl616();
    CHECK(c_info() == 4, "INFO: не 4 МБ");
    CHECK(c_find("NEXT", next.sha) == CC_NONE, "FIND в пустом кэше");

    /* 2. Запись NEXT, поиск */
    uint64_t t0 = t_ms;
    CHECK(upload("NEXT", &next, NULL) == CC_OK, "запись NEXT");
    printf("запись NEXT (%u байт): %u мс модели, стираний %u\n", next.n, (unsigned)(t_ms - t0), fl_erases);
    CHECK(c_find("NEXT", next.sha) == CC_FOUND, "FIND NEXT");
    CHECK(c_find("NEXT", b.sha) == CC_STALE, "FIND NEXT с другим SHA");
    CHECK(c_find("OTHER", next.sha) == CC_NONE, "FIND другого имени");

    /* 3. Загрузка, ядро работает, просьба о перезагрузке — слот 0 */
    CHECK(c_load("NEXT", NULL) == CC_LOADING, "LOAD NEXT: нет LOADING");
    CHECK(jtag_loads == 1 && jtag_n == next.n && memcmp(jtag_img, next.b, next.n) == 0, "JTAG получил не тот образ");
    CHECK(fpga_kind == 1, "ядро из кэша не работает");
    CHECK(steps_while_busy(100) == 100, "ядро из кэша: цикл не держится");
    boot_flag = 1;
    int n = steps_while_busy(1000);
    CHECK(reconfigs == 1 && fpga_kind == 0 && n < 1000, "BOOT: перезагрузок %u, ПЛИС %d, шагов %d", reconfigs, fpga_kind, n);

    /* 4. Перезагрузка BL616: каталог из flash */
    reboot_bl616();
    CHECK(c_find("NEXT", next.sha) == CC_FOUND, "после перезагрузки BL616 нет NEXT");

    /* 5. Круг: три ядра помещаются, четвёртое вытесняет самое давнее */
    CHECK(upload("B", &b, NULL) == CC_OK, "запись B");
    CHECK(upload("C", &c, NULL) == CC_OK, "запись C");
    CHECK(c_find("NEXT", next.sha) == CC_FOUND && c_find("B", b.sha) == CC_FOUND && c_find("C", c.sha) == CC_FOUND, "три ядра");
    CHECK(upload("D", &d, NULL) == CC_OK, "запись D");
    CHECK(c_find("NEXT", next.sha) == CC_NONE, "NEXT не вытеснено");
    CHECK(c_find("B", b.sha) == CC_FOUND && c_find("C", c.sha) == CC_FOUND && c_find("D", d.sha) == CC_FOUND, "B, C, D");
    CHECK(upload("E", &e, NULL) == CC_OK, "запись E");
    CHECK(c_find("B", b.sha) == CC_NONE && c_find("C", c.sha) == CC_FOUND, "B не вытеснено");
    /* то же имя — замена */
    CHECK(upload("C", &c2, NULL) == CC_OK, "запись C2");
    CHECK(c_find("C", c.sha) == CC_STALE && c_find("C", c2.sha) == CC_FOUND, "замена C");
    CHECK(c_load("D", d.sha) == CC_LOADING && memcmp(jtag_img, d.b, d.n) == 0, "LOAD D после круга");
    boot_flag = 1;
    steps_while_busy(1000);
    CHECK(fpga_kind == 0, "D: назад в слот 0");

    /* 6. Ошибки */
    memset(mb, 0, sizeof mb);
    CHECK(c_data(&b, 0, 1984) == CC_BAD_CMD, "DATA без BEGIN");
    put_name_sha("X", b.sha); mb[1] = 100;
    CHECK(cmd(CC_BEGIN) == CC_BAD_SIZE, "BEGIN 100 байт");
    put_name_sha("X", b.sha); mb[1] = 4u << 20;
    CHECK(cmd(CC_BEGIN) == CC_BAD_SIZE, "BEGIN 4 МБ");
    put_name_sha("", b.sha); mb[1] = b.n;
    CHECK(cmd(CC_BEGIN) == CC_BAD_NAME, "BEGIN без имени");
    put_name_sha("X", b.sha); mb[1] = b.n; memset(&mb[CC_W_NAME], 'A', 16);
    CHECK(cmd(CC_BEGIN) == CC_BAD_NAME, "BEGIN, имя без нуля");
    img_t bad1, bad2, bad3;
    img_make(&bad1, 300000, 7, 1);
    img_make(&bad2, 300000, 8, 2);
    img_make(&bad3, 300000, 9, 3);
    CHECK(upload("BAD1", &bad1, NULL) == CC_BAD_IMAGE, "IDCODE не тот");
    CHECK(upload("BAD2", &bad2, NULL) == CC_NO_MARK, "нет метки VDAC4");
    CHECK(upload("BAD3", &bad3, NULL) == CC_BAD_IMAGE, "адрес перехода не 0");
    CHECK(upload("BADSHA", &e, b.sha) == CC_BAD_SHA, "SHA-256 не тот");
    CHECK(c_find("BAD2", bad2.sha) == CC_NONE && c_find("BADSHA", e.sha) == CC_NONE, "плохие ядра в каталоге");
    /* порядок посылок */
    put_name_sha("ORD", e.sha); mb[1] = e.n;
    CHECK(cmd(CC_BEGIN) == CC_OK, "BEGIN ORD");
    CHECK(c_data(&e, 0, 1984) == CC_OK, "ORD 0");
    CHECK(c_data(&e, 0, 1984) == CC_OK, "ORD повтор 0");
    CHECK(c_data(&e, 3968, 1984) == CC_BAD_OFFSET, "ORD через кусок");
    CHECK(c_data(&e, e.n - 10, 20) == CC_BAD_OFFSET, "ORD за концом");
    CHECK(cmd(CC_END) == CC_BAD_OFFSET, "END не всё принято");
    CHECK(cmd(CC_ABORT) == CC_OK, "ABORT");
    CHECK(cmd(CC_END) == CC_BAD_CMD, "END после ABORT");
    CHECK(cmd(0x55) == CC_BAD_CMD, "неизвестная команда");
    /* брошенная запись: 10 с без команд */
    put_name_sha("IDLE", e.sha); mb[1] = e.n;
    CHECK(cmd(CC_BEGIN) == CC_OK, "BEGIN IDLE");
    for (int i = 0; i < 1200; i++) { core_cache_poll((uint32_t)t_ms); t_ms += 10; }
    CHECK(c_data(&e, 0, 1984) == CC_BAD_CMD, "запись не брошена за 10 с");

    /* 7. Ядро испорчено во flash: LOAD → BAD_SHA, ядро вычеркнуто */
    {
        /* найти D во flash по его началу */
        uint32_t at = 0;
        for (uint32_t a = 0x90000; a < fl_size; a += 65536)
            if (memcmp(fl + a, d.b, 256) == 0) at = a;
        CHECK(at != 0, "D не найдено во flash");
        fl[at + 500000] ^= 0x10;
        unsigned loads = jtag_loads;
        CHECK(c_load("D", NULL) == CC_BAD_SHA, "испорченное D");
        CHECK(jtag_loads == loads, "испорченное D грузилось");
        CHECK(c_find("D", d.sha) == CC_NONE, "испорченное D не вычеркнуто");
    }

    /* 8. Загрузка не вышла: назад в слот 0, ответ — LOAD_FAIL | код */
    jtag_result = CORE_JTAG_NOT_DONE;
    unsigned rc = reconfigs;
    put_name_sha("E", NULL);
    seq++;
    mb[0] = CC_MAGIC << 24 | CC_LOAD << 16 | (seq & 0xFFFF);
    uint32_t got = run(1000);
    CHECK(got == CC_LOADING, "LOAD E: нет LOADING");
    /* LOADING записан до загрузки, а ответ об ошибке — после того, как слот 0 ответит */
    info = 0;
    for (int i = 0; i < 300 && (info >> 24) != CC_INFO_MAGIC; i++) { core_cache_poll((uint32_t)t_ms); t_ms += 10; }
    CHECK(reconfigs == rc + 1, "неудачная загрузка: нет перезагрузки");
    CHECK(info == (CC_INFO_MAGIC << 24 | (uint32_t)(CC_LOAD_FAIL | CORE_JTAG_NOT_DONE) << 16 | (seq & 0xFFFF)),
          "ответ неудачной загрузки %08X", (unsigned)info);
    jtag_result = CORE_JTAG_OK;
    /* ядро не отвечает BL616 — через 3 с назад в слот 0 переходом по адресу 0 (прошивка 35) */
    CHECK(c_load("E", NULL) == CC_LOADING, "LOAD E");
    foreign_alive = 0;
    rc = reconfigs;
    unsigned bt = boots;
    n = steps_while_busy(2000);
    CHECK(reconfigs == rc && boots == bt + 1 && boot_addr == 0 && fpga_kind == 0 && n >= 290 && n < 1000,
          "молчащее ядро: перезагрузок %u, переходов %u (адрес %06X), шагов %d", reconfigs - rc, boots - bt,
          (unsigned)boot_addr, n);
    foreign_alive = 1;

    /* 9. Новая копия каталога испорчена: берётся прежняя — без последнего ядра, остальные целы */
    {
        const char *names[5] = {"NEXT", "B", "C", "D", "E"};
        const img_t *ims[5] = {&next, &b, &c2, &d, &e};
        int was[5];
        for (int i = 0; i < 5; i++) was[i] = c_find(names[i], ims[i]->sha) == CC_FOUND;
        img_t f;
        img_make(&f, 300000, 10, 0);
        CHECK(upload("F", &f, NULL) == CC_OK, "запись F");
        uint32_t ga, gb;
        memcpy(&ga, fl + 0x80004, 4);
        memcpy(&gb, fl + 0x81004, 4);
        uint32_t newer = ga > gb ? 0x80000u : 0x81000u;
        fl[newer + 100] ^= 0x01;
        reboot_bl616();
        CHECK(c_info() == 4, "INFO после порчи каталога");
        CHECK(c_find("F", f.sha) == CC_NONE, "испорченная копия каталога взята");
        for (int i = 0; i < 5; i++)
            CHECK((c_find(names[i], ims[i]->sha) == CC_FOUND) == was[i], "прежняя копия каталога: %s", names[i]);
        CHECK(c_load("E", NULL) == CC_LOADING && memcmp(jtag_img, e.b, e.n) == 0, "LOAD E из прежней копии");
        boot_flag = 1;
        steps_while_busy(1000);
    }

    CHECK(fl_overwrite == 0, "запись flash поверх нестёртого: %u байт", fl_overwrite);
    CHECK(fl_bad_erase == 0, "стирание не по секторам: %u", fl_bad_erase);

    /* 10. Чужие данные на месте кэша: ничего не пишется до FORMAT */
    memset(fl, 0xFF, fl_size);
    fl[0x200000] = 0x5A;
    reboot_bl616();
    CHECK(c_info() == 0xFF, "INFO при чужих данных");
    put_name_sha("B", b.sha); mb[1] = b.n;
    CHECK(cmd(CC_BEGIN) == CC_FOREIGN, "BEGIN при чужих данных");
    CHECK(fl[0x200000] == 0x5A, "чужие данные тронуты");
    mb[1] = 0x12345678;
    CHECK(cmd(CC_FORMAT) == CC_BAD_CMD, "FORMAT без ключа");
    mb[1] = CC_FORMAT_KEY;
    CHECK(cmd(CC_FORMAT) == CC_OK, "FORMAT");
    CHECK(c_info() == 4, "INFO после FORMAT");
    CHECK(upload("B", &b, NULL) == CC_OK && c_find("B", b.sha) == CC_FOUND, "запись после FORMAT");

    /* 11. Разбор Codex 21.10.2026 (прошивка 15).
       № 7: чистая flash, обрыв первой записи между BEGIN и END — после перезапуска кэш не «чужой» */
    memset(fl, 0xFF, fl_size);
    reboot_bl616();
    put_name_sha("CUT", e.sha); mb[1] = e.n;
    CHECK(cmd(CC_BEGIN) == CC_OK, "BEGIN на чистой flash");
    CHECK(c_data(&e, 0, 1984) == CC_OK && c_data(&e, 1984, 1984) == CC_OK, "DATA на чистой flash");
    reboot_bl616();                                    /* питание пропало до END */
    CHECK(c_info() == 4, "после обрыва первой записи INFO %u (FOREIGN — 255)", c_info());
    CHECK(upload("B", &b, NULL) == CC_OK && c_find("B", b.sha) == CC_FOUND, "запись после обрыва первой");
    /* № 4: отказ JTAG до команд настройки (IDCODE не тот) — ПЛИС не перезагружается, ответ сразу */
    jtag_result = CORE_JTAG_NO_FPGA;
    rc = reconfigs;
    CHECK(c_load("B", NULL) == (CC_LOAD_FAIL | CORE_JTAG_NO_FPGA), "LOAD при отказе до настройки");
    CHECK(reconfigs == rc && fpga_kind == 0, "отказ до настройки: перезагрузок %u", reconfigs - rc);
    jtag_result = CORE_JTAG_OK;
    /* № 1/2: BL616 запустился при ядре из кэша в ПЛИС — ящик не трогается (b_rd/b_wr проверяют), ядро
       ведётся: просьба о перезагрузке — слот 0, затем обычный запуск */
    fpga_kind = 1;
    core_cache_adopt(&bus, (uint32_t)t_ms);
    CHECK(steps_while_busy(100) == 100, "принятое ядро: цикл не держится");
    rc = reconfigs;
    boot_flag = 1;
    n = steps_while_busy(1000);
    CHECK(reconfigs == rc + 1 && fpga_kind == 0 && n < 1000, "принятое ядро и BOOT: перезагрузок %u, шагов %d",
          reconfigs - rc, n);
    reboot_bl616();
    CHECK(c_find("B", b.sha) == CC_FOUND, "после принятого ядра кэш на месте");

    /* 12. Flash 2 МБ: помещается одно ядро */
    fl_size = 2u << 20;
    memset(fl, 0xFF, fl_size);
    reboot_bl616();
    CHECK(c_info() == 2, "INFO 2 МБ");
    CHECK(upload("NEXT", &next, NULL) == CC_OK, "2 МБ: NEXT");
    CHECK(upload("B", &b, NULL) == CC_OK, "2 МБ: B");
    CHECK(c_find("NEXT", next.sha) == CC_NONE && c_find("B", b.sha) == CC_FOUND, "2 МБ: одно ядро");
    CHECK(fl_overwrite == 0 && fl_bad_erase == 0, "flash: поверх %u, стираний не по секторам %u", fl_overwrite, fl_bad_erase);

    /* 13. BOOT (прошивка 35): ядро из слота flash Tang (видео — слот 3) */
    {
        reboot_bl616();
        bt = boots;
        rc = reconfigs;
        mb[1] = 0x300000u;
        CHECK(c_load_cmd(CC_BOOT) == CC_LOADING && boots == bt + 1 && boot_addr == 0x300000u,
              "BOOT слот 3: переходов %u, адрес %06X", boots - bt, (unsigned)boot_addr);
        n = steps_while_busy(100);
        CHECK(n == 100 && fpga_kind == 1, "ядро слота 3: цикл %d, ПЛИС %d", n, fpga_kind);
        boot_flag = 1;                                 /* ядро слота просит назад — Reconfig, адрес 0 */
        n = steps_while_busy(1000);
        CHECK(reconfigs == rc + 1 && boots == bt + 1 && fpga_kind == 0 && n < 1000,
              "ядро слота 3 и BOOT: перезагрузок %u, ПЛИС %d, шагов %d", reconfigs - rc, fpga_kind, n);
        /* адрес не начало слота или за слотом 6 — отказ, ПЛИС не тронута */
        const uint32_t bad_addr[3] = {0x300001u, 0x380000u, 0x700000u};
        for (int i = 0; i < 3; i++) {
            mb[1] = bad_addr[i];
            CHECK(c_load_cmd(CC_BOOT) == CC_BAD_OFFSET && boots == bt + 1 && fpga_kind == 0,
                  "BOOT %06X: не отказ", (unsigned)bad_addr[i]);
        }
        /* битстрим слота испорчен: ПЛИС не настроена, через 3 с — переход в слот 0 */
        mb[1] = 0x400000u;
        CHECK(c_load_cmd(CC_BOOT) == CC_LOADING && boot_addr == 0x400000u, "BOOT слот 4");
        n = steps_while_busy(2000);
        CHECK(boots == bt + 3 && boot_addr == 0 && fpga_kind == 0 && n >= 290 && n < 1000,
              "испорченный слот: переходов %u (последний %06X), ПЛИС %d, шагов %d", boots - bt,
              (unsigned)boot_addr, fpga_kind, n);
        /* JTAG не видит ПЛИС — ответ сразу, ПЛИС не тронута */
        jtag_result = CORE_JTAG_NO_FPGA;
        mb[1] = 0x300000u;
        CHECK(c_load_cmd(CC_BOOT) == (CC_LOAD_FAIL | CORE_JTAG_NO_FPGA) && fpga_kind == 0 &&
              steps_while_busy(10) == 0, "BOOT без ПЛИС");
        jtag_result = CORE_JTAG_OK;
        /* чужие данные на месте кэша BOOT не мешают */
        memset(fl, 0xFF, fl_size);
        fl[0x100000] = 0x5A;
        reboot_bl616();
        CHECK(c_info() == 0xFF, "INFO при чужих данных (BOOT)");
        mb[1] = 0x300000u;
        CHECK(c_load_cmd(CC_BOOT) == CC_LOADING && fpga_kind == 2, "BOOT при чужих данных");
        boot_flag = 1;
        steps_while_busy(1000);
        CHECK(fpga_kind == 0, "BOOT при чужих данных: назад в слот 0");
    }

    printf("%s\n", bad ? "ТЕСТ НЕ ПРОЙДЕН" : "ТЕСТ ПРОЙДЕН");
    return bad != 0;
}
