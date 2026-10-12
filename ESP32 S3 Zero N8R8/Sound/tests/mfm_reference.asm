; Эталон для tests/test_mfm.py: оригинальный Z80-проигрыватель MFM из
; ZXM-MoonSound (mfm_player.asm, порт Mick 2015 проигрывателя Moonsoft) как
; есть, плюс точки входа для стенда (mfm_reference.py). Собирается sjasmplus с
; --inc=<папка mfm_sample_02>: там mfm_player.asm и его таблицы.
		DEVICE	NOSLOT64K
		ORG	6200h
ref_init:	jp	MBPlayer_init		; 6200
ref_play:	jp	MBPlayer_play		; 6203
ref_stop:	jp	MBPlayer_stop		; 6206
		INCLUDE	"mfm_player.asm"
ref_end:
		SAVEBIN	"mfm_reference.bin", 6200h, ref_end - 6200h
