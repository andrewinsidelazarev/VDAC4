/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Клавиатура Evo (PS/2 набора 2, очередь AVR — регистр Gluk #F0) → события клавиш ядра ZX Next
 * на VDAC4: регистр LINK KEY (0x10) — одно событие {отпущена, номер[6:0]}, номер — код USB HID
 * (страница клавиатуры), модификаторы E0…E7 → 104…111, как команда 1 hid.v FPGA-Companion и карта
 * 128 бит usb_keyboard NextNano.
 *
 * Разбор: префиксы E0 (расширенные клавиши) и F0 (отпускание), E1 (Pause: восемь байтов, только
 * нажатие — событие «нажата» и сразу «отпущена»), ложные Shift E0 12 / E0 59 вокруг PrintScreen и
 * клавиш блока стрелок — пропускаются. Байт 0xFF или 0x00 (переполнение очереди AVR, ошибка
 * клавиатуры) — все клавиши отпущены (событие PS2_HID_CLEAR).
 */
#ifndef PS2_HID_H
#define PS2_HID_H

#include <stdint.h>

#define PS2_HID_RELEASE 0x80u      /* бит 7 события — отпущена */
#define PS2_HID_CLEAR   0x100u     /* не событие KEY: все клавиши отпущены (запись KEYS_CLEAR) */
#define PS2_HID_NONE    0xFFFFu

typedef struct {
    uint8_t e0, f0;     /* префиксы текущего кода */
    uint8_t e1;         /* байтов Pause осталось пропустить */
} ps2_hid_t;

void ps2_hid_init(ps2_hid_t *s);
/* Байт PS/2 → до двух событий в out[] (Pause — нажатие и отпускание); вернёт их число.
   Событие — {отпущена, номер[6:0]} или PS2_HID_CLEAR. */
int ps2_hid_byte(ps2_hid_t *s, uint8_t byte, uint16_t out[2]);
/* Номер клавиши ядра Next (0…111) для кода набора 2; 0 — клавиши нет. ext — был префикс E0. */
uint8_t ps2_hid_code(uint8_t code, int ext);

#endif
