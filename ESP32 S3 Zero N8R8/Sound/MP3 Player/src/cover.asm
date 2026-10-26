; Обложка MP3 в окне проигрывателя (Docs/VDAC4.md, «Обложка MP3»).
;
; Длинную метку ID3 (а обложка лежит в ней) плагин пропускает на карте, чтобы
; звук начался сразу. Пока очередь ESP полна, плагин досылает метку фоном:
; AUDIO_COVER [смещение LE32][до 1020 байт] с того места, которое назвала ESP в
; прошлом ответе. ESP проходит заголовки кадров и просит только кадр APIC, так
; что большие кадры метки по SPI не идут. Собрав картинку, ESP сводит JPEG в
; миниатюру 64×64 (4 бита на точку), плагин забирает её AUDIO_THUMB по 512
; байтов и показывает спрайтом TS-Conf поверх текстового экрана WC справа в
; окне. Цвета спрайта в текстовом режиме — 16 цветов палитры текста WC (номер
; палитры спрайта в режиме высокого разрешения не действует, video_out.v
; TS-Conf), под них ESP картинку и сводит.
;
; Картинки в метке нет (или метки нет вовсе) — плагин ищет её в папке трека:
; cover.jpg, folder.jpg, front.jpg. Нашлась — шлёт файл ESP кадрами
; AUDIO_PICTURE, дальше — та же миниатюра. Обложку в самом файле без ID3 (Ogg:
; METADATA_BLOCK_PICTURE) находит декодер: флаг 64 — и плагин сразу ждёт
; миниатюру (Cover_FromDecoder), картинка из папки тогда не нужна.
;
; Метка и картинка читаются потоком 1 WC (S1_Read): у него своя позиция, звук
; идёт потоком 0 подряд и не сбивается. Кусок — от нужного места до конца его
; сектора; вперёд поток 1 двигает LOADNONE (без чтения данных), назад — снова
; с начала файла.
;
; Графика спрайта — в видеостранице плагина COVER_VPAGE (физическая — WC
; считает от своей первой видеостраницы, функции 65 и 71), строки по 256 байтов
; (битовая карта спрайтов 512 точек шириной), спрайт — в её левом верхнем углу
; (tnum 0). Описатели спрайтов пишутся через окно FMADDR (#15AF) в #B000 за
; концом кода (main.asm): запись туда попадает и в ОЗУ под окном — там только
; временная строка окна. Чужую память — например шрифт под #0000 — так не
; задеть.

COVER_BURST     equ 8                   ; кадров обложки подряд, затем кадр — окну и HALT
COVER_POLL      equ 16                  ; кадров между вопросами о миниатюре
COVER_PART      equ 512                 ; байтов миниатюры в ответе AUDIO_THUMB
COVER_PARTS     equ 4
COVER_VPAGE     equ 0                   ; видеостраница плагина под графику спрайта
COVER_COL       equ 2                   ; место миниатюры в окне: слева (ui.asm)
COVER_ROW       equ 2                   ; и строка (8 строк по 8 точек)

COVER_IDLE      equ 0                   ; обложки нет или она не нужна
COVER_SEND      equ 1                   ; метка досылается
COVER_WAIT      equ 2                   ; ESP сводит миниатюру
COVER_SHOWN     equ 3                   ; спрайт на экране
COVER_DECODER   equ 5                   ; FLAC: дождаться разбора всех метаданных
COVER_FILE      equ 4                   ; досылается картинка из папки трека

PICTURE_MAX_E   equ #10                 ; длина картинки — меньше 1 МиБ (столько берёт ESP)

; Порты TS-Config и функции WC для графики.
TS_CONFIG       equ #06AF               ; бит 7 — спрайты (S_EN)
TS_FMADDR       equ #15AF               ; бит 4 — окно CRAM/SFILE, биты 3..0 — A15..A12
TS_PAGE3        equ #13AF               ; страница окна #C000
FN_MNGCVPL      equ 65                  ; видеостраница плагина A' -> окно #C000
FN_GVSGP        equ 71                  ; C — видеостраница плагина под графику спрайтов
SPR_ACT_LEAP_64 equ #6E                 ; старший байт R0: LEAP, ACT, высота 64
SPR_SIZE_64     equ #0E                 ; старший байт R1: ширина 64

; После пропуска метки: досылать ли её ESP. Нужны метка ID3 (любой длины:
; из неё и обложка, и название с исполнителем) и свой файл, найденный заново.
Cover_Begin:
        xor a
        ld (CoverState),a
        ld a,(Mp3CanSeek)
        or a
        ret z
        ; FLAC может содержать большую PICTURE перед первым звуковым кадром.
        ; Поиск в папке — только после её разбора, чтобы не заменить её файлом.
        ld a,(Mp3Ext)
        cp 'F'
        jr nz,.id3
        ld a,COVER_DECODER
        ld (CoverState),a
        ret
.id3:
        ld a,(Mp3HasTag)
        or a
        jp z,Picture_Find                ; метки нет — картинка разве что в папке
        ld hl,0
        ld (CoverWant),hl
        ld (CoverWant+2),hl
        ld a,COVER_BURST
        ld (CoverBurst),a
        ld a,COVER_SEND
        ld (CoverState),a
        ret

; Шаг обложки, когда очереди звука хватает. Выход: NZ — кадр обложки ушёл
; (вызывающему — сразу к звуку и клавишам); Z — работы нет или пора отдать кадр
; окну и HALT.
Cover_Step:
        ld a,(CoverState)
        cp COVER_SEND
        jr z,.send
        cp COVER_WAIT
        jp z,Cover_Poll
        cp COVER_FILE
        jp z,Picture_Step
        cp COVER_DECODER
        jr z,.decoder
        xor a
        ret
.decoder:
        ld a,(Mp3Status+ST_FLAGS)
        bit 6,a
        jp nz,Cover_FromDecoder
        and 2                           ; первый PCM: все метаданные FLAC уже прочитаны
        ret z
        jp Picture_Find
.send:
        ld hl,CoverBurst
        dec (hl)
        jr nz,.go
        ld (hl),COVER_BURST
        xor a
        ret
.go:
        ; Кусок метки с места, которое просит ESP, — потоком 1.
        ld hl,Mp3FentryName
        ld de,Mp3FileSize
        call S1_File
        ld hl,CoverWant
        call S1_Read
        jp c,Cover_Stop                  ; свой файл не открылся: обложки не будет
        ld (ProtoTail),hl
        ld (ProtoTailLen),bc             ; 0 — конец файла: ESP так и узнает
        xor a
        ld (ProtoHold),a
        ld a,CMD_AUDIO_COVER
        ld hl,CoverWant
        ld bc,4
        call Proto_Send2
        jp c,Cover_Stop
        ld a,RESP_AUDIO_COVER
        ld de,MP3_WAIT
        call Mp3_Wait
        jp c,Cover_Stop                  ; прошивка без обложки отвечает отказом
        ; [итог][откуда дальше LE32][свободно в очереди звука LE32]
        ld hl,(ProtoBuf+1)
        ld (CoverWant),hl
        ld hl,(ProtoBuf+3)
        ld (CoverWant+2),hl
        ld hl,(ProtoBuf+5)
        ld (Mp3Free),hl
        ld hl,(ProtoBuf+7)
        ld (Mp3Free+2),hl
        ld a,(ProtoBuf)
        or a
        jr z,.sent
        ; Метка пройдена: название, исполнитель, альбом, год — в окно.
        push af
        call Ui_FetchTags
        pop af
        dec a
        jr z,.thumb
        call Picture_Find                ; в метке картинки нет — поискать в папке
        or 1                             ; NZ: кадр ушёл
        ret
.thumb:
        ld a,COVER_WAIT
        ld (CoverState),a
        ld a,(WC_FRAMES)
        ld (CoverPollFrame),a
.sent:
        or 1                             ; NZ
        ret

; Картинка в папке трека: FENTRY в потоке 1 (его каталог — папка трека, см.
; Esp_Reopen). Нашлась и не длиннее 1 МиБ — досылать её (COVER_FILE).
Picture_Find:
        xor a
        ld (CoverState),a
        ld (ProtoErr),a
        call S1_Select
        ld hl,PictureNames
.next:
        ld a,(hl)
        cp #FF
        jr z,.done                       ; имена кончились
        ld (PictureName),hl
        push hl
        call WC_FENTRY                   ; NZ — есть, длина в DE:HL
        pop bc
        jr nz,.found
        ld h,b                           ; к следующему [#00][имя][0]
        ld l,c
        inc hl
.skip:
        ld a,(hl)
        inc hl
        or a
        jr nz,.skip
        jr .next
.found:
        ld a,d
        or a
        jr nz,.done                      ; 16 МиБ и больше
        ld a,e
        cp PICTURE_MAX_E
        jr nc,.done
        or h
        jr nz,.sized
        ld a,l
        cp 4
        jr c,.done                       ; пустой файл
.sized:
        ld (PictureSize),hl
        ld (PictureSize+2),de
        ld hl,0
        ld (PictureOff),hl
        ld (PictureOff+2),hl
        ld a,COVER_FILE
        ld (CoverState),a
.done:
        xor a
        ld (S1Open),a                    ; поиск сбил поток 1: файл — заново
        jp S0_Select

; Кусок картинки — потоком 1, кадром AUDIO_PICTURE. Выход: NZ — кадр ушёл.
Picture_Step:
        ld hl,(PictureName)
        ld de,PictureSize
        call S1_File
        ld hl,PictureOff
        call S1_Read
        jr c,.stop                       ; картинка пропала из папки
        ld a,b
        or c
        jr z,.stop                       ; файл короче своей длины
        ld (ProtoTail),hl
        ld (ProtoTailLen),bc
        ; [длина файла LE32][смещение LE32][данные]
        ld hl,(PictureSize)
        ld (PictureHeader),hl
        ld hl,(PictureSize+2)
        ld (PictureHeader+2),hl
        ld hl,(PictureOff)
        ld (PictureHeader+4),hl
        ld hl,(PictureOff+2)
        ld (PictureHeader+6),hl
        xor a
        ld (ProtoHold),a
        ld a,CMD_AUDIO_PICTURE
        ld hl,PictureHeader
        ld bc,8
        call Proto_Send2
        jr c,.stop
        ld a,RESP_AUDIO_PICTURE
        ld de,MP3_WAIT
        call Mp3_Wait
        jr c,.stop                       ; прошивка без AUDIO_PICTURE отвечает отказом
        ; [итог][принято LE32][свободно в очереди звука LE32]
        ld hl,(ProtoBuf+5)
        ld (Mp3Free),hl
        ld hl,(ProtoBuf+7)
        ld (Mp3Free+2),hl
        ld a,(ProtoBuf)
        cp 1
        jr z,.received
        or a
        jr nz,.stop                      ; 2 — ESP не взяла
        ; Принято всего — дальше с этого места.
        ld hl,(ProtoBuf+1)
        ld (PictureOff),hl
        ld hl,(ProtoBuf+3)
        ld (PictureOff+2),hl
        or 1                             ; NZ
        ret
.received:
        ld a,COVER_WAIT                  ; ESP сводит миниатюру
        ld (CoverState),a
        ld a,(WC_FRAMES)
        ld (CoverPollFrame),a
        or 1                             ; NZ
        ret
.stop:
        xor a
        ld (CoverState),a
        ld (ProtoErr),a
        or 1                             ; NZ
        ret

; --- поток 1 -----------------------------------------------------------------

; Файл потока 1: HL — имя [#00][имя][0], DE — адрес его длины (4 байта).
; Другой файл — поток 1 откроется заново.
S1_File:
        ld bc,(S1Name)
        ld (S1Name),hl
        ld (S1SizePtr),de
        or a
        sbc hl,bc
        ret z
        xor a
        ld (S1Open),a
        ret

; Сектор файла потока 1 с местом (HL — смещение LE32) — в S1Buffer.
; Выход: HL — байт места в буфере, BC — байтов от него до конца сектора (не
; дальше конца файла; 0 — место за концом); CF=1 — файл не открылся или
; карта не прочиталась. Поток 0 снова выбран.
S1_Read:
        ld de,S1Off
        ld bc,4
        ldir
        ; Осталось в файле от места: длина минус смещение.
        ld hl,(S1SizePtr)
        ld e,(hl)
        inc hl
        ld d,(hl)
        inc hl
        push de
        ld e,(hl)
        inc hl
        ld d,(hl)                        ; DE — старшее слово длины
        pop hl                           ; HL — младшее
        ld bc,(S1Off)
        or a
        sbc hl,bc
        ex de,hl
        ld bc,(S1Off+2)
        sbc hl,bc
        jr c,.past                       ; место за концом файла
        ld a,h
        or l
        jr nz,.plenty
        ld a,d
        or e
        jr z,.past                       ; ровно конец
        jr .left
.plenty:
        ld de,#FFFF
.left:
        ld (S1Left),de
        call S1_Select
        ld a,(S1Open)
        or a
        jr z,.reopen
        call .sector
        ld de,(S1Next)
        or a
        sbc hl,de                        ; секторов вперёд
        jr nc,.skip
.reopen:
        ; Файл — с начала (или назад): FENTRY, GFILE.
        ld hl,(S1Name)
        call WC_FENTRY
        jr z,.fail
        call WC_GFILE
        ld a,1
        ld (S1Open),a
        ld hl,0
        ld (S1Next),hl
        call .sector
.skip:
        push hl
        call Mp3_SkipSectors
        pop de
        jr c,.fail
        ld hl,(S1Next)
        add hl,de
        inc hl                           ; и сектор, который читается сейчас
        ld (S1Next),hl
        ld hl,S1Buffer
        ld b,1
        call WC_LOAD512
        jr c,.fail
        call S0_Select
        ; HL — место в буфере, BC — до конца сектора, но не дальше конца файла.
        ld a,(S1Off+1)
        and 1
        ld b,a
        ld a,(S1Off)
        ld c,a
        ld hl,512
        or a
        sbc hl,bc
        ld de,(S1Left)
        push hl
        or a
        sbc hl,de
        pop hl
        jr c,.fits
        ex de,hl
.fits:
        push hl
        ld hl,S1Buffer
        add hl,bc
        pop bc
        or a
        ret
.past:
        ld bc,0
        or a
        ret
.fail:
        xor a
        ld (S1Open),a
        call S0_Select
        scf
        ret
; HL — номер сектора места (S1Off >> 9; файлы до 32 МиБ).
.sector:
        ld hl,(S1Off+1)
        ld a,(S1Off+3)
        srl a
        rr h
        rr l
        ret

S1_Select:
        ld d,1
        jr S_Select
S0_Select:
        ld d,0
S_Select:
        ld bc,#FFFF
        jp WC_STREAM

; Обложку нашёл сам декодер (флаг 64 в AUDIO_STATUS; Ogg: METADATA_BLOCK_PICTURE,
; PLAYER 1.5): она важнее картинки из папки, как APIC у MP3 — что бы ни шло,
; ждать её миниатюру (AUDIO_THUMB флаг снимает). Только у потока: у модулей и
; радио места под обложку нет.
Cover_FromDecoder:
        ld a,(Mp3Kind)
        or a
        ret nz
        ld a,(CoverState)
        cp COVER_WAIT
        ret z                            ; уже ждём
        ld a,COVER_WAIT
        ld (CoverState),a
        ld a,(WC_FRAMES)
        sub COVER_POLL                   ; спросить на следующем шаге
        ld (CoverPollFrame),a
        ret

; Обложки не будет: отказ ESP не должен потом выдать себя за причину другой ошибки.
Cover_Stop:
        xor a
        ld (CoverState),a
        ld (ProtoErr),a
        ret                              ; Z

; ESP сводит миниатюру: спрашивать раз в COVER_POLL кадров, готовую — забрать.
Cover_Poll:
        ld a,(UiHidden)
        or a
        jr nz,.idle                      ; окна нет — миниатюру не забирать (Ui_Show)
        ld a,(WC_FRAMES)
        ld hl,CoverPollFrame
        sub (hl)
        cp COVER_POLL
        jr c,.idle
        ld a,(WC_FRAMES)
        ld (hl),a
        xor a
        ld (CoverPart),a
.part:
        ld a,(CoverPart)
        ld (Mp3Arg),a
        ld a,CMD_AUDIO_THUMB
        ld hl,Mp3Arg
        ld bc,1
        call Proto_Send
        jr c,Cover_Stop
        ld a,RESP_AUDIO_THUMB
        ld de,MP3_WAIT
        call Mp3_Wait
        jr c,Cover_Stop
        ld a,(ProtoBuf)
        or a
        jr z,.idle                       ; ещё сводится
        dec a
        jr nz,Cover_Stop                 ; не свелась
        ld hl,(ProtoRxLen)
        ld de,2+COVER_PART
        or a
        sbc hl,de
        jr nz,Cover_Stop
        call Cover_PutPart
        ld hl,CoverPart
        inc (hl)
        ld a,(hl)
        cp COVER_PARTS
        jr c,.part
        call Cover_Show
.idle:
        xor a
        ret

; Часть миниатюры (ProtoBuf+2, 16 строк по 32 байта) — в графику спрайта.
Cover_PutPart:
        ld a,(WC_PAGE3)
        ld (CoverPage3),a
        ld a,COVER_VPAGE
        ex af,af'
        ld a,FN_MNGCVPL
        call WC_API                      ; прямо, не Wc_Api: окно #C000 — наше до конца
        ld a,(CoverPart)
        add a,a
        add a,a
        add a,a
        add a,a                          ; первая строка части
        add a,#C0
        ld d,a
        ld e,0
        ld hl,ProtoBuf+2
        ld b,COVER_PART/32
.row:
        push bc
        push de
        ld bc,32
        ldir
        pop de
        inc d                            ; строка битовой карты — 256 байтов
        pop bc
        djnz .row
        ; Окно #C000 — как было (так его ставит и сам WC: MNGC в MD20.ASM).
        ld a,(CoverPage3)
        ld (WC_PAGE3),a
        ld bc,TS_PAGE3
        out (c),a
        ret

; Миниатюра в графике: страница спрайтов, описатели, включить слой спрайтов.
Cover_Show:
        ld c,COVER_VPAGE
        ld a,FN_GVSGP
        call Wc_Api
        ; Описатель 0 — миниатюра 64×64, с LEAP: на нём кончается слой S0.
        ; Описатели 1 и 2 — пустые с LEAP: кончают слои S1 и S2, так что
        ; остальные (чужие) описатели не рисуются.
        ld a,(Mp3Window+3)               ; Y окна, вычисленный WC при открытии
        add a,COVER_ROW
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl                        ; строки текста по 8 точек
        ld a,h
        or SPR_ACT_LEAP_64
        ld h,a
        ld (CoverSprites),hl
        ld a,(Mp3Window+2)               ; X окна
        add a,COVER_COL
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl                        ; знак текста — 4 точки TS-Conf
        ld a,h
        or SPR_SIZE_64
        ld h,a
        ld (CoverSprites+2),hl
        di
        ld bc,TS_FMADDR
        ld a,%00010000 | (high SpriteFile >> 4)
        out (c),a
        ld hl,CoverSprites
        ld de,SpriteFile                 ; SFILE в окне FMADDR (затирает UiLine)
        ld bc,CoverSpritesEnd-CoverSprites
        ldir
        ld bc,TS_FMADDR
        xor a
        out (c),a
        ei
        ld bc,TS_CONFIG
        ld a,%10000000
        out (c),a
        ld a,COVER_SHOWN
        ld (CoverState),a
        ret

; Перед выходом: спрайты выключить. WC регистр TSConfig не трогает.
Cover_Hide:
        ld a,(CoverState)
        cp COVER_SHOWN
        ret nz
        ld bc,TS_CONFIG
        xor a
        out (c),a
        ld (CoverState),a
        ret

CoverState:     db COVER_IDLE
PictureName:    dw 0                    ; найденное имя картинки ([#00][имя][0])
PictureSize:    ds 4
PictureOff:     ds 4                    ; сколько её приняла ESP
PictureHeader:  ds 8                    ; [длина LE32][смещение LE32]
; Регистр в длинных именах FENTRY может различать: Windows пишет Folder.jpg.
PictureNames:   db #00,"cover.jpg",0
                db #00,"Cover.jpg",0
                db #00,"COVER.JPG",0
                db #00,"folder.jpg",0
                db #00,"Folder.jpg",0
                db #00,"FOLDER.JPG",0
                db #00,"front.jpg",0
                db #00,"Front.jpg",0
                db #00,"FRONT.JPG",0
                db #FF
CoverWant:      ds 4                    ; откуда ESP ждёт следующий кусок метки
CoverBurst:     db COVER_BURST
CoverPollFrame: db 0
CoverPart:      db 0
CoverPage3:     db 0
S1Name:         dw 0                    ; файл потока 1: [#00][имя][0]
S1SizePtr:      dw 0                    ; адрес его длины
S1Open:         db 0                    ; открыт (FENTRY, GFILE)
S1Next:         dw 0                    ; номер сектора, который поток 1 прочтёт следующим
S1Off:          ds 4                    ; место, которое просят
S1Left:         dw 0                    ; байтов файла от места (не больше 65535)
; Три описателя спрайтов по три слова: R0 (Y, высота, ACT, LEAP), R1 (X,
; ширина), R2 (tnum, палитра).
CoverSprites:
        dw 0, 0, 0
        dw #4000, 0, 0
        dw #4000, 0, 0
CoverSpritesEnd:
