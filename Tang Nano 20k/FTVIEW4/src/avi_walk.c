// SPDX-License-Identifier: Apache-2.0
// Проход цепочки кластеров FAT32 (fat_walk): банки изображений и ядра видео (пропуск секторов,
// avi_index.c) и банк списка (фоновая таблица перемотки, core_bg.c).

// Пропуск секторов от начала файла (сразу после avi_rewind, на границе
// кластера). LOADNON ядра на каждый кластер заново читает сектор FAT (CURIT
// без кэша): на файле 52 МБ с кластером 4 КиБ это 12 808 чтений SD, и первая
// перемотка в Unreal шла ~30 с. Поэтому цепочку проходит сам плагин. CURIT
// (#44A5, вход для внешних плагинов) читает сектор FAT в SECBU один раз на
// 128 кластеров, следующие записи берутся из SECBU (#3000) напрямую: страница
// WildDOS стоит в окне #0000, пока работает плагин (#6000 = #600B). Поток на
// итоговый кластер ставит GIPAG (#44D7), остаток кластера дочитывается
// LOAD512. От LOADNON пропуск не зависит: до исправления ядра 2026-09-21 он
// проходил ~81 блок за вызов вместо B.
#define WD_SECBU  0x3000
#define WD_BSECPC 0x3894
#define WD_CUHL   0x38A8
u32 fw_cluster, fw_count;
u8 fw_base[4], fw_rest;

// fw_count — секторы от начала текущего кластера потока (CUHL/CUDE). Делит
// их на BSECPC (степень двойки): остаток — в fw_rest, целые кластеры
// проходит по цепочке и ставит поток на итоговый кластер (GIPAG). fw_base —
// кластер без младших 7 бит, чей сектор FAT сейчас в SECBU. Возврат 1; 0 —
// CURIT не прочитал FAT, ссылка не кластер (0, 1, повреждённый, конец
// цепочки раньше срока) или GIPAG не встал. Деление здесь, а не в C: SDCC 4.3
// проверял результат переменного сдвига через IY, заданный лишь в теле цикла.
bool fat_walk() __naked
{
  __asm
    push ix
    push iy
    ld a, (0x3894)
    ld c, a
    dec a
    ld b, a
    ld a, (_fw_count)
    and b
    ld (_fw_rest), a
    ld hl, (_fw_count)
    ld de, (_fw_count + 2)
00010$:
    srl c
    jr c, 00011$
    srl d
    rr e
    rr h
    rr l
    jr 00010$
00011$:
    ld (_fw_count), hl
    ld (_fw_count + 2), de
    ld a, h
    or l
    or d
    or e
    jp z, 00008$
    ld hl, (0x38A8)
    ld de, (0x38AA)
00001$:
    ld a, l
    and #0x80
    ld (_fw_base), a
    ld a, h
    ld (_fw_base + 1), a
    ld (_fw_base + 2), de
    call 0x44A5
    jp c, 00009$
00002$:
    ld c, (hl)
    inc hl
    ld b, (hl)
    inc hl
    ld e, (hl)
    inc hl
    ld a, (hl)
    and #0x0F
    ld d, a
    or e
    jr nz, 00003$
    ld a, b
    or a
    jr nz, 00004$
    ld a, c
    cp #2
    jp c, 00009$
    jr 00004$
00003$:
    ld a, d
    cp #0x0F
    jr nz, 00004$
    ld a, e
    and b
    inc a
    jr nz, 00004$
    ld a, c
    cp #0xF7
    jp nc, 00009$
00004$:
    ld (_fw_cluster), bc
    ld (_fw_cluster + 2), de
    ld hl, (_fw_count)
    ld a, h
    or l
    jr nz, 00005$
    ld hl, (_fw_count + 2)
    dec hl
    ld (_fw_count + 2), hl
    ld hl, #0
00005$:
    dec hl
    ld (_fw_count), hl
    ld a, h
    or l
    jr nz, 00006$
    ld hl, (_fw_count + 2)
    ld a, h
    or l
    jr z, 00012$
00006$:
    ld a, c
    and #0x80
    ld hl, #_fw_base
    cp (hl)
    jr nz, 00007$
    inc hl
    ld a, b
    cp (hl)
    jr nz, 00007$
    inc hl
    ld a, e
    cp (hl)
    jr nz, 00007$
    inc hl
    ld a, d
    cp (hl)
    jr nz, 00007$
    ld a, c
    and #0x7F
    ld l, a
    ld h, #0
    add hl, hl
    add hl, hl
    ld a, h
    add a, #0x30
    ld h, a
    jp 00002$
00007$:
    ld l, c
    ld h, b
    jp 00001$
00012$:
    ld hl, #_fw_cluster
    call 0x44D7
    jr nz, 00009$
00008$:
    pop iy
    pop ix
    ld a, #1
    ret
00009$:
    pop iy
    pop ix
    xor a
    ret
  __endasm;
}
