// SPDX-License-Identifier: Apache-2.0
// Разбор idx1 в таблицу перемотки (index_scan): банк изображений (avi_index.c, при первой
// перемотке) и банк списка (фоновая таблица банка ядра видео, core_bg.c). До включения нужны
// idx_entry() и INDEX_WRITE.

// Рабочие переменные построения — статические: стек ядра WC у плагина
// неглубокий, а здесь он глубже всего (перемотка -> чтение idx1 -> пропуск
// секторов в ядре). ib_exit — шаг, на котором построение остановилось
// (0 — таблица есть): после отказа его видно в памяти страницы.
// Раскладка — для ASM ниже: запись таблицы ib_entry — смещение чанка, затем
// бегущие счётчики видеочанков и звуковых байтов (их не нужно копировать в
// запись); смещение и длина чанка idx1 — рядом в ib_ol.
u32 ib_pos, ib_size, ib_n, ib_frame, ib_sample, ib_base, ib_tmp;
u32 ib_entry[3], ib_sample_step, ib_pcm_total, ib_ol[2];
u16 ib_k, ib_count, ib_step, ib_rem, ib_sample_rem, ib_fps;
u8 ib_tries, ib_exit, ib_kind;
#define ib_video ib_entry[1]
#define ib_audio ib_entry[2]

// Проход idx1 (ib_n записей по 16 байт с текущей позиции чтения) — таблица
// до ib_count записей. Для каждого чанка: смещение и длина — в ib_ol, вид
// (ib_kind: 1 — видео "00dc"/"00db", 2 — звук "01wb", если он есть, 0 —
// прочее); пока ничего не сосчитано, база смещений: от FOURCC 'movi', если
// смещение меньше movi_start (так пишет ffmpeg), иначе абсолютные. Затем,
// пока запись k нужна этому чанку (видео с номером не меньше кадра k*step
// или звук, доходящий до сэмпла S(k)), — запись: base + смещение и бегущие
// счётчики (ib_entry) в RAM_G #D0000 + k*12; k, кадр и S(k) растут (S — с
// остатком от деления на fps, как в C-версии). Потом счётчик видеочанков +1
// или звуковых байтов + длина. Выход — по концу idx1, числу записей, ошибке
// чтения или Esc. Помощники: 00070$ — (HL)-1, 00075$ — (HL)+1, 00080$ —
// CF = (HL) < (DE), 00090$ — (DE) += (HL); все над u32 в памяти. IX = p.
// Было на C 1,3 КиБ; эталон — avigen.seek_table (IndexTests).
void index_scan(AVI_SHARED *p) __naked
{
  p;
  __asm
    push ix
    push hl
    pop ix
00001$:
    ld hl, #_ib_n
    ld a, (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    jp z, 00099$
    ld hl, #_ib_n
    call 00070$
    ld hl, (_ib_k)
    ld de, (_ib_count)
    or a
    sbc hl, de
    jp nc, 00099$
    call _idx_entry
    ld a, (_view_failed)
    ld hl, #0x6004
    or (hl)
    jp nz, 00099$
    push de
    ld hl, #8
    add hl, de
    ld de, #_ib_ol
    ld bc, #8
    ldir
    pop hl
    ld c, #0
    ld a, (hl)
    cp #0x30
    jr nz, 00010$
    inc hl
    ld a, (hl)
    inc hl
    cp #0x31
    jr z, 00004$
    cp #0x30
    jr nz, 00010$
    ld a, (hl)
    cp #0x64
    jr nz, 00010$
    inc hl
    ld a, (hl)
    cp #0x63
    jr z, 00003$
    cp #0x62
    jr nz, 00010$
00003$:
    inc c
    jr 00010$
00004$:
    ld a, (hl)
    cp #0x77
    jr nz, 00010$
    inc hl
    ld a, (hl)
    cp #0x62
    jr nz, 00010$
    ld hl, #_ib_pcm_total
    ld a, (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    jr z, 00010$
    ld c, #2
00010$:
    ld a, c
    ld (_ib_kind), a
    ld hl, (_ib_k)
    ld a, h
    or l
    jr nz, 00020$
    ld hl, #_ib_entry + 4
    ld b, #8
00011$:
    or (hl)
    inc hl
    djnz 00011$
    jr nz, 00020$
    push ix
    pop de
    ld hl, #4
    add hl, de
    push hl
    ld de, #_ib_base
    ld bc, #4
    ldir
    pop de
    ld hl, #_ib_ol
    call 00080$
    ld hl, #_ib_base
    jr c, 00012$
    xor a
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    jr 00020$
00012$:
    ld a, (hl)
    sub #4
    ld (hl), a
    ld b, #3
00013$:
    inc hl
    ld a, (hl)
    sbc a, #0
    ld (hl), a
    djnz 00013$
00020$:
    ld hl, (_ib_k)
    ld de, (_ib_count)
    or a
    sbc hl, de
    jp nc, 00040$
    ld a, (_ib_kind)
    dec a
    jr nz, 00022$
    ld hl, #_ib_entry + 4
    ld de, #_ib_frame
    call 00080$
    jp c, 00040$
    jr 00030$
00022$:
    dec a
    jp nz, 00040$
    ld hl, #_ib_entry + 8
    ld de, #_ib_tmp
    ld bc, #4
    ldir
    ld hl, #_ib_ol + 4
    ld de, #_ib_tmp
    call 00090$
    ld hl, #_ib_sample
    ld de, #_ib_tmp
    call 00080$
    jp nc, 00040$
00030$:
    ld hl, #_ib_base
    ld de, #_ib_entry
    ld bc, #4
    ldir
    ld hl, #_ib_ol
    ld de, #_ib_entry
    call 00090$
    ld hl, #12
    push hl
    ld hl, #0x000D
    push hl
    ld hl, (_ib_k)
    add hl, hl
    add hl, hl
    ld d, h
    ld e, l
    add hl, hl
    add hl, de
    push hl
    ld hl, #_ib_entry
    push hl
    call INDEX_WRITE
    pop af
    pop af
    pop af
    pop af
    ld hl, (_ib_k)
    inc hl
    ld (_ib_k), hl
    ld hl, (_ib_frame)
    ld de, (_ib_step)
    add hl, de
    ld (_ib_frame), hl
    jr nc, 00031$
    ld hl, (_ib_frame + 2)
    inc hl
    ld (_ib_frame + 2), hl
00031$:
    ld hl, #_ib_sample_step
    ld de, #_ib_sample
    call 00090$
    ld hl, (_ib_rem)
    ld de, (_ib_sample_rem)
    add hl, de
    ld de, (_ib_fps)
    or a
    sbc hl, de
    jr nc, 00032$
    add hl, de
    ld (_ib_rem), hl
    jp 00020$
00032$:
    ld (_ib_rem), hl
    ld hl, #_ib_sample
    call 00075$
    jp 00020$
00040$:
    ld a, (_ib_kind)
    dec a
    jr nz, 00041$
    ld hl, #_ib_entry + 4
    call 00075$
    jp 00001$
00041$:
    dec a
    jp nz, 00001$
    ld hl, #_ib_ol + 4
    ld de, #_ib_entry + 8
    call 00090$
    jp 00001$
00099$:
    pop ix
    ret
00070$:
    ld b, #4
    scf
00071$:
    ld a, (hl)
    sbc a, #0
    ld (hl), a
    inc hl
    ret nc
    djnz 00071$
    ret
00075$:
    ld b, #4
00076$:
    inc (hl)
    ret nz
    inc hl
    djnz 00076$
    ret
00080$:
    ld b, #4
    or a
00081$:
    ld a, (de)
    ld c, a
    ld a, (hl)
    sbc a, c
    inc hl
    inc de
    djnz 00081$
    ret
00090$:
    ld b, #4
    or a
00091$:
    ld a, (de)
    adc a, (hl)
    ld (de), a
    inc hl
    inc de
    djnz 00091$
    ret
  __endasm;
}
