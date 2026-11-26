; Файлы данных приложения с SD-карты Evo — в ESP (прошивка 0.1.165+).
; Собирается только с файлами данных (HAS_DATA, build.bat ИМЯ.ELF … ДАННЫЕ).
;
; Зачем. У приложения бывают большие файлы (у Quake 2 — PAK0.PAK, 50–180 МБ). В SPG их не
; положить (вся память Evo — 4 МБ), поэтому они лежат на карте Evo в папке приложения, рядом
; с его SPG. ESP хранит копию на флешке USB (/SYSTEM/CACHE/ПРИЛОЖЕНИЕ/): MOD_QUERY с флагом 4
; спрашивает, есть ли там годная копия (та же длина, не старее файла на карте). Есть — файл
; не передаётся. Нет — запускатель читает файл с карты и отдаёт его кусками MOD_PUT, потом
; MOD_COMMIT (ESP переименовывает готовый файл на флешке). Первый раз это минуты, потом —
; мгновенно.
;
; Порядок в запускателе: ядро ПЛИС → ELF → файлы данных → APP_START. Папку приложения и
; записи каталога файлов данных Data_Prepare находит заранее, до передачи ядра и ELF:
; нет карты или файлов — ничего не передаётся.
;
; Как читается файл любой длины (до 4 ГБ — предел FAT32). Запись каталога даёт первый
; кластер и длину. Сектор за сектором по кластеру, кластер кончился — следующий по FAT
; (ScanNextCluster из app_dir.asm), пока не прочитана вся длина. Смещение и остаток —
; 32-битные числа, поэтому предела 32 МБ (как у таблицы кусков DOOM fastread.asm) нет.
; Сектор читает процедура порта драйвера FAT32 (#3902) — сразу в буфер порции.
;
; Как передаётся. Порция — 8 КБ (16 секторов), буфер DATA_BUF в странице SCRATCH_PAGE.
;   - Быстрый путь (прошивка с каналом SPI версии 2+ и приёмом блоков DMA — Data_Probe):
;     управляющий запрос raw media [6A][длина LE16][смещение LE32] — ровно 7 байтов, флаг
;     #10 обмена; ESP открывает приём, и вся порция уходит одной передачей DMA «ОЗУ → SPI»,
;     без контрольной суммы — так же MP3-плеер отдаёт звук (Sound/MP3 Player/src/fast_media.asm).
;     Ответ EA ESP шлёт, когда запишет порцию на флешку (~50 мс на 8 КБ). Пока она пишет,
;     Z80 уже читает с карты следующую порцию — так время карты и флешки не складывается.
;   - Канал без быстрого пути — кадрами MOD_PUT по PUT_DATA (1016) байтов, как ELF, на каждый
;     кадр — свой ответ EA (медленно, но работает).
;
; Флешки нет (прошивка 0.1.166+, ответ 5) — файл не передаётся: ESP читает его по запросу
; у запускателя, пока приложение работает (раздел «Флешки у ESP нет» ниже).
;
; Бордюр: голубой и белый попеременно (смена — каждые 32 КБ) — идёт файл данных; жёлтый —
; у ESP нет флешки USB, а прошивка старше 0.1.166 (ответ 4); фиолетовый — на флешке нет места
; (ответ 2); красный — остальные ошибки (нет карты, папки или файла, ESP не ответила, не
; приняла кусок, файл без флешки раздроблен сильнее 896 кусков).

DATA_PIECE      equ 8192                ; порция: 16 секторов, одна передача DMA
DATA_BUF        equ #E000               ; порция — в странице SCRATCH_PAGE, окно #C000
DATA_INFO_SIZE  equ 12                  ; на файл: длина, время FAT, первый кластер (по 4)

COLOR_DATA      equ 5                   ; голубой: идёт файл данных (смена с белым — ход)
COLOR_DATA2     equ 7                   ; белый
COLOR_ERROR     equ 2                   ; красный
COLOR_NO_ROOM   equ 3                   ; фиолетовый: ответ 2 — на флешке нет места
COLOR_NO_USB    equ 6                   ; жёлтый: ответ 4 — флешки USB у ESP нет

DATA_QUERY_FLAG equ 4                   ; MOD_QUERY: файл данных приложения
DATA_HAVE       equ 0                   ; ответы MOD_QUERY: годная копия есть,
DATA_SEND       equ 1                   ; шли,
DATA_NO_ROOM    equ 2                   ; нет места,
DATA_NO_USB     equ 4                   ; нет флешки (3 — плохое имя)
DATA_REMOTE     equ 5                   ; нет флешки: ESP читает файл по запросу (0.1.166)

EVT_DATA_READ   equ #6E                 ; ESP -> Z80: [номер запроса][файл][смещение LE32][длина LE16]
CMD_DATA_PIECE  equ #6F                 ; Z80 -> ESP: [номер запроса][смещение LE32][данные]
RUN_TABLE       equ #C400               ; пробеги цепочек файлов на учёте — до DATA_BUF
RUN_TABLE_END   equ DATA_BUF
RUN_SIZE        equ 8                   ; пробег: [первый кластер LE32][кластеров LE32]

LINK_F_MEDIA    equ #10                 ; флаг обмена: управляющий запрос raw media
FAST_PUT_TRIES  equ 3                   ; столько раз порция может не сойтись (PUT_BAD)
FAST_WAIT_TRIES equ 64                  ; столько пауз по ~0,1 с ждать места в очереди ESP

; ---------------------------------------------------------------------------
; Подготовка: драйвер FAT32, папка приложения, записи всех файлов данных.
; Выход: CF=1 — нет карты, папки или какого-то файла.
Data_Prepare:
        call MapScratch                 ; буферы поиска — в окне #C000
        ld hl,FAT32_PORT                ; таблица процедур порта SD (Z-Controller)
        xor a
        call FAT32_BIND
        call FAT32_DEVICE_INIT          ; карта: сброс, режим SD
        jr nz,.fail
        call FAT32_MOUNT                ; первый том FAT32 карты
        jr nz,.fail
        call PathCfgLoad                ; папка, которую помнит ESP, — если годится
        jr nz,.located
        call LocateApp                  ; иначе — поиск по карте
        ret c
        call PathCfgSave                ; найденное — в ESP (её отказ не мешает)
.located:
        jp Data_FindAll
.fail:
        scf
        ret

; Записи каталога всех файлов данных — в DataInfo: длина (+28), время и дата FAT (+22,
; так их и ждёт MOD_QUERY), первый кластер (+26 — младшее слово, +20 — старшее).
; Выход: CF=1 — какого-то файла нет.
Data_FindAll:
        ld hl,AppDir
        call FAT32_SET_DIR_CLUSTER
        ld hl,DataNames
        ld ix,DataInfo
        ld b,DATA_COUNT
.next:
        push bc
        push ix
        call FindName                   ; HL — уже за именем
        pop ix
        pop bc
        jr z,.missing
        push hl
        push bc
        push ix
        pop de                          ; DE — запись DataInfo
        ld hl,FAT32_ENTRY+28            ; длина
        ld bc,4
        ldir
        ld hl,FAT32_ENTRY+22            ; время, дата
        ld c,4
        ldir
        ld hl,FAT32_ENTRY+26            ; первый кластер, младшее слово
        ldi
        ldi
        ld hl,FAT32_ENTRY+20            ; старшее слово (верхние 4 разряда — не номер)
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        ld a,(hl)
        and #0F
        ld (de),a
        inc de
        push de
        pop ix                          ; IX — следующая запись DataInfo
        pop bc
        pop hl
        djnz .next
        or a
        ret
.missing:
        scf
        ret

; ---------------------------------------------------------------------------
; Все файлы данных — в ESP. Выход: CF=1, A — цвет бордюра ошибки.
Data_SendAll:
        call Data_Probe
        xor a
        ld (DataIndex),a                ; номер файла — его место в списке DATA_NAMES
        ld hl,DataNames
        ld ix,DataInfo
        ld b,DATA_COUNT
.next:
        push bc
        push hl
        push ix
        call Data_SendOne
        pop ix
        pop hl
        pop bc
        ret c                           ; A — цвет
.skip:                                  ; HL — за имя
        ld a,(hl)
        inc hl
        or a
        jr nz,.skip
        ld de,DATA_INFO_SIZE
        add ix,de
        ld a,(DataIndex)
        inc a
        ld (DataIndex),a
        djnz .next
        or a
        ret

; Один файл: HL — имя (ИМЯ.EXT, 0), IX — его запись DataInfo.
; Выход: CF=0 — у ESP годная копия (уже была или передана сейчас); CF=1, A — цвет ошибки.
Data_SendOne:
        ld (DataNamePtr),hl
        ld (DataInfoPtr),ix
        ld a,COLOR_DATA
        out (#FE),a
        ; --- MOD_QUERY: [4][длина LE32][время FAT LE32][ИМЯ.EXT, 0][ПРИЛОЖЕНИЕ, 0] -------
        ld de,QueryBuf
        ld a,DATA_QUERY_FLAG
        ld (de),a
        inc de
        push ix
        pop hl
        ld bc,8                         ; длина и время — подряд в DataInfo
        ldir
        ld hl,(DataNamePtr)
        call .copy
        ld hl,AppStem                   ; имя приложения — имя ELF без «.ELF»: папка на флешке
        call .copy
        ld a,(DataIndex)                ; номер файла: «флешки нет — читать умею сам» (0.1.166)
        ld (de),a
        inc de
        ex de,hl
        ld de,QueryBuf
        or a
        sbc hl,de
        ld b,h
        ld c,l                          ; длина запроса
        ld hl,QueryBuf
        ld a,CMD_MOD_QUERY
        call Proto_Send
        ld a,RESP_MOD_QUERY
        ld b,64                         ; ESP может считать свободное место флешки — секунды
        call WaitLong
        ld a,COLOR_ERROR
        ret c
        ld a,(ProtoBuf)
        cp DATA_HAVE
        ret z                           ; копия на флешке годная: передачи нет (CF=0)
        cp DATA_SEND
        jr z,.send
        cp DATA_REMOTE
        jr z,.remote
        ld e,COLOR_NO_ROOM
        cp DATA_NO_ROOM
        jr z,.refused
        ld e,COLOR_NO_USB
        cp DATA_NO_USB
        jr z,.refused
        ld e,COLOR_ERROR                ; 3 — плохое имя (или ответ, которого не знаем)
.refused:
        ld a,e
        scf
        ret
.copy:                                  ; строка HL с нулём — в DE
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        or a
        jr nz,.copy
        ret
.remote:
        ; Флешки нет (0.1.166): файл остаётся на карте, ESP будет просить его куски. Здесь —
        ; только пробеги его цепочки (Data_BuildRuns), передачи нет.
        ld ix,(DataInfoPtr)
        call Data_BuildRuns
        ld a,COLOR_ERROR
        ret                             ; CF — итог

.send:
        ; --- начало файла: осталось — вся длина, смещение 0, первый кластер, сектор 0 ----
        ld hl,(DataInfoPtr)
        ld de,DataLeft
        ld bc,4
        ldir
        ld bc,4                         ; время пропустить
        add hl,bc
        ld de,ScanCluster
        ld c,4
        ldir
        xor a
        ld (ScanSector),a
        ld (ScanFatValid),a
        ld hl,0
        ld (DataOffset),hl
        ld (DataOffset+2),hl
        call MapScratch                 ; ELF и ядро уходили через то же окно #C000
        call Data_ReadNext              ; первая порция (у пустого файла — никакой)
        ld a,COLOR_ERROR
        ret c
        jr .more
.chunk:
        ld a,(DataFast)
        or a
        jr z,.frames
        ; Быстрый путь: порция уходит одной передачей DMA, ответ EA — после записи на флешку.
        call Data_FastPut
        ld a,COLOR_ERROR
        ret c
        ld bc,(ChunkLen)
        call Data_Advance
        call Data_Blink
        call Data_ReadNext              ; пока ESP пишет на флешку — следующая порция с карты
        ld a,COLOR_ERROR
        ret c
        call Data_WaitPut               ; теперь — ответ на прежнюю
        ld a,COLOR_ERROR
        ret c
        jr .more
.frames:
        call Data_FramesPut             ; кадрами по 1016 байтов, на каждый — ответ
        ld a,COLOR_ERROR
        ret c
        call Data_Blink
        call Data_ReadNext
        ld a,COLOR_ERROR
        ret c
.more:
        ld hl,(ChunkLen)
        ld a,h
        or l
        jr nz,.chunk                    ; 0 — файл прочитан весь
        ; --- MOD_COMMIT: ESP переименовывает готовый файл на флешке -----------------------
        ld a,CMD_MOD_COMMIT
        call Proto_SendEmpty
        ld a,RESP_MOD_COMMIT
        ld b,64
        call WaitLong
        ld a,COLOR_ERROR
        ret c
        ld a,(ProtoBuf)
        dec a
        ld a,COLOR_ERROR
        scf
        ret nz                          ; не 1 — не положен
        or a                            ; CF=0
        ret

; Следующая порция, если файл ещё не кончился; иначе ChunkLen = 0. Выход: CF=1 — ошибка.
Data_ReadNext:
        ld hl,(DataLeft)
        ld de,(DataLeft+2)
        ld a,h
        or l
        or d
        or e
        jr nz,Data_ReadChunk
        ld (ChunkLen),hl                ; HL = 0; CF=0 после OR
        ret

; Порция с карты — в DATA_BUF: min(DATA_PIECE, осталось) байтов, целыми секторами (хвост
; последнего сектора — за концом файла, его не передают). Выход: ChunkLen, DataLeft уменьшен;
; CF=1 — сектор не прочитался или цепочка кластеров кончилась раньше длины файла.
Data_ReadChunk:
        ld hl,DATA_PIECE
        ld de,(DataLeft+2)
        ld a,d
        or e
        jr nz,.len                      ; осталось 64 КБ и больше
        ld de,(DataLeft)
        push hl
        or a
        sbc hl,de
        pop hl
        jr c,.len                       ; осталось больше порции
        ex de,hl                        ; HL — остаток
.len:
        ld (ChunkLen),hl
        ex de,hl                        ; DE — порция
        ld hl,(DataLeft)                ; осталось −= порция (32 бита)
        or a
        sbc hl,de
        ld (DataLeft),hl
        ld hl,(DataLeft+2)
        ld de,0
        sbc hl,de
        ld (DataLeft+2),hl
        ld hl,(ChunkLen)                ; секторов = (порция + 511) / 512
        ld de,511
        add hl,de
        ld a,h
        srl a
        ld (DataSectors),a
        ld hl,DATA_BUF
        ld (DataDest),hl
.sector:
        ld a,(ScanSector)
        ld hl,ScanSectorsPerCluster
        cp (hl)
        jr c,.lba
        call ScanNextCluster            ; кластер прочитан весь — следующий по FAT
        ret c
        scf
        ret z                           ; цепочка кончилась, а длина — ещё нет
        xor a
        ld (ScanSector),a
.lba:
        call ScanClusterLba
        ret c                           ; кластер вне тома
        ld bc,(DataDest)
        call ScanReadLba
        scf
        ret nz
        ld hl,ScanSector
        inc (hl)
        ld hl,(DataDest)
        inc h
        inc h                           ; +512
        ld (DataDest),hl
        ld hl,DataSectors
        dec (hl)
        jr nz,.sector
        or a
        ret

; Смещение += BC (32 бита).
Data_Advance:
        ld hl,(DataOffset)
        add hl,bc
        ld (DataOffset),hl
        ret nc
        ld hl,(DataOffset+2)
        inc hl
        ld (DataOffset+2),hl
        ret

; Ход передачи на бордюре: голубой или белый — по разряду 15 смещения (смена каждые 32 КБ:
; при ~120 КБ/с — около двух раз в секунду).
Data_Blink:
        ld a,(DataOffset+1)
        rlca
        and 1
        add a,a
        add a,COLOR_DATA                ; 5 или 7
        out (#FE),a
        ret

; Ответ EA на кусок MOD_PUT. Выход: CF=1 — не пришёл или кусок не принят.
Data_WaitPut:
        ld a,RESP_MOD_PUT
        ld b,16                         ; запись на флешку бывает и медленной
        call WaitLong
        ret c
        ld a,(ProtoBuf)
        cp 1
        ret z                           ; принят; CF=0
        scf
        ret

; Порция быстрым путём: управляющий запрос raw media [6A][ChunkLen LE16][DataOffset LE32]
; и порция DATA_BUF одной передачей DMA (блоками по 512 байтов; хвост последнего блока ESP
; отбросит). ESP ставит в очередь обычный MOD_PUT [смещение][данные] — его разбирает тот же
; обработчик, что кадры. Выход: CF=1 — ESP не открыла приём или трижды не приняла порцию.
Data_FastPut:
        ld a,CMD_MOD_PUT
        ld (FastCtl),a
        ld hl,(ChunkLen)
        ld (FastCtl+1),hl
        ld (FastBytes),hl
        ld hl,(DataOffset)
        ld (FastCtl+3),hl
        ld hl,(DataOffset+2)
        ld (FastCtl+5),hl
        ld a,7
        ld (FastCtlLen),a
; Порция DATA_BUF (FastBytes байтов) быстрым путём; управление — FastCtl, FastCtlLen байтов
; (у MOD_PUT — 7, у куска по запросу — 8). Выход: CF=1 — ESP не открыла приём или трижды
; не приняла порцию.
Data_FastSend:
        ld hl,DATA_BUF
        ld (SpiBulkBuf),hl              ; откуда DMA берёт порцию (страница — SpgPage3)
        ld hl,(FastBytes)               ; блоков по 512 = (порция − 1) / 512 + 1
        dec hl
        ld a,h
        srl a
        inc a
        ld (FastBlocks),a
        ld a,FAST_PUT_TRIES
        ld (FastTries),a
        ld hl,FAST_WAIT_TRIES
        ld (FastWait),hl
.ask:
        ; Управление кладётся заново каждый раз: запрос «записан» (без данных) кладёт свою
        ; контрольную сумму в начало SpiTxBuf.
        ld hl,FastCtl
        ld de,SpiTxBuf
        ld bc,8
        ldir
        ld a,(FastCtlLen)
        ld (SpiTxLen),a                 ; в обмене — только байты управления
        ld hl,SpiRsp+3
        ld (SpiDest),hl                 ; данных в ответ не будет
        xor a
        ld (SpiRoom),a
        ld a,LINK_F_MEDIA
        call SpiLink_Turn
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_READY
        jr nz,.ready
        ; Места на порцию в очереди ESP нет (прежний кусок ещё в ней): подождать, спросить снова.
        ; Ждём без HALT: в окне #0000 — страница драйвера, обработчика прерываний там нет.
        ld hl,(FastWait)
        dec hl
        ld (FastWait),hl
        ld a,h
        or l
        scf
        ret z
        call Data_Pause
        jr .ask
.ready:
        ld bc,TS_DMASADDRL
        ld a,SPI_CMD_WRDMA
        call SpiLink_BulkOpen           ; адрес порции — в DMA, начало записи в ESP
        ld a,(FastBlocks)
        dec a
        ld bc,TS_DMANUM
        out (c),a                       ; блоков минус один
        ld a,TS_DMA_RAM_SPI
        call SpiLink_BulkRun
        ld a,SPI_CMD_WR_DONE
        call SpiLink_BulkDone
        ld a,LINK_F_PUT_DONE            ; «записан»: ESP кладёт порцию в очередь
        call SpiLink_Send
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_OK
        ret nz                          ; принята; CF=0
        ; Не сошлась (PUT_BAD): приём у ESP открылся поздно (на плате MP3-плеера 22.11.2026
        ; ESP получала от блока 8 КБ лишь хвост). Порция цела в DATA_BUF — послать её снова.
        ld hl,FastTries
        dec (hl)
        jr nz,.ask
        scf
        ret

; Пауза около 0,1 с (65536 витков по 26 тактов на 14 МГц).
Data_Pause:
        ld hl,0
.loop:
        dec hl
        ld a,h
        or l
        jr nz,.loop
        ret

; Порция кадрами MOD_PUT (канал без быстрого пути): [смещение LE32] — прямо из DataOffset,
; данные — прямо из DATA_BUF (Proto_Send2 шлёт кадр из двух частей без копии); на каждый
; кадр — ответ EA. Выход: CF=1 — ESP не приняла.
Data_FramesPut:
        ld hl,DATA_BUF
        ld (FramePtr),hl
        ld hl,(ChunkLen)
        ld (FrameLeft),hl
.frame:
        ld hl,(FrameLeft)
        ld bc,PUT_DATA
        call MinHlBc                    ; BC — длина кадра
        ld (FrameLen),bc
        ld hl,(FramePtr)
        ld (ProtoTail),hl
        ld (ProtoTailLen),bc
        xor a
        ld (ProtoHold),a
        ld hl,DataOffset
        ld bc,4
        ld a,CMD_MOD_PUT
        call Proto_Send2
        ret c
        call Data_WaitPut
        ret c
        ld bc,(FrameLen)
        call Data_Advance
        ld hl,(FramePtr)
        add hl,bc
        ld (FramePtr),hl
        ld hl,(FrameLeft)
        or a
        sbc hl,bc
        ld (FrameLeft),hl
        jr nz,.frame
        ret                             ; CF=0: вычитание без заёма

; Есть ли быстрый путь: прошивка принимает блоки DMA от Z80 (SpiPutCan — из ответа на начало
; сеанса) и версия канала SPI — 2 и выше (байт 62 общего буфера, за признаком #D7 в байте 61).
; Выход: DataFast = 1 — есть.
Data_Probe:
        xor a
        ld (DataFast),a
        ld a,(SpiPutCan)
        or a
        ret z
        ld b,LINK_STATE_ADDR
        call SpiLink_OpenRead           ; A — байт 60 (состояние), дальше — 61, 62
        in a,(c)
        cp #D7
        jp nz,SpiLink_Close
        in a,(c)
        call SpiLink_Close
        cp 2
        ret c
        ld a,1
        ld (DataFast),a
        ret

; ===========================================================================
; Флешки у ESP нет (прошивка 0.1.166+): файл остаётся на карте Evo и читается по запросу.
;
; Зачем. Пользователь 23.11.2026: «Игра должна работать и без флешки otg esp32». Флешка — кэш, а
; сами файлы и так лежат на карте Evo рядом с запускателем. Без флешки ESP ставит файл на учёт
; (ответ 5 на MOD_QUERY — его даёт только запросу, где за именем приложения стоит номер файла:
; так запускатель говорит «умею читать сам»), а когда приложение читает файл, ESP шлёт событие
; EVT_DATA_READ: [номер запроса][номер файла][смещение LE32][длина LE16] — смещение кратно 512,
; длина до 32 КБ. Запускатель, пока приложение работает, опрашивает канал (Data_Poll), читает
; эти секторы с карты и отдаёт их кусками CMD_DATA_PIECE: [номер запроса][смещение LE32][данные]
; — быстрым путём DMA по 8 КБ (как MOD_PUT) или кадрами по 1016 байтов. Ответа на кусок нет: не
; дошло — ESP через 4 с спросит снова с новым номером. Не прочитался сектор — кусок без данных.
;
; Как найти сектор по смещению быстро. Ходить по цепочке FAT на каждый запрос — долго (у pak в
; 180 МБ — тысячи ссылок). Поэтому, когда файл ставится на учёт, запускатель один раз проходит
; его цепочку и записывает её непрерывные куски — «пробеги»: [первый кластер LE32][кластеров
; LE32] (Data_BuildRuns). Файл, записанный на карту целиком, — обычно один-два пробега. Запрос:
; номер сектора в файле = смещение / 512, кластер в файле = номер / секторов в кластере; пробеги
; перебираются, пока кластер не окажется внутри одного из них (Data_Seek), дальше — сектор за
; сектором, к следующему кластеру — тем же пробегом или следующим (Data_NextCluster).
; Таблица пробегов — в странице SCRATCH_PAGE с #C400 до DATA_BUF (там прежде были кадры поиска
; папки — к этому времени поиск кончился): 896 пробегов на все файлы. Сильнее раздроблен — файл не
; берётся (красный бордюр).

; ---------------------------------------------------------------------------
; Пробеги цепочки файла DataIndex (запись DataInfo — IX) — в таблицу, файл — на учёт.
; Выход: CF=1 — кластер негодный, ошибка чтения FAT или таблица полна.
Data_BuildRuns:
        call MapScratch                 ; FAT — в ScanFatBuffer окна #C000 (там было ELF)
        ld a,(DataIndex)
        call RemoteEntry                ; HL — запись учёта
        ld de,(RunFree)
        ld (hl),e                       ; первая запись пробегов
        inc hl
        ld (hl),d
        inc hl
        ld (RemoteCountPtr),hl
        xor a
        ld (hl),a                       ; пробегов пока 0
        inc hl
        ld (hl),a
        ld (ScanFatValid),a
        ld (ScanSector),a
        ld a,(ix+8)                     ; первый кластер
        ld (ScanCluster),a
        ld a,(ix+9)
        ld (ScanCluster+1),a
        ld a,(ix+10)
        ld (ScanCluster+2),a
        ld a,(ix+11)
        ld (ScanCluster+3),a
        call ScanClusterLba             ; кластер 0, 1 или за томом — негодный
        ret c
        call .start
.next:
        ld hl,ScanCluster               ; ждём, что следующий — этот + 1
        ld de,RunNext
        call ScanCopy32
        ld hl,RunNext
        call Inc32
        call ScanNextCluster
        ret c                           ; ошибка чтения FAT или испорченная ссылка
        jr z,.store                     ; цепочка кончилась: последний пробег — в таблицу
        ld hl,ScanCluster
        ld de,RunNext
        ld b,4
.same:
        ld a,(de)
        cp (hl)
        jr nz,.new
        inc hl
        inc de
        djnz .same
        ld hl,RunLen                    ; кластер подряд — тот же пробег
        call Inc32
        jr .next
.new:
        call .store
        ret c
        call .start
        jr .next
.start:                                 ; новый пробег: с ScanCluster, длина 1
        ld hl,ScanCluster
        ld de,RunStart
        call ScanCopy32
        ld hl,1
        ld (RunLen),hl
        dec hl
        ld (RunLen+2),hl
        ret
.store:                                 ; пробег — в таблицу. Выход: CF=1 — места нет
        ld hl,(RunFree)
        ld de,RUN_TABLE_END-RUN_SIZE+1
        or a
        sbc hl,de
        ccf
        ret c
        ld de,(RunFree)
        ld hl,RunStart                  ; RunStart и RunLen — подряд
        ld bc,RUN_SIZE
        ldir
        ld (RunFree),de
        ld hl,(RemoteCountPtr)
        ld e,(hl)
        inc hl
        ld d,(hl)
        inc de
        ld (hl),d
        dec hl
        ld (hl),e
        ld a,1
        ld (DataRemote),a               ; есть файл на учёте: запускатель обслуживает запросы
        or a
        ret

; Запись учёта файла A: HL = RemoteInfo + A × 4 ([адрес первого пробега][число пробегов]).
RemoteEntry:
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        ld de,RemoteInfo
        add hl,de
        ret

; (HL) += 1, 32 бита.
Inc32:
        ld b,4
.byte:
        inc (hl)
        ret nz
        inc hl
        djnz .byte
        ret

; ---------------------------------------------------------------------------
; Запрос ESP, пока приложение работает: был кадр — разобрать; EVT_DATA_READ — обслужить.
; Без файлов на учёте канал не опрашивается вовсе (как до 0.1.166). Портит всё.
Data_Poll:
        ld a,(DataRemote)
        or a
        ret z
        call Proto_Poll
        ret nc                          ; кадра нет
        cp EVT_DATA_READ
        ret nz                          ; не наш (сводка Wi-Fi и прочее) — мимо
; Обслужить запрос из ProtoBuf: [номер запроса][номер файла][смещение LE32][длина LE16].
Data_Serve:
        ld hl,(ProtoRxLen)
        ld de,8
        or a
        sbc hl,de
        ret nz                          ; не та длина — не запрос
        ld a,(ProtoBuf)
        ld (ServeSeq),a
        ld a,(ProtoBuf+1)
        ld (ServeIndex),a
        ld hl,ProtoBuf+2
        ld de,ServeOffset
        ld bc,6                         ; смещение и длина — подряд
        ldir
        ld hl,(ServeLeft)
        ld a,h
        or l
        jr z,.fail                      ; пустой запрос — не бывает
        ld a,(ServeIndex)
        cp DATA_COUNT
        jr nc,.fail                     ; такого файла нет
        call Data_Seek
        jr c,.fail                      ; файл не на учёте или смещение за цепочкой
.piece:
        ld hl,(ServeLeft)               ; кусок: min(осталось, DATA_PIECE)
        ld bc,DATA_PIECE
        call MinHlBc
        ld (PieceLen),bc
        call Data_ReadPiece
        jr c,.fail
        call Data_SendPiece
        ret c                           ; канал не принял — ESP спросит снова
        ld bc,(PieceLen)
        ld hl,(ServeOffset)             ; смещение += кусок (32 бита)
        add hl,bc
        ld (ServeOffset),hl
        jr nc,.left
        ld hl,(ServeOffset+2)
        inc hl
        ld (ServeOffset+2),hl
.left:
        ld hl,(ServeLeft)
        or a
        sbc hl,bc
        ld (ServeLeft),hl
        jr nz,.piece
        ret
.fail:
        ; Кусок без данных — «не прочитала»: [номер запроса][смещение] кадром с суммой.
        ld hl,ServeSeq                  ; ServeSeq и ServeOffset — подряд
        ld bc,5
        ld a,CMD_DATA_PIECE
        jp Proto_Send

; Смещение ServeOffset (кратно 512) файла ServeIndex — в кластер и сектор: ScanCluster,
; ScanSector, RunLeft (сколько кластеров пробега ещё за этим), RunPtr и RunsLeft (следующие
; пробеги). Выход: CF=1 — файл не на учёте или смещение за последним пробегом.
Data_Seek:
        ; номер сектора в файле = смещение / 512: 23 разряда — B:H:L
        ld hl,(ServeOffset+1)
        ld a,(ServeOffset+3)
        srl a
        rr h
        rr l
        ld b,a
        ld a,(ScanSectorsPerCluster)    ; сектор в кластере — младшие разряды
        dec a
        and l
        ld (ScanSector),a
        ld a,(ScanSectorsPerCluster)    ; кластер в файле = номер / секторов в кластере
.shift:
        srl a
        jr c,.shifted
        srl b
        rr h
        rr l
        jr .shift
.shifted:
        ld (SeekK),hl
        ld l,b
        ld h,0
        ld (SeekK+2),hl
        ld a,(ServeIndex)
        call RemoteEntry
        ld e,(hl)                       ; IX — первый пробег
        inc hl
        ld d,(hl)
        inc hl
        ld c,(hl)                       ; BC — пробегов
        inc hl
        ld b,(hl)
        push de
        pop ix
.run:
        ld a,b
        or c
        scf
        ret z                           ; пробеги кончились (или файла нет на учёте)
        ; K − длина пробега: заём — кластер внутри этого пробега
        ld hl,(SeekK)
        ld e,(ix+4)
        ld d,(ix+5)
        or a
        sbc hl,de
        ld (SeekTemp),hl
        ld hl,(SeekK+2)
        ld e,(ix+6)
        ld d,(ix+7)
        sbc hl,de
        jr c,.found
        ld (SeekK+2),hl                 ; K −= длина, к следующему пробегу
        ld hl,(SeekTemp)
        ld (SeekK),hl
        ld de,RUN_SIZE
        add ix,de
        dec bc
        jr .run
.found:
        ; кластер = начало пробега + K; за ним в пробеге — длина − K − 1
        ld hl,(SeekK)
        ld e,(ix+0)
        ld d,(ix+1)
        add hl,de
        ld (ScanCluster),hl
        ld hl,(SeekK+2)
        ld e,(ix+2)
        ld d,(ix+3)
        adc hl,de
        ld (ScanCluster+2),hl
        ld l,(ix+4)
        ld h,(ix+5)
        ld de,(SeekK)
        or a
        sbc hl,de
        ld (RunLeft),hl
        ld l,(ix+6)
        ld h,(ix+7)
        ld de,(SeekK+2)
        sbc hl,de
        ld (RunLeft+2),hl
        call Dec32RunLeft
        dec bc
        ld (RunsLeft),bc
        ld de,RUN_SIZE
        add ix,de
        ld (RunPtr),ix
        or a
        ret

; RunLeft −= 1 (32 бита).
Dec32RunLeft:
        ld hl,(RunLeft)
        ld a,h
        or l
        dec hl
        ld (RunLeft),hl
        ret nz
        ld hl,(RunLeft+2)
        dec hl
        ld (RunLeft+2),hl
        ret

; Следующий кластер файла: в том же пробеге (+1) или первый следующего пробега.
; Выход: CF=1 — пробеги кончились (файл короче, чем просят).
Data_NextCluster:
        ld hl,(RunLeft)
        ld de,(RunLeft+2)
        ld a,h
        or l
        or d
        or e
        jr z,.run
        call Dec32RunLeft
        ld hl,ScanCluster
        call Inc32
        or a
        ret
.run:
        ld hl,(RunsLeft)
        ld a,h
        or l
        scf
        ret z
        dec hl
        ld (RunsLeft),hl
        ld hl,(RunPtr)
        ld de,ScanCluster
        call ScanCopy32                 ; начало пробега
        ld de,RunLeft
        call ScanCopy32                 ; длина — и сразу минус этот кластер
        ld (RunPtr),hl
        call Dec32RunLeft
        or a
        ret

; Кусок PieceLen байтов с карты — в DATA_BUF целыми секторами (с ScanCluster/ScanSector).
; Выход: CF=1 — сектор не прочитался, кластер негодный или файл кончился раньше.
Data_ReadPiece:
        ld hl,(PieceLen)                ; секторов = (кусок + 511) / 512
        ld de,511
        add hl,de
        ld a,h
        srl a
        ld (DataSectors),a
        ld hl,DATA_BUF
        ld (DataDest),hl
.sector:
        ld a,(ScanSector)
        ld hl,ScanSectorsPerCluster
        cp (hl)
        jr c,.lba
        call Data_NextCluster           ; кластер прочитан весь — следующий по пробегам
        ret c
        xor a
        ld (ScanSector),a
.lba:
        call ScanClusterLba
        ret c
        ld bc,(DataDest)
        call ScanReadLba
        scf
        ret nz
        ld hl,ScanSector
        inc (hl)
        ld hl,(DataDest)
        inc h
        inc h                           ; +512
        ld (DataDest),hl
        ld hl,DataSectors
        dec (hl)
        jr nz,.sector
        or a
        ret

; Кусок DATA_BUF — в ESP: быстрым путём (управление [6F][длина LE16][номер][смещение LE32] —
; 8 байтов, данные одной передачей DMA) или кадрами [номер][смещение][≤1016 байтов] без ответов.
; Выход: CF=1 — канал не принял.
Data_SendPiece:
        ld a,(DataFast)
        or a
        jr z,.frames
        ld a,CMD_DATA_PIECE
        ld (FastCtl),a
        ld hl,(PieceLen)
        ld (FastCtl+1),hl
        ld (FastBytes),hl
        ld hl,ServeSeq                  ; номер и смещение — подряд
        ld de,FastCtl+3
        ld bc,5
        ldir
        ld a,8
        ld (FastCtlLen),a
        jp Data_FastSend
.frames:
        ld hl,ServeSeq                  ; заголовок кадра — свой: смещение растёт от кадра к кадру
        ld de,FrameHead
        ld bc,5
        ldir
        ld hl,DATA_BUF
        ld (FramePtr),hl
        ld hl,(PieceLen)
        ld (FrameLeft),hl
.frame:
        ld hl,(FrameLeft)
        ld bc,PUT_DATA
        call MinHlBc
        ld (FrameLen),bc
        ld hl,(FramePtr)
        ld (ProtoTail),hl
        ld (ProtoTailLen),bc
        xor a
        ld (ProtoHold),a
        ld hl,FrameHead
        ld bc,5
        ld a,CMD_DATA_PIECE
        call Proto_Send2
        ret c
        ld bc,(FrameLen)
        ld hl,(FrameHead+1)             ; смещение следующего кадра
        add hl,bc
        ld (FrameHead+1),hl
        jr nc,.carry
        ld hl,(FrameHead+3)
        inc hl
        ld (FrameHead+3),hl
.carry:
        ld hl,(FramePtr)
        add hl,bc
        ld (FramePtr),hl
        ld hl,(FrameLeft)
        or a
        sbc hl,bc
        ld (FrameLeft),hl
        jr nz,.frame
        ret                             ; CF=0

; Страница SCRATCH_PAGE — в окно #C000 (и в ячейку, по которой DMA знает страницу окна).
MapScratch:
        ld a,SCRATCH_PAGE
        ld (SpgPage3),a
        ld bc,PAGE3
        out (c),a
        ret

; --- переменные ---------------------------------------------------------------
DataNames:      DATA_NAMES              ; имена файлов данных подряд, каждое — с нулём
AppStem:        APP_STEM                ; имя приложения (ELF без «.ELF»), с нулём
DataInfo:       ds DATA_COUNT*DATA_INFO_SIZE
DataNamePtr:    dw 0
DataInfoPtr:    dw 0
DataLeft:       dd 0                    ; сколько байтов файла ещё не прочитано с карты
DataOffset:     dd 0                    ; смещение следующего куска MOD_PUT
ChunkLen:       dw 0                    ; байтов в DATA_BUF
DataSectors:    db 0
DataDest:       dw 0
DataFast:       db 0                    ; 1 — быстрый путь DMA
FastBlocks:     db 0
FastTries:      db 0
FastWait:       dw 0
FramePtr:       dw 0
FrameLeft:      dw 0
FrameLen:       dw 0
FastCtl:        ds 8                    ; управление быстрого пути: [команда][длина LE16][своё]
FastCtlLen:     db 0
FastBytes:      dw 0                    ; байтов порции
DataIndex:      db 0                    ; номер текущего файла в списке
; учёт файлов без флешки (0.1.166)
DataRemote:     db 0                    ; 1 — есть файл на учёте: Data_Poll опрашивает канал
RemoteInfo:     ds DATA_COUNT*4         ; на файл: [адрес первого пробега][число пробегов]
RemoteCountPtr: dw 0
RunFree:        dw RUN_TABLE            ; свободное место таблицы пробегов
RunStart:       dd 0                    ; строящийся пробег (RunStart и RunLen — подряд)
RunLen:         dd 0
RunNext:        dd 0
ServeSeq:       db 0                    ; запрос ESP: номер; ServeSeq и ServeOffset — подряд
ServeOffset:    dd 0                    ; смещение; ServeOffset и ServeLeft — подряд
ServeLeft:      dw 0                    ; сколько ещё отдать
ServeIndex:     db 0                    ; номер файла
PieceLen:       dw 0
FrameHead:      ds 5                    ; кадр куска: [номер][смещение LE32]
SeekK:          dd 0                    ; кластер в файле (Data_Seek)
SeekTemp:       dw 0
RunLeft:        dd 0                    ; кластеров пробега ещё за текущим
RunPtr:         dw 0                    ; следующий пробег
RunsLeft:       dw 0                    ; сколько их
