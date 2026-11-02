#ifndef CUI_CARD_H
#define CUI_CARD_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { CUI_CARD_MAX_BYTES = 24576, CUI_CARD_ARENA_BYTES = 25600 };
/* 0 — прочитаны все count байтов. Иначе ошибка. Запросы не пересекают сектор 512. */
typedef int (*cui_read_fn)(void *ctx, uint32_t offset, void *dst, uint32_t count);
typedef struct {
    const uint8_t *rle;
    uint32_t bytes;
    uint16_t w, h;
} cui_card_image_t;
typedef struct {
    char title[49], full_name[97], year[9], manufacturer[49];
    char line1[33], line2[33], category[17], manage_key[9];
    uint16_t accent, palette_count;
    uint16_t palette[256];
    cui_card_image_t icon, image;
} cui_card_t;

/* Включает запас выравнивания для произвольного адреса arena; 0 — неверный размер. */
size_t cui_card_arena_required(uint32_t card_bytes);
/* Вход копируется в arena. NULL — битая карточка или недостаточно памяти.
   Результат и его данные действуют до изменения/освобождения arena владельцем. */
const cui_card_t *cui_card_parse(const void *data, uint32_t size, void *arena, size_t arena_size);
/* ELF32 LE Xtensa. Размер файла передаёт владелец. NULL — нет годной карточки.
   Весь ELF не загружается. Arena при отказе может быть изменена. */
const cui_card_t *cui_card_read_elf(cui_read_fn read, void *ctx, uint32_t file_size, void *arena,
                                    size_t arena_size);
#ifdef __cplusplus
}
#endif
#endif
