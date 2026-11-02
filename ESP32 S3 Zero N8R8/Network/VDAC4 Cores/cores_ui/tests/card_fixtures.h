#pragma once
// Только стенд ПК. В рабочем коде нет файловой системы или имён знакомых ядер.
#include "cui_card.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
namespace fixtures {
inline uint8_t arenas[3][CUI_CARD_ARENA_BYTES];
inline const cui_card_t *cards[3];
inline int sector_read(void *ctx, uint32_t offset, void *dst, uint32_t count) {
    if (!count || count > 512 || offset / 512 != (offset + count - 1) / 512)
        abort();
    auto *file = static_cast<FILE *>(ctx);
    uint8_t sector[512];
    if (fseek(file, long(offset & ~511u), SEEK_SET))
        return -1;
    size_t got = fread(sector, 1, sizeof(sector), file);
    uint32_t local = offset % 512;
    if (local > got || count > got - local)
        return -1;
    memcpy(dst, sector + local, count);
    return 0;
}
inline void load() {
    const char *paths[] = {"elf/NES.ELF", "elf/NEXT.ELF", "elf/AMIGA.ELF"};
    for (int i = 0; i < 3; ++i) {
        FILE *file = fopen(paths[i], "rb");
        if (!file || fseek(file, 0, SEEK_END))
            abort();
        long bytes = ftell(file);
        if (bytes < 0)
            abort();
        cards[i] =
            cui_card_read_elf(sector_read, file, uint32_t(bytes), arenas[i], sizeof(arenas[i]));
        fclose(file);
        if (!cards[i]) {
            fprintf(stderr, "Cannot parse fixture %s\n", paths[i]);
            abort();
        }
    }
}
}
