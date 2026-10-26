; Wild Commander Improved v1.11i — boot.$C «WC110B», 30602 байта,
; SHA-256 de18ad1c93a9063e1b28b6a597f019dcbfa4f67d14cf8b7c9976293ad473e317.
; Создано tools/make_wc_tables.py из самого boot.$C: не править руками.

WC_TRAMP        equ #4FE8          ; 16 нулей перед DOS_SWP — переходник к резиденту
WC_DOS_SWP      equ #4FF8
WC_SD2_LDHL     equ #501C          ; LD HL,DR2 ветки A=6 (второй SD ZC)
WC_SD2_TAIL     equ #5028          ; CALL SELDEV_INVALIDATE / NOP / POP IX / RET
WC_SWP_LEN      equ 71
WC_MNG8         equ #68B1
WC_MNGC         equ #68C1
WC_DRACT        equ #6868
WC_ORIGA        equ #63ED
WC_XPAN         equ #6149
WC_STATUS       equ #8723          ; страница #02 в окне #8000
WC_WINDOW       equ #7EA0          ; описатели панелей
WC_WINDO2       equ #7EC4
WC_FREE02       equ #AB4F          ; одноразовые данные запуска в странице #02, после запуска свободны
WC_FREE02_END   equ #B000
WC_FREE02_SUM   equ #4B04          ; их сумма байтов в boot.$C
