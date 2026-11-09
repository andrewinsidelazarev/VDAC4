; Заголовок .WMF для Wild Commander, формат #0A.
; Плагин запускается из меню плагинов (F10). Страница 0 — код (#8000); с 0.20 страница 1 — таблицы
; CRC16 и буфер порции DMA (окно #C000, Page1_Map). Заголовок занимает 512 байтов.

        ds 16
        db "WildCommanderMDL"
        db #0A
        db 0
        db 2                          ; всего страниц: код и (0.20) таблицы с буфером порции
        db 0                          ; страница 0 подключается по адресу #8000

        ; Пары [страница, число 512-байтных секторов с округлением вверх]; пара с нулём
        ; секторов кончает список.
        db 0,(mainEnd-mainStart+511)/512
        db 1,(page1End-page1Start+511)/512
        db 0,0
        db 0,0
        db 0,0
        db 0,0

        ds 16
        db "NES"
        db "ADF"
        db "MD "                      ; 0.17: Mega Drive (.bin — только из меню F1 ядра)
        db "GEN"
        db "SMD"
        ds 27*3
        db 0
        dd #FFFFFFFF                  ; файл читает плагин, WC не грузит его в RAM

.plugin_name
        db "VDAC4 Cores v0.21"
        ds 32-($-.plugin_name),' '
        db #05                        ; меню F10, просмотрщик и расширения .NES, .ADF, .MD, .GEN, .SMD
        ds 6
        db "Play NES / ADF / Mega Drive ROM"
        ds 32
