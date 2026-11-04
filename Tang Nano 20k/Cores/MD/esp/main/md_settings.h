/* SPDX-License-Identifier: GPL-3.0-or-later
 * Настройки Mega Drive (MD.ELF, 03.11.2026) — /CORES/md.cfg на карте Tang: picture_size — размер
 * картинки против вылета за край на старых HDMI-приёмниках: 0 — AUTO (как SIZE TS-Config, слот 0),
 * 1 — 100 %, 2 — 95 %, 3 — 90 %; в ядро — регистр LINK 21 {535A, size}. Имена nes_size_* — те, что
 * ждёт меню F1 из NES.ELF (game_menu.h, F8).
 * aspect_wide (MD.ELF 0.4, ядро 0.2) — 1: картинка 16:9 на всю ширину (F7 в игре и в меню, как у NES), 0: 4:3;
 * в ядро — бит 2 того же регистра. Старое ядро бит не хранит — пропорции у него только 4:3.
 */
#include "core_cfg.h"
static core_cfg_t md_cfg;
#define R_TS_CTRL   0x40u         /* CTRL TS-Config / FT812 слота 0: SIZE — биты 16:15 */
#define R_NES_SIZE  0x84u         /* md_link.sv: W/R {535A, size[1:0]} */
#define NES_SIZE_CORE 0x06010001u
#define MD_WIDE_CORE  0x06010002u     /* ядро 0.2: 16:9 */
static unsigned nes_size_cfg;     /* 0 — AUTO, 1…3 — 100 / 95 / 90 % */
static unsigned md_wide;          /* 1 — 16:9 */
static unsigned slot0_size;
static int slot0_size_known;

static void slot0_size_read(void)
{
    uint32_t v = 0, w = 0, ctrl = 0;
    slot0_size_known = 0;
    if (link_rd(4, &v) != ZM_OK || link_rd(4, &w) != ZM_OK || v != w) return;
    if ((v >> 24) != 0x01 && (v >> 24) != 0x02) return;
    if (link_rd(R_TS_CTRL, &ctrl) != ZM_OK) return;
    slot0_size = (ctrl >> 15) & 3;
    if (slot0_size == 3) slot0_size = 0;
    slot0_size_known = 1;
}

static unsigned nes_size_effective(void) { return nes_size_cfg ? nes_size_cfg - 1 : slot0_size; }
static unsigned nes_size_percent(unsigned s) { return s == 1 ? 95 : s == 2 ? 90 : 100; }

static int nes_size_apply(void)
{
    const unsigned s = nes_size_effective() | (core_version >= MD_WIDE_CORE && md_wide ? 4u : 0u);
    uint32_t r = 0;
    return link_wr(R_NES_SIZE, 0x535A0000u | s) == ZM_OK && link_rd(R_NES_SIZE, &r) == ZM_OK &&
           (r >> 16) == 0x535A && (r & 7) == s;
}

static int md_cfg_prepare(void)
{
    unsigned size = 0;
    if (core_cfg_load(&md_cfg, core, core_name, &running) != ZM_OK) {
        fail("Cannot read core .cfg; file kept intact");
        return 0;
    }
    nes_size_cfg = core_cfg_integer(&md_cfg, "picture_size", 3, &size, NULL, NULL) == 1 ? size : 0;
    md_wide = core_cfg_integer(&md_cfg, "aspect_wide", 1, &size, NULL, NULL) == 1 ? size : 0;
    return 1;
}

static void md_cfg_apply(void)
{
    const unsigned s = nes_size_effective();
    if (nes_size_apply())
        core->log("md: picture size %u%% (%s), %s", nes_size_percent(s),
                  nes_size_cfg ? "md.cfg" : slot0_size_known ? "AUTO: slot 0" : "AUTO: slot 0 unknown",
                  core_version < MD_WIDE_CORE ? "4:3 (core without 16:9)" : md_wide ? "16:9" : "4:3");
    else core->log("md: picture size was not applied");
}

/* при выходе: размер — в md.cfg, если меняли (ошибка записи — только в журнал) */
static void md_cfg_save(void)
{
    if (!core_cfg_set(&md_cfg, "picture_size", nes_size_cfg, 3)) return;
    if (core_version >= MD_WIDE_CORE && !core_cfg_set(&md_cfg, "aspect_wide", md_wide, 1)) return;
    if (md_cfg.dirty && core_cfg_save(&md_cfg, core, &running) != ZM_OK)
        core->log("md: md.cfg was not written");
}
