; Окно проигрывателя для Wild Commander (макет согласован 2026-10-04):
; 66×21 в середине экрана, чёрный фон.
;
;   слева — обложка (спрайт 64×64, cover.asm); справа название, исполнитель,
;   альбом и год, формат, номер файла; ниже время с полосой (у модуля и MIDI,
;   пока файл принимается, — полоса загрузки), строка состояния, столбики —
;   спектр у MP3 и MIDI или уровни каналов у модуля (AUDIO_VIEW), громкость и
;   «H Help». У модуля и MIDI обложки нет: текст — от левого края.
;
; Строки собираются по одной в UiLine — текст с кодами цвета TXTPR (WCIF.ASM:
; одиночный код c — чернила c&7, два одинаковых — яркие c|8) — и печатаются
; TXTPR каждая со своего места. Справка (H) и подробности (I) — отдельные окна
; поверх; пока одно из них открыто, основное окно не перерисовывается, а
; спрайт обложки выключен.

UI_WIDTH        equ 66
UI_HEIGHT       equ 21
UI_INNER        equ UI_WIDTH-2          ; знаков в строке окна
UI_COL_MP3      equ 20                  ; столбец текста у MP3: слева место обложки
UI_COL_MODULE   equ 2
UI_TITLE        equ 32                  ; название модуля (AUDIO_INFO), без нуля
UI_ROW_TITLE    equ 2
UI_ROW_SECOND   equ 3
UI_ROW_THIRD    equ 4
UI_ROW_FORMAT   equ 6
UI_ROW_TRACK    equ 7
UI_ROW_TIME     equ 9
UI_ROW_STATUS   equ 10
UI_ROW_BARS     equ 11
UI_BAR_ROWS     equ 6
UI_ROW_NUMBERS  equ UI_ROW_BARS+UI_BAR_ROWS
UI_ROW_VOLUME   equ 19
UI_BARS_WIDTH   equ UI_INNER-1          ; столбики: столбцы 2..64
UI_VOL_CELLS    equ 16
UI_HELP_COL     equ 57                  ; «H Help» в правом нижнем углу
UI_TEMP_YELLOW  equ 70                  ; «ESP NN'C» жёлтым — кристалл ESP от 70 °C
UI_TEMP_RED     equ 75                  ; красным — от 75 °C
UI_BAR_MP3      equ 32                  ; ячеек полосы времени (эскиз)
UI_BAR_MODULE   equ 47
UI_VIEW_MAX     equ 32
UI_VIEW_EVERY   equ 4                   ; кадров между опросами спектра
UI_PEAK_HOLD    equ 8                   ; опросов, сколько пик стоит
UI_TAGS_SIZE    equ 200

VIEW_SPECTRUM   equ 1
VIEW_CHANNELS   equ 2
VIEW_LIST       equ 3                   ; радио: список станций — готовый текст TXTPR от ESP

; Коды цвета TXTPR.
C_GREY          equ 7                   ; одиночный: белый (7)
C_CYAN          equ 5                   ; одиночный: голубой (5)
C_BLUE          equ 1                   ; одиночный: синий (1)
C_BLACK         equ 8                   ; одиночный: чёрный (0) — в окнах со светлым фоном
C_WHITE2        equ 7                   ; парой: ярко-белый (15)
C_YELLOW2       equ 6                   ; парой: жёлтый (14)
C_CYAN2         equ 5                   ; парой: ярко-голубой (13)
C_GREEN2        equ 4                   ; парой: ярко-зелёный (12)
C_RED2          equ 2                   ; парой: ярко-красный (10)

CH_FULL         equ #DB                 ; █
CH_LOWER        equ #DC                 ; ▄
CH_UPPER        equ #DF                 ; ▀
CH_SHADE        equ #B0                 ; ░
CH_PLAYED       equ #CD                 ; ═
CH_LEFT         equ #C4                 ; ─

DIALOG_NONE     equ 0
DIALOG_HELP     equ 1
DIALOG_INFO     equ 2

; Создать окно. Фон под ним WC сохраняет сам (PRWOW), RRESB возвращает.
Ui_Open:
        ; X/Y=#FF PRWOW заменяет серединой текущего текстового режима WC и
        ; записывает вычисленное в дескриптор, поэтому #FF и нулевой адрес
        ; буфера фона ставятся перед каждым показом: страница плагина живёт
        ; между запусками.
        ld ix,Mp3Window
        ld (ix+2),#FF
        ld (ix+3),#FF
        ld (ix+8),0
        ld (ix+9),0
        call WC_PRWOW
        ld hl,UiTags
        ld de,UiTags+1
        ld bc,UiClearEnd-UiTags-1
        ld (hl),0
        ldir
        ld hl,UiStateNoFile
        ld (UiState),hl
        ld hl,0
        ld (UiHint),hl
        ; У модуля и MIDI альбома нет: строки от формата и ниже — на одну выше
        ; (у MP3 и OPL — метки названия, автора и альбома).
        ld a,(Mp3Kind)
        dec a
        cp KIND_MIDI
        ld a,0
        jr nc,.shift
        inc a
.shift:
        ld (UiShift),a
        call Esp_LoadDone                ; полоса загрузки — файла плеера
        ; дальше — Ui_Draw

; Перерисовать окно целиком.
Ui_Draw:
        ld a,(UiDialog)
        or a
        ret nz
        call Ui_DrawTitle
        call Ui_DrawSecond
        call Ui_DrawThird
        call Ui_DrawFormat
        call Ui_DrawTrack
        call Ui_DrawTime
        call Ui_DrawStatus
        call Ui_DrawBars
        jp Ui_DrawVolume

; HL — имя файла (с нулём).
Ui_SetName:
        ld de,UiName
        ld b,UI_NAME
.copy:
        ld a,(hl)
        ld (de),a
        or a
        ret z
        inc hl
        inc de
        djnz .copy
        xor a
        ld (de),a
        ret

; HL — строка состояния; строка «ERROR…» печатается красным.
Ui_SetState:
        ld (UiState),hl
        ret

; HL — подсказка (в окне не показывается; нужна подробностям и тестам).
Ui_SetHint:
        ld (UiHint),hl
        ret

; --- сборка строк ------------------------------------------------------------

; Начать строку: A — код цвета, B — 1 (одиночный) или 2 (яркий, парой).
Ui_Begin:
        ld de,UiLine
; Дописать код цвета A B раз.
Ui_Color:
        ld (de),a
        inc de
        djnz Ui_Color
        ret

; Строку HL (с нулём) — по DE; DE — на её нуле (следующая допишется).
Ui_CopyText:
        ld a,(hl)
        ld (de),a
        or a
        ret z
        inc hl
        inc de
        jr Ui_CopyText

; Дописать строку HL (с нулём); управляющие байты — точками.
Ui_Text:
        ld a,(hl)
        or a
        ret z
        cp 32
        jr nc,.put
        ld a,'.'
.put:
        ld (de),a
        inc de
        inc hl
        jr Ui_Text

; Подогнать кусок, начатый с HL, под ширину B: дополнить пробелами или обрезать.
; Выход: DE = HL + B. Сохраняет HL.
Ui_Fit:
        push hl
        ld a,l
        add a,b
        ld l,a
        jr nc,.target
        inc h
.target:
        push hl
        or a
        sbc hl,de
        pop hl
        jr c,.cut
.pad:
        ld a,e
        cp l
        jr nz,.space
        ld a,d
        cp h
        jr z,.done
.space:
        ld a,' '
        ld (de),a
        inc de
        jr .pad
.cut:
        ex de,hl
.done:
        pop hl
        ret

; Напечатать собранную строку в строке окна A (у модуля строки от формата и
; ниже — на UiShift выше), столбец C.
Ui_PrintAt:
        cp UI_ROW_FORMAT
        jr c,.row
        ld hl,UiShift
        sub (hl)
.row:
        ld b,a
; Напечатать собранную строку с места B (строка окна), C (столбец).
Ui_Print:
        xor a
        ld (de),a
        ld d,b
        ld e,c
        ld hl,UiLine
        ld ix,Mp3Window
        jp WC_TXTPR

; Столбец текста C и его ширина B: у MP3 слева место обложки.
Ui_TextPlace:
        ld bc,(UI_INNER-UI_COL_MP3+1)*256+UI_COL_MP3
        ld a,(Mp3Kind)
        or a
        jr nz,.module                    ; модуль и MIDI: обложки нет
        ld a,(Mp3Call)
        cp CALL_MENU
        ret nz
.module:
        ld bc,(UI_INNER-UI_COL_MODULE+1)*256+UI_COL_MODULE
        ret

; Кусок текста одного цвета на всю ширину текста: A, B — цвет, HL — строка,
; E — строка окна.
Ui_TextRow:
        ld c,e
        push bc                          ; C — строка окна
        push hl
        call Ui_Begin
        pop hl
        push de
        call Ui_Text
        call Ui_TextPlace
        pop hl
        call Ui_Fit
        pop hl                           ; L — строка окна
        ld a,l
        jp Ui_PrintAt

; Метка n (0 — название, 1 — исполнитель, 2 — альбом, 3 — год) из UiTags.
; Вход: B = n. Выход: HL.
Ui_Tag:
        ld hl,UiTags
; Строка n = B из ряда строк с нулями от HL. Выход: HL.
Ui_Nth:
        inc b
.next:
        dec b
        ret z
.skip:
        ld a,(hl)
        inc hl
        or a
        jr nz,.skip
        jr .next

; --- строки окна ------------------------------------------------------------

; Название: метка, название модуля из AUDIO_INFO или имя файла.
Ui_DrawTitle:
        ld b,0
        call Ui_Tag
        ld a,(hl)
        or a
        jr nz,.have
        ld hl,UiSongTitle
        ld a,(hl)
        or a
        jr nz,.have
        ld hl,UiName
.have:
        ld e,UI_ROW_TITLE
        ld a,C_WHITE2
        ld b,2
        jp Ui_TextRow

; У MP3 — исполнитель (жёлтым), у модуля и MIDI — имя файла (если в первой
; строке название, а не оно же); у OPL — автор, у радио — песня или состояние
; станции, а без них — как у модуля.
Ui_DrawSecond:
        ld b,1
        call Ui_Tag
        ld a,(Mp3Kind)
        or a
        jr z,.artist
        cp KIND_OPL
        jr c,.module                     ; OPL и радио — как MP3
        ld a,(hl)
        or a
        jr z,.module
.artist:
        ld e,UI_ROW_SECOND
        ld a,C_YELLOW2
        ld b,2
        jp Ui_TextRow
.module:
        ld b,0
        call Ui_Tag
        ld a,(UiSongTitle)
        or (hl)
        ld hl,UiEmpty
        jr z,.named
        ld hl,UiName
.named:
        ld e,UI_ROW_SECOND
        ld a,C_GREY
        ld b,1
        jp Ui_TextRow

; У MP3 — альбом и через запятую год; у модуля — «Samples: 31  Effects: 12»,
; у MIDI — «Instruments: 14  Tracks: 9» (AUDIO_TAGS за метками); без файла —
; как вызвать плеер.
Ui_DrawThird:
        ld a,C_GREY
        ld b,1
        call Ui_Begin
        push de
        ld a,(Mp3Call)
        cp CALL_MENU
        jr nz,.tags
        ld hl,UiHintNoFile
        call Ui_Text
        jr .fit
.tags:
        ld a,(Mp3Kind)
        cp KIND_OPL
        jr nc,.album                     ; у OPL — игра и год, у радио — «Station 3 of 11»
        or a
        jr nz,.module
.album:
        ld b,2
        call Ui_Tag
        call Ui_Text
        ld b,3
        call Ui_Tag
        ld a,(hl)
        or a
        jr z,.fit
        ex (sp),hl                       ; HL — начало куска, на стеке — год
        push hl
        or a
        sbc hl,de                        ; ноль — альбома нет
        pop hl
        ex (sp),hl
        jr z,.year
        ld a,','
        ld (de),a
        inc de
        ld a,' '
        ld (de),a
        inc de
.year:
        call Ui_Text
.fit:
        call Ui_TextPlace
        pop hl
        call Ui_Fit
        ld b,UI_ROW_THIRD
        jp Ui_Print
.module:
        ld hl,UiTextSamples
        ld a,(Mp3Kind)
        cp KIND_MIDI
        jr nz,.labels
        ld hl,UiTextInstruments          ; MIDI: инструменты и дорожки
.labels:
        ld (UiLabels),hl
        ld b,5
        call Ui_Tag                      ; HL — [сэмплов LE16][эффектов]
        push hl
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a
        ld a,h
        or l
        jr z,.effects
        push hl
        ld hl,(UiLabels)
        call Ui_Text
        pop hl
        call Dec16
.effects:
        pop hl
        inc hl
        inc hl
        ld a,(hl)
        or a
        jr z,.fit
        ld l,a
        ld h,0
        push hl
        ld hl,(UiLabels)
        ld b,1
        call Ui_Nth                      ; вторая подпись — за первой
        call Ui_Text
        pop hl
        call Dec16
        jr .fit

; Формат: «MP3  128 kbps  44.1 kHz  stereo», «XM  8 channels  33 positions» или
; «MIDI  10 channels».
Ui_DrawFormat:
        ld a,C_CYAN
        ld b,1
        call Ui_Begin
        push de
        ld a,(Mp3Kind)
        or a
        jr nz,.module
        ld hl,Mp3Ext                     ; .FLAC в имени 8.3 — .FLA
        ld a,(hl)
        cp 'F'
        jr nz,.stream_name
        ld hl,UiFmtFlac
.stream_name:
        call Ui_Text
        ld a,(Mp3Status+ST_FLAGS)
        and 2
        jp z,.fit                        ; формат ещё не распознан
        ld hl,UiGap
        call Ui_Text
        ld hl,(Mp3Status+ST_BITRATE)
        call Dec16
        ld hl,UiTextKbps
        call Ui_Text
        ; Частота: кГц с одним знаком после точки, если он не ноль.
        call Ui_RateSplit                ; A — тысячи, HL — остаток меньше 1000
        push hl
        ld l,a
        ld h,0
        call Dec16
        pop hl
        ld bc,-100
        ld a,-1
.hundreds:
        inc a
        add hl,bc
        jr c,.hundreds
        or a
        jr z,.khz
        add a,'0'
        ld c,a
        ld a,'.'
        ld (de),a
        inc de
        ld a,c
        ld (de),a
        inc de
.khz:
        ld hl,UiTextKhz
        call Ui_Text
        ld hl,UiTextMono
        ld a,(Mp3Status+ST_CHANNELS)
        cp 2
        jr c,.channels
        ld hl,UiTextStereo
.channels:
        call Ui_Text
        jr .fit
.module:
        ld a,(Mp3Kind)
        cp KIND_OPL
        jr c,.not_opl
        ; OPL: «VGZ  YMF262 (OPL3)» — микросхемы за метками AUDIO_TAGS; радио —
        ; «M3U  MP3  128 kbps  44.1 kHz  stereo» (формат потока там же).
        ld hl,Mp3Ext
        call Ui_Text
        ld a,(Mp3Status+ST_FLAGS)
        and 2
        jr z,.fit
        ld hl,UiGap
        call Ui_Text
        ld b,5
        call Ui_Tag
        call Ui_Text
        jr .fit
.not_opl:
        ld hl,UiFmtModule
        cp KIND_MIDI
        jr nz,.fallback
        ld hl,UiFmtMidi
.fallback:
        ld a,(Mp3Status+ST_FLAGS)
        and 2
        jr z,.named
        ld a,(Mp3Status+ST_FORMAT)
        sub FORMAT_MOD
        cp FORMAT_MIDI-FORMAT_MOD+1
        jr nc,.named                     ; формат неизвестен плагину
        add a,a
        add a,low UiFmtNames
        ld l,a
        ld a,0
        adc a,high UiFmtNames
        ld h,a                           ; HL — элемент таблицы (DE — строка окна, не трогать)
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a
.named:
        call Ui_Text
        ld a,(Mp3Status+ST_FLAGS)
        and 2
        jr z,.fit
        ld hl,UiGap
        call Ui_Text
        ld a,(Mp3Status+ST_MOD_CHANNELS)
        ld l,a
        ld h,0
        call Dec16
        ld hl,UiTextChannels
        call Ui_Text
        ld a,(Mp3Kind)
        cp KIND_MIDI
        jr z,.fit                        ; у MIDI позиций нет
        ld hl,(Mp3Status+ST_LENGTH)
        call Dec16
        ld hl,UiTextPositions
        call Ui_Text
.fit:
        call Ui_TextPlace
        pop hl
        call Ui_Fit
        ld a,UI_ROW_FORMAT
        jp Ui_PrintAt

; «Track 3 of 9» у MP3 и MIDI, «Position 12 of 33   Row 40» у модуля.
Ui_DrawTrack:
        ld a,C_GREY
        ld b,1
        call Ui_Begin
        push de
        ld a,(Mp3Kind)
        cp KIND_MODULE
        jr z,.module
        ld hl,(Mp3Count)
        ld a,h
        or l
        jr z,.fit                        ; вызван не на файле панели
        ld hl,UiTextFile
        call Ui_Text
        ld hl,(TrackNo)                  ; без «..», каталогов и чужих файлов
        call Dec16
        ld hl,UiTextOf
        call Ui_Text
        ld hl,(TrackCount)
        call Dec16
        jr .fit
.module:
        ld a,(Mp3Status+ST_FLAGS)
        and 2
        jr z,.fit
        ld hl,UiTextPosition
        call Ui_Text
        ld hl,(Mp3Status+ST_POSITION)
        inc hl
        call Dec16
        ld hl,UiTextOf
        call Ui_Text
        ld hl,(Mp3Status+ST_LENGTH)
        call Dec16
        ld hl,UiTextRow
        call Ui_Text
        ld a,(Mp3Status+ST_ROW)
        ld l,a
        ld h,0
        call Dec16
.fit:
        call Ui_TextPlace
        pop hl
        call Ui_Fit
        ld a,UI_ROW_TRACK
        jp Ui_PrintAt

; Время: «02:17 ═══════───── 05:34». Пока модуль принимается или ESP его
; разбирает — «Loading ═════───── 45%».
Ui_DrawTime:
        ld a,(UiShift)
        or a
        ld a,UI_BAR_MP3
        jr z,.width
        ld a,UI_BAR_MODULE
.width:
        ld (UiBarWidth),a
        ld a,(UiLoading)
        or a
        jp nz,Ui_DrawLoad
        ld a,C_WHITE2
        ld b,2
        call Ui_Begin
        ; Сыграно: из AUDIO_VIEW, а у старой прошивки — из записи состояния.
        ld hl,(UiPosition)
        ld a,(UiViewOff)
        or a
        jr z,.elapsed
        ld hl,(Mp3Status+ST_SECONDS)
.elapsed:
        push hl
        call Ui_Time
        ld a,' '
        ld (de),a
        inc de
        ; Полоса: доля сыгранного от длительности; длительность неизвестна —
        ; у MP3 доля от отданного в декодер, у модуля — позиция из числа позиций.
        pop hl
        ld bc,(UiDuration)
        ld a,b
        or c
        jr nz,.bar
        ld a,(Mp3Kind)
        or a
        jr nz,.positions                 ; MIDI до первого AUDIO_VIEW — пусто
        ld hl,Mp3Total
        call Ui_Kb16
        ld b,h
        ld c,l
        push bc
        ld hl,Mp3Status+ST_BUFFERED
        call Ui_Kb16
        push hl
        ld hl,Mp3Sent
        call Ui_Kb16
        pop bc
        or a
        sbc hl,bc
        jr nc,.sent
        ld hl,0
.sent:
        pop bc
        jr .bar
.positions:
        ld hl,(Mp3Status+ST_POSITION)
        ld bc,(Mp3Status+ST_LENGTH)
.bar:
        ld (UiBarTotal),bc
        ex de,hl                         ; DE — сыграно, HL — конец строки
        push hl
        ld a,(UiBarWidth)
        ld b,a
        call Ui_Cells                    ; A — закрашено
        pop de
        call Ui_BarCells
        ; Длительность или «--:--».
        ld a,' '
        ld (de),a
        inc de
        ld a,C_GREY
        ld b,1
        call Ui_Color
        ld hl,(UiDuration)
        ld a,h
        or l
        jr z,.unknown
        call Ui_Time
        jr .print
.unknown:
        ld hl,UiNoTime
        call Ui_Text
.print:
        jp Ui_PrintTime

; Полоса загрузки: отдано из длины ((UiLoadSent), (UiLoadTotal) — файл плеера
; или модуль ESP); ESP разбирает файл — полна.
Ui_DrawLoad:
        ld a,C_GREY
        ld b,1
        call Ui_Begin
        ld hl,UiTextLoading
        call Ui_Text
        push de
        ld hl,(UiLoadTotal)
        call Ui_Kb16
        ld (UiBarTotal),hl
        ld hl,(UiLoadSent)
        call Ui_Kb16
        ex de,hl                         ; DE — отдано, КБ
        ld a,(UiBarWidth)
        dec a                            ; «Loading » и проценты на знак длиннее времени
        ld b,a
        call Ui_Cells
        pop de
        call Ui_BarCells
        ld a,' '
        ld (de),a
        inc de
        ; Проценты.
        ld hl,(UiLoadTotal)
        call Ui_Kb16
        ld (UiBarTotal),hl
        ld hl,(UiLoadSent)
        call Ui_Kb16
        push de
        ex de,hl
        ld b,100
        call Ui_Cells
        pop de
        ld l,a
        ld h,0
        ld a,C_GREY
        ld b,1
        call Ui_Color
        call Dec16
        ld a,'%'
        ld (de),a
        inc de
; Строку времени (или загрузки) — дополнить пробелами до ширины текста, чтобы
; не осталось хвоста прежней, и напечатать.
Ui_PrintTime:
        call Ui_TextPlace
        push bc
        ld hl,UiLine
        call Ui_PadTo
        pop bc
        ld a,UI_ROW_TIME
        jp Ui_PrintAt

; Дополнить кусок HL..DE пробелами до B видимых знаков (коды цвета не в счёт).
Ui_PadTo:
        call Ui_Visible
        sub b
        ret nc
        neg
        ld b,a
.space:
        ld a,' '
        ld (de),a
        inc de
        djnz .space
        ret

; Полоса: A ячеек закрашено из (UiCellsWidth) — ═ ярко-голубым, остальное
; ─ синим. Вход: DE — куда писать.
Ui_BarCells:
        ld c,a
        ld a,(UiCellsWidth)
        sub c
        ld (UiRest),a
        ld a,c
        or a
        jr z,.rest
        ld a,C_CYAN2
        ld b,2
        call Ui_Color
        ld b,c
.played:
        ld a,CH_PLAYED
        ld (de),a
        inc de
        djnz .played
.rest:
        ld a,(UiRest)
        or a
        ret z
        ld c,a
        ld a,C_BLUE
        ld b,1
        call Ui_Color
        ld b,c
.empty:
        ld a,CH_LEFT
        ld (de),a
        inc de
        djnz .empty
        ret

; Ячеек из B закрашено: B * DE / (UiBarTotal), не больше B. Ширина
; запоминается для Ui_BarCells. Выход: A. Сохраняет DE.
Ui_Cells:
        ld a,b
        ld (UiCellsWidth),a
        ld hl,(UiBarTotal)
        ld a,h
        or l
        ret z                            ; длина неизвестна: пусто
        or a
        sbc hl,de
        ld a,b
        ret c
        ret z                            ; не меньше длины: полна
        ; Сложить DE B раз, вычитая длину всякий раз, когда набралось.
        ld hl,0
        ld c,0
.step:
        add hl,de
        jr c,.over
        push hl
        push de
        ld de,(UiBarTotal)
        or a
        sbc hl,de
        pop de
        jr nc,.take
        pop hl
        jr .next
.take:
        inc sp
        inc sp
        inc c
        jr .next
.over:
        push de
        ld de,(UiBarTotal)
        or a
        sbc hl,de
        pop de
        inc c
.next:
        djnz .step
        ld a,c
        ret

; Секунды HL — «MM:SS» по DE.
Ui_Time:
        ld bc,0
.minutes:
        ld a,h
        or a
        jr nz,.minute
        ld a,l
        cp 60
        jr c,.seconds
.minute:
        ld a,l
        sub 60
        ld l,a
        jr nc,.counted
        dec h
.counted:
        inc bc
        jr .minutes
.seconds:
        push hl
        ld a,b
        or a
        jr nz,.wide
        ld a,c
        cp 10
        jr nc,.wide
        ld a,'0'                         ; минуты — двумя знаками
        ld (de),a
        inc de
.wide:
        ld h,b
        ld l,c
        call Dec16
        ld a,':'
        ld (de),a
        inc de
        pop hl
        ld a,l
        ld b,'0'-1
.tens:
        inc b
        sub 10
        jr nc,.tens
        add a,10+'0'
        ld c,a
        ld a,b
        ld (de),a
        inc de
        ld a,c
        ld (de),a
        inc de
        ret

; Строка состояния: «ERROR…» — красным.
Ui_DrawStatus:
        ld hl,(UiState)
        ld a,(hl)
        cp 'E'
        jr nz,.normal
        inc hl
        ld a,(hl)
        dec hl
        cp 'R'                           ; «ERROR…», а не «ESP…»
        jr nz,.normal
        ld a,C_RED2
        ld b,2
        jr .row
.normal:
        ld de,UiStatePlaying
        or a
        sbc hl,de
        add hl,de
        jr z,.quiet
        ld de,UiStateFinishing
        or a
        sbc hl,de
        add hl,de
        jr nz,.grey
.quiet:
        ld hl,UiEmpty
.grey:
        ld a,C_GREY
        ld b,1
.row:
        ld e,UI_ROW_STATUS
        jp Ui_TextRow

; Громкость, температура кристалла ESP и «H Help»: «Vol ████████░░░░ 75%   ESP 47C»,
; справа «H Help». Температуры нет (старые PLAYER и прошивка) — без неё.
Ui_DrawVolume:
        ld a,C_GREY
        ld b,1
        call Ui_Begin
        push de
        ld a,(UiStatusSeen)
        or a
        jp z,.blank                      ; громкость ESP ещё не известна
        ld hl,UiTextVol
        call Ui_Text
        ld hl,255
        ld (UiBarTotal),hl
        ld a,(Mp3Status+ST_VOLUME)
        push de
        ld e,a
        ld d,0
        ld b,UI_VOL_CELLS
        call Ui_Cells
        pop de
        ld c,a
        push bc
        or a
        jr z,.shade
        ld a,C_GREEN2
        ld b,2
        call Ui_Color
        pop bc
        push bc
        ld b,c
.full:
        ld a,CH_FULL
        ld (de),a
        inc de
        djnz .full
.shade:
        pop bc
        ld a,UI_VOL_CELLS
        sub c
        jr z,.percent
        ld b,a
        push bc
        ld a,C_GREY
        ld b,1
        call Ui_Color
        pop bc
.cell:
        ld a,CH_SHADE
        ld (de),a
        inc de
        djnz .cell
.percent:
        ld a,C_GREY
        ld b,1
        call Ui_Color
        ld a,' '
        ld (de),a
        inc de
        ld hl,255
        ld (UiBarTotal),hl
        ld a,(Mp3Status+ST_VOLUME)
        push de
        ld e,a
        ld d,0
        ld b,100
        call Ui_Cells
        pop de
        ld l,a
        ld h,0
        call Dec16
        ld a,'%'
        ld (de),a
        inc de
        ld a,(Mp3Status+ST_TEMPERATURE)
        or a
        jr z,.blank
        ; Перегрев кристалла: от UI_TEMP_YELLOW — ярко-жёлтым, от UI_TEMP_RED —
        ; ярко-красным (код цвета парой; «H Help» дальше задаёт свой цвет сам).
        ld c,C_RED2
        cp UI_TEMP_RED
        jr nc,.hot
        ld c,C_YELLOW2
        cp UI_TEMP_YELLOW
        jr c,.cool
.hot:
        ld a,c
        ld b,2
        call Ui_Color
.cool:
        ld hl,UiTextEsp
        call Ui_Text
        ld a,(Mp3Status+ST_TEMPERATURE)
        ld l,a
        ld h,0
        call Dec16
        ld hl,UiTextDegrees
        call Ui_Text
.blank:
        ; До «H Help»: видимых знаков — от начала куска без кодов цвета. Коды
        ; вставлены по одному-два байта: их пересчитать проще, чем помнить.
        pop hl
        call Ui_Visible                  ; A — видимых знаков от HL до DE
        ld b,a
        ld a,UI_HELP_COL-2
        sub b
        jr c,.help
        jr z,.help
        ld b,a
.pad:
        ld a,' '
        ld (de),a
        inc de
        djnz .pad
.help:
        ld a,C_YELLOW2
        ld b,2
        call Ui_Color
        ld a,'H'
        ld (de),a
        inc de
        ld a,C_GREY
        ld b,1
        call Ui_Color
        ld hl,UiTextHelp
        call Ui_Text
        ld bc,UI_ROW_VOLUME*256+2
        jp Ui_Print

; Видимых знаков в куске HL..DE (коды цвета 1..8 не в счёт). Выход: A.
Ui_Visible:
        push hl
        ld c,0
.next:
        ld a,l
        cp e
        jr nz,.byte
        ld a,h
        cp d
        jr z,.done
.byte:
        ld a,(hl)
        inc hl
        cp 9
        jr c,.next
        inc c
        jr .next
.done:
        ld a,c
        pop hl
        ret

; --- столбики -----------------------------------------------------------------

; Опросить спектр или уровни каналов (не чаще раза в UI_VIEW_EVERY кадров) и
; нарисовать столбики. Старая прошивка AUDIO_VIEW не знает: больше не спрашивать.
Ui_View:
        ld a,(UiViewOff)
        or a
        ret nz
        ld a,(WC_FRAMES)
        ld hl,UiViewFrame
        sub (hl)
        cp UI_VIEW_EVERY
        ret c
        ld a,(WC_FRAMES)
        ld (hl),a
        ld a,CMD_AUDIO_VIEW
        call Proto_SendEmpty
        ret c
        ld a,RESP_AUDIO_VIEW
        ld de,MP3_WAIT
        call Mp3_Wait
        jr nc,.answer
        ld a,1
        ld (UiViewOff),a
        xor a
        ld (ProtoErr),a                  ; отказ старой прошивки — не ошибка звука
        ld (UiViewCount),a
        ret
.answer:
        ; [вид][число][длительность с LE16][позиция с LE16][значения 0..12]
        ld a,(ProtoBuf)
        ld (UiViewKind),a
        ld a,(ProtoBuf+1)
        cp UI_VIEW_MAX+1
        jr c,.count
        ld a,UI_VIEW_MAX
.count:
        ld (UiViewCount),a
        ld hl,(ProtoBuf+2)
        ld (UiDuration),hl
        ld hl,(ProtoBuf+4)
        ld (UiPosition),hl
        ; Уровни и пики: пик держится UI_PEAK_HOLD опросов, потом опускается.
        or a
        jr z,.draw
        ld b,a
        ld hl,ProtoBuf+6
        ld de,UiLevels
        ld ix,UiPeaks
.level:
        ld a,(hl)
        cp 13
        jr c,.clamped
        ld a,12
.clamped:
        ld (de),a
        cp (ix+0)
        jr c,.lower
        jr z,.lower
        ld (ix+0),a                      ; новый пик
        ld (ix+UI_VIEW_MAX),UI_PEAK_HOLD
        jr .advance
.lower:
        ld a,(ix+UI_VIEW_MAX)
        or a
        jr z,.fall
        dec (ix+UI_VIEW_MAX)
        jr .advance
.fall:
        ld a,(ix+0)
        or a
        jr z,.advance
        dec (ix+0)
.advance:
        inc hl
        inc de
        inc ix
        djnz .level
.draw:
        ld a,(UiDialog)
        or a
        ret nz
        ld a,(UiViewKind)
        cp VIEW_LIST
        call z,Ui_DrawList               ; радио листают: список вместо столбиков
        call Ui_DrawBars
        jp Ui_DrawTime

; Радио: список станций (AUDIO_VIEW вида VIEW_LIST) — строки ESP с кодами цвета и
; #0D, по ширине столбиков; печатается с их места целиком, со строкой чисел.
; Только из свежего ответа: при перерисовке окна в ProtoBuf может быть другое.
Ui_DrawList:
        ld hl,ProtoBuf+6
        ld de,UI_ROW_BARS*256+2
        ld ix,Mp3Window
        jp WC_TXTPR

; Шесть строк столбиков и (у модуля до 16 каналов) номера каналов. Столбик —
; █ и ▄ ярко-зелёным (12 ступеней на 6 строк), пик над ним — ярко-красным.
; Спектр — 32 столбика по знаку; каналы — до 16 по три знака, больше — по знаку.
; Группа столбиков — посередине окна (у модуля с немногими каналами).
Ui_DrawBars:
        ld a,(UiDialog)
        or a
        ret nz
        ld a,1
        ld (UiBarW),a
        ld a,(UiViewKind)
        cp VIEW_LIST
        ret z                            ; список станций печатает Ui_View
        cp VIEW_CHANNELS
        jr nz,.width
        ld a,(UiViewCount)
        cp 17
        jr nc,.width
        ld a,3
        ld (UiBarW),a
.width:
        ; Слева (UI_BARS_WIDTH - ширина группы) / 2 пробелов; ширина — число
        ; столбиков на (ширина столбика + пробел) без последнего пробела.
        ld a,(UiViewCount)
        or a
        jr z,.centered
        ld b,a
        ld a,(UiBarW)
        inc a
        ld c,a
        xor a
.group:
        add a,c
        djnz .group
        dec a
        ld c,a
        ld a,UI_BARS_WIDTH
        sub c
        jr nc,.centered
        xor a
.centered:
        srl a
        ld (UiBarPad),a
        ld c,0                           ; строка столбиков, сверху
.row:
        push bc
        ld a,c
        ld (UiBarRow),a
        ; Порог строки: верхняя — ступени 11..12, нижняя — 1..2.
        ld a,UI_BAR_ROWS
        sub c
        add a,a
        ld (UiBarLevel),a                ; полный знак — с этой ступени
        ld a,C_GREEN2
        ld b,2
        call Ui_Begin
        xor a
        ld (UiVisibleCount),a
        call Ui_BarIndent
        ld a,(UiViewCount)
        or a
        jr z,.pad
        ld b,a
        ld hl,UiLevels
        ld ix,UiPeaks
.bar:
        push bc
        ld a,(UiBarLevel)
        ld c,a
        ld a,(hl)                        ; ступень столбика
        cp c
        ld a,CH_FULL
        jr nc,.char
        ld a,(hl)
        inc a
        cp c
        ld a,CH_LOWER
        jr z,.char
        ; Пусто — может быть, тут пик: ступень пика в этой строке.
        ld a,(ix+0)
        cp (hl)
        jr c,.blank
        jr z,.blank
        cp c
        jr z,.peak_upper                 ; чётная — верхняя половина строки
        inc a
        cp c
        jr nz,.blank
        ld a,CH_LOWER
        jr .peak
.peak_upper:
        ld a,CH_UPPER
.peak:
        ld (UiPeakChar),a
        ld a,C_RED2
        ld b,2
        call Ui_Color
        ld a,(UiPeakChar)
        call Ui_BarRun
        ld a,C_GREEN2
        ld b,2
        call Ui_Color
        jr .gap
.blank:
        ld a,' '
.char:
        call Ui_BarRun
.gap:
        pop bc
        push bc
        dec b
        jr z,.last
        ld a,' '
        ld (de),a
        inc de
        call Ui_CountVisible
.last:
        pop bc
        inc hl
        inc ix
        djnz .bar
.pad:
        ld a,(UiVisibleCount)
        cp UI_BARS_WIDTH
        jr nc,.print
        ld a,' '
        ld (de),a
        inc de
        call Ui_CountVisible
        jr .pad
.print:
        pop bc
        push bc
        ld a,c
        add a,UI_ROW_BARS
        ld c,2
        call Ui_PrintAt
        pop bc
        inc c
        ld a,c
        cp UI_BAR_ROWS
        jp c,.row
        ; Номера каналов под столбиками по три знака.
        ld a,C_GREY
        ld b,1
        call Ui_Begin
        push de
        ld a,(UiBarW)
        cp 3
        jr nz,.numbers_done
        call Ui_BarIndent
        ld a,(UiViewCount)
        ld b,a
        ld c,1
.number:
        push bc
        ld a,c
        cp 10
        jr nc,.two
        ld a,' '
        ld (de),a
        inc de
.two:
        ld l,c
        ld h,0
        call Dec16
        ld a,' '
        ld (de),a
        inc de
        ld (de),a
        inc de
        pop bc
        inc c
        djnz .number
.numbers_done:
        pop hl
        ld b,UI_BARS_WIDTH
        call Ui_Fit
        ld c,2
        ld a,UI_ROW_NUMBERS
        jp Ui_PrintAt

; Отступ группы столбиков: (UiBarPad) пробелов по DE.
Ui_BarIndent:
        ld a,(UiBarPad)
        or a
        ret z
        ld b,a
.space:
        ld a,' '
        ld (de),a
        inc de
        call Ui_CountVisible
        djnz .space
        ret

; Знак A ширины столбика (UiBarW) раз.
Ui_BarRun:
        push bc
        ld c,a
        ld a,(UiBarW)
        ld b,a
.put:
        ld a,c
        ld (de),a
        inc de
        call Ui_CountVisible
        djnz .put
        pop bc
        ret

Ui_CountVisible:
        push hl
        ld hl,UiVisibleCount
        inc (hl)
        pop hl
        ret

; --- метки --------------------------------------------------------------------

; Метки файла (AUDIO_TAGS): [название,0][исполнитель,0][альбом,0][год,0][номер,0].
; Старая прошивка AUDIO_TAGS не знает — тогда название из AUDIO_INFO.
Ui_FetchTags:
        ld a,CMD_AUDIO_TAGS
        call Proto_SendEmpty
        jr c,.info
        ld a,RESP_AUDIO_TAGS
        ld de,MP3_WAIT
        call Mp3_Wait
        jr c,.info
        ld hl,(ProtoRxLen)
        ld de,UI_TAGS_SIZE-1
        or a
        sbc hl,de
        add hl,de
        jr c,.sized
        ex de,hl
.sized:
        ld b,h
        ld c,l
        ld hl,ProtoBuf
        ld de,UiTags
        ld a,b
        or c
        jr z,.done
        ldir
.done:
        xor a
        ld (de),a
        ld (ProtoErr),a
        jp Ui_Draw
.info:
        xor a
        ld (ProtoErr),a
        call Mod_Info
        xor a
        ld (ProtoErr),a
        jp Ui_Draw

; --- окна поверх: справка и подробности -----------------------------------------

; Клавиши окна: H — справка, I — подробности (повторно — закрыть). Сырая
; таблица KBSCN: латиница при любой раскладке.
Ui_Keys:
        ld a,1
        call WC_KBSCN
        ret z
        or #20
        cp 'h'
        ld c,DIALOG_HELP
        jr z,.toggle
        cp 'i'
        ret nz
        ld c,DIALOG_INFO
.toggle:
        push bc
        ld a,FN_WAITUP                   ; одно нажатие — одно переключение
        call Wc_Api
        pop bc
        ld a,(UiDialog)
        cp c
        push af
        push bc
        call Ui_Close
        pop bc
        pop af
        ret z                            ; то же окно — только закрыть
        ld a,c
        ld (UiDialog),a
        call Ui_SpritesOff
        ld a,(UiDialog)
        cp DIALOG_INFO
        jr z,Ui_OpenInfo
        ld ix,UiHelpWindow
        call Ui_DialogOpen
        ld ix,UiHelpWindow
        ld hl,UiHelpText
        ld de,#0202
        jp WC_TXTPR

Ui_OpenInfo:
        ld ix,UiInfoWindow
        call Ui_DialogOpen
        jp Ui_DrawInfo

; PRWOW окна IX в середине экрана.
Ui_DialogOpen:
        ld (ix+2),#FF
        ld (ix+3),#FF
        ld (ix+8),0
        ld (ix+9),0
        jp WC_PRWOW

; Esc: открыто окно поверх — закрыть его; иначе NZ — стоп.
Ui_Esc:
        call WC_ESC
        ret z
        ld a,(UiDialog)
        or a
        jr nz,.close
        or 1
        ret
.close:
        call Ui_Close
        xor a
        ret

; Закрыть окно поверх (если открыто) и перерисовать основное.
Ui_Close:
        ld a,(UiDialog)
        or a
        ret z
        ld ix,UiHelpWindow
        cp DIALOG_HELP
        jr z,.restore
        ld ix,UiInfoWindow
.restore:
        call WC_RRESB
        xor a
        ld (UiDialog),a
        call Ui_SpritesOn
        jp Ui_Draw

; Спрайт обложки — поверх текста: на время окна поверх его выключить.
Ui_SpritesOff:
        ld a,(CoverState)
        cp COVER_SHOWN
        ret nz
        ld bc,TS_CONFIG
        xor a
        out (c),a
        ret
Ui_SpritesOn:
        ld a,(CoverState)
        cp COVER_SHOWN
        ret nz
        ld bc,TS_CONFIG
        ld a,%10000000
        out (c),a
        ret

; Подробности: имя, состояние, формат, отдано, очередь, сбои, громкость.
Ui_DrawInfo:
        ld a,(UiDialog)
        cp DIALOG_INFO
        ret nz
        ld a,1
        ld (UiInfoNext),a
        call Ui_InfoBegin
        ld hl,UiInfoFile
        call Ui_Text
        ld hl,UiName
        call Ui_Text
        call Ui_InfoEnd
        call Ui_InfoBegin
        ld hl,UiInfoState
        call Ui_Text
        ld hl,(UiState)
        call Ui_Text
        call Ui_InfoEnd
        ; Формат: «44100 Hz, 2 ch, 128 kbps».
        call Ui_InfoBegin
        ld hl,UiInfoFormat
        call Ui_Text
        call Ui_RateHz
        ld hl,UiTextHz
        call Ui_Text
        ld a,(Mp3Status+ST_CHANNELS)
        ld l,a
        ld h,0
        call Dec16
        ld hl,UiTextCh
        call Ui_Text
        ld a,(Mp3Kind)
        or a
        jr nz,.whole                     ; у файла целиком потока нет
        ld hl,UiTextComma
        call Ui_Text
        ld hl,(Mp3Status+ST_BITRATE)
        call Dec16
        ld hl,UiTextKbps
        call Ui_Text
.whole:
        call Ui_InfoEnd
        ; Отдано: «123 of 4567 KB».
        call Ui_InfoBegin
        ld hl,UiInfoSent
        call Ui_Text
        ld hl,Mp3Sent
        call Ui_AppendKb
        ld hl,UiTextOf
        call Ui_Text
        ld hl,Mp3Total
        call Ui_AppendKb
        ld hl,UiTextKb
        call Ui_Text
        call Ui_InfoEnd
        ; Очередь ESP.
        call Ui_InfoBegin
        ld hl,UiInfoQueue
        call Ui_Text
        ld hl,Mp3Status+ST_BUFFERED
        call Ui_AppendKb
        ld hl,UiTextKb
        call Ui_Text
        call Ui_InfoEnd
        ; Сбои: кадры с ошибкой, паузы из-за пустой очереди, отказы I2S.
        call Ui_InfoBegin
        ld hl,UiInfoFaults
        call Ui_Text
        ld hl,(Mp3Status+ST_ERRORS)
        call Dec16
        ld hl,UiTextStarve
        call Ui_Text
        ld hl,(Mp3Status+ST_UNDERRUNS)
        call Dec16
        ld hl,UiTextI2s
        call Ui_Text
        ld hl,(Mp3Status+ST_I2S)
        call Dec16
        call Ui_InfoEnd
        ; Громкость 0..255.
        call Ui_InfoBegin
        ld hl,UiInfoVolume
        call Ui_Text
        ld a,(Mp3Status+ST_VOLUME)
        ld l,a
        ld h,0
        call Dec16
        jp Ui_InfoEnd

Ui_InfoBegin:
        ld a,C_BLACK
        ld b,1
        call Ui_Begin
        ld (UiInfoStart),de
        ret

; Дополнить до ширины окна подробностей и напечатать в его следующей строке.
Ui_InfoEnd:
        ld hl,(UiInfoStart)
        ld b,UI_INFO_WIDTH-4
        call Ui_Fit
        xor a
        ld (de),a
        ld a,(UiInfoNext)
        inc a
        ld (UiInfoNext),a
        ld d,a
        ld e,2
        ld hl,UiLine
        ld ix,UiInfoWindow
        jp WC_TXTPR

; Показать запись состояния (после каждого AUDIO_STATUS). Строки окна — не
; чаще раза в 10 кадров (или сразу, если сменилось состояние), столбики и
; время — по опросу AUDIO_VIEW (Ui_View).
Ui_ShowStatus:
        ld a,(Mp3Status+ST_STATE)
        sub AUDIO_LOADING
        jr z,.loading
        ld a,-1
.loading:
        inc a
        ld (UiLoading),a
        ld hl,(UiState)
        ld de,(UiDrawnState)
        or a
        sbc hl,de
        jr nz,.draw
        ld a,(WC_FRAMES)
        ld hl,UiDrawFrame
        sub (hl)
        cp 10
        jr c,.view
.draw:
        ld a,(WC_FRAMES)
        ld (UiDrawFrame),a
        ld hl,(UiState)
        ld (UiDrawnState),hl
        call Ui_DrawInfo
        call Ui_Draw
.view:
        jp Ui_View

; Окно — как есть сейчас, без ожидания (перед выходом, после клавиш).
Ui_Flush:
        call Ui_DrawInfo
        call Ui_Draw
        ld a,(WC_FRAMES)
        ld (UiDrawFrame),a
        ld hl,(UiState)
        ld (UiDrawnState),hl
        ret

; Громкость сменилась: строку громкости — сразу.
Ui_VolumeChanged:
        ld a,(UiDialog)
        or a
        jp nz,Ui_DrawInfo
        jp Ui_DrawVolume

; Ход передачи файла (или модуля ESP): полоса загрузки, не чаще раза за кадр.
Ui_ShowLoad:
        ld a,1
        ld (UiLoading),a
        ld a,(UiDialog)
        or a
        ret nz
        ld a,(WC_FRAMES)
        ld hl,UiLoadFrame
        cp (hl)
        ret z
        ld (hl),a
        call Ui_DrawStatus
        jp Ui_DrawTime

UiBarTotal:     dw 0                    ; длина полосы в тех же единицах, что и сыграно
UiCellsWidth:   db 0

; Частота из записи состояния: A — тысячи (кГц), HL — остаток меньше 1000. С
; флагом ST_F_RATE_HIGH (частота больше 65535: 88,2 и 96 кГц; PLAYER 1.19) к полю
; LE16 прибавляется 65536 = 65·1000 + 536. Портит BC; DE не трогает.
Ui_RateSplit:
        ld hl,(Mp3Status+ST_RATE)
        ld bc,-1000
        ld a,-1
.thousands:
        inc a
        add hl,bc
        jr c,.thousands
        ld bc,1000
        add hl,bc                        ; HL — остаток меньше 1000
        ld c,a
        ld a,(Mp3Status+ST_FLAGS)
        and ST_F_RATE_HIGH
        ld a,c
        ret z
        add a,65
        ld bc,536
        add hl,bc                        ; остаток + 536 — меньше 1536
        ld bc,-1000
        add hl,bc
        jr c,.carry                      ; было не меньше 1000 — ещё тысяча
        ld bc,1000
        add hl,bc
        ret
.carry:
        inc a
        ret

; Дописать по DE частоту в герцах полностью («96000»): тысячи и остаток тремя
; цифрами. Портит AF, BC, HL.
Ui_RateHz:
        call Ui_RateSplit
        or a
        jr z,Dec16                       ; меньше 1000 Гц — как есть
        push hl
        ld l,a
        ld h,0
        call Dec16
        pop hl
        ld bc,-100
        ld a,'0'-1
.hundreds:
        inc a
        add hl,bc
        jr c,.hundreds
        ld bc,100
        add hl,bc
        ld (de),a
        inc de
        ld bc,-10
        ld a,'0'-1
.tens:
        inc a
        add hl,bc
        jr c,.tens
        ld bc,10
        add hl,bc
        ld (de),a
        inc de
        ld a,l
        add a,'0'
        ld (de),a
        inc de
        ret

; HL — адрес 32-битного числа байтов. Выход: HL — килобайты, не больше 65535.
; Сохраняет DE.
Ui_Kb16:
        push de
        inc hl
        ld e,(hl)
        inc hl
        ld d,(hl)
        inc hl
        ld a,(hl)                       ; A:D:E — число, сдвинутое на 8 бит
        srl a
        rr d
        rr e
        srl a
        rr d
        rr e
        ex de,hl
        pop de
        or a
        ret z
        ld hl,#FFFF
        ret

; Дописать по DE число килобайтов: HL — адрес 32-битного числа байтов.
Ui_AppendKb:
        call Ui_Kb16
; Дописать по DE десятичное число HL без ведущих нулей; DE сдвигается за него.
; Портит AF, BC, HL.
Dec16:
        xor a
        ld (DecStarted),a
        ld bc,-10000
        call .digit
        ld bc,-1000
        call .digit
        ld bc,-100
        call .digit
        ld bc,-10
        call .digit
        ld a,l
        add a,'0'
        ld (de),a
        inc de
        ret
.digit:
        ld a,'0'-1
.count:
        inc a
        add hl,bc
        jr c,.count
        sbc hl,bc                       ; вернуть лишнее вычитание (CF здесь сброшен)
        cp '0'
        jr nz,.emit
        ld a,(DecStarted)
        or a
        ret z                           ; ведущий ноль не печатается
        ld a,'0'
.emit:
        ld (de),a
        inc de
        ld a,1
        ld (DecStarted),a
        ret

; --- данные --------------------------------------------------------------------

UI_NAME         equ 40
UI_INFO_WIDTH   equ 50

DecStarted:     db 0
UiDialog:       db DIALOG_NONE
UiState:        dw UiStateNoFile
UiHint:         dw 0
UiViewFrame:    db 0
UiBarWidth:     db 0
UiBarW:         db 1
UiBarPad:       db 0                    ; отступ группы столбиков слева
UiBarRow:       db 0
UiBarLevel:     db 0
UiPeakChar:     db 0
UiVisibleCount: db 0
UiInfoNext:     db 0
UiRest:         db 0
UiDrawFrame:    db 0
UiLoadFrame:    db 0
UiDrawnState:   dw 0
UiShift:        db 0                    ; модуль и MIDI: строки от формата — на одну выше
UiLoadSent:     dw Mp3Sent              ; полоса загрузки: отдано (32 бита)
UiLoadTotal:    dw Mp3Total             ; ... из длины
UiLabels:       dw UiTextSamples        ; подписи третьей строки модуля: две подряд
UiInfoStart:    dw 0
UiName:         ds UI_NAME+1
UiSongTitle:    ds UI_TITLE+1           ; название модуля (AUDIO_INFO) с нулём

; Обнуляется при каждом открытии окна (Ui_Open).
UiTags:         ds UI_TAGS_SIZE         ; AUDIO_TAGS как пришёл
UiViewKind:     db 0
UiViewCount:    db 0
UiViewOff:      db 0                    ; прошивка AUDIO_VIEW не знает
UiLoading:      db 0                    ; модуль принимается или разбирается
UiStatusSeen:   db 0                    ; запись состояния ESP уже была (громкость)
UiDuration:     dw 0                    ; секунд, 0 — неизвестно
UiPosition:     dw 0
UiLevels:       ds UI_VIEW_MAX
UiPeaks:        ds UI_VIEW_MAX          ; пики, за ними — сколько им ещё стоять
UiPeakHold:     ds UI_VIEW_MAX
UiClearEnd:


; Дескрипторы окон формата Wild Commander; текст печатается через TXTPR.
Mp3Window:
        db #81                          ; тень и рамка стиля 1
        db 0
        db #FF,#FF                      ; X,Y: середина, см. Ui_Open
        db UI_WIDTH,UI_HEIGHT
        db #0D                          ; чёрный фон, рамка ярко-голубая (эскиз)
        db 0
        dw 0
        db 0,0
        dw UiHeader
        dw 0
        dw 0
UiHelpWindow:
        db #81,0,#FF,#FF,40,14,#70,0
        dw 0
        db 0,0
        dw UiHelpHeader
        dw 0,0
UiInfoWindow:
        db #81,0,#FF,#FF,UI_INFO_WIDTH,11,#70,0
        dw 0
        db 0,0
        dw UiInfoHeader
        dw 0,0

UiHeader:       db #0B,2,7,7," VDAC4 Media Player ",0
UiHelpHeader:   db #0E," Help ",0
UiInfoHeader:   db #0E," Technical info ",0
; Справка: блок 36×10 ровно посередине окна 40×14 (поля по одной строке и
; одному столбцу), «Esc  Close» — по центру.
UiHelpText:
        db 1,"Space ",8,"  Pause / resume",#0D
        db 1,#1B," ",#1A,"   ",8,"  Seek 10 s (module: position)",#0D
        db 1,"PgUp  ",8,"  Previous file",#0D
        db 1,"PgDn  ",8,"  Next file",#0D
        db 1,#18," ",#19,"   ",8,"  Volume",#0D
        db 1,"I     ",8,"  Technical info",#0D
        db 1,"H     ",8,"  This help",#0D
        db 1,"Esc   ",8,"  Stop and exit",#0D,#0D
        db #0E,1,"Esc",8,"  Close",0

UiGap:             db "  ",0
UiTextKbps:        db " kbps  ",0
UiTextKhz:         db " kHz  ",0
UiTextMono:        db "mono",0
UiTextStereo:      db "stereo",0
UiTextHz:          db " Hz, ",0
UiTextCh:          db " ch",0
UiTextComma:       db ", ",0
UiTextChannels:    db " channels  ",0
UiTextPositions:   db " positions",0
UiTextFile:        db "Track ",0
UiTextSamples:     db "Samples: ",0          ; пары подписей: вторая — сразу за первой
                   db "  Effects: ",0
UiTextInstruments: db "Instruments: ",0
                   db "  Tracks: ",0
UiEmpty:           db 0
UiTextOf:          db " of ",0
UiTextKb:          db " KB",0
UiTextPosition:    db "Position ",0
UiTextRow:         db "   Row ",0
UiTextStarve:      db ", starve ",0
UiTextI2s:         db ", i2s ",0
UiTextLoading:     db "Loading ",0
UiTextVol:         db "Vol ",0
UiTextEsp:         db "   ESP ",0
UiTextDegrees:     db "'C",0            ; «ESP 70'C»: знака «°» в шрифте WC нет (#F8 — уголок рамки)
UiTextHelp:        db " Help",0
UiNoTime:          db "--:--",0
UiFmtFlac:         db "FLAC",0
UiFmtMod:          db "MOD",0
UiFmtXm:           db "XM",0
UiFmtS3m:          db "S3M",0
UiFmtIt:           db "IT",0
UiFmtMidi:         db "MIDI",0
UiFmtNames:        dw UiFmtMod,UiFmtXm,UiFmtS3m,UiFmtIt,UiFmtMidi
UiFmtModule:       db "Module",0
UiNameNone:        db "(no file)",0
UiInfoFile:        db "File  : ",0
UiInfoState:       db "State : ",0
UiInfoFormat:      db "Format: ",0
UiInfoSent:        db "Sent  : ",0
UiInfoQueue:       db "Queue : ",0
UiInfoFaults:      db "Faults: decode ",0
UiInfoVolume:      db "Volume: ",0

UiHintPlay:        db "Seek: on",0              ; подсказки в окне не видны (тесты)
UiHintLost:        db "No seek: file not found",0
UiHintModule:      db "Seek: positions",0
UiHintNoFile:      db "Enter on MP3, MOD, XM, S3M, IT, MID; DAC test: audio_test",0

UiStageEsp:        db "Looking for the VDAC4 ESP",0
UiStateSending:    db "Sending to the ESP: ",0
UiStateStoring:    db "Storing on the ESP: ",0     ; за ними — имя (EspStateText)
UiStateChecking:   db "Checking the ESP for: ",0   ; ресурс: PSRAM или флешка ESP
UiStageOpen:       db "Opening the audio stream",0
UiStageLoad:       db "Sending the file to the ESP",0
UiStageStart:      db "ESP is loading the file",0
UiStatePlaying:    db "Playing",0
UiStateSkipTag:    db "Skipping the ID3 tag (cover art)",0
UiStatePaused:     db "Paused",0
UiStateBuffering:  db "Buffering",0
UiStateFinishing:  db "File sent, playing the rest",0
UiStateNoFile:     db "Open a file from the panel",0
UiErrorNoEsp:      db "ERROR: no ESP on the SPI",0
UiErrorSilent:     db "ERROR: ESP does not answer",0
UiErrorAudioOff:   db "ERROR: ESP audio (I2S) is off",0
UiErrorClosed:     db "ERROR: ESP closed the stream",0
UiErrorCard:       db "ERROR: the file could not be read",0
UiErrorModOpen:    db "ERROR: no room on the ESP for the file",0
UiErrorModSlow:    db "ERROR: ESP did not start playing",0
UiErrorModFormat:  db "ERROR: ESP could not load the file",0
UiErrorEspMissing: db "ERROR: missing /wc/esp/",0      ; за ними — имя (esp_modules.asm)
UiErrorEspRoom:    db "ERROR: no room on the ESP for ",0
UiErrorEspTook:    db "ERROR: ESP failed on ",0
