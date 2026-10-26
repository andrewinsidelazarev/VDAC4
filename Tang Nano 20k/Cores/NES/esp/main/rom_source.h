/* SPDX-License-Identifier: Apache-2.0
 * Приём файла WC в PSRAM до переключения FPGA. Дополнительная CRC16 защищает
 * смещение и данные; одинаковый повтор пакета не добавляется второй раз. */
#include "rom_image.h"
static uint8_t *rom_data;
static size_t rom_size, rom_received;
static int rom_ready;
static volatile int snap_rx;      /* snapshot.h: 0x77…0x79 несут .sst с карты Evo, не образ */

static uint16_t rom_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xffff;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (unsigned b = 0; b < 8; ++b)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static uint32_t rom_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void rom_discard(void)
{
    if (rom_data) core->free(rom_data);
    rom_data = NULL;
    rom_size = rom_received = 0;
    rom_ready = 0;
}

/* Вызывается только из command, пока worker не загружает и не исполняет ядро. */
static int rom_receive(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    if (state == 2 && evo_rom_wait) { core->mutex_lock(lock); input_ms = core->millis(); core->mutex_unlock(lock); }
    uint8_t reply[5] = {0};
    /* во время игры — только образ, который меню (game_menu.h) просило с карты Evo */
    const int in_game = state == 2 && evo_rom_wait;
    const int idle = state == 0 || state == 3 || state == 255 || in_game;
    if (!idle) {
        core->reply(cmd | 0x80, reply, sizeof reply);
        return ZM_OK;
    }
    if (cmd == 0x77 && n == 4) {
        const uint32_t size = rom_u32(p);
        if (size >= 16 && size <= NES_ROM_MAX) {
            rom_discard();
            rom_data = core->alloc(size, ZM_MEM_PSRAM);
            if (rom_data) {
                rom_size = size;
                error_text[0] = 0;
                if (!in_game) state = 0;
                reply[0] = 1;
            } else fail("Not enough PSRAM for cartridge");
        } else fail("Invalid cartridge file size");
    } else if (cmd == 0x78 && n > 6 && n <= 518 && rom_data && !rom_ready) {
        const size_t at = rom_u32(p), count = n - 6u;
        const uint16_t expected = (uint16_t)p[n-2] | (uint16_t)p[n-1] << 8;
        if (rom_crc16(p, n-2) == expected && at <= rom_received && count <= rom_size - at) {
            if (at == rom_received) {
                memcpy(rom_data + at, p + 4, count);
                rom_received += count;
                reply[0] = 1;
            } else if (count <= rom_received - at && !memcmp(rom_data + at, p + 4, count)) {
                reply[0] = 1;
            }
        }
    } else if (cmd == 0x79 && n == 0 && rom_data && rom_received == rom_size) {
        const char *error = snap_rx ? NULL : rom_check(rom_data, rom_size);
        if (snap_rx) { rom_ready = 1; reply[0] = 1; }
        else if (error) fail(error);
        else if (in_game || save_prepare(rom_data,rom_size,0)) { rom_ready = 1; reply[0] = 1; }
    }
    wr32(reply + 1, (uint32_t)rom_received);
    core->reply(cmd | 0x80, reply, sizeof reply);
    return ZM_OK;
}
