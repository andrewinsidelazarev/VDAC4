/* Headers are usable by a C caller as well as by the C++ renderer. */
#include "cores_ui.h"
static cui_read_fn reader;
static const cui_card_t *(*parse)(const void *, uint32_t, void *, size_t) = cui_card_parse;
static const cui_card_t *(*read_elf)(cui_read_fn, void *, uint32_t, void *, size_t) = cui_card_read_elf;
void check_api(void *arena, const void *bytes, uint32_t n) {
    cui_core_t core = {0};
    core.card = parse(bytes, n, arena, cui_card_arena_required(n));
    core.card = read_elf(reader, 0, n, arena, CUI_CARD_ARENA_BYTES);
    (void)core;
}
