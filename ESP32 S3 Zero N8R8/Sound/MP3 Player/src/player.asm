; Точка входа плагина. PLUGIN обязан быть первым байтом по адресу #8000.
;
; Вход от Wild Commander: A — способ вызова, DE:HL — длина файла, BC — имя
; файла (только при вызове на файле), IX — структура активной панели.
;
; Плагин ничего не декодирует. Музыку он читает с карты по 16 секторов и
; отдаёт ESP кадрами AUDIO_DATA потоком; ESP декодирует его по мере прихода.
; Трекерный модуль (MOD, XM, S3M, IT) и MIDI уходят тем же путём, но целиком до
; начала звука: ESP держит файл в PSRAM и играет трекером или синтезатором
; (банк звуков MIDI.SF2 из /WC/ESP — ресурс ESP, esp_modules.asm). Что это за
; файл, плагин решает по расширению имени; вид модуля ESP узнаёт по содержимому.
;
; Списка файлов плагин не ведёт: соседний файл панели WC открывает сам, если
; плагин вернулся с кодом 2 (следующий) или 4 (предыдущий), и снова вызывает
; плагин по расширению. Так сделаны и переход клавишами PgDn и PgUp, и
; автопереход в конце файла (как у проигрывателя espmp3_wc для прошивки
; TS-Labs).
;
; Звук идёт потоком 0 WC подряд (LOAD512), без повторного поиска каждого блока.
; Всё, что нужно с произвольного места, — метка ID3 и картинка из папки для
; обложки (cover.asm) — читается потоком 1 (API 57: у каждого потока своя
; позиция), поэтому обложка и метки грузятся фоном и звук не сбивают.
;
; Перемотка (влево и вправо): у модуля — на позицию таблицы порядка; у MP3 —
; на MP3_SEEK_SECONDS: ESP по AUDIO_FLUSH выбрасывает очередь и состояние
; декодера, а поток 0 встаёт на новое место — свой файл заново (FENTRY,
; GFILE), целые секторы пропускает LOADNONE (без чтения данных), хвост сектора
; уходит первым блоком (Mp3_Reposition).

MP3_BLOCK_SIZE  equ 1024                ; тело одного кадра AUDIO_DATA
MP3_READ_SIZE   equ 8192                ; чтение SD независимо от пакета ESP
MP3_WAIT        equ 12000               ; опросов в ожидании ответа ESP
MP3_PING_TRIES  equ 4
MP3_STATUS_EVERY equ 16                 ; блоков между запросами состояния
MP3_DRAIN_EVERY equ 10                  ; кадров между запросами, пока ESP играет
MP3_VOLUME_STEP equ 16
MP3_SEEK_SECONDS equ 10
MOD_START_FRAMES equ 250                ; сколько кадров ждать разбора модуля (5 с)
MOD_ROOM_FRAMES equ 50                  ; пауза перед новой попыткой открыть файл (1 с)

; Способы вызова плагина.
CALL_EXTENSION  equ #00                 ; Enter или F3 на файле
CALL_MENU       equ #03                 ; F10: с типом #04 WC так не вызывает; файла нет
CALL_VIEWER     equ #04                 ; меню выбора просмотрщика

; Клавиши и функции API Wild Commander, которых нет в общем wc_api.inc.
FN_LEFT         equ #13
FN_RIGHT        equ #14
FN_GIPAGPL      equ #32                 ; поток — снова на начало файла плагина
FN_LOADNONE     equ #3D                 ; пропустить B секторов потока без чтения

; Коды возврата в WC при вызове по расширению.
EXIT_NEXT       equ 2                   ; перейти к следующему файлу панели
EXIT_PREV       equ 4                   ; к предыдущему

; Запись состояния звука (ответ AUDIO_STATUS): 24 байта, с прошивки 0.1.20 — 32.
ST_STATE        equ 0                   ; 0 тишина, 1 играет, 2 доигрывает, 3 тон,
                                        ; 4 I2S не запущен, 5 модуль принимается
ST_FLAGS        equ 1                   ; бит 0 — поток доигран, бит 1 — формат распознан,
                                        ; бит 2 — очередь набирается, бит 3 — пауза
ST_F_BUFFERING  equ 4
ST_F_WANTS_ROM  equ 16                  ; декодеру OPL нужно ПЗУ OPL4, а его нет
ST_F_TAGS       equ 32                  ; метки сменились (радио: песня; поток: от декодера)
ST_F_COVER      equ 64                  ; декодер нашёл обложку (Ogg) — спросить миниатюру
ST_F_RATE_HIGH  equ 128                 ; частота больше 65535: к ST_RATE +65536 (PLAYER 1.19)
ST_FREE         equ 2                   ; свободно в очереди, LE32
ST_BUFFERED     equ 6                   ; лежит в очереди, LE32
ST_RATE         equ 10                  ; частота отсчётов, LE16 (старший бит — ST_F_RATE_HIGH)
ST_CHANNELS     equ 12
ST_BITRATE      equ 13                  ; кбит/с, LE16
ST_SECONDS      equ 15                  ; сыграно секунд, LE16
ST_I2S          equ 17                  ; отказов записи в I2S, LE16
ST_ERRORS       equ 19                  ; кадров с ошибкой, LE16
ST_UNDERRUNS    equ 21                  ; пауз из-за пустой очереди, LE16
ST_VOLUME       equ 23
ST_FORMAT       equ 24                  ; 1 MP3, 2 MOD, 3 XM, 4 S3M
ST_ROW          equ 25                  ; модуль: строка паттерна
ST_POSITION     equ 26                  ; модуль: позиция таблицы порядка, LE16
ST_LENGTH       equ 28                  ; модуль: длина таблицы порядка, LE16
ST_MOD_CHANNELS equ 30                  ; модуль: каналов
ST_TEMPERATURE  equ 31                  ; кристалл ESP, °C (0 — неизвестна; PLAYER 1.0)
ST_SCREEN       equ 32                  ; флаги экрана Tang (PLAYER 1.26, SCREEN_F_*, screen.asm)
ST_SIZE         equ 33

AUDIO_IDLE      equ 0
AUDIO_PLAYING   equ 1
AUDIO_DRAINING  equ 2
AUDIO_OFFLINE   equ 4
AUDIO_LOADING   equ 5

FORMAT_MOD      equ 2
FORMAT_XM       equ 3
FORMAT_S3M      equ 4
FORMAT_IT       equ 5
FORMAT_MIDI     equ 6
FORMAT_OPL      equ 7
FORMAT_SAA      equ 8
OPEN_MODULE     equ #80                 ; AUDIO_OPEN: файл целиком (модуль, MIDI, OPL)
OPEN_MIDI       equ #40                 ; ... это MIDI: банк ESP не освобождает
OPEN_RELEASE    equ #20                 ; AUDIO_OPEN [#20]: ESP освобождает ресурсы

; Что за файл (по расширению).
KIND_MP3        equ 0
KIND_MODULE     equ 1
KIND_MIDI       equ 2
KIND_OPL        equ 3                   ; музыка OPL: VGM, VGZ, DRO, IMF, WLF, MWM, MFM
KIND_KIT        equ 4                   ; набор волн MWM (.MWK): не играть, пропустить
KIND_RADIO      equ 5                   ; плейлист интернет-радио (.M3U): файл целиком, поток
                                        ; станции — MP3 на ESP; окно — как у OPL (метки)

KIND_TSFM       equ 7                   ; компилированная музыка AY/TurboSound и TFC
KIND_SAA        equ 6                   ; E-Tracker: SAA / COP

PLUGIN:
        push ix
        ld (Mp3Call),a
        ld a,(ix+#23)                    ; сортировка панели — для «Track N of M»
        ld (TrackSort),a
        ld a,(Mp3Call)
        ; В альтернативных регистрах WC отдаёт номер объекта панели (HL') и
        ; число объектов (DE'): у последнего следующего файла нет.
        exx
        ld (Mp3Index),hl
        ld (Mp3Count),de
        exx
        ld (Mp3Total),hl
        ld (Mp3Total+2),de
        ld (Mp3Left),hl
        ld (Mp3Left+2),de
        ld (Mp3FileSize),hl
        ld (Mp3FileSize+2),de
        ; Имя файла Wild Commander передаёт только при вызове на файле.
        ld (Mp3NamePtr),bc
        cp CALL_MENU
        ld hl,UiNameNone                 ; файла нет: окно — как у потока
        jr z,.named
        ld h,b
        ld l,c
.named:
        push hl
        call Mp3_PickKind
        pop hl
        ; Набор волн MWM — не музыка: дальше в ту сторону, куда шёл плеер (код
        ; прошлого выхода; страница живёт между запусками), а открыт сам — выход.
        ; Код — в ноль: WC вернулся на тот же набор (край панели) — стоп.
        ld a,(Mp3Kind)
        cp KIND_KIT
        jr nz,.music
        ld a,FN_WAITUP                   ; клавиша, что привела сюда, в WC больше не сработает
        call Wc_Api
        ld hl,Mp3ExitCode
        ld a,(hl)
        ld (hl),0
        pop ix
        ret
.music:
        call Ui_SetName
        ld bc,2                          ; API 14: Z80 at 14 MHz
        ld a,FN_TURBOPL
        call Wc_Api
        call Track_Count                 ; номер среди файлов плеера (track.asm)
        ; Страница плагина живёт в памяти между запусками: счётчики обнуляем
        ; здесь, а не полагаемся на db 0.
        ld hl,0
        ld (Mp3Sent),hl
        ld (Mp3Sent+2),hl
        ld (Mp3BlockLen),hl
        ld (Mp3ReadLeft),hl
        ld (Mp3TagLen),hl
        ld (Mp3TagLen+2),hl
        ld hl,Mp3Block
        ld (Mp3BlockPtr),hl
        xor a
        ld (Mp3Blocks),a
        ld (ModRomTried),a
        ld (ModRoomTried),a
        ld (ProtoErr),a
        ld (Mp3ExitCode),a
        ld (Mp3Paused),a
        ld (Mp3CanSeek),a
        ld (S1Open),a
        ld (UiSongTitle),a
        ld (CoverState),a
        ld (Mp3HasTag),a
        ld (Mp3Silent),a                 ; ESP молчала в прошлый раз — теперь спрашивать снова
        ld (UiDialog),a                  ; окна поверх от прошлого вызова нет
        ; Запись состояния — от прошлого файла: формат и позиции в окне — не его.
        ld hl,Mp3Status
        ld de,Mp3Status+1
        ld bc,ST_SIZE-1
        ld (hl),a
        ldir
        ; Флаг ухода прошлого выхода (Mp3_Leave) — не этому вызову: с ним PLAYER считает окно
        ; закрытым, и экран оверлеем Tang не показывался до выключения Evo (плагин 1.19).
        ld a,UI_F_OPEN
        ld (UiBlockFlags),a
        ld (UiHidden),a                  ; окно — скрыто, пока не решено (screen.asm)
        ; Прерывания WC (часы, опрос клавиатуры) не трогаем: окно часы не
        ; закрывает, и они должны идти.
        call WC_GEDPL
        call Ui_Open

        ld hl,UiStageEsp
        call Ui_SetState
        call Ui_Draw
        call ZiFi_Init
        ld hl,UiErrorNoEsp
        jp c,Mp3_Fatal
        call Fast_Probe
        call Proto_ResetRx
        ld hl,0
        ld (ProtoBurstLeft),hl
        ; Готовность прошивки: PING до первого READY.
        ld b,MP3_PING_TRIES
.ping:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        ld a,RESP_READY
        ld de,MP3_WAIT
        call Mp3_Wait
        pop bc
        jr nc,.ready
        djnz .ping
        ld hl,UiErrorSilent
        jp Mp3_Fatal
.ready:
        call Ui_Probe                    ; нет Tang с оверлеями — окно сразу
        ld a,(Mp3Call)
        cp CALL_MENU
        jp z,Mp3_NoFile
        cp CALL_EXTENSION
        jr z,.file
        cp CALL_VIEWER
        jp nz,Mp3_Exit                   ; другим способом плагин не вызывается
.file:
        ; Проигрыватель на ESP — модуль из /WC/ESP/ (esp_modules.asm). CF=1 —
        ; MIDI играть нечем: HL — причина (0 — Esc).
        call Esp_Prepare
        jr nc,.prepared
        ld a,h
        or l
        jp z,Mp3_Exit
        jp Mp3_Fatal
.prepared:
        ld a,(Mp3Kind)
        or a                             ; KIND_MP3
        jp nz,Mod_Play                   ; модуль и MIDI — целиком
        jp Mp3_Play

; Что за файл, по расширению имени HL (первые три знака после последней точки):
; MP3 и WAV — поток, MID, RMI, KAR — MIDI, VGM, VGZ, DRO, IMF, WLF, MWM — OPL, остальное —
; модуль. Расширение — в Mp3Ext заглавными (AUDIO_OPEN, строка формата).
Mp3_PickKind:
        ld de,0                          ; адрес последней точки
.scan:
        ld a,(hl)
        or a
        jr z,.end
        cp '.'
        jr nz,.next
        ld d,h
        ld e,l
.next:
        inc hl
        jr .scan
.end:
        ld a,KIND_MP3
        ld (Mp3Kind),a
        ld hl,Mp3Ext
        ld b,3
.blank:
        ld (hl),' '
        inc hl
        djnz .blank
        ld a,d
        or e
        ret z                            ; без расширения — как MP3
        inc de                           ; DE — расширение
        push de
        ld hl,Mp3Ext
        ld b,3
.ext:
        ld a,(de)
        or a
        jr z,.ext_done
        cp 'a'
        jr c,.upper
        cp 'z'+1
        jr nc,.upper
        sub 'a'-'A'
.upper:
        ld (hl),a
        inc hl
        inc de
        djnz .ext
.ext_done:
        pop de
        ld de,Mp3Ext
        ld hl,Mp3Kinds
.kind:
        ld a,(hl)
        or a
        jr z,.module
        push de
        ld b,3
.compare:
        ld a,(de)
        and #DF                          ; заглавная (цифра 3 тоже совпадёт: #33 & #DF = #13)
        ld c,a
        ld a,(hl)
        and #DF
        cp c
        jr nz,.differ
        inc hl
        inc de
        djnz .compare
        pop de
        ld a,(hl)
        ld (Mp3Kind),a
        ret
.differ:
        inc hl                           ; остаток имени в таблице
        djnz .differ
        inc hl                           ; вид
        pop de
        jr .kind
.module:
        ld a,KIND_MODULE
        ld (Mp3Kind),a
        ret

Mp3Kinds:       db "MP3",KIND_MP3,"WAV",KIND_MP3,"AAC",KIND_MP3,"OGG",KIND_MP3,"OGA",KIND_MP3,"FLA",KIND_MP3
                db "MID",KIND_MIDI,"RMI",KIND_MIDI,"KAR",KIND_MIDI
                db "VGM",KIND_OPL,"VGZ",KIND_OPL,"DRO",KIND_OPL,"IMF",KIND_OPL,"WLF",KIND_OPL
                db "MWM",KIND_OPL,"MFM",KIND_OPL,"MWK",KIND_KIT,"M3U",KIND_RADIO
                db "SAA",KIND_SAA,"COP",KIND_SAA
                db "AS0",KIND_TSFM
                db "ASC",KIND_TSFM
                db "FTC",KIND_TSFM
                db "GTR",KIND_TSFM
                db "PSC",KIND_TSFM
                db "PSM",KIND_TSFM
                db "PT1",KIND_TSFM
                db "PT2",KIND_TSFM
                db "PT3",KIND_TSFM
                db "SQT",KIND_TSFM
                db "ST3",KIND_TSFM
                db "STC",KIND_TSFM
                db "STP",KIND_TSFM
                db "TS ",KIND_TSFM
                db "TFC",KIND_TSFM
                db "TFD",KIND_TSFM
                db "MTC",KIND_TSFM
                db 0

; Из меню F10 файла нет: подсказка, как вызвать, и выход по Esc.
Mp3_NoFile:
        ld hl,UiHintNoFile
        call Ui_SetHint
        ld hl,UiStateNoFile
        jp Mp3_Fatal

; --- MP3: поток ----------------------------------------------------------------

Mp3_Play:
        ld hl,UiHintPlay
        call Ui_SetHint
        ld hl,UiStageOpen
        call Ui_SetState
        call Ui_Draw
        ; Перемотка и обложка — если свой файл нашёлся заново (Esp_Reopen).
        ld a,(EspReopened)
        ld (Mp3CanSeek),a
        or a
        ld hl,UiHintLost
        call z,Ui_SetHint
        call Mp3_SkipTag
        jp c,.tag_stop                   ; Esc или ошибка карты при пропуске метки
        ; AUDIO_OPEN [0][длина потока LE32][флаги][расширение]: по длине ESP
        ; считает длительность (длинная метка уже пропущена и в длину не входит),
        ; по расширению PLAYER 1.5 выбирает декодер (AAC — AAC.ELF, OGG и OGA —
        ; OGG.ELF, остальное — MP3.ELF).
        xor a
        ld (ModOpen),a
        ld hl,(Mp3Total)
        ld (ModOpen+1),hl
        ld hl,(Mp3Total+2)
        ld (ModOpen+3),hl
        ; Шестой байт — для журнала ESP: перемотка, свой файл найден, метка.
        ld a,(Mp3HasTag)
        add a,a
        ld hl,EspReopened
        or (hl)
        add a,a
        ld hl,Mp3CanSeek
        or (hl)
        ld (ModOpen+5),a
        ld hl,Mp3Ext
        ld de,ModOpen+6
        ld bc,3
        ldir
        ld a,CMD_AUDIO_OPEN
        ld hl,ModOpen
        ld bc,9
        call Proto_Send
        ld a,RESP_AUDIO_OPEN
        ld de,MP3_WAIT
        call Mp3_Wait
        jp c,Mp3_NoAnswer
        ld a,(ProtoBuf)
        or a
        ld hl,UiErrorAudioOff
        jp z,Mp3_Fatal
        call Mp3_TakeFree
        call Ui_Screen                   ; окно или экран Tang — по ответу
        call Cover_Begin
        ld hl,UiStatePlaying
        ld (Mp3PhaseText),hl
        call Mp3_Refresh                 ; громкость и первая строка состояния

.loop:
        call Ui_Esc
        jp nz,.stop
        call Mp3_TrackKeys               ; раньше Mp3_Keys: см. Mp3_TrackKeys
        jp nz,.skip
        call Mp3_Keys
        call Mp3_SeekKeys
        jp nz,.skip
        ; Непринятый блок уходит повторно, новый с карты не читается.
        ld hl,(Mp3BlockLen)
        ld a,h
        or l
        jr nz,.send
        ; Файл отдан целиком?
        ld hl,(Mp3Left)
        ld de,(Mp3Left+2)
        ld a,h
        or l
        or d
        or e
        jp z,.finish
        ; Блок уходит, только если он точно поместится в очереди ESP.
        ld hl,(Mp3Free+2)
        ld a,h
        or l
        jr nz,.read
        ld hl,Mp3SendSize+1
        ld a,(Mp3Free+1)
        cp (hl)
        jr c,.wait
.read:
        call Mp3_ReadBlock
        jp c,Mp3_CardError
.send:
        call Mp3_SendBlock
        jp c,Mp3_NoAnswer
        ld a,RESP_AUDIO_DATA
        ld de,MP3_WAIT
        call Mp3_Wait
        jp c,Mp3_NoAnswer
        call Mp3_TakeFree
        ld a,(ProtoBuf)
        cp 1
        jr z,.accepted
        cp 2
        ld hl,UiErrorClosed
        jp z,Mp3_Fatal
        ; 0 — места не хватило: тот же блок уйдёт после паузы.
.wait:
        ; Очередь полна: время досылать обложку. Клавиши — каждый проход.
        call Cover_Step
        jp nz,.loop
        ei
        halt
        call Mp3_Refresh
        jp c,Mp3_NoAnswer
        jp .loop
.accepted:
        call Mp3_Account
        ld hl,Mp3Blocks
        inc (hl)
        ld a,(hl)
        ld hl,Mp3StatusMask
        and (hl)
        jp nz,.loop
        call Mp3_Refresh
        jp c,Mp3_NoAnswer
        jp .loop

.finish:
        ; Файл отдан: ESP доигрывает накопленное, плагин ждёт конца.
        ld hl,UiStateFinishing
        ld (Mp3PhaseText),hl
        call Ui_SetState
        ld a,1
        call Mp3_Close
        jp c,Mp3_NoAnswer
        jp Mp3_WaitEnd

.tag_stop:
        ld a,(Mp3TagError)
        or a
        jp nz,Mp3_CardError
        jr .stop
.skip:
        ; PgDn или PgUp: A — код возврата. Звук обрывается сразу.
        call Mp3_SetExit
.stop:
        xor a
        call Mp3_Close
        jp Mp3_Exit

; SD читается по MP3_READ_SIZE байтов в свободную область страницы 1.
; ESP получает порции <=8192 (raw DMA) или <=1024 (старый протокол) без копии.
; Блок, не принятый ESP, остаётся в Mp3BlockPtr/Len для повторной отправки.
; Выход: Mp3BlockPtr, Mp3BlockLen; CF=1 — карта не прочиталась.
Mp3_ReadBlock:
        ld hl,(Mp3ReadLeft)
        ld a,h
        or l
        jr nz,.cached
        ld hl,(Mp3Left)
        ld de,(Mp3Left+2)
        ld a,d
        or e
        jr nz,.full
        ld a,h
        cp high MP3_READ_SIZE
        jr c,.sized
.full:
        ld hl,MP3_READ_SIZE
.sized:
        ld (Mp3ReadLeft),hl
        ; Округлить до сектора; последний неполный сектор отправляется точно
        ; по длине файла. Страница подключается штатным API, с тенью PAGE3.
        dec hl
        ld a,h
        srl a
        inc a
        ld b,a
        ld hl,Mp3ReadBuffer
        ld (Mp3ReadPtr),hl
        call Page1_Map
        call WC_LOAD512
        ei                              ; файловый драйвер мог вернуть DI
        ret c
        ld hl,(Mp3ReadLeft)
.cached:
        ld de,(Mp3SendSize)
        or a
        sbc hl,de
        jr nc,.part
        add hl,de
        ex de,hl                        ; DE — хвост меньше одного пакета
        ld hl,0
.part:
        ld (Mp3ReadLeft),hl
        ld (Mp3BlockLen),de
        ld hl,(Mp3ReadPtr)
        ld (Mp3BlockPtr),hl
        add hl,de
        ld (Mp3ReadPtr),hl
        or a
        ret

Fast_Probe:
        call Page1_Call
        dw P1.Fast_Probe

Mp3_SendBlock:
        ld a,(FastMedia)
        or a
        jr z,.legacy
        call Page1_Call
        dw P1.Fast_Audio
.legacy:
        call Mp3_MapBlock
        ld a,CMD_AUDIO_DATA
        ld hl,(Mp3BlockPtr)
        ld bc,(Mp3BlockLen)
        jp Proto_Send

; Хвост ID3/перемотки — в странице 0; прочие блоки — в странице 1.
Mp3_MapBlock:
        ld a,(Mp3BlockPtr+1)
        cp #C0
        ret c
        jp Page1_Map

; Пропустить HL секторов потока: LOADNONE (API 61) двигает его, не читая
; данных. CF=1 — ошибка карты.
Mp3_SkipSectors:
        ld a,h
        or l
        ret z
        ld b,255
        ld a,h
        or a
        jr nz,.chunk
        ld a,l
        cp b
        jr nc,.chunk
        ld b,a
.chunk:
        ld e,b
        ld d,0
        or a
        sbc hl,de
        push hl
        ld hl,Mp3Block                   ; данных LOADNONE не пишет; HL — как у LOAD512
        ld a,FN_LOADNONE
        call Wc_Api
        pop hl
        ret c
        jr Mp3_SkipSectors

; Пропустить A:HL секторов: по 65536 (два раза по #8000), остаток — Mp3_SkipSectors.
; CF=1 — ошибка карты.
Mp3_SkipSectorsLong:
        or a
        jp z,Mp3_SkipSectors
.round:
        push af
        push hl
        ld hl,#8000
        call Mp3_SkipSectors
        ld hl,#8000
        call nc,Mp3_SkipSectors
        pop hl
        jr c,.fail
        pop af
        dec a
        jr nz,.round
        jp Mp3_SkipSectors
.fail:
        pop af
        scf
        ret

; Поток 0 — на начало своего файла (FENTRY по имени 8.3, GFILE). CF=1 — не нашёлся.
Mp3_OwnStart:
        call S0_Select
        ld hl,Mp3FentryName
        call WC_FENTRY
        scf
        ret z
        call WC_GFILE
        or a
        ret

; Звук — с места Mp3TagLen + Mp3Sent (после перемотки): поток 0 на начало
; своего файла, LOADNONE до сектора с этим местом, хвост сектора — первым
; блоком (файлы до 32 МиБ). CF=1 — свой файл не нашёлся или не прочитался.
Mp3_Reposition:
        ld hl,(Mp3TagLen)
        ld bc,(Mp3Sent)
        add hl,bc
        ld (Mp3SeekPos),hl
        ld hl,(Mp3TagLen+2)
        ld bc,(Mp3Sent+2)
        adc hl,bc
        ld (Mp3SeekPos+2),hl
        call Mp3_OwnStart
        ret c
        ld hl,(Mp3SeekPos+1)
        ld a,(Mp3SeekPos+3)
        srl a
        rr h
        rr l                             ; A:HL — номер сектора (прежде брались
        call Mp3_SkipSectorsLong         ; 16 битов: дальше 32 МиБ — в начало)
        ret c
        ld a,(Mp3SeekPos+1)
        and 1
        ld b,a
        ld a,(Mp3SeekPos)
        ld c,a
; Хвост сектора с байта BC (0..511) — первым блоком: сектор читается в
; Mp3Block, хвост переносится в начало, длина — не больше остатка файла. На
; границе сектора (BC=0) блока нет. CF=1 — сектор не прочитался.
Mp3_TailBlock:
        ld hl,0
        ld (Mp3ReadLeft),hl            ; перемотка отменяет упреждающее чтение
        ld (Mp3BlockLen),hl
        ld a,b
        or c
        ret z
        push bc
        ld hl,Mp3Block
        ld b,1
        call WC_LOAD512
        pop bc
        ret c
        ld hl,512
        or a
        sbc hl,bc                        ; байтов от места до конца сектора
        ld de,(Mp3Left+2)
        ld a,d
        or e
        jr nz,.sized
        ld de,(Mp3Left)
        push hl
        or a
        sbc hl,de
        pop hl
        jr c,.sized
        ex de,hl                         ; файл кончается раньше конца сектора
.sized:
        ld (Mp3BlockLen),hl
        push hl
        ld hl,Mp3Block
        add hl,bc
        ld de,Mp3Block
        pop bc
        ld a,b
        or c
        jr z,.moved
        ldir
.moved:
        ld hl,Mp3Block
        ld (Mp3BlockPtr),hl
        or a
        ret

; Блок принят: осталось меньше, отдано больше.
Mp3_Account:
        ld bc,(Mp3BlockLen)
        ld hl,(Mp3Left)
        or a
        sbc hl,bc
        ld (Mp3Left),hl
        jr nc,.left_done
        ld hl,(Mp3Left+2)
        dec hl
        ld (Mp3Left+2),hl
.left_done:
        ld hl,(Mp3Sent)
        add hl,bc
        ld (Mp3Sent),hl
        jr nc,.sent_done
        ld hl,(Mp3Sent+2)
        inc hl
        ld (Mp3Sent+2),hl
.sent_done:
        ld hl,0
        ld (Mp3BlockLen),hl
        ret

; Ждать, пока ESP доиграет (поток закрыт, модуль играет до конца песни):
; клавиши — каждый кадр, состояние — пять раз в секунду. Потом — следующий файл.
Mp3_WaitEnd:
        ld a,1
        ld (Mp3DrainTick),a
.drain:
        ei
        halt
        call Ui_Esc
        jr nz,.stop
        call Mp3_TrackKeys               ; раньше Mp3_Keys: см. Mp3_TrackKeys
        jr nz,.skip
        call Mp3_Keys
        call Mp3_SeekKeys
        jr nz,.skip
        ; Перемотка потока удалась (PLAYER 1.7 снова открыл поток): файл с нового
        ; места ещё не отдан — обратно к передаче. Только поток (MP3, WAV...):
        ; у модуля, отвергнутого посреди приёма, остаток тоже не ноль, но слать
        ; его уже нельзя — ESP пропускает файл сама.
        ld a,(Mp3Kind)
        or a                             ; KIND_MP3
        jr nz,.no_resume
        ld hl,(Mp3Left)
        ld a,h
        or l
        ld hl,(Mp3Left+2)
        or h
        or l
        jr nz,.resume
.no_resume:
        call Cover_Step                  ; кусок обложки за кадр, если она ещё идёт
        ld hl,Mp3DrainTick
        dec (hl)
        jr nz,.drain
        ld (hl),MP3_DRAIN_EVERY
        call Mp3_Refresh                 ; и метки, если сменились (радио: песня)
        jp c,Mp3_NoAnswer
        ld a,(Mp3Status+ST_STATE)
        cp AUDIO_PLAYING
        jr z,.drain
        cp AUDIO_DRAINING
        jr z,.drain
        ; Файл доигран: дальше — следующий файл панели.
        ld a,EXIT_NEXT
        call Mp3_SetExit
        jp Mp3_Exit
.resume:
        ld hl,UiStatePlaying
        ld (Mp3PhaseText),hl
        call Ui_SetState
        jp Mp3_Play.loop
.skip:
        call Mp3_SetExit
.stop:
        xor a
        call Mp3_Close
        jp Mp3_Exit

; --- модуль и MIDI: файл целиком, затем звук ----------------------------------

Mod_Play:
        ld hl,UiHintModule
        ld a,(Mp3Kind)
        cp KIND_MIDI
        jr c,.hint
        ld hl,UiHintPlay                 ; MIDI и OPL перематываются на 10 с
.hint:
        call Ui_SetHint
        ; Длину решает ESP: IT она берёт потоком любой длины, прочее — до
        ; 6 МиБ (MODULE_MAX в PLAYER.ELF); не возьмёт — откажет в AUDIO_OPEN.
        ld hl,UiStageLoad
        ld (Mp3PhaseText),hl
        call Ui_SetState
        call Ui_Draw
        ; AUDIO_OPEN [#80 или #C0 — MIDI][длина LE32][расширение]: по
        ; расширению ESP узнаёт IMF (у него нет подписи).
        ld a,(Mp3Kind)
        cp KIND_MIDI
        ld a,OPEN_MODULE
        jr nz,.flags
        or OPEN_MIDI
.flags:
        ld (ModOpen),a
        ld hl,(Mp3Total)
        ld (ModOpen+1),hl
        ld hl,(Mp3Total+2)
        ld (ModOpen+3),hl
        ld hl,Mp3Ext
        ld de,ModOpen+5
        ld bc,3
        ldir
        ld a,CMD_AUDIO_OPEN
        ld hl,ModOpen
        ld bc,8
        call Proto_Send
        jp c,Mp3_NoAnswer
        ld a,RESP_AUDIO_OPEN
        ld de,MP3_WAIT
        call Mp3_Wait
        jp c,Mp3_NoAnswer
        call Ui_Screen                   ; окно или экран Tang — по ответу
        ld a,(ProtoBuf)
        or a
        jr nz,.load
        ; Места нет и после ресурсов (их PLAYER отпускает сам): то, что держит
        ; сам PLAYER (его блоки лежат где придётся и дробят PSRAM), — прочь
        ; перезапуском приложения, временные буферы прошивки — паузой, и ещё раз.
        ld hl,UiErrorModOpen
        ld a,(EspAppRunning)
        or a
        jp z,Mp3_Fatal                   ; встроенный плеер: перезапускать нечего
        ld a,(ModRoomTried)
        or a
        jp nz,Mp3_Fatal
        inc a
        ld (ModRoomTried),a
        call Esp_Restart
        ld hl,UiErrorModOpen
        ld a,(EspAppRunning)
        or a
        jp z,Mp3_Fatal
        ld b,MOD_ROOM_FRAMES
.room:
        ei
        halt
        djnz .room
        jp Mod_Play
.load:
        call Ui_Esc
        jp nz,.stop
        call Mp3_TrackKeys
        jp nz,.skip
        ld hl,(Mp3Left)
        ld de,(Mp3Left+2)
        ld a,h
        or l
        or d
        or e
        jr z,.loaded
        call Mp3_ReadBlock
        jp c,Mp3_CardError
        call Mp3_SendBlock
        jp c,Mp3_NoAnswer
        ld a,RESP_AUDIO_DATA
        ld de,MP3_WAIT
        call Mp3_Wait
        jp c,Mp3_NoAnswer
        ld a,(ProtoBuf)
        cp 2                             ; ESP файл уже отвергла (IT потоком, PLAYER 1.2):
        jr z,.loaded                     ; дальше не слать — она пропускает его сама
        cp 1
        ld hl,UiErrorClosed
        jp nz,Mp3_Fatal                  ; модуль ESP берёт всегда, пока не набран
        call Mp3_Account
        ld hl,Mp3Blocks
        inc (hl)
        ld a,(hl)
        ld hl,Mp3StatusMask
        and (hl)
        jr nz,.load
        call Ui_ShowLoad
        jr .load
.loaded:
        call Ui_ShowLoad
        ; Файл у ESP целиком: она разбирает модуль. Ждать, пока это кончится.
        ld hl,UiStageStart
        call Ui_SetState
        call Ui_Draw
        ld b,MOD_START_FRAMES
.start:
        push bc
        ei
        halt
        call Mp3_Refresh
        pop bc
        jp c,Mp3_NoAnswer
        ld a,(Mp3Status+ST_STATE)
        cp AUDIO_LOADING
        jr nz,.started
        djnz .start
        ld hl,UiErrorModSlow
        jp Mp3_Fatal
.started:
        cp AUDIO_PLAYING
        ld hl,UiErrorModFormat
        jp nz,Mp3_Fatal                  ; ESP модуль не разобрала
        ; Декодеру OPL понадобилось ПЗУ OPL4 (VGM для MoonSound): прислать его
        ; из /WC/ESP и открыть файл заново (один раз).
        ld a,(Mp3Status+ST_FLAGS)
        and ST_F_WANTS_ROM
        jr z,.playing
        ld hl,ModRomTried
        ld a,(hl)
        or a
        jr nz,.playing
        inc (hl)
        call Esp_LoadRom
        jr c,.as_is
        ld hl,(Mp3Total)
        ld (Mp3Left),hl
        ld hl,(Mp3Total+2)
        ld (Mp3Left+2),hl
        ld hl,0
        ld (Mp3Sent),hl
        ld (Mp3Sent+2),hl
        jp Mod_Play
.as_is:
        ld a,(EspWhy)
        cp WHY_ESC
        jp z,.stop
.playing:
        call Ui_FetchTags                ; название модуля
        ld hl,UiStatePlaying
        ld (Mp3PhaseText),hl
        call Mp3_Refresh
        jp c,Mp3_NoAnswer
        jp Mp3_WaitEnd
.skip:
        call Mp3_SetExit
.stop:
        xor a
        call Mp3_Close
        jp Mp3_Exit

; Название модуля и его длина (AUDIO_INFO): [формат][каналы][длина LE16][название, 0].
; Выход: CF=1 — ESP не ответила.
Mod_Info:
        ld a,CMD_AUDIO_INFO
        call Proto_SendEmpty
        ret c
        ld a,RESP_AUDIO_INFO
        ld de,MP3_WAIT
        call Mp3_Wait
        ret c
        ld hl,ProtoBuf+4
        ld de,UiSongTitle
        ld bc,UI_TITLE
.copy:
        ld a,(hl)
        ld (de),a
        or a
        ret z                            ; CF=0
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,.copy
        xor a
        ld (de),a
        ret

; --- клавиши -----------------------------------------------------------------

; Перемотка влево и вправо. Опрос не чаще раза за кадр, как требует автоповтор WC.
; Выход: NZ и A — код возврата в WC (перемотка за конец файла — следующий файл);
; иначе Z.
Mp3_SeekKeys:
        ld a,(WC_FRAMES)
        ld hl,Mp3SeekFrame
        cp (hl)
        jr z,.none
        ld (hl),a
        ld a,FN_LEFT
        call Wc_Api
        ld c,-1
        jr nz,.seek
        ld a,FN_RIGHT
        call Wc_Api
        ld c,1
        jr z,.none
.seek:
        ld a,(Mp3Kind)
        or a                             ; модуль и MIDI перематывает ESP
        jr nz,.module
        ld a,c
        jp Mp3_Seek
.module:
        ld a,c
        ld (Mp3Arg),a
        ld a,CMD_AUDIO_SEEK
        ld hl,Mp3Arg
        ld bc,1
        call Proto_Send
        jr c,.none
        ld a,RESP_AUDIO_SEEK
        ld de,MP3_WAIT
        call Mp3_Wait
        call Mp3_Refresh
.none:
        xor a
        ret

; Перемотка MP3 на MP3_SEEK_SECONDS: A=-1 — назад, A=1 — вперёд. Новое место
; в файле — время, умноженное на поток (кбит/с * 125 = байт/с): у постоянного
; потока точно, у переменного приблизительно.
; Выход: NZ и A=EXIT_NEXT — перемотка за конец файла; иначе Z.
Mp3_Seek:
        ld c,a
        ld a,(Mp3CanSeek)
        or a
        ret z
        ld hl,(Mp3Status+ST_BITRATE)
        ld a,h
        or l
        ret z                            ; поток ещё не распознан
        ld hl,(Mp3Status+ST_SECONDS)
        ld de,MP3_SEEK_SECONDS
        bit 7,c
        jr nz,.back
        add hl,de
        jr .time
.back:
        or a
        sbc hl,de
        jr nc,.time
        ld hl,0
.time:
        ld (Mp3SeekSec),hl
        ; Байт: секунды * кбит/с * 125.
        ld de,(Mp3Status+ST_BITRATE)
        call Mul16                       ; DE:HL = HL * DE
        ld a,125
        call Mul32x8
        ; За концом файла — следующий файл.
        push hl
        push de
        ld bc,(Mp3Total)
        or a
        sbc hl,bc
        ex de,hl
        ld bc,(Mp3Total+2)
        sbc hl,bc
        pop de
        pop hl
        jr c,.inside
        ld a,EXIT_NEXT
        or a                             ; NZ
        ret
.inside:
        ld (Mp3Target),hl
        ld (Mp3Target+2),de
        ; AUDIO_FLUSH [время, мс LE32][место, байт LE32]: ESP выбрасывает
        ; очередь; по месту WAV встаёт на начало отсчёта.
        ld hl,(Mp3SeekSec)
        ld de,1000
        call Mul16
        ld (Mp3FlushMs),hl
        ld (Mp3FlushMs+2),de
        ld a,CMD_AUDIO_FLUSH
        ld hl,Mp3FlushMs
        ld bc,8                          ; и Mp3Target следом
        call Proto_Send
        jr c,.done
        ld a,RESP_AUDIO_FLUSH
        ld de,MP3_WAIT
        call Mp3_Wait
        jr c,.done
        ld a,(ProtoBuf)
        or a
        jr z,.done                       ; ESP не перемотала: всё как было
        call Mp3_TakeFree
        ; С нового места: отдано — до него, остаток — после.
        ld hl,(Mp3Target)
        ld (Mp3Sent),hl
        ld de,(Mp3Target+2)
        ld (Mp3Sent+2),de
        ld hl,(Mp3Total)
        ld bc,(Mp3Target)
        or a
        sbc hl,bc
        ld (Mp3Left),hl
        ld hl,(Mp3Total+2)
        sbc hl,de
        ld (Mp3Left+2),hl
        call Mp3_Reposition              ; поток 0 — на новое место
        jr c,.lost
        call Mp3_Refresh
.done:
        xor a
        ret
.lost:
        ; Свой файл больше не находится: дальше не прочитать — к следующему.
        xor a
        ld (Mp3CanSeek),a
        ld a,EXIT_NEXT
        or a                             ; NZ
        ret

; DE:HL = HL * DE (16 x 16 бит). Портит AF, BC.
Mul16:
        ld b,h
        ld c,l                           ; BC — множимое
        ld hl,0
        ld a,16
.bit:
        add hl,hl
        rl e
        rl d
        jr nc,.skip
        add hl,bc
        jr nc,.skip
        inc de
.skip:
        dec a
        jr nz,.bit
        ret

; DE:HL = DE:HL * A (32 x 8 бит, младшие 32 бита). Портит AF, BC.
Mul32x8:
        ld (MulFactor),hl
        ld (MulFactor+2),de
        ld c,a                           ; множитель
        ld hl,0
        ld de,0
        ld b,8
.bit:
        add hl,hl                        ; результат <<= 1
        rl e
        rl d
        sla c
        jr nc,.next
        push bc                          ; результат += множимое
        ld bc,(MulFactor)
        add hl,bc
        ex de,hl
        ld bc,(MulFactor+2)
        adc hl,bc
        ex de,hl
        pop bc
.next:
        djnz .bit
        ret

MulFactor:      ds 4

; Запомнить код возврата A: следующий или предыдущий файл. Переход WC делает
; только при вызове по расширению; за последним объектом панели следующего нет.
Mp3_SetExit:
        ld c,a
        ld a,(Mp3Call)
        or a                             ; CALL_EXTENSION
        ret nz
        ld a,c
        cp EXIT_NEXT
        jr nz,.store
        ld hl,(Mp3Index)
        ld de,(Mp3Count)
        or a
        sbc hl,de
        ret nc
.store:
        ld a,c
        ld (Mp3ExitCode),a
        ret

; Клавиши трека: пробел — пауза и обратно, PgDn и PgUp — соседний файл панели
; (у радио — соседняя станция: AUDIO_SEEK, автоповтор листает).
; Опрос не чаще раза за кадр, как требует автоповтор WC. Вызывать раньше
; Mp3_Keys: у WC событие нажатия одно на всех (общий счётчик автоповтора,
; PS2P.ASM) — его получает первая спросившая функция, и KBSCN в Ui_Keys (H, I)
; забирал нажатие пробела: короткое нажатие терялось.
; Выход: NZ и A — код возврата в WC, если пора уходить на другой файл; иначе Z.
Mp3_TrackKeys:
        ld a,(WC_FRAMES)
        ld hl,Mp3TrackFrame
        cp (hl)
        jr z,.none
        ld (hl),a
        ld a,FN_PGDN
        call Wc_Api
        ld c,EXIT_NEXT
        jr nz,.track
        ld a,FN_PGUP
        call Wc_Api
        ld c,EXIT_PREV
        jr nz,.track
        ld a,FN_SPACE
        call Wc_Api
        jr z,.none
        ld a,(Mp3Paused)
        xor 1
        ld (Mp3Paused),a
        ld (Mp3Arg),a
        ld a,CMD_AUDIO_PAUSE
        ld hl,Mp3Arg
        ld bc,1
        call Proto_Send
        jr c,.undo
        ld a,RESP_AUDIO_PAUSE
        ld de,MP3_WAIT
        call Mp3_Wait
        jr nc,.shown
.undo:
        ; ESP не ответила (прошивка без паузы): состояние прежнее.
        ld a,(Mp3Paused)
        xor 1
        ld (Mp3Paused),a
.shown:
        call Mp3_PickState
        call Ui_Draw
        ld a,FN_WAITUP                   ; одно нажатие — одно переключение
        call Wc_Api
.none:
        xor a
        ret
.track:
        ld a,(Mp3Kind)
        cp KIND_RADIO
        jr nz,.panel
        ld a,3                           ; радио: PgDn (EXIT_NEXT 2) — +1, PgUp (4) — −1
        sub c
        ld c,a
        jp Mp3_SeekKeys.module
.panel:
        ; Дождаться отпускания: иначе плагин, вызванный на соседнем файле,
        ; увидит ту же клавишу и уйдёт дальше.
        push bc
        ld a,FN_WAITUP
        call Wc_Api
        pop bc
        ld a,c
        or a                             ; NZ, A — код возврата
        ret

; Текст строки состояния: пауза и набор очереди важнее названия этапа.
Mp3_PickState:
        ld hl,UiStatePaused
        ld a,(Mp3Paused)
        or a
        jr nz,.set
        ld hl,UiStateBuffering
        ld a,(Mp3Kind)
        or a
        jr nz,.phase                     ; у файла целиком очереди нет
        ld a,(Mp3Status+ST_FLAGS)
        and ST_F_BUFFERING
        jr nz,.set
.phase:
        ld hl,(Mp3PhaseText)
.set:
        jp Ui_SetState

; Ответ ESP с кодом A, не дольше DE опросов (как Proto_WaitCmd), но отказ
; ESP (RESP_ERROR: «unsupported», ошибка) — сразу CF=1 и ProtoErr=1. Старая
; прошивка многих команд не знает (AUDIO_VIEW, AUDIO_TAGS, обложка, модули):
; ждать до конца — секунды на каждую. Выход: CF=0 — ответ в ProtoBuf.
Mp3_Wait:
        ld (ProtoWant),a
.loop:
        push de
        call Proto_Poll
        pop de
        jr nc,.idle
        ld hl,ProtoWant
        cp (hl)
        ret z                            ; CF=0 (CP равных)
        cp RESP_ERROR
        jr nz,.idle
        ld a,1
        ld (ProtoErr),a
        call Proto_SaveErr
        scf
        ret
.idle:
        dec de
        ld a,d
        or e
        jr nz,.loop
        scf
        ret

; Закрыть поток: A=1 — доиграть накопленное, A=0 — оборвать.
; Выход: CF=1 — ESP не ответила.
Mp3_Close:
        ld (Mp3Arg),a
        ld a,CMD_AUDIO_CLOSE
        ld hl,Mp3Arg
        ld bc,1
        call Proto_Send
        ret c
        ld a,RESP_AUDIO_CLOSE
        ld de,MP3_WAIT
        jp Mp3_Wait

; Свободное место очереди из ответа AUDIO_OPEN / AUDIO_DATA / AUDIO_FLUSH:
; [статус][LE32].
Mp3_TakeFree:
        ld hl,(ProtoBuf+1)
        ld (Mp3Free),hl
        ld hl,(ProtoBuf+3)
        ld (Mp3Free+2),hl
        ret

; Спросить состояние звука и показать его. В теле запроса — что знает только плагин
; (имя файла, номер, окно поверх, строка состояния): по нему PLAYER 1.21 ведёт экран
; оверлеем Tang (Ui_PackBlock); прежний PLAYER и прошивка тело не читают.
; Выход: CF=1 — ESP не ответила.
Mp3_Refresh:
        call Ui_PackBlock
        ld a,CMD_AUDIO_STATUS
        call Proto_Send
        ret c
        ld a,RESP_AUDIO_STATUS
        ld de,MP3_WAIT
        call Mp3_Wait
        ret c
        ; Следующий кадр затрёт ProtoBuf, а запись нужна и между запросами.
        ; Прошивка до 0.1.20 присылает 24 байта: остальное — нули.
        ld hl,Mp3Status
        ld de,Mp3Status+1
        ld bc,ST_SIZE-1
        ld (hl),0
        ldir
        ld bc,(ProtoRxLen)
        ld a,b
        or a
        jr nz,.clamp
        ld a,c
        cp ST_SIZE+1
        jr c,.sized
.clamp:
        ld bc,ST_SIZE
.sized:
        ld a,b
        or c
        jr z,.copied
        ld hl,ProtoBuf
        ld de,Mp3Status
        ldir
.copied:
        ld a,1
        ld (UiStatusSeen),a
        ld hl,(Mp3Status+ST_FREE)
        ld (Mp3Free),hl
        ld hl,(Mp3Status+ST_FREE+2)
        ld (Mp3Free+2),hl
        call Mp3_PickState
        call Ui_Track                    ; экран Tang показан — окна нет; не появился — окно
        call Ui_ShowStatus
        ; Флаг 32 — метки сменились: у радио — песня или станция, у файла —
        ; название от декодера (комментарии Vorbis, LIST INFO у WAV; PLAYER 1.5).
        ld a,(Mp3Status+ST_FLAGS)
        and ST_F_TAGS
        call nz,Ui_FetchTags
        ld a,(Mp3Status+ST_FLAGS)
        and ST_F_COVER
        call nz,Cover_FromDecoder
        or a
        ret

; Клавиши громкости: не чаще раза за кадр, как требует автоповтор WC.
Mp3_Keys:
        ld a,(WC_FRAMES)
        ld hl,Mp3Frame
        cp (hl)
        ret z
        ld (hl),a
        call Ui_Keys                     ; H — справка, I — подробности
        ld a,FN_UP
        call Wc_Api
        jr nz,.up
        ld a,FN_DOWN
        call Wc_Api
        ret z
        ld a,(Mp3Status+ST_VOLUME)
        sub MP3_VOLUME_STEP
        jr nc,.set
        xor a
        jr .set
.up:
        ld a,(Mp3Status+ST_VOLUME)
        add a,MP3_VOLUME_STEP
        jr nc,.set
        ld a,255
.set:
        ld (Mp3Status+ST_VOLUME),a
        ld (Mp3Arg),a
        ld a,CMD_AUDIO_VOLUME
        ld hl,Mp3Arg
        ld bc,1
        call Proto_Send
        ret c
        ld a,RESP_AUDIO_VOLUME
        ld de,MP3_WAIT
        call Mp3_Wait
        jp Ui_VolumeChanged

; Метка ID3v2 в начале файла: обложка в ней бывает в мегабайты, ESP она не
; нужна, а по SPI шла бы десятки секунд тишины. Первый сектор читается
; сразу; если в нём начало метки длиннее сектора, её целые сектора
; дочитываются с карты вхолостую, а первым блоком уходит хвост сектора, где
; метка кончается. Короткую метку пропускает сама ESP.
; Выход: Mp3Block, Mp3BlockLen — первый блок к отправке (0 — его нет);
; Mp3Left и Mp3Total — уже без метки. CF=1 — стоп: Mp3TagError=1 — карта не
; прочиталась (прежде ошибки здесь не проверялись, и поток уходил со сдвигом).
Mp3_SkipTag:
        xor a
        ld (Mp3TagError),a
        ld hl,(Mp3Left)
        ld de,(Mp3Left+2)
        ld a,h
        or l
        or d
        or e
        ret z                            ; пустой файл; CF=0
        ld hl,Mp3Block
        ld b,1
        call WC_LOAD512
        jp c,.card
        call Mp3_SectorBytes
        ld (Mp3BlockLen),hl
        ld a,h
        or a
        jr nz,.check
        ld a,l
        cp 10
        jp c,.none                       ; заголовка метки целиком нет
.check:
        ld hl,Mp3Block
        ld a,(hl)
        cp 'I'
        jp nz,.none
        inc hl
        ld a,(hl)
        cp 'D'
        jp nz,.none
        inc hl
        ld a,(hl)
        cp '3'
        jp nz,.none
        ld a,1
        ld (Mp3HasTag),a                 ; метку — и короткую — ESP разберёт ради обложки и названия
        ; Длина: четыре байта по семь значащих битов, старший — первым.
        ld hl,0
        ld de,0
        ld a,(Mp3Block+6)
        call .digit
        jp c,.none
        ld a,(Mp3Block+7)
        call .digit
        jp c,.none
        ld a,(Mp3Block+8)
        call .digit
        jp c,.none
        ld a,(Mp3Block+9)
        call .digit
        jp c,.none
        ld bc,10                         ; сам заголовок метки
        add hl,bc
        jr nc,.header
        inc de
.header:
        ld a,(Mp3Block+5)
        and #10                          ; за меткой ещё 10 байтов подписи
        jr z,.sized
        add hl,bc
        jr nc,.sized
        inc de
.sized:
        ; DE:HL — длина метки. Не длиннее сектора — её пропустит ESP.
        ld a,d
        or e
        jr nz,.long
        ld a,h
        cp 2
        jp c,.none
.long:
        ; Длиннее самого файла метка быть не может: тогда это не метка.
        push hl
        push de
        ld bc,(Mp3Left)
        or a
        sbc hl,bc
        ex de,hl
        ld bc,(Mp3Left+2)
        sbc hl,bc
        pop de
        pop hl
        jp nc,.none
        ld (Mp3TagLen),hl
        ld (Mp3TagLen+2),de
        ; Целых секторов в метке: DE:HL >> 9. Первый уже прочитан.
        ld a,d
        srl a
        jp nz,.none                      ; метка в десятки мегабайтов — не трогаем
        ld a,e
        rra
        ld b,a
        ld a,h
        rra
        ld c,a                           ; BC — секторов целиком
        dec bc
        ld (Mp3TagSectors),bc
        ld hl,UiStateSkipTag
        call Ui_SetState
        call Ui_Draw
        ld hl,(Mp3TagSectors)
        call Mp3_SkipSectors             ; LOADNONE: данные метки не читаются
        jr c,.card

        ; Длина без метки: и то, что осталось отдать, и то, что показывается.
        ld hl,(Mp3Left)
        ld bc,(Mp3TagLen)
        or a
        sbc hl,bc
        ld (Mp3Left),hl
        ld (Mp3Total),hl
        ld hl,(Mp3Left+2)
        ld bc,(Mp3TagLen+2)
        sbc hl,bc
        ld (Mp3Left+2),hl
        ld (Mp3Total+2),hl
        ; Где в секторе метка кончается: хвост этого сектора — первым блоком.
        ld a,(Mp3TagLen+1)
        and 1
        ld b,a
        ld a,(Mp3TagLen)
        ld c,a
        call Mp3_TailBlock
        jr c,.card
        or a                             ; CF=0: продолжать
        ret
.card:
        ld a,1
        ld (Mp3TagError),a
        scf
        ret
.none:
        ; Метки нет (или её пропустит ESP): от начала файла.
        ld hl,0
        ld (Mp3TagLen),hl
        ld (Mp3TagLen+2),hl
        or a                             ; первый сектор — первым блоком
        ret
; A — очередные семь битов; DE:HL = DE:HL << 7 | A. CF=1 — старший бит A
; выставлен: это не длина метки.
.digit:
        cp #80
        ccf
        ret c
        ld c,a
        ld b,7
.shift:
        add hl,hl
        rl e
        rl d
        djnz .shift
        ld a,l
        or c
        ld l,a
        or a
        ret

; HL = min(512, Mp3Left): сколько байтов файла в очередном секторе.
Mp3_SectorBytes:
        ld hl,(Mp3Left+2)
        ld a,h
        or l
        ld hl,512
        ret nz
        ld hl,(Mp3Left)
        push hl
        ld de,512
        or a
        sbc hl,de
        pop hl
        ret c
        ld hl,512
        ret

; --- выход ---------------------------------------------------------------------

; Карта не прочиталась.
Mp3_CardError:
        xor a
        call Mp3_Close
        ld hl,UiErrorCard
        jr Mp3_Fatal

; ESP не ответила на команду. Если она успела прислать причину (#EE) —
; например, прошивка без звука отвечает "unsupported:30", — показать её.
Mp3_NoAnswer:
        ld a,1
        ld (Mp3Silent),a                 ; на выходе ESP уже не спрашивать
        ld hl,UiErrorSilent
        ld a,(ProtoErr)
        or a
        jr z,Mp3_Fatal
        ld hl,ProtoErrText
; HL — сообщение. Показать и ждать Esc.
Mp3_Fatal:
        push hl
        call Ui_Show                     ; ошибка — в окне на Evo
        pop hl
        call Ui_SetState
        call Ui_Draw
.wait:
        ei
        halt
        call WC_ESC
        jr z,.wait
Mp3_Exit:
        call Ui_Close                    ; окно поверх — прочь
        call Ui_Flush                    ; окно — как есть на выходе
        ; Совсем в WC (не к соседнему файлу): экран оверлеем Tang — уйти сразу, а не через 4 с
        ld a,(Mp3ExitCode)
        ld hl,Mp3Silent
        or (hl)
        call z,Mp3_Leave
        call Cover_Hide
        ld ix,Mp3Window
        call WC_RRESB
        call WC_GEDPL                    ; вернуть основной экран WC
        ld bc,#FF00                     ; API 14: restore the original WC clock/cache
        ld a,FN_TURBOPL
        call Wc_Api
        pop ix
        ; 0 — просто выход (панели перечитывать незачем), 2 и 4 — соседний файл.
        ld a,(Mp3ExitCode)
        ret

; Последнее AUDIO_STATUS с флагом ухода: PLAYER 1.21 сразу гасит экран оверлеем Tang.
; Ответ ждётся как обычно; ошибки не важны — без ответа экран уйдёт сам.
Mp3_Leave:
        ld a,UI_F_OPEN|UI_F_LEAVING
        ld (UiBlockFlags),a
        call Ui_PackBlock
        ld a,CMD_AUDIO_STATUS
        call Proto_Send
        ret c
        ld a,RESP_AUDIO_STATUS
        ld de,MP3_WAIT
        jp Mp3_Wait

; --- данные -------------------------------------------------------------------

Mp3Call:        db 0                    ; способ вызова плагина
Mp3Silent:      db 0                    ; ESP перестала отвечать (Mp3_NoAnswer)
Mp3Kind:        db KIND_MP3             ; MP3, модуль или MIDI (по расширению)
Mp3NamePtr:     dw 0                    ; имя файла от WC (BC входа)
Mp3Total:       ds 4                    ; длина файла (у MP3 — без длинной метки)
Mp3Left:        ds 4                    ; ещё не отдано
Mp3Sent:        ds 4                    ; отдано ESP (у MP3 — от начала звука)
Mp3Free:        ds 4                    ; свободно в очереди ESP
Mp3BlockLen:    dw 0                    ; длина блока к отправке; 0 — блока нет
Mp3BlockPtr:    dw Mp3Block             ; страница 0 или Mp3ReadBuffer
Mp3ReadPtr:     dw Mp3ReadBuffer
Mp3ReadLeft:    dw 0                    ; прочитано с SD, ещё не выделено в пакет
Mp3SendSize:    dw MP3_BLOCK_SIZE
Mp3StatusMask:  db MP3_STATUS_EVERY-1
FastMedia:      db 0                    ; link version 2: raw media DMA
FastSectors:    db 0
FastControlLen: db 0
FastWaitLeft:   db 0
EspFastTransfer: db 0
EspTransferSize: dw ESP_CHUNK
Mp3Blocks:      db 0                    ; счётчик блоков для редкого запроса состояния
Mp3Frame:       db 0                    ; кадр последнего опроса клавиш громкости
Mp3Arg:         db 0                    ; однобайтовое тело команды
Mp3Index:       dw 0                    ; номер объекта панели, с которого вызван плагин
Mp3Count:       dw 0                    ; число объектов в панели
TrackNo:        dw 0                    ; «Track N of M»: номер среди файлов плеера
TrackCount:     dw 0                    ; и их число (Track_Count)
TrackSort:      db 0                    ; сортировка панели (IX+#23 при вызове)
Mp3ExitCode:    db 0                    ; код возврата в WC
Mp3Paused:      db 0                    ; 1 — пауза
Mp3TrackFrame:  db 0                    ; кадр последнего опроса клавиш трека
Mp3SeekFrame:   db 0                    ; кадр последнего опроса клавиш перемотки
Mp3DrainTick:   db 0                    ; кадров до следующего запроса состояния
Mp3TagLen:      ds 4                    ; длина пропущенной метки ID3
Mp3HasTag:      db 0                    ; файл начинается с метки ID3 (любой длины)
Mp3TagSectors:  dw 0                    ; сколько её секторов ещё дочитать
Mp3TagError:    db 0                    ; 1 — пропуск метки оборвала ошибка карты
Mp3PhaseText:   dw UiStatePlaying       ; текст состояния текущего этапа
Mp3CanSeek:     db 0                    ; свой файл нашёлся заново: MP3 перематывается
Mp3SeekPos:     ds 4                    ; новое место, байт от начала файла
Mp3FileSize:    ds 4                    ; длина файла целиком (с меткой)
Mp3SeekSec:     dw 0                    ; время нового места, с
Mp3FlushMs:     ds 4                    ; тело AUDIO_FLUSH: время и место (Mp3Target)
Mp3Target:      ds 4                    ; новое место, байт от начала звука
ModOpen:        ds 9                    ; тело AUDIO_OPEN
ModRomTried:    db 0                    ; ПЗУ OPL4 уже просили (файл открыт заново)
ModRoomTried:   db 0                    ; места уже искали перезапуском приложения
Mp3Ext:         ds 3                    ; расширение файла заглавными (добито пробелами)
                db 0
Mp3Status:      ds ST_SIZE              ; последняя запись состояния
Mp3Entry:       ds 32                   ; каталожная запись файла (TENTRY)
Mp3FentryName:  db #00                  ; FENTRY: обычный файл
                ds 13                   ; ИМЯ.РАС и ноль
