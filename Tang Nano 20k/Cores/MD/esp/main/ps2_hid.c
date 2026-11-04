/* SPDX-License-Identifier: Apache-2.0 */
/* PS/2 набора 2 → коды USB HID для ядра ZX Next (ps2_hid.h). */
#include "ps2_hid.h"

/* Без префикса: код набора 2 → HID (модификаторы — уже 104…111) */
static const uint8_t plain[0x84] = {
    [0x1C] = 4,  [0x32] = 5,  [0x21] = 6,  [0x23] = 7,  [0x24] = 8,  [0x2B] = 9,  [0x34] = 10,
    [0x33] = 11, [0x43] = 12, [0x3B] = 13, [0x42] = 14, [0x4B] = 15, [0x3A] = 16, [0x31] = 17,
    [0x44] = 18, [0x4D] = 19, [0x15] = 20, [0x2D] = 21, [0x1B] = 22, [0x2C] = 23, [0x3C] = 24,
    [0x2A] = 25, [0x1D] = 26, [0x22] = 27, [0x35] = 28, [0x1A] = 29,
    [0x16] = 30, [0x1E] = 31, [0x26] = 32, [0x25] = 33, [0x2E] = 34, [0x36] = 35, [0x3D] = 36,
    [0x3E] = 37, [0x46] = 38, [0x45] = 39,
    [0x5A] = 40, [0x76] = 41, [0x66] = 42, [0x0D] = 43, [0x29] = 44, [0x4E] = 45, [0x55] = 46,
    [0x54] = 47, [0x5B] = 48, [0x5D] = 49, [0x4C] = 51, [0x52] = 52, [0x0E] = 53, [0x41] = 54,
    [0x49] = 55, [0x4A] = 56, [0x58] = 57,
    [0x05] = 58, [0x06] = 59, [0x04] = 60, [0x0C] = 61, [0x03] = 62, [0x0B] = 63, [0x83] = 64,
    [0x0A] = 65, [0x01] = 66, [0x09] = 67, [0x78] = 68, [0x07] = 69,
    [0x7E] = 71, [0x77] = 83, [0x7C] = 85, [0x7B] = 86, [0x79] = 87,
    [0x69] = 89, [0x72] = 90, [0x7A] = 91, [0x6B] = 92, [0x73] = 93, [0x74] = 94, [0x6C] = 95,
    [0x75] = 96, [0x7D] = 97, [0x70] = 98, [0x71] = 99, [0x61] = 100,
    [0x14] = 104, [0x12] = 105, [0x11] = 106, [0x59] = 109,
};

/* С префиксом E0 */
static uint8_t extended(uint8_t code)
{
    switch (code) {
    case 0x14: return 108;     /* правый Ctrl */
    case 0x11: return 110;     /* правый Alt */
    case 0x1F: return 107;     /* левый Win */
    case 0x27: return 111;     /* правый Win */
    case 0x2F: return 101;     /* Menu */
    case 0x70: return 73;      /* Insert */
    case 0x6C: return 74;      /* Home */
    case 0x7D: return 75;      /* Page Up */
    case 0x71: return 76;      /* Delete */
    case 0x69: return 77;      /* End */
    case 0x7A: return 78;      /* Page Down */
    case 0x74: return 79;      /* → */
    case 0x6B: return 80;      /* ← */
    case 0x72: return 81;      /* ↓ */
    case 0x75: return 82;      /* ↑ */
    case 0x4A: return 84;      /* KP / */
    case 0x5A: return 88;      /* KP Enter */
    case 0x7C: return 70;      /* PrintScreen */
    default: return 0;         /* E0 12 / E0 59 — ложные Shift, питание и прочее */
    }
}

void ps2_hid_init(ps2_hid_t *s)
{
    s->e0 = 0;
    s->f0 = 0;
    s->e1 = 0;
}

uint8_t ps2_hid_code(uint8_t code, int ext)
{
    if (ext)
        return extended(code);
    return code < sizeof plain ? plain[code] : 0;
}

int ps2_hid_byte(ps2_hid_t *s, uint8_t b, uint16_t out[2])
{
    if (b == 0xFF || b == 0x00) {            /* переполнение очереди AVR, ошибка клавиатуры */
        ps2_hid_init(s);
        out[0] = PS2_HID_CLEAR;
        return 1;
    }
    if (s->e1) {                             /* Pause: E1 14 77 E1 F0 14 F0 77 */
        if (--s->e1 == 0) {
            out[0] = 72;
            out[1] = 72 | PS2_HID_RELEASE;
            return 2;
        }
        return 0;
    }
    if (b == 0xE1) {
        s->e1 = 7;
        s->e0 = s->f0 = 0;
        return 0;
    }
    if (b == 0xE0) {
        s->e0 = 1;
        return 0;
    }
    if (b == 0xF0) {
        s->f0 = 1;
        return 0;
    }
    if (b == 0xFA || b == 0xAA || b == 0xEE || b == 0xFE) {   /* ответы клавиатуры, не клавиши */
        s->e0 = s->f0 = 0;
        return 0;
    }
    const uint8_t key = ps2_hid_code(b, s->e0);
    const int up = s->f0;
    s->e0 = s->f0 = 0;
    if (key == 0)
        return 0;
    out[0] = (uint16_t)(key | (up ? PS2_HID_RELEASE : 0));
    return 1;
}
