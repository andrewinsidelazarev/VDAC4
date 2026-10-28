
	.module crt0
	.globl	_main
	.globl	_main_dispatch
	.globl	_far_entry
	.globl	_wc_video_bank
	.globl	_core_try
	.globl	_far_call
	.globl	_far_call_img
	.globl	_far_call_core
	.globl	_ret_sp

	.area	_HEADER (ABS)
	.org 	0x8000
	jp _main
	; Вход для вызова из видеобанка (far_call). Адрес одинаков в обоих
	; банках; в видеобанке far_entry — пустая функция, её никто не вызывает.
	jp _far_entry

; Во всех банках эти короткие шлюзы находятся по одним адресам. API 79
; переключает #8000, а следующая инструкция уже берётся из нового банка.
; Адреса main_dispatch и far_entry у каждого банка свои; общий входной кадр
; лежит в #BFE0..#BFED. Начало #BF68: DATA видеобанка кончается на #BF63.
	.area _BANKSWITCH (ABS)
	.org 0xBF68
; Видеобанк — без возврата: стек — входной WC.
_wc_video_bank:
	ld sp, (_ret_sp)
	ld a, #9
	call bank_frame
	jp _main_dispatch

; Банк ядра видео VDAC4 (страница 11) с возвратом в банк изображений: его
; far_entry играет ролик через ядро видео и выходит в WC сам; вернулся —
; ядра видео нет (ESP, VIDEO.ELF), и банк изображений идёт прежним путём
; (FT812). Кадр переносится туда и обратно.
_core_try:
	push ix
	push iy
	ld a, #11
	call bank_frame
	call 0x8003
	xor a
	call bank_frame
	pop iy
	pop ix
	ret

; Переход в банк A с общим кадром: логические адреса кадра совпадают,
; физические страницы различаются — 14 байтов идут через стек ядра, который
; API 79 не переключает.
bank_frame:
	ld hl, #0xBFE0
	ld b, #7
00001$:
	ld e, (hl)
	inc hl
	ld d, (hl)
	inc hl
	push de
	djnz 00001$
	ex af, af'
	ld a, #79		; API 79 MNG8_PL (десятичный номер)
	call 0x6006
	ld hl, #0xBFED
	ld b, #7
00002$:
	pop de
	ld (hl), d
	dec hl
	ld (hl), e
	dec hl
	djnz 00002$
	ret

; Видеобанк вызывает far_entry другого банка: A — его страница (0 — банк
; изображений, 10 — банк списка роликов), DE — адрес блока параметров в окне
; #C000 (оно общее для банков). Возврат — только через блок: A и DE портит
; обратное переключение. IX/IY сохраняются. Банк изображений зовёт банк списка
; через far_call_img, банк ядра видео — far_call_core: то же, но назад — в
; страницу 0 или 11, а не 9. Страница возврата лежит на стеке ядра: его API 79
; не переключает.
_far_call_core:
	ld c, #11
	jr far_go
_far_call_img:
	ld c, #0
	jr far_go
_far_call:
	ld c, #9
far_go:
	push ix
	push iy
	push bc
	push de
	ex af, af'
	ld a, #79		; API 79 MNG8_PL (десятичный номер)
	call 0x6006
	pop hl
	push hl
	call 0x8003
	pop hl
	pop bc
	ld a, c
	ex af, af'
	ld a, #79		; API 79 MNG8_PL (десятичный номер)
	call 0x6006
	pop iy
	pop ix
	ret

	;; Ordering of segments for the linker.
	.area	_HOME
	.area	_CODE
	.area	_INITIALIZER
	.area   _GSINIT
	.area   _GSFINAL

	.area	_DATA
	.area	_INITIALIZED
	.area	_BSEG
	.area   _BSS
	.area   _HEAP

	.area   _GSINIT
	.area   _GSFINAL

