/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Обновление прошивки BL616 по сети: ESP кладёт образ кусками в почтовый ящик ПЛИС, BL616
 * собирает его в RAM, проверяет SHA-256 и заголовок BFNP и только тогда пишет во flash
 * (fw_commit.c — из TCM: код во flash после стирания выполнять нельзя). Протокол —
 * fw_mailbox.h. Переносимо: к ПЛИС — через шину сопроцессора (ftc_bus_t).
 */
#include <stdlib.h>
#include <string.h>

#include "fw_mailbox.h"
#include "fw_update.h"
#include "sha256.h"

#define INFO_ADDR   0x3FFFFCu
#define IDLE_MS     50u          /* как часто без обновления смотреть заголовок */
#define TIMEOUT_MS  10000u       /* без новой команды — обновление брошено */
#define OWN_MS      3000u        /* после ответа CP_INFO не перебивать обычным словом */

static ftc_bus_t bus;
static struct {
    int active;
    uint32_t seen;               /* заголовок, уже разобранный (или бывший при запуске) */
    uint8_t *buf;
    uint32_t size;
    int own;                     /* buf — из malloc (иначе — fixed_buf) */
    uint8_t sha[32];
    uint32_t t_cmd, t_check, t_ack;
    int acked;
} u;
static uint32_t payload[FWU_PAYLOAD_MAX / 4];
static uint8_t *fixed_buf;
static uint32_t fixed_size;
static int clobbered;

static uint32_t mb_word(uint32_t i)
{
    uint32_t w;
    bus.rd(bus.ctx, FWU_MB_BASE + 4 * i, &w, 1);
    return w;
}

static void ack(uint32_t seq, uint32_t status, uint32_t now)
{
    uint32_t v = (FWU_INFO_MAGIC << 24) | ((status & 0xFF) << 16) | (seq & 0xFFFF);
    bus.wr(bus.ctx, INFO_ADDR, &v, 1);
    u.t_ack = now;
    u.acked = 1;
}

static void drop(void)
{
    if (u.own)
        free(u.buf);
    else if (u.buf)
        clobbered = 1;
    u.buf = NULL;
    u.own = 0;
    u.active = 0;
}

void fw_update_set_buffer(void *buf, uint32_t size)
{
    fixed_buf = (uint8_t *)buf;
    fixed_size = size;
}

int fw_update_clobbered(void)
{
    int r = clobbered;
    clobbered = 0;
    return r;
}

void fw_update_init(const ftc_bus_t *b)
{
    bus = *b;
    memset(&u, 0, sizeof u);
    u.seen = mb_word(0);
}

int fw_update_owns_info(uint32_t now)
{
    return u.active || (u.acked && now - u.t_ack < OWN_MS);
}

int fw_update_poll(uint32_t now)
{
    if (!u.active && now - u.t_check < IDLE_MS)
        return 0;
    u.t_check = now;
    /* ESP пишет ящик своим клоком: заголовок, пойманный в момент записи, может быть смесью
       старого и нового — верим только двум одинаковым чтениям подряд */
    uint32_t hdr = mb_word(0);
    if (hdr != mb_word(0))
        return u.active;
    if (hdr == u.seen) {
        if (u.active && now - u.t_cmd > TIMEOUT_MS)
            drop();
        return u.active;
    }
    u.seen = hdr;
    if ((hdr >> 24) != FWU_MAGIC)
        return u.active;
    uint32_t cmd = (hdr >> 16) & 0xFF, seq = hdr & 0xFFFF;
    u.t_cmd = now;
    switch (cmd) {
    case FWU_BEGIN: {
        uint32_t w[9];
        bus.rd(bus.ctx, FWU_MB_BASE + 4, w, 9);
        drop();
        if (w[0] && fixed_buf && w[0] <= fixed_size) {
            u.buf = fixed_buf;
            u.own = 0;
        } else if (w[0] && w[0] <= FWU_MAX_IMAGE && (u.buf = malloc(w[0])) != NULL) {
            u.own = 1;
        } else {
            u.buf = NULL;
            ack(seq, FWU_BAD_SIZE, now);
            break;
        }
        u.size = w[0];
        memcpy(u.sha, &w[1], 32);           /* слова — младшим байтом вперёд, как digest */
        u.active = 1;
        ack(seq, FWU_OK, now);
        break;
    }
    case FWU_DATA: {
        if (!u.active) {
            ack(seq, FWU_BAD_CMD, now);
            break;
        }
        uint32_t w[2];
        bus.rd(bus.ctx, FWU_MB_BASE + 4, w, 2);
        uint32_t len = w[0], off = w[1];
        if (len == 0 || len > FWU_PAYLOAD_MAX || off > u.size || len > u.size - off) {
            ack(seq, FWU_BAD_OFFSET, now);
            break;
        }
        bus.rd(bus.ctx, FWU_MB_BASE + 4 * FWU_PAYLOAD_WORD, payload, (len + 3) / 4);
        memcpy(u.buf + off, payload, len);
        ack(seq, FWU_OK, now);
        break;
    }
    case FWU_END: {
        if (!u.active) {
            ack(seq, FWU_BAD_CMD, now);
            break;
        }
        uint8_t got[32];
        sha256(u.buf, u.size, got);
        if (memcmp(got, u.sha, 32) != 0) {
            ack(seq, FWU_BAD_SHA, now);
            break;
        }
        if (u.size < 4096 || memcmp(u.buf, "BFNP", 4) != 0) {
            ack(seq, FWU_BAD_IMAGE, now);
            break;
        }
        ack(seq, FWU_WRITING, now);
        fw_update_commit(u.buf, u.size);   /* на BL616 не возвращается: перезагрузка */
        drop();
        break;
    }
    case FWU_ABORT:
        drop();
        ack(seq, FWU_OK, now);
        break;
    default:
        ack(seq, FWU_BAD_CMD, now);
        break;
    }
    return u.active;
}
