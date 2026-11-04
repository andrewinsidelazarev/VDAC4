/* SPDX-License-Identifier: GPL-3.0-or-later
 * Клавиатура как пад 1 (MD.ELF 0.2, 03.11.2026) — как вкладка KEYS у AMIGA.ELF: включение и своя
 * клавиша на каждую кнопки пада, в /CORES/md.cfg (keyboard_pad, key_up … key_mode — коды HID).
 * По умолчанию режим включён: другой работы у клавиатуры в Mega Drive нет. Kempston работает всегда.
 * Клавиша одна на одно действие: занятая при назначении меняется местами. F1, F7, F12 и Esc не назначаются
 * (меню, 4:3/16:9, выход в WC, отмена). Модификаторы HID E0…E7 — 104…111 (ps2_hid.h).
 */
#define HID_DEL     76
#define HID_F12     69
#define KJ_HID_F1   58            /* как HID_F1 / HID_ESC меню (game_menu.h подключается позже) */
#define KJ_HID_ESC  41
#define KJ_HID_F7   64            /* 4:3 / 16:9 — в игре и в меню */
/* строка вкладки = действие, строка 0 — включение. Индекс без вычитаний: адрес «таблица минус 1»
 * лёг бы вне секций модуля (check_elf_pointers.py) */
enum { KJ_SWITCH, KJ_UP, KJ_DOWN, KJ_LEFT, KJ_RIGHT, KJ_A, KJ_B, KJ_C, KJ_START, KJ_X, KJ_Y, KJ_Z, KJ_MODE, KJ_COUNT };
/* бит пада: {Z,Y,X,Mode,Start,C,B,A,Вверх,Вниз,Влево,Вправо} — как JOY_1 system.sv */
static const uint16_t kj_bit[KJ_COUNT] = {0, 1u << 3, 1u << 2, 1u << 1, 1u << 0, 1u << 4, 1u << 5, 1u << 6,
                                          1u << 7, 1u << 9, 1u << 10, 1u << 11, 1u << 8};
static const char kj_name[KJ_COUNT][17] = {"KEYBOARD AS PAD", "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "C",
                                           "START", "X", "Y", "Z", "MODE"};
static const char kj_cfg_key[KJ_COUNT][13] = {"keyboard_pad", "key_up", "key_down", "key_left", "key_right",
                                              "key_a", "key_b", "key_c", "key_start", "key_x", "key_y", "key_z",
                                              "key_mode"};
/* стрелки, Z X C — A B C, Enter — Start, A S D — X Y Z, Space — Mode */
static const uint8_t kj_default[KJ_COUNT] = {1, 82, 81, 80, 79, 29, 27, 6, 40, 4, 22, 7, 44};
static uint8_t kj_key[KJ_COUNT] = {1, 82, 81, 80, 79, 29, 27, 6, 40, 4, 22, 7, 44};
static int kj_on = 1;

static int kj_allowed(unsigned code) { return code && code < 128 && code != KJ_HID_F1 && code != HID_F12 && code != KJ_HID_ESC &&
                                              code != KJ_HID_F7; }

/* пад из нажатых клавиш (keys[код HID] — держат) */
static uint32_t kj_pad(const uint8_t *keys)
{
    uint32_t p = 0;
    if (!kj_on) return 0;
    for (unsigned a = KJ_UP; a < KJ_COUNT; a++) if (keys[kj_key[a]]) p |= kj_bit[a];
    return p;
}

/* из md.cfg; чужое или порченое — всё по умолчанию (в журнал) */
static void kj_load(const core_cfg_t *cfg)
{
    uint8_t v[KJ_COUNT];
    unsigned x = 0;
    int bad = 0;
    memcpy(v, kj_default, sizeof v);
    const int on = core_cfg_integer(cfg, kj_cfg_key[KJ_SWITCH], 1, &x, NULL, NULL);
    if (on == 1) v[KJ_SWITCH] = (uint8_t)x;
    else if (on < 0) bad = 1;
    for (unsigned a = KJ_UP; a < KJ_COUNT; a++) {
        const int r = core_cfg_integer(cfg, kj_cfg_key[a], 127, &x, NULL, NULL);
        if (r < 0) bad = 1;
        else if (r == 1) {
            if (!kj_allowed(x)) bad = 1;
            else v[a] = (uint8_t)x;
        }
    }
    for (unsigned a = KJ_UP; a < KJ_COUNT && !bad; a++)
        for (unsigned k = KJ_UP; k < a; k++) if (v[k] == v[a]) bad = 1;
    if (bad) { core->log("md: md.cfg keys ignored, bad or repeated codes"); memcpy(v, kj_default, sizeof v); }
    memcpy(kj_key, v, sizeof kj_key);
    kj_on = v[KJ_SWITCH];
}

/* в md.cfg сразу; 0 — не записалось (действует до выхода) */
static int kj_save(void)
{
    core_cfg_set(&md_cfg, kj_cfg_key[KJ_SWITCH], (unsigned)kj_on, 1);
    for (unsigned a = KJ_UP; a < KJ_COUNT; a++) core_cfg_set(&md_cfg, kj_cfg_key[a], kj_key[a], 127);
    if (!md_cfg.dirty) return 1;
    return core_cfg_save(&md_cfg, core, &running) == ZM_OK;
}

/* Отдельная функция: иначе компилятор сложит low[code-40] в литерал «адрес минус смещение» */
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline, noclone))
#endif
static const char *kj_name_at(const char *table, unsigned width, unsigned index) { return table + width * index; }
static void kj_key_name(unsigned code, char *out, size_t cap)
{
    /* массивы знаков, не указателей: лишние перемещения модулю ни к чему */
    static const char low[][10] = {"ENTER", "ESC", "BKSP", "TAB", "SPACE", "-", "=", "[", "]", "BACKSLASH", "#", ";",
                                   "'", "BACKQUOTE", ",", ".", "/", "CAPS"};
    static const char nav[][10] = {"PRTSC", "SCROLL", "PAUSE", "INS", "HOME", "PGUP", "DEL", "END", "PGDN", "RIGHT",
                                   "LEFT", "DOWN", "UP", "NUM", "PAD /", "PAD *", "PAD -", "PAD +", "PAD ENTER"};
    static const char mod[][8] = {"L CTRL", "L SHIFT", "L ALT", "L WIN", "R CTRL", "R SHIFT", "R ALT", "R WIN"};
    if (code >= 4 && code <= 29) snprintf(out, cap, "%c", 'A' + code - 4);
    else if (code >= 30 && code <= 38) snprintf(out, cap, "%c", '1' + code - 30);
    else if (code == 39) snprintf(out, cap, "0");
    else if (code >= 40 && code <= 57) snprintf(out, cap, "%s", kj_name_at(low[0], sizeof low[0], code - 40));
    else if (code >= 58 && code <= 69) snprintf(out, cap, "F%u", code - 57);
    else if (code >= 70 && code <= 88) snprintf(out, cap, "%s", kj_name_at(nav[0], sizeof nav[0], code - 70));
    else if (code >= 89 && code <= 97) snprintf(out, cap, "PAD %u", code - 88);
    else if (code == 98) snprintf(out, cap, "PAD 0");
    else if (code == 99) snprintf(out, cap, "PAD .");
    else if (code >= 104 && code <= 111) snprintf(out, cap, "%s", kj_name_at(mod[0], sizeof mod[0], code - 104));
    else snprintf(out, cap, "KEY %u", code);
}
