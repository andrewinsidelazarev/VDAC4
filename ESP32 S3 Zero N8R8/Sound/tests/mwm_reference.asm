; Эталон для tests/test_mwm.py: оригинальный Z80-проигрыватель MWM из
; ZXM-MoonSound (mwm_player.asm, порт Mick 2015 проигрывателя Moonsoft) как
; есть, плюс точки входа для стенда (mwm_reference.py). Собирается sjasmplus с
; --inc=<папка moonmusic_01>: там mwm_player.asm и его таблицы.
		DEVICE	NOSLOT64K
		ORG	6200h
ref_init:	jp	MBPlayer_init		; 6200
ref_play:	jp	MBPlayer_play		; 6203
ref_stop:	jp	MBPlayer_stop		; 6206
MoonSound_flg_next:				; ставит проигрыватель в конце песни
		db	0
		INCLUDE	"mwm_player.asm"
ref_end:
		SAVEBIN	"mwm_reference.bin", 6200h, ref_end - 6200h
