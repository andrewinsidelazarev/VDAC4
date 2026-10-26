; VDAC4 VFS — драйвер устройства 6 Wild Commander (образ по #3800, его кладёт RES_SWAP).
; Чтение: сектор 512 байтов приходит от ESP тремя частями (248 + 248 + 16) через резидент в буфер
; D_BOUNCE и копируется в HL. Запись (только SD2 = карта Tang, иначе ESP отказывает): сектор из HL —
; в D_BOUNCE, резидент шлёт его одной командой. Таблица и общий блок CORE32 — как у SD-ZC: CORE32 обращается
; к ним по постоянным адресам (KERNEL.ASM), начальные значения блока — побайтно из SD-ZC v1.11i.
; Резидент исполняется в окне #C000, поэтому вызов D_CALL на его время закрывает прерывания
; (обработчик плагина в IM2 мог бы сменить страницу #C000) и возвращает прежние страницу и IFF.

ABT             equ #384A               ; код ошибки обмена: CORE32 смотрит его после RDDSE/SDDSE
LTHL            equ #3864               ; общая позиция потока (XPOZI)
LTDE            equ #3866

        jp D_SELDEV
        jp D_DEVINI
        jp D_REINI
        jp D_XPOZI
        jp D_PROZ
        jp D_RDDSE
        jp D_SDDSE
        jp D_RDD256
        ds #3848-$,0                    ; 16 резервных строк таблицы
        incbin "sdzc_state.bin"         ; #3848..#38CF: общий блок CORE32
        assert $ == D_LBA, сдвинулся общий блок драйвера
D_BLKNUM:       ds 4                    ; позиция чтения (BLKNUM SD-ZC)
D_BLKT:         db 0
        assert $ == #38D5, сдвинулось начало кода драйвера
D_PAGE:         db 0                    ; физическая страница резидента (ставит установка)
D_PART:         db 0                    ; часть сектора 0..2
D_COUNT:        db 0
D_TRIES:        db 0
D_RES:          db 0
D_RESCF:        db 0

D_SELDEV:
        xor a
        ret
D_REINI:
        ret
D_XPOZI:
        ld (LTHL),hl
        ld (LTDE),de
D_PROZ:
        ld (D_BLKNUM),hl
        ld (D_BLKNUM+2),de
        ret

; Конец RES_SWAP: E — прежняя страница #C000, BC=#13AF; на стеке — AF с IFF входа DOS_SWP, под ним
; IX вызывающего. Дальше — штатный конец DOS_SWP: SELDEV_INVALIDATE, POP IX, RET.
D_TAIL:
        out (c),e
        pop af
        jp po,.closed
        ei
.closed:
        xor a                           ; SELDEV: вариант устройства не нужен
        jp WC_SD2_TAIL

; Вызов резидента: A — операция RES_IO. Выход: CF и A — как у RES_IO. Сохраняет HL, IX.
D_CALL:
        push hl
        ld l,a
        ld a,i
        di
        push af                         ; P/V — были ли прерывания открыты
        ld bc,#13AF
        in e,(c)
        push de                         ; E — страница окна #C000
        ld a,(D_PAGE)
        out (c),a
        ld a,l
        call RES_IO
        ld (D_RES),a
        sbc a,a
        ld (D_RESCF),a
        pop de
        ld bc,#13AF
        out (c),e
        pop af
        jp po,.closed
        ei
.closed:
        ld a,(D_RESCF)
        rra
        ld a,(D_RES)
        pop hl
        ret

; Операция A до ответа: ждать порциями RES_WAIT, между ними прерывания открыты.
; Выход: CF=0 — да, CF=1 — нет. Сохраняет HL.
D_DO:
        call D_CALL
        ret nc
        cp RES_PENDING
        scf
        ret nz
        ld a,RES_TRIES
        ld (D_TRIES),a
.wait:
        ld a,RES_WAIT
        call D_CALL
        ret nc
        cp RES_PENDING
        scf
        ret nz
        ld a,(D_TRIES)
        dec a
        ld (D_TRIES),a
        jr nz,.wait
        scf
        ret

; Устройство готово: канал к ESP и том VDAC4 VFS собран. Z — да, NZ — нет.
D_DEVINI:
        ld a,RES_BEGIN
        call D_DO
        jr c,D_FAIL
        ld a,RES_INFO
        call D_DO
        jr c,D_FAIL
D_OK:
        xor a
        ld (ABT),a
        ret
D_RDD256:                               ; 256-байтовых блоков нет: LOAD256 на VFS — отказ
D_FAIL:
        ld a,1
        ld (ABT),a
        or a
        ret

; A секторов из HL в D_BLKNUM…, HL продвигается. Том VFS (не карта Tang) — отказ ESP: ABT=1.
D_SDDSE:
        or a
        jr z,D_OK
        ld (D_COUNT),a
        ld a,RES_BEGIN
        call D_DO
        jr c,D_FAIL
.sector:
        ld de,D_BOUNCE
        ld bc,512
        ldir                            ; окно #C000 ещё прежнее: HL может быть и там
        ld a,RES_WRITE
        call D_DO
        jr c,D_FAIL
        call D_NEXT
        jr nz,.sector
        jr D_OK

; Следующий сектор: D_BLKNUM+1, D_COUNT-1. NZ — ещё есть. Сохраняет HL.
D_NEXT:
        push hl
        ld hl,(D_BLKNUM)
        ld bc,1
        add hl,bc
        ld (D_BLKNUM),hl
        ld hl,(D_BLKNUM+2)
        dec bc
        adc hl,bc
        ld (D_BLKNUM+2),hl
        pop hl
        ld a,(D_COUNT)
        dec a
        ld (D_COUNT),a
        ret

; A секторов с D_BLKNUM в HL, HL продвигается. Сеанс канала — заново на каждый вызов: прежний
; запрос, оборванный тайм-аутом, не подменит ответ.
D_RDDSE:
        or a
        jr z,D_OK
        ld (D_COUNT),a
        ld a,RES_BEGIN
        call D_DO
        jr c,D_FAIL
.sector:
        xor a
        ld (D_PART),a
.part:
        ld a,RES_READ
        call D_DO
        jr c,D_FAIL
        ld a,(D_PART)
        inc a
        ld (D_PART),a
        cp 3
        jr c,.part
        ex de,hl
        ld hl,D_BOUNCE
        ld bc,512
        ldir                            ; окно #C000 уже прежнее: HL может быть и там
        ex de,hl
        call D_NEXT
        jr nz,.sector
        jr D_OK
