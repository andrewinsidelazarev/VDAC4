; Номер файла и число файлов для «Track N of M» (Ui_DrawTrack). Код — во второй
; странице плагина (окно #C000, вход Track_Count в esp_data.asm).
;
; WC даёт номер объекта панели и число объектов (HL', DE' — Mp3Index, Mp3Count), а
; в них и «..», и каталоги, и файлы не для плеера (обложка, набор волн MWK): в
; папке из шести MWM песня шла как «Track 2 of 7». Панель при любой сортировке
; ставит «..» и каталоги первыми — они вычитаются всегда; при сортировке по
; расширению (CSORT=2, так ставит WC Setup) вычитаются и чужие файлы: у них
; другое расширение, и место в панели решает только оно (ключ +1..+3 элемента
; панели — расширение имени 8.3). Без сортировки каталоги идут вперемешку с
; файлами — вычитается только «..» (он первый). Сами Mp3Index и Mp3Count не
; меняются: по ним Mp3_SetExit решает, есть ли следующий файл.
;
; Список берётся обходом каталога панели через API 58 (короткие имена: их
; расширение и есть ключ сортировки) в клоне потока панели (STREAM #FE), до
; Esp_Prepare: свой файл она находит потом заново, а обход потока панели не трогает.

TRACK_SORT_EXT  equ 2                   ; IX+#23 панели: 0 нет, 1 имя, 2 расширение, 3 размер, 4 дата

; Вход: (TrackSort) — сортировка панели, (Mp3Ext) — расширение своего файла.
; Выход: (TrackNo), (TrackCount); посчитать не вышло — номера WC как есть.
Track_Count:
        ld hl,(Mp3Index)
        ld (TrackNo),hl
        ld hl,(Mp3Count)
        ld (TrackCount),hl
        ld a,h
        or l
        ret z                            ; вызван не на файле панели
        ld hl,0
        ld (TrackDirs),hl
        ld (TrackOther),hl
        ld (TrackBefore),hl
        xor a
        ld (TrackParent),a
        ld d,STREAM_CLONE
        call WC_STREAM
        ld a,1                           ; ADIR 1: обход каталога потока с начала (как у FTP-плагина;
                                         ; ADIR 0 открывает каталог по сохранённому пути)
        ex af,af'
        ld a,FN_ADIR
        call Wc_Api
.next:
        ld a,%10000000                   ; короткие имена, любая запись, без полей
        ex af,af'
        ld de,Mp3Block                   ; [атрибут][имя 8.3][0]
        ld a,FN_FINDNEXT
        call Wc_Api
        jr z,.done
        ld hl,Mp3Block
        ld a,(hl)
        inc hl
        and #10
        jr z,.file
        ; Каталог. «.» панель не показывает, «..» — первым.
        ld a,(hl)
        cp '.'
        jr nz,.dir
        inc hl
        ld a,(hl)
        or a
        jr z,.next                       ; «.»
        cp '.'
        jr nz,.dir
        ld (TrackParent),a               ; «..»
.dir:
        ld hl,(TrackDirs)
        inc hl
        ld (TrackDirs),hl
        jr .next
.file:
        call Track_Ext
        call Track_Playable
        jr z,.next                       ; файл плеера — в счёте WC он и так есть
        ld hl,(TrackOther)
        inc hl
        ld (TrackOther),hl
        ; Раньше своего файла при сортировке по расширению — меньшее расширение
        ; (равного не бывает: то же расширение — файл плеера).
        ld de,Mp3Ext
        ld hl,TrackExt
        ld b,3
.compare:
        ld a,(de)
        cp (hl)
        jr nz,.decided
        inc de
        inc hl
        djnz .compare
        jr .next
.decided:
        jr c,.next                       ; своё меньше: чужой — после
        ld hl,(TrackBefore)
        inc hl
        ld (TrackBefore),hl
        jr .next
.done:
        ld a,1                           ; ADIR 1: FindNext — заново
        ex af,af'
        ld a,FN_ADIR
        call Wc_Api
        ; Что вычесть: из номера — BC, из числа — DE.
        ld a,(TrackSort)
        or a
        jr nz,.sorted
        ld a,(TrackParent)               ; без сортировки: только «..»
        or a
        ret z
        ld bc,1
        ld d,b
        ld e,c
        jr .apply
.sorted:
        ld bc,(TrackDirs)
        ld d,b
        ld e,c
        cp TRACK_SORT_EXT
        jr nz,.apply                     ; другая сортировка: только каталоги
        ld hl,(TrackBefore)
        add hl,bc
        ld b,h
        ld c,l
        ld hl,(TrackOther)
        add hl,de
        ex de,hl
.apply:
        ; Номер и число — только если сошлось: 1 <= номер <= число.
        ld hl,(Mp3Count)
        or a
        sbc hl,de
        ret c
        ret z
        push hl                          ; число
        ld hl,(Mp3Index)
        or a
        sbc hl,bc
        pop de
        ret c
        ret z                            ; номер 0
        ex de,hl                         ; HL — число, DE — номер
        push hl
        or a
        sbc hl,de
        pop hl
        ret c                            ; номер больше числа
        ld (TrackCount),hl
        ld (TrackNo),de
        ret

; Расширение имени 8.3 HL (после последней точки, до трёх знаков, добито
; пробелами, как поле записи FAT) — в TrackExt заглавными: API 58 отдаёт
; короткое имя строчными (Acs ядра, как его показывает панель), а ключ
; сортировки и Mp3Ext — заглавные. Портит A, B, DE, HL.
Track_Ext:
        ld de,0                          ; за последней точкой
.scan:
        ld a,(hl)
        or a
        jr z,.end
        inc hl
        cp '.'
        jr nz,.scan
        ld d,h
        ld e,l
        jr .scan
.end:
        ld hl,TrackExt
        ld b,3
.blank:
        ld (hl),' '
        inc hl
        djnz .blank
        ld a,d
        or e
        ret z
        ld hl,TrackExt
        ld b,3
.copy:
        ld a,(de)
        or a
        ret z
        cp 'a'
        jr c,.upper
        cp 'z'+1
        jr nc,.upper
        sub 'a'-'A'
.upper:
        ld (hl),a
        inc hl
        inc de
        djnz .copy
        ret

; Файл плеера ли: TrackExt среди расширений, на которые WC зовёт плагин
; (WC_EXTENSION_LIST в wc_header.inc — держать в согласии; здесь — короткие
; имена 8.3: FLAC — «FLA», MIDI — «MID»), кроме набора волн MWK — его плагин
; пропускает.
; Выход: Z — да. Портит A, B, DE, HL.
Track_Playable:
        ld hl,TrackKinds
.kind:
        ld a,(hl)
        or a
        jr z,.other
        ld de,TrackExt
        ld b,3
.compare:
        ld a,(de)
        cp (hl)
        jr nz,.skip
        inc de
        inc hl
        djnz .compare
        xor a                            ; Z — совпало
        ret
.skip:
        inc hl
        djnz .skip
        jr .kind
.other:
        or 1                             ; NZ
        ret

TrackKinds:     db "MP3","WAV","MOD","XM ","S3M","IT ","MID","RMI","KAR","VGM","VGZ"
                db "DRO","IMF","WLF","MWM","MFM","MO ","M3U","AAC","OGG","OGA","FLA","SAA","COP"
                db "AS0","ASC","FTC","GTR","PSC","PSM","PT1","PT2","PT3","SQT","ST3","STC","STP","TS ","TFC"
                db "TFD","MTC",0
TrackExt:       ds 3
TrackDirs:      dw 0                    ; каталогов в панели, с «..»
TrackOther:     dw 0                    ; файлов не для плеера
TrackBefore:    dw 0                    ; из них раньше своего (сортировка по расширению)
TrackParent:    db 0                    ; есть «..»
