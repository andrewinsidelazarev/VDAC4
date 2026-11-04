/* SPDX-License-Identifier: GPL-3.0-or-later
 * SRAM картриджа (MD.ELF 0.1, 03.11.2026): пока без сохранений на карту — при загрузке игры SRAM
 * заполняется FF (у Sonic 1 Remastered — нули, как в Genesis-MiSTer). Батарейные .srm — следующий шаг.
 * Имена save_* — те, что ждут меню F1 и приём образа из NES.ELF (game_menu.h, rom_source.h).
 */
static int save_prepared = 1, save_restored = 1;
static int save_prepare(const uint8_t *rom, size_t size, int from_sd)
{
    (void)rom; (void)size; (void)from_sd;
    save_prepared = save_restored = 1;
    return 1;
}
static void save_discard(void) {}

static int br_fill(uint32_t addr, size_t words, uint32_t v);
static int md_saves_restore(uint32_t sram_word, const md_rom_info_t *info)
{
    return br_fill(sram_word, 32768, (info->ctrl & MD_CTRL_SRAM00) ? 0u : 0xFFFFFFFFu);
}
