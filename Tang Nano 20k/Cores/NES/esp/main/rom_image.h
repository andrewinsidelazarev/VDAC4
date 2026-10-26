/* SPDX-License-Identifier: Apache-2.0
 * Проверка формата до переключения FPGA. Ограничения соответствуют GameLoader
 * и карте адресов cart.sv закреплённой версии NESTang. */
#ifndef NES_ROM_IMAGE_H
#define NES_ROM_IMAGE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "rom_mappers.h"
#define NES_ROM_MAX (16u + 2u * 1024u * 1024u + 1024u * 1024u)

static const char *rom_check(const uint8_t *p, size_t size)
{
    if (!p || size < 16 || memcmp(p, "NES\x1a", 4)) return "Not an iNES cartridge";
    if (!p[4] || p[4] > 128 || p[5] > 128) return "ROM exceeds 2 MiB PRG / 1 MiB CHR";
    if (p[6] & 4) return "Trainer cartridges are not supported";
    if (p[7] & 3) return "VS / PlayChoice cartridges unsupported";
    const int nes20 = (p[7] & 12) == 8;
    if ((p[7] & 12) && !nes20) return "Unsupported iNES header format";
    unsigned mapper = (p[6] >> 4) | (p[7] & 0xf0);
    if (nes20) {
        if (p[9]) return "Extended NES 2.0 ROM size unsupported";
        mapper |= (unsigned)(p[8] & 15) << 8;
        if (p[14] & 3) return "Extra NES 2.0 ROM sections unsupported";
    } else {
        /* GameLoader игнорирует старшие биты mapper в старых dirty-заголовках. */
        unsigned dirty = p[9] & 0xfe;
        for (unsigned i = 10; i < 16; ++i) dirty |= p[i];
        if (dirty) mapper &= 15;
    }
    int supported = 0;
    for (size_t i = 0; i < sizeof rom_mappers; ++i)
        if (rom_mappers[i] == mapper) supported = 1;
    if (!supported) return "Mapper is not included in this core";
    const size_t expected = 16u + (size_t)p[4] * 16384u + (size_t)p[5] * 8192u;
    if (size != expected) return "ROM file length does not match header";
    return NULL;
}
#endif
