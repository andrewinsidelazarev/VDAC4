; Запускатель приложения ESP (ELF) для ZX Evolution с TS-Conf: программа SPG.
;
; Один исходник на все приложения: имя ELF, его длину и страницы задаёт сборка
; (build.bat → build/app_cfg.inc). ELF лежит в самом SPG — блоками по 16 КБ в
; страницах APP_PAGE0…, загрузчик SPG кладёт их в память.
;
; Порядок:
;   1. связь с ESP платы VDAC4 по SPI (shared/z80: zifi_link.asm, proto.asm);
;   2. MOD_QUERY: есть ли этот ELF (имя, длина, время) в кэше ESP; нет — MOD_PUT
;      кусками и MOD_COMMIT (ESP пишет его в свою flash — секунды). Если в SPG есть
;      ядро ПЛИС (build.bat ИМЯ.ELF ЯДРО.BIN, ESP 0.1.164+), оно идёт первым тем же
;      путём с флагом 2: ESP кладёт его на флешку USB (/cores) и в следующий раз годную
;      копию с карты не берёт; грузит ядро само приложение (core_load);
;   2б. дополнительные модули ELF (build.bat ИМЯ.ELF … +МОДУЛЬ.ELF, 26.11.2026): после
;      главного ELF тем же путём (флаг 0); приложение грузит их само (module_open_ex). ESP
;      модуль не взяла — запуск идёт дальше: приложение решает, как жить без него;
;   2а. файлы данных приложения с SD-карты Evo (build.bat ИМЯ.ELF … ДАННЫЕ, ESP 0.1.165+):
;      папку на карте программа находит сама (app_dir.asm), файлы читает драйвером FAT32 и
;      отдаёт ESP после ELF с флагом 4 — ESP хранит их на флешке USB (app_data.asm). Флешки
;      нет (ESP 0.1.166+) — файлы остаются на карте: пока приложение работает, программа
;      отвечает на запросы ESP кусками этих файлов (Data_Poll — во всех ожиданиях ниже);
;   3. монитор — на FT812: VCONFIG TS-Conf = #04 (это и есть M_SEL: пока он 1,
;      ядро FT812 в Tang держится);
;   4. APP_START — приложение работает (например, BOING.ELF рисует через FT812);
;   5. любая клавиша — APP_STOP, монитор — обратно на TS-Conf.
; Выход из SPG — кнопкой Reset (как у ZiFi): через 3 с после M_SEL = 0 Tang и так
; возвращает ядро TS-Config.
;
; Бордюр показывает, где программа: синий — связь и загрузка ELF, зелёный —
; приложение работает, красный — ошибка (нет ESP, ESP не взяла ELF, приложение
; не запустилось). С файлами данных ещё: голубой и белый попеременно — идёт файл
; данных, жёлтый — у ESP нет флешки USB, фиолетовый — на флешке нет места.
;
; Память. Без файлов данных: код и переменные — с #8000 (страница 2), стек — #5FFF,
; окно #C000 — страницы ELF и ядра по очереди. С файлами данных (как у DOOM VDAC4):
; окно #0000 — ОЗУ, страница FAT32_WORK_PAGE (буферы драйвера FAT32), окно #4000 —
; FAT32_PAGE (код драйвера), поэтому стек — в окне #8000 под #C000; окно #C000 — ещё и
; рабочая страница SCRATCH_PAGE (поиск папки, порция файла 8 КБ).

        device zxspectrum128
        DEFINE ZIFI_SPI                 ; канал до ESP — SPI платы VDAC4

VCONFIG         equ #00AF               ; TS-Conf: видеорежим; #04 — изображение FT812
PAGE3           equ #13AF               ; TS-Conf: страница окна #C000
VID_FT812       equ #04
VID_ZX          equ #00

; CMD_MOD_QUERY…CMD_APP_STOP и RESP_* — из shared/z80/proto.asm

PUT_DATA        equ 1016                ; данных в одном MOD_PUT (кратно 8; + смещение — до 1024)
PAGE_BYTES      equ #4000

        INCLUDE "build/app_cfg.inc"     ; APP_NAME, APP_LEN, APP_TIME, APP_PAGE0; HAS_CORE, CORE_*

        IFDEF HAS_DATA                  ; файлы данных: DATA_*, APP_STEM, FAT32_PAGE, SCRATCH_PAGE
        ; Файлы данных идут порциями DMA (быстрый путь MOD_PUT): драйвер канала — с блоками DMA.
        ; Страницы окон #8000 и #C000 DMA узнаёт из двух ячеек; у плагинов WC это ячейки WC
        ; (#6002, #6003), но в окне #4000 у SPG — драйвер FAT32, поэтому ячейки — свои.
        DEFINE ZIFI_SPI_DMA
WC_PAGE2        equ SpgPage2
WC_PAGE3        equ SpgPage3
        INCLUDE "fat32.inc"             ; входы драйвера FAT32 (shared/fat32/include)
MEMCONFIG       equ #21AF               ; TS-Conf: что в окне #0000
MEM_W0_RAM      equ #0E                 ; окно #0000 — ОЗУ, страница из PAGE0, запись разрешена
PAGE0           equ #10AF               ; TS-Conf: страница окна #0000
PAGE1           equ #11AF               ; TS-Conf: страница окна #4000
DATA_STACK      equ #C000               ; стек — под окном #C000 (растёт вниз)
        ENDIF

        org #8000
Start:
        di
        IFDEF HAS_DATA
        ld sp,DATA_STACK
        ; Драйвер FAT32 — в окна #0000 и #4000 (страницы положил туда загрузчик SPG).
        ld bc,MEMCONFIG
        ld a,MEM_W0_RAM
        out (c),a
        ld bc,PAGE0
        ld a,FAT32_WORK_PAGE
        out (c),a
        ld bc,PAGE1
        ld a,FAT32_PAGE
        out (c),a
        ELSE
        ld sp,#5FFF
        ENDIF
        ld a,1                          ; синий бордюр: связь и загрузка
        out (#FE),a
        call ZiFi_Init
        jp c,Fail
        IFDEF HAS_DATA
        ; --- папка приложения на карте и файлы данных в ней — до передачи ядра и ELF:
        ; нет карты, папки или файла — ничего не передаётся (красный бордюр).
        call Data_Prepare
        jp c,Fail
        ENDIF
        IFDEF HAS_CORE
        ; --- ядро ПЛИС с карты Evo (флаг 2, ESP 0.1.164+): первым, тем же путём, что ELF ----
        ld hl,CoreFile
        call SendFile
        jp c,Fail
        ENDIF
        ld hl,AppFile
        call SendFile
        jp c,Fail
        IFDEF HAS_MODS
        ; --- дополнительные модули ELF — после главного: отказ ESP запуск не останавливает
        call SendMods
        ENDIF
        IFDEF HAS_DATA
        ; --- файлы данных — в ESP (флаг 4): годную копию на флешке ESP не берёт заново
        call Data_SendAll
        jp c,FailColor                  ; A — цвет: жёлтый, фиолетовый или красный
        ENDIF
        ; --- монитор — FT812 ещё до запуска: M_SEL = 1 держит ядро FT812 в Tang, пока
        ; приложение переводит его туда (ft_open), — иначе через 3 с тишины при M_SEL = 0
        ; ядро ушло бы обратно в TS-Config. Не запустилось — Fail вернёт VCONFIG 0.
        ld bc,VCONFIG
        ld a,VID_FT812
        out (c),a
        ; --- APP_START: [имя, 0] ---------------------------------------------------
        ld hl,AppName
        ld bc,AppNameEnd-AppName
        ld a,CMD_APP_START
        call Proto_Send
        IFDEF HAS_DATA
        call WaitStart                  ; и запросы к файлам данных, что придут раньше ответа
        ELSE
        ld a,RESP_APP_START
        ld b,16
        call WaitLong
        ENDIF
        jp c,Fail
        ld a,(ProtoBuf)                 ; итог int8: 0 — работает
        or a
        jp nz,Fail

        ; --- приложение работает, ждать клавишу ------------------------------------
        ld a,4                          ; зелёный бордюр
        out (#FE),a
        call WaitRelease                ; клавишу запуска в WC ещё держат
        IFDEF DIAG_TRAIL
        ld a,#4B
        call Trail
        ld hl,0
        ENDIF
.run:
        IFDEF HAS_DATA
        push hl
        call Data_Poll                  ; запрос ESP к файлам данных — обслужить
        pop hl
        ENDIF
        IFDEF DIAG_TRAIL
        dec hl                          ; раз в 65536 опросов — метка «жду клавишу»
        ld a,h
        or l
        ld a,#4B
        call z,Trail
        ENDIF
        xor a
        in a,(#FE)
        cpl
        and #1F
        jr z,.run

        ; --- стоп ------------------------------------------------------------------
        IFDEF DIAG_TRAIL
        ld a,#4C                        ; клавиша увидена
        call Trail
        ENDIF
        ld a,CMD_APP_STOP
        call Proto_SendEmpty
        ld a,RESP_APP_STOP
        ld b,4
        call WaitLong
        IFDEF DIAG_TRAIL
        ld a,#4D                        ; ответ на APP_STOP пришёл (#4E — нет)
        jr nc,.stop_trail
        inc a
.stop_trail:
        call Trail
        ENDIF
        ld bc,VCONFIG
        ld a,VID_ZX
        out (c),a
        xor a                           ; чёрный бордюр: всё, дальше — Reset
        out (#FE),a
Halt:
        jr Halt

Fail:
        ld bc,VCONFIG
        ld a,VID_ZX
        out (c),a
        ld a,2                          ; красный бордюр
        out (#FE),a
        jr Halt

        IFDEF HAS_DATA
; Ошибка со своим цветом бордюра A (файлы данных: нет флешки, нет места).
FailColor:
        ld e,a
        ld bc,VCONFIG
        ld a,VID_ZX
        out (c),a
        ld a,e
        out (#FE),a
        jr Halt
        ENDIF

        IFDEF HAS_DATA
; Ответ на APP_START (не дольше 16 × 65535 опросов), а до него — запросы ESP к файлам данных:
; приложение читает их уже в start(), и его запрос приходит раньше ответа (WaitLong бы его
; выбросил, и ESP ждала бы 4 с до повтора). Выход: CF=0 — ответ в ProtoBuf.
WaitStart:
        ld b,16
.outer:
        push bc
        ld de,#FFFF
.inner:
        push de
        call Proto_Poll
        jr nc,.idle
        cp RESP_APP_START
        jr z,.got
        cp EVT_DATA_READ
        call z,Data_Serve
.idle:
        pop de
        dec de
        ld a,d
        or e
        jr nz,.inner
        pop bc
        djnz .outer
        scf
        ret
.got:
        pop de
        pop bc
        or a
        ret
        ENDIF

; Ждать ответ A не дольше B × 65535 опросов. Выход: CF=0 — пришёл (ProtoBuf).
WaitLong:
        ld (WaitResp),a
.loop:
        push bc
        ld a,(WaitResp)
        ld de,#FFFF
        call Proto_WaitCmd
        pop bc
        ret nc
        djnz .loop
        scf
        ret

; Все клавиши отпущены. Приложение уже работает: запросы ESP к файлам данных — обслуживать.
WaitRelease:
        IFDEF DIAG_TRAIL
        ld hl,0
.loop:
        dec hl                          ; раз в 65536 опросов — метка «клавиша ещё нажата»
        ld a,h
        or l
        ld a,#4A
        call z,Trail
        ENDIF
        IFDEF HAS_DATA
        push hl
        call Data_Poll
        pop hl
        ENDIF
        xor a
        in a,(#FE)
        cpl
        and #1F
        IFDEF DIAG_TRAIL
        jr nz,.loop
        ELSE
        jr nz,WaitRelease
        ENDIF
        ret

        IFDEF DIAG_TRAIL
; Проверочная сборка (build.bat с ESP_APP_DIAG=1): метка A для FT_SEEN Tang (LINK 0x50,
; ядра 1.16+) — посылка FT812 «A 00 00», команда хоста без действия (#4A…#4E). Команда хоста
; без ACTIVE автопереход ядра не включает. Где Z80 — видно по `tang rd 50` в консоли ESP.
Trail:
        push bc
        ld c,a
        ld a,#07                        ; #77: выбран FT812, SD-карта — нет
        out (#77),a
        ld a,c
        out (#57),a
        xor a
        out (#57),a
        out (#57),a
        ld a,#03                        ; никто не выбран
        out (#77),a
        ld a,c
        pop bc
        ret
        ENDIF

; Файл в ESP: описание по HL (флаги, длина LE32, время FAT LE32, первая страница, адрес и
; длина имени с нулём — как FileFlags…FileNameLen) — в переменные; MOD_QUERY; уже есть
; (0) — готово; шли (1) — весь файл кусками MOD_PUT из страниц и MOD_COMMIT. Выход: CF=1 —
; ESP не ответила, не взяла (2 — нет места, 3 — плохое имя) или не положила.
SendFile:
        ld de,FileFlags
        ld bc,FileDescEnd-FileFlags
        ldir
        ; --- MOD_QUERY: [флаги][длина LE32][время FAT LE32][имя, 0] -----------------
        ld a,(FileFlags)
        ld (QueryBuf),a
        ld hl,FileLen                   ; длина и время лежат подряд
        ld de,QueryBuf+1
        ld bc,8
        ldir
        ld hl,(FileNamePtr)
        ld bc,(FileNameLen)
        ldir
        ld hl,(FileNameLen)
        ld bc,9
        add hl,bc
        ld b,h
        ld c,l
        ld hl,QueryBuf
        ld a,CMD_MOD_QUERY
        call Proto_Send
        ld a,RESP_MOD_QUERY
        ld b,8                          ; ~8 × 65535 опросов
        call WaitLong
        ret c
        ld a,(ProtoBuf)
        or a
        ret z                           ; 0 — уже есть (кэш ESP, флешка): передачи нет
        cp 1
        scf
        ret nz                          ; 2 — нет места, 3 — плохое имя
        ; --- MOD_PUT: [смещение LE32][данные], весь файл по PUT_DATA байтов ----------
        ld hl,0
        ld (PutOffset),hl
        ld (PutOffset+2),hl
.put:
        call PutChunk
        ret c
        ld hl,(PutOffset+2)             ; дошли до длины файла?
        ld de,(FileLen+2)
        or a
        sbc hl,de
        jr c,.put
        jr nz,.put_done
        ld hl,(PutOffset)
        ld de,(FileLen)
        or a
        sbc hl,de
        jr c,.put
.put_done:
        ld a,CMD_MOD_COMMIT
        call Proto_SendEmpty
        ld a,RESP_MOD_COMMIT
        ld b,64                         ; запись во flash ESP или на флешку — до десятков секунд
        call WaitLong
        ret c
        ld a,(ProtoBuf)
        or a
        scf
        ret z                           ; 0 — не положила
        or a
        ret

; Один MOD_PUT с PutOffset: до PUT_DATA байтов файла из страниц FilePage0…
; (кусок не переходит границу страницы: 16384 не кратно PUT_DATA, поэтому длина
; урезается до конца страницы). Выход: CF=1 — ESP не приняла.
PutChunk:
        ; страница и смещение в ней: смещение в файле / 16384
        ld hl,(PutOffset)
        ld a,(PutOffset+2)
        ld c,a                          ; C:HL — 24-битное смещение
        ld a,h
        rlca
        rlca
        and 3
        ld b,a
        ld a,c
        rlca
        rlca
        or b                            ; A = смещение >> 14
        ld b,a
        ld a,(FilePage0)
        add a,b
        ld bc,PAGE3
        out (c),a
        ld a,h
        and #3F
        or #C0
        ld h,a                          ; HL — адрес в окне #C000
        ld (PutSrc),hl
        ; длина: min(PUT_DATA, конец страницы, конец ELF)
        ld de,0
        ex de,hl
        or a
        sbc hl,de                       ; HL = #10000 − адрес = до конца окна (0 при #0000 не бывает)
        ld bc,PUT_DATA
        call MinHlBc                    ; BC = min(HL, BC)
        push bc
        ld hl,(FileLen)                 ; осталось = длина − смещение (≤ 64 КБ хватает на min)
        ld de,(PutOffset)
        or a
        sbc hl,de
        ld a,(PutOffset+2)
        ld e,a
        ld a,(FileLen+2)
        sbc a,e
        pop bc
        jr nz,.len_ok                   ; осталось ≥ 64 КБ
        call MinHlBc
.len_ok:
        ld (PutLen),bc
        ; кадр: [смещение LE32][данные]
        ld hl,(PutOffset)
        ld (PutBuf),hl
        ld a,(PutOffset+2)
        ld l,a
        ld h,0
        ld (PutBuf+2),hl
        ld hl,(PutSrc)
        ld de,PutBuf+4
        ld bc,(PutLen)
        ldir
        ld hl,PutBuf
        ld bc,(PutLen)
        inc bc
        inc bc
        inc bc
        inc bc
        ld a,CMD_MOD_PUT
        call Proto_Send
        ld a,RESP_MOD_PUT
        ld b,4
        call WaitLong
        ret c
        ld a,(ProtoBuf)
        or a
        scf
        ret z                           ; 0 — не принято
        ; смещение += длина
        ld hl,(PutOffset)
        ld bc,(PutLen)
        add hl,bc
        ld (PutOffset),hl
        ld a,(PutOffset+2)
        adc a,0
        ld (PutOffset+2),a
        or a
        ret

        IFDEF HAS_MODS
; Модули из таблицы ModFiles (build/app_mods.inc: MOD_COUNT описаний, как AppFile) — по очереди
; через SendFile. Модуль ESP не взяла (CF=1) — следующий всё равно идёт: приложение проверит само.
SendMods:
        ld hl,ModFiles
        ld b,MOD_COUNT
.next:
        push bc
        push hl
        call SendFile
        pop hl
        ld bc,FileDescEnd-FileFlags     ; описание модуля — той же длины, что у AppFile
        add hl,bc
        pop bc
        djnz .next
        ret
        ENDIF

; BC = min(HL, BC) без знака.
MinHlBc:
        push hl
        or a
        sbc hl,bc
        pop hl
        ret nc                          ; HL ≥ BC — BC
        ld b,h
        ld c,l
        ret

; Описания файлов для SendFile: флаги MOD_QUERY, длина, время FAT, первая страница, имя
AppFile:        db 0                    ; флаги 0 — приложение (ELF)
                dd APP_LEN
                dd APP_TIME
                db APP_PAGE0
                dw AppName
                dw AppNameEnd-AppName
        IFDEF HAS_CORE
CoreFile:       db 2                    ; флаг 2 — ядро ПЛИС с карты Evo (ESP 0.1.164)
                dd CORE_LEN
                dd CORE_TIME
                db CORE_PAGE0
                dw CoreName
                dw CoreNameEnd-CoreName
CoreName:       CORE_NAME
CoreNameEnd:
        ENDIF
AppName:        APP_NAME
AppNameEnd:
        IFDEF HAS_MODS
        INCLUDE "build/app_mods.inc"    ; ModFiles и имена модулей (prep.py)
        ENDIF
; Текущий файл SendFile — в том же порядке, что описания
FileFlags:      db 0
FileLen:        ds 4
FileTime:       ds 4
FilePage0:      db 0
FileNamePtr:    dw 0
FileNameLen:    dw 0
FileDescEnd:
WaitResp:       db 0
PutOffset:      ds 4
PutSrc:         dw 0
PutLen:         dw 0
QueryBuf:       ds 32
PutBuf:         ds PUT_DATA+4

        IFDEF HAS_DATA
SpgPage2:       db 2                    ; страница окна #8000 — код SPG (Block … 2)
SpgPage3:       db 0                    ; страница окна #C000 для DMA (MapScratch)
        INCLUDE "app_dir.asm"           ; папка приложения на карте: path.cfg ESP, поиск
        INCLUDE "app_data.asm"          ; файлы данных: MOD_QUERY с флагом 4, порции DMA
        ENDIF
        INCLUDE "zifi_link.asm"
        INCLUDE "proto.asm"
CodeEnd:
        IFDEF HAS_DATA
        ASSERT CodeEnd <= DATA_STACK-256    ; под стек — не меньше 256 байтов
        ENDIF

        SAVEBIN "build/esp_app.bin",Start,CodeEnd-Start
