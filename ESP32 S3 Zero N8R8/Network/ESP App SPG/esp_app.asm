; Запускатель приложения ESP (ELF) для ZX Evolution с TS-Conf: программа SPG.
;
; Один исходник на все приложения: имя ELF, его длину и страницы задаёт сборка
; (build.bat → build/app_cfg.inc). ELF лежит в самом SPG — блоками по 16 КБ в
; страницах APP_PAGE0…, загрузчик SPG кладёт их в память.
;
; Порядок:
;   1. связь с ESP платы VDAC4 по SPI (shared/z80: zifi_link.asm, proto.asm);
;   2. MOD_QUERY: есть ли этот ELF (имя, длина, время) в кэше ESP; нет — MOD_PUT
;      кусками и MOD_COMMIT (ESP пишет его в свою flash — секунды);
;   3. монитор — на FT812: VCONFIG TS-Conf = #04 (это и есть M_SEL: пока он 1,
;      ядро FT812 в Tang держится);
;   4. APP_START — приложение работает (например, BOING.ELF рисует через FT812);
;   5. любая клавиша — APP_STOP, монитор — обратно на TS-Conf.
; Выход из SPG — кнопкой Reset (как у ZiFi): через 3 с после M_SEL = 0 Tang и так
; возвращает ядро TS-Config.
;
; Бордюр показывает, где программа: синий — связь и загрузка ELF, зелёный —
; приложение работает, красный — ошибка (нет ESP, ESP не взяла ELF, приложение
; не запустилось).

        device zxspectrum128
        DEFINE ZIFI_SPI                 ; канал до ESP — SPI платы VDAC4

VCONFIG         equ #00AF               ; TS-Conf: видеорежим; #04 — изображение FT812
PAGE3           equ #13AF               ; TS-Conf: страница окна #C000
VID_FT812       equ #04
VID_ZX          equ #00

; CMD_MOD_QUERY…CMD_APP_STOP и RESP_* — из shared/z80/proto.asm

PUT_DATA        equ 1016                ; данных в одном MOD_PUT (кратно 8; + смещение — до 1024)
PAGE_BYTES      equ #4000

        INCLUDE "build/app_cfg.inc"     ; APP_NAME, APP_LEN, APP_TIME, APP_PAGE0

        org #8000
Start:
        di
        ld sp,#5FFF
        ld a,1                          ; синий бордюр: связь и загрузка
        out (#FE),a
        call ZiFi_Init
        jp c,Fail

        ; --- MOD_QUERY: [флаги][длина LE32][время FAT LE32][имя, 0] -----------------
        ld hl,QueryBuf
        xor a
        ld (hl),a                       ; флаги 0 — приложение
        inc hl
        ld de,APP_LEN & #FFFF
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        ld de,APP_LEN >> 16
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        ld de,APP_TIME & #FFFF
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        ld de,APP_TIME >> 16
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        ex de,hl
        ld hl,AppName
        ld bc,AppNameEnd-AppName
        ldir
        ld hl,QueryBuf
        ld bc,9+AppNameEnd-AppName
        ld a,CMD_MOD_QUERY
        call Proto_Send
        ld a,RESP_MOD_QUERY
        ld b,8                          ; ~8 × 65535 опросов
        call WaitLong
        jp c,Fail
        ld a,(ProtoBuf)
        or a
        jr z,.cached                    ; 0 — уже в кэше ESP
        cp 1
        jp nz,Fail                      ; 2 — нет места, 3 — плохое имя

        ; --- MOD_PUT: [смещение LE32][данные], весь ELF по PUT_DATA байтов -----------
        ld hl,0
        ld (PutOffset),hl
        ld (PutOffset+2),hl
.put:
        call PutChunk
        jp c,Fail
        ld hl,(PutOffset+2)             ; дошли до APP_LEN?
        ld de,APP_LEN >> 16
        or a
        sbc hl,de
        jr c,.put
        jr nz,.put_done
        ld hl,(PutOffset)
        ld de,APP_LEN & #FFFF
        or a
        sbc hl,de
        jr c,.put
.put_done:
        ld a,CMD_MOD_COMMIT
        call Proto_SendEmpty
        ld a,RESP_MOD_COMMIT
        ld b,64                         ; запись во flash ESP — до десятков секунд
        call WaitLong
        jp c,Fail
        ld a,(ProtoBuf)
        or a
        jp z,Fail
.cached:
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
        ld a,RESP_APP_START
        ld b,16
        call WaitLong
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

; Все клавиши отпущены.
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

; Один MOD_PUT с PutOffset: до PUT_DATA байтов ELF из страниц APP_PAGE0…
; (кусок не переходит границу страницы: 16384 не кратно PUT_DATA, поэтому длина
; урезается до конца страницы). Выход: CF=1 — ESP не приняла.
PutChunk:
        ; страница и смещение в ней: смещение ELF / 16384
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
        add a,APP_PAGE0
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
        ld hl,APP_LEN & #FFFF           ; осталось = APP_LEN − смещение (≤ 64 КБ хватает на min)
        ld de,(PutOffset)
        or a
        sbc hl,de
        ld a,(PutOffset+2)
        ld e,a
        ld a,APP_LEN >> 16
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

AppName:        APP_NAME
AppNameEnd:
WaitResp:       db 0
PutOffset:      ds 4
PutSrc:         dw 0
PutLen:         dw 0
QueryBuf:       ds 32
PutBuf:         ds PUT_DATA+4

        INCLUDE "zifi_link.asm"
        INCLUDE "proto.asm"
CodeEnd:

        SAVEBIN "build/esp_app.bin",Start,CodeEnd-Start
