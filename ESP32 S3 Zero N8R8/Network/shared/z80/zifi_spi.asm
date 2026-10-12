; Обмен с ESP32-S3 платы VDAC4 по SPI ZX Evolution (TS-Config).
;
; Файл заменяет zifi_uart.asm: те же процедуры ZiFi_* с теми же входами и
; выходами, поэтому proto.asm и плагины от канала не зависят. Выбор канала —
; в zifi_link.asm (ключ сборки ZIFI_SPI).
;
; Порты те же, что у SD-карты: #57 — данные SPI, #77 — выбор устройства.
; Запись #13 в #77 выбирает ESP (бит 4) и снимает карту (бит 1), #03 снимает
; всех. Запись байта в #57 передаёт его. Чтение #57 начинает новый обмен с
; байтом #FF, а отдаёт то, что Evo приняла к концу цикла чтения порта: байт
; прошлого обмена, если новый ещё идёт, либо уже нового, если он успел
; закончиться. В опубликованном HDL TS-Config обмен идёт на 14 МГц и не
; успевает ни на какой скорости Z80. На плате встретилось и второе: с прошивкой
; Evo zxevo_fw_vdac2-spi28 чтение отдало байт нового обмена (обмен вдвое
; быстрее успевает закончиться при Z80 на 3,5 МГц). Поэтому перед данными
; RDBUF приходит то два, то один ничего не значащий байт, и начало данных
; драйвер находит сам — см. SpiLink_OpenRead.
;
; ESP — ведомый в режиме SPI Slave HD. У него есть общий буфер из 64 байтов,
; который контроллер SPI обслуживает сам, без программы:
;   WRBUF: #01, адрес, пустой байт, данные...  — запись в буфер;
;   RDBUF: #02, адрес, пустой байт, данные...  — чтение буфера.
; Поэтому Z80 никогда не ждёт ESP внутри обмена: она пишет запрос в байты
; 0..59 и перечитывает их, пока там не появится ответ.
;
;   запрос: [#C3][номер][флаги][длина][место][данные...][сумма]
;   ответ:  [#3C][номер][статус][длина][принято][данные...][сумма]
;
; За один обмен в каждую сторону уходит до 54 байтов. Ответ ESP пишет с конца:
; признак #3C — последним, так что вместе со своим номером он означает готовый
; ответ. Не дождавшись его, Z80 повторяет запрос с тем же номером: ESP второй
; раз данные не принимает, а возвращает прежний ответ.
;
; Байт 60 — состояние: #5A — у ESP ничего нет, #A5 — есть данные для Z80; любое
; другое значение означает, что прошивка не работает. Пока передавать нечего,
; Z80 читает один этот байт и начинает обмен, только увидев #A5.
;
; Блок через DMA (ключ сборки ZIFI_SPI_DMA, только плагины WC). Общий буфер
; несёт 54 байта за обмен, и каждый байт проходит через Z80. Запрос с флагом
; LINK_F_BULK означает, что Z80 может забрать 512 байтов разом: ESP выставляет
; блок для чтения через DMA ведомого SPI (команда #04) и отвечает статусом
; LINK_S_BULK без данных; Z80 забирает блок DMA TS-Config прямо в память и
; завершает чтение командой #08 (порядок — как у драйвера TS-Labs).
;
; Суммой обмена блок не покрыт, и считать сумму 512 байтов Z80 дорого. Вместо
; этого она тем же DMA отсылает принятый блок обратно (команда #03, конец —
; #07), а сравнивает ESP: байт обратной посылки стоит Z80 считанные такты. Итог
; приходит в ответе на следующий запрос, который данных не несёт; до него блок
; вызывающему не отдаётся. LINK_S_ECHO_BAD — копия неверна: если при этом стоит
; LINK_S_BULK, блок выставлен снова, иначе ESP от DMA отказалась и те же байты
; отдаст обычными ответами. LINK_S_BULK без него — блок верен, а следующий уже
; выставлен. Поэтому кадры, пришедшие блоками, на плате своим CRC16 не
; проверяются (см. vfs.asm плагинов).
;
; Обратно, от Z80 к ESP, — так же блоками по 512 байтов (SpiLink_PutBlock),
; и прямо из памяти вызывающего, без копии: Proto_Send отдаёт данные кадра
; блоком DMA там, где они лежат (чётный адрес в окне #8000 или #C000), считая
; заодно их XOR. Z80 спрашивает запросом LINK_F_PUT (байт 4 — этот XOR), ESP
; открывает приём и отвечает LINK_S_PUT_READY; Z80 пишет блок DMA «ОЗУ → SPI»
; (команды #03 и #07) и запросом LINK_F_PUT_DONE получает итог: LINK_S_PUT_OK
; либо LINK_S_PUT_BAD — тогда всё снова. Накопленные байты обмена (заголовок
; кадра, сумма прежнего) уходят в том же запросе LINK_F_PUT: ESP ставит их в
; очередь раньше блока. Делается это, только если прошивка в ответе на начало
; сеанса поставила LINK_S_PUT_CAPABLE: прошивка без этого приняла бы байт 4 за
; место под данные. Короткие кадры и хвосты — обычными обменами.
; Тот же DMA переносит принятое из порции в буфер кадра (SpiLink_Copy, «ОЗУ →
; ОЗУ»). Программам без WC этот путь не собирается: DMA адресует память номером
; страницы, а его знает только WC (WC_PAGE2 и WC_PAGE3).
;
; Передаваемые байты копятся в SpiTxBuf и уходят, когда набралось 54, и по
; ZiFi_Flush (её вызывает Proto_Send в конце кадра). Принимаемые байты ESP
; отдаёт только в обмене, начатом ZiFi_ReadBurst, сразу в буфер вызывающего.
;
; Файл состоит из двух частей, обычно они идут подряд. Программа, у которой
; свободное место в странице — два куска, может поставить первую часть
; отдельно (ключ ZIFI_SPI_HEAD_ONLY); вторая встаёт на обычное место через
; zifi_link.asm. Плагин SMB занимает свою страницу почти целиком, поэтому код
; плотный, а процедуры, которых плагин не вызывает, в него не попадают
; (IFUSED).

; === часть 1: внешние процедуры и данные ====================================
        IFNDEF ZIFI_SPI_HEAD_PLACED
        DEFINE ZIFI_SPI_HEAD_PLACED

ZIFI_SPI_DATA   equ #57
ZIFI_SPI_CFG    equ #77

; Значения порта #77. Бит 0 всегда 1, кроме SPI_HOST_MASTER: запись с битом 0
; и битом 7 в нуле возвращает шину SPI под управление Evo, если прежняя
; программа отдала её ESP. Записывать #77 с битом 0 = 0 и битом 7 = 1 нельзя:
; на плате VDAC4 ESP ведущим быть не может.
SPI_HOST_MASTER equ #02
SPI_SELECT_NONE equ #03
SPI_SELECT_ESP  equ #13

SPI_CMD_WRBUF   equ #01
SPI_CMD_RDBUF   equ #02

LINK_REQUEST    equ #C3
LINK_RESPONSE   equ #3C
LINK_HEADER     equ 5
LINK_CHUNK      equ 54                  ; данных за один обмен
LINK_STATE_ADDR equ 60
LINK_TX_EMPTY   equ #5A
LINK_TX_PENDING equ #A5

LINK_F_DROP     equ #01                 ; выбросить всё, что ESP ещё не отдала
LINK_F_BULK     equ #02                 ; Z80 может принять блок через DMA
LINK_F_SESSION  equ #80                 ; начало сеанса: запрос не считать повтором
LINK_S_MORE     equ #01                 ; у ESP остались данные
LINK_S_BULK     equ #02                 ; блок выставлен для чтения через DMA
LINK_S_STALE    equ #04                 ; этот блок устарел: прочитать и выбросить
LINK_S_ECHO_BAD equ #08                 ; обратная посылка не совпала: копию выбросить
LINK_F_PUT      equ #04                 ; Z80 отдаёт блок через DMA; байт 4 — его XOR
LINK_F_PUT_DONE equ #08                 ; блок записан: сверить
LINK_S_PUT_READY equ #10                ; приём блока открыт
LINK_S_PUT_OK   equ #20                 ; блок сошёлся и принят
LINK_S_PUT_BAD  equ #40                 ; не сошёлся: отдать снова
LINK_S_PUT_CAPABLE equ #80              ; прошивка принимает блоки от Z80
LINK_BULK       equ 512

; Ответ обычно готов через 0,1-0,3 мс. Повтор запроса нужен, только если ESP
; его не разобрала, поэтому ждём долго: около 25 мс на 14 МГц (0,1 с на
; 3,5 МГц). Шестнадцать попыток — 0,4 с (1,6 с) до признания, что ESP молчит.
LINK_POLLS      equ 2048
LINK_ATTEMPTS   equ 16
; Сколько обменов подряд ESP может не брать данные (её очередь занята).
LINK_PUSH_TRIES equ 2048
; Сколько раз блок DMA может не сойтись, прежде чем уйти обычными обменами.
LINK_PUT_TRIES  equ 3
; Сколько раз ZiFi_Init ждёт осмысленного байта состояния: ESP после сброса
; запускается секунду-две.
LINK_INIT_TRIES equ 16384

; Буфер, который ZiFi_ReadBurst заполняет за вызов: 54 байта данных и
; контрольная сумма ответа следом за ними либо блок DMA.
        IFDEF ZIFI_SPI_DMA
ZIFI_BURST_SIZE equ LINK_BULK
        ELSE
ZIFI_BURST_SIZE equ LINK_CHUNK+1
        ENDIF

; Найти прошивку VDAC4 и начать сеанс: очереди в обе стороны пусты.
; Выход: CF=0 — ESP на связи, CF=1 — прошивка не найдена или не отвечает.
; Сохраняет HL.
ZiFi_Init:
        push hl
        ld a,SPI_HOST_MASTER
        out (ZIFI_SPI_CFG),a
        xor a
        ld (SpiTxLen),a
        ld (SpiMore),a
        IFDEF ZIFI_SPI_DMA
        ld (SpiPutCan),a                ; пока прошивка не сказала, что умеет
        ENDIF
        ld de,LINK_INIT_TRIES
.wait:
        call SpiLink_ReadState
        cp LINK_TX_EMPTY
        jr z,.found
        cp LINK_TX_PENDING
        jr z,.found
        dec de
        ld a,d
        or e
        jr nz,.wait
        pop hl
        scf
        ret
.found:
        ; Ответ с верным номером и суммой даёт только работающая прошивка:
        ; случайный байт состояния за неё не сойдёт.
        ld a,LINK_F_SESSION|LINK_F_DROP
        call SpiLink_Send
        IFDEF ZIFI_SPI_DMA
        ; Умеет ли прошивка принимать блоки от Z80: ответ на начало сеанса.
        push af
        ld a,(SpiRsp)
        and LINK_S_PUT_CAPABLE
        ld (SpiPutCan),a
        pop af
        ENDIF
        pop hl
        ret

; Есть ли у ESP данные для Z80.
; Выход: Z=0 — есть (ZiFi_ReadBurst их получит), Z=1 — нет.
; Сохраняет BC, DE, HL.
ZiFi_RxReady:
        ld a,(SpiMore)
        or a
        ret nz
        push bc
        call SpiLink_ReadState
        pop bc
        sub LINK_TX_PENDING
        jr z,.yes
        xor a
        ret
.yes:
        inc a
        ret

; HL — приёмник не меньше ZIFI_BURST_SIZE байтов (с ключом ZIFI_SPI_DMA — с
; чётного адреса в окне #8000 или #C000). Принять очередную порцию.
; Выход: BC — число байтов, Z=1 — данных нет; CF=0. Сохраняет DE.
ZiFi_ReadBurst:
        push de
        IFDEF ZIFI_SPI_DMA
        ld (SpiBulkBuf),hl
        ; Блок уже выставлен: о нём сказал ответ, подтвердивший прежний.
        ld a,(SpiMore)
        and LINK_S_BULK
        jr nz,.fetch
.plain:
        ENDIF
        ld (SpiDest),hl
        ; Накопленное для передачи уходит этим же обменом.
        ld a,(SpiTxLen)
        ld hl,SpiMore
        or (hl)
        jr nz,.turn
        call SpiLink_ReadState
        cp LINK_TX_PENDING
        jr nz,.none
.turn:
        ld a,LINK_CHUNK
        ld (SpiRoom),a
        IFDEF ZIFI_SPI_DMA
        ld a,LINK_F_BULK
        ELSE
        xor a
        ENDIF
        call SpiLink_Turn
        jr c,.none
        IFDEF ZIFI_SPI_DMA
        ld a,(SpiRsp)
        and LINK_S_BULK|LINK_S_STALE
        jr z,.inline
        cp LINK_S_BULK
        jr z,.fetch
        ; Блок прежнего сеанса: прочитать и выбросить, обратно не слать.
        call SpiLink_BulkRead
        ld hl,SpiMore
        res 1,(hl)                      ; LINK_S_BULK: выставленного блока больше нет
        jr .none
.fetch:
        call SpiLink_BulkRead
        call SpiLink_BulkEcho
        ; Итог сравнения — в ответе на следующий запрос. Данных он не несёт:
        ; за неподтверждённым блоком очередь ESP ждёт.
        ld a,LINK_F_BULK
        call SpiLink_Send
        jr c,.none                      ; ESP замолчала: блок не подтверждён
        ld a,(SpiRsp)
        and LINK_S_ECHO_BAD
        ld bc,LINK_BULK
        jr z,.done                      ; блок верен
        ; Копия неверна. Блок выставлен снова — читать ещё раз; иначе ESP от
        ; DMA отказалась, и те же байты придут обычными ответами.
        ld a,(SpiMore)
        and LINK_S_BULK
        jr nz,.fetch
        ld hl,(SpiBulkBuf)
        jr .plain
.inline:
        ENDIF
        ld a,(SpiRsp+1)
        ld c,a
        ld b,0
        jr .done
.none:
        ld bc,0
.done:
        pop de
        ld a,b
        or c
        ret

; Передать байт. Он попадает в накопитель; обмен с ESP происходит, когда
; накопитель полон.
; Вход: A — байт. Выход: CF=0 — принят, CF=1 — ESP не отвечает или не берёт
; данные. Сохраняет A, BC, DE, HL.
ZiFi_PutChar:
        push hl
        push de
        push bc
        ld c,a
        ld a,(SpiTxLen)
        cp LINK_CHUNK
        jr c,.store
        push bc
        ld c,LINK_CHUNK
        call SpiLink_Push
        pop bc
        jr c,.done
        ld a,(SpiTxLen)
.store:
        ld e,a
        ld d,0
        ld hl,SpiTxBuf
        add hl,de
        ld (hl),c
        inc a
        ld (SpiTxLen),a                 ; CF=0 остался от сложения адреса
.done:
        ld a,c
        pop bc
        pop de
        pop hl
        ret

; Конец первой части.
SpiHeadEnd:

        ENDIF                           ; ZIFI_SPI_HEAD_PLACED

; === часть 2: обмен ==========================================================
        IFNDEF ZIFI_SPI_HEAD_ONLY

; Дослать накопленное. Вызывается в конце каждого кадра протокола.
; Выход: CF=0 — всё принято ESP, CF=1 — нет. Сохраняет BC, DE, HL.
ZiFi_Flush:
        push hl
        push de
        push bc
        ld c,1
        call SpiLink_Push
        pop bc
        pop de
        pop hl
        ret

        IFUSED ZiFi_ClearRx
; Выбросить всё, что ESP ещё не отдала.
ZiFi_ClearRx:
        push hl
        push de
        xor a
        ld (SpiMore),a
        ld a,LINK_F_DROP
        call SpiLink_Send
        pop de
        pop hl
        ret
        ENDIF

        IFUSED ZiFi_ClearTx
; Выбросить накопленное для передачи. Байты, уже принятые ESP, не вернуть —
; как и байты, ушедшие в линию UART.
ZiFi_ClearTx:
        xor a
        ld (SpiTxLen),a
        ret
        ENDIF

        IFUSED ZiFi_GetChar
; Неблокирующее чтение одного байта (обмен на каждый байт: только для редких
; случаев).
; Выход: CF=1 и A=байт либо CF=0, если данных нет. Сохраняет BC, DE, HL.
ZiFi_GetChar:
        push hl
        push de
        push bc
        call ZiFi_RxReady
        jr z,.none
        ld hl,SpiOneByte
        ld (SpiDest),hl
        ld a,1
        ld (SpiRoom),a
        xor a
        call SpiLink_Turn
        jr c,.none
        ld a,(SpiRsp+1)
        or a
        jr z,.none
        ld a,(SpiOneByte)
        scf
        jr .done
.none:
        or a
.done:
        pop bc
        pop de
        pop hl
        ret
SpiOneByte:     ds 2                    ; байт и контрольная сумма ответа
        ENDIF

        IFUSED ZiFi_PutZ
; Передать нуль-терминированную строку без самого нулевого байта.
; Вход: HL — строка; выход: CF=1 при отказе.
ZiFi_PutZ:
        ld a,(hl)
        or a
        jp z,ZiFi_Flush
        call ZiFi_PutChar
        ret c
        inc hl
        jr ZiFi_PutZ
        ENDIF

        IFUSED ZiFi_PutBlock
; Передать ровно BC байтов, включая возможные нули и CR/LF.
; Выход: CF=1 при отказе.
ZiFi_PutBlock:
        ld a,b
        or c
        jp z,ZiFi_Flush
        ld a,(hl)
        call ZiFi_PutChar
        ret c
        inc hl
        dec bc
        jr ZiFi_PutBlock
        ENDIF

        IFUSED ZiFi_SetTimeout
; DE задаёт величину тайм-аута. Программный предел равен DE << 11 опросов.
ZiFi_SetTimeout:
        ld (ZiFiTimeout),de
        xor a
        ld (ZiFiTimeout+2),a
        ld b,11
.shift:
        ld hl,(ZiFiTimeout)
        add hl,hl
        ld (ZiFiTimeout),hl
        ld a,(ZiFiTimeout+2)
        adc a,a
        ld (ZiFiTimeout+2),a
        djnz .shift
        ret

; Уменьшить программный счётчик на один опрос.
; Выход: CF=1 — время осталось, CF=0 — тайм-аут истёк. HL сохраняется.
ZiFi_CheckTimeout:
        push hl
        ld hl,(ZiFiTimeout)
        ld a,h
        or l
        jr nz,.dec_low
        ld a,(ZiFiTimeout+2)
        or a
        jr z,.expired
        dec a
        ld (ZiFiTimeout+2),a
.dec_low:
        dec hl
        ld (ZiFiTimeout),hl
        pop hl
        scf
        ret
.expired:
        pop hl
        or a
        ret
ZiFiTimeout:    ds 3                    ; 24-битный программный счётчик ожидания
        ENDIF

; Обмениваться, пока в накопителе не останется меньше C байтов.
; Выход: CF=0 — получилось; CF=1 — ESP не отвечает либо не берёт данные,
; накопленное сброшено. Портит AF, DE, HL.
SpiLink_Push:
        ld de,LINK_PUSH_TRIES
.again:
        ld a,(SpiTxLen)
        cp c
        ccf
        ret nc                          ; осталось меньше C: готово, CF=0
        push de
        push bc
        xor a
        call SpiLink_Send
        pop bc
        pop de
        ret c
        dec de
        ld a,d
        or e
        jr nz,.again
        ; ESP отвечает, но данные не берёт. Обрывок кадра ей не нужен.
        ld (SpiTxLen),a
        scf
        ret

; Прочитать байт состояния (адрес 60). Выход: A. Портит BC.
SpiLink_ReadState:
        ld b,LINK_STATE_ADDR
        call SpiLink_OpenRead
; Снять выбор ESP. Сохраняет A; флаги — по A. Портит B.
SpiLink_Close:
        ld b,a
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ld a,b
        or a
        ret

; Начать чтение общего буфера с адреса B и принять первый байт.
; Выход: A — байт по адресу B; очередное чтение порта C отдаёт следующий.
; Портит B.
;
; Перед данными Evo отдаёт два ничего не значащих байта либо один (см. начало
; файла), и заранее неизвестно, сколько. Данные опознаются по первому байту:
; читать начинают только с признака (#C3 или #3C по адресу 0, #5A или #A5 по
; адресу 60), а у признака старшая половина байта — инверсия младшей. До
; начала данных линия MISO стоит неподвижно, и принятый в это время байт —
; #FF либо #00 — признаком не бывает.
SpiLink_OpenRead:
        ld a,SPI_CMD_RDBUF
        call SpiLink_Open
        in a,(c)                        ; не данные при любом порядке
        call .first
        ret z                           ; признак: это и есть байт B
.first:
        in b,(c)
        ld a,b
        rrca
        rrca
        rrca
        rrca
        xor b
        inc a                           ; Z=1: половины взаимно обратны
        ld a,b
        ret

        IFDEF ZIFI_SPI_DMA
TS_DMASADDRL    equ #1AAF               ; адрес источника DMA: младший байт, за
                                        ; ним (#1BAF, #1CAF) старший и страница ОЗУ
TS_DMADADDRL    equ #1DAF               ; адрес приёмника: так же #1DAF..#1FAF
TS_DMALEN       equ #26AF               ; слов в блоке минус один
TS_DMACTR       equ #27AF               ; запуск; при чтении бит 7 — DMA занят
TS_DMANUM       equ #28AF               ; блоков минус один
TS_DMA_RAM_RAM  equ #01
TS_DMA_SPI_RAM  equ #02
TS_DMA_RAM_SPI  equ #82
SPI_CMD_WRDMA   equ #03
SPI_CMD_RDDMA   equ #04
SPI_CMD_WR_DONE equ #07
SPI_CMD_RD_DONE equ #08
; Откуда читать 512 байтов, чтобы вытеснить из кэша процессора строки
; приёмника (см. ниже): начало страницы плагина. Приёмник лежит выше.
SPI_CACHE_SWEEP equ #8000

; Отослать блок по адресу (SpiBulkBuf) обратно в ESP через DMA TS-Config.
; Портит AF, BC, HL.
SpiLink_BulkEcho:
        ld bc,TS_DMASADDRL
        ld a,SPI_CMD_WRDMA
        call SpiLink_BulkOpen
        ld a,TS_DMA_RAM_SPI
        call SpiLink_BulkRun
        ld a,SPI_CMD_WR_DONE
; Конец чтения или записи — отдельная транзакция из одной команды A.
SpiLink_BulkDone:
        ld b,a
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        ld a,b
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ret

; Задать DMA блок LINK_BULK байтов по адресу (SpiBulkBuf) и начать транзакцию
; SPI. Вход: BC — порт младшего байта адреса (источника либо приёмника), A —
; команда ESP. За командой уходят байт адреса и пустой байт. Сохраняет DE.
SpiLink_BulkOpen:
        push af
        ld hl,(SpiBulkBuf)
        call SpiLink_DmaAddr
        ld b,high TS_DMALEN
        ld a,LINK_BULK/2-1
        out (c),a
        ld b,high TS_DMANUM
        xor a
        out (c),a
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        pop af
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)            ; пошёл байт адреса
        in a,(ZIFI_SPI_DATA)            ; пошёл пустой байт
        ret

; Адрес HL (окно #8000..#FFFF) — в три регистра DMA подряд с порта BC: младший
; байт, старший в пределах страницы и номер страницы ОЗУ, которую WC держит в
; этом окне. Выход: B — у третьего регистра. Портит A.
SpiLink_DmaAddr:
        out (c),l
        inc b
        ld a,h
        and #3F
        out (c),a
        inc b
        ld a,(WC_PAGE2)
        bit 6,h
        jr z,.page
        ld a,(WC_PAGE3)
.page:
        out (c),a
        ret

; Скопировать BC байтов из (HL) в (DE); оба адреса — в окнах #8000..#FFFF.
; Адреса одной чётности переносит DMA «ОЗУ → ОЗУ» кусками до 512 байтов (кусок
; не должен пересекать границу окна), разной — LDIR: DMA ходит словами.
; Записанное DMA минует кэш процессора: читать это Z80 можно только после
; SpiLink_Sweep. Выход: HL, DE — за скопированным, BC=0. Портит AF.
SpiLink_Copy:
        ld a,l
        xor e
        rra
        jr c,.ldir                      ; разная чётность
        bit 0,l
        jr z,.even
        ld a,b
        or c
        ret z
        ldi                             ; оба нечётные: байт, и дальше оба чётные
.even:
        ld a,b
        cp high LINK_BULK
        jr nc,.full
        or a
        jr nz,.part
        ld a,c
        cp 2
        jr c,.ldir                      ; остался байт либо ничего
.part:
        push bc
        res 0,c                         ; кусок — чётная часть остатка
        jr .run
.full:
        push bc
        ld bc,LINK_BULK
.run:
        ; BC — байтов в куске (чётное, 2..512); на стеке — сколько всего.
        ld a,b
        rra                             ; бит 0 B — в CF
        ld a,c
        rra                             ; A — слов в куске (512 байтов — 0)
        dec a                           ; слов минус один
        push bc
        push de
        push hl
        ld bc,TS_DMALEN
        out (c),a
        ld b,high TS_DMANUM
        xor a
        out (c),a
        ld b,high TS_DMASADDRL
        call SpiLink_DmaAddr            ; источник
        ex de,hl
        ld b,high TS_DMADADDRL
        call SpiLink_DmaAddr            ; приёмник
        ld b,high TS_DMACTR
        ld a,TS_DMA_RAM_RAM
        out (c),a
.wait:
        in a,(c)
        rla
        jr c,.wait
        pop hl
        pop de
        pop bc
        add hl,bc
        ex de,hl
        add hl,bc
        ex de,hl
        ex (sp),hl                      ; HL — сколько было, на стеке — источник
        or a
        sbc hl,bc
        ld b,h
        ld c,l
        pop hl
        jr .even
.ldir:
        ld a,b
        or c
        ret z
        ldir
        ret

; Вытеснить из кэша процессора всё, что в нём есть: после записи DMA там могли
; остаться прежние байты приёмника. 512 чтений подряд из другого места
; занимают все его строки (приём драйвера TS-Labs); читает их POP — так вдвое
; быстрее LDIR. Стек на это время чужой, поэтому прерывания закрыты.
; Портит AF, B, HL.
SpiLink_Sweep:
        ld a,i
        push af                         ; P/V — были ли прерывания разрешены
        di
        ld (.stack+1),sp
        ld sp,SPI_CACHE_SWEEP
        ld b,LINK_BULK/8
.sweep:
        pop hl
        pop hl
        pop hl
        pop hl
        djnz .sweep
.stack:
        ld sp,0
        pop af
        ret po
        ei
        ret

; Запустить DMA в режиме A, дождаться конца и снять выбор ESP.
SpiLink_BulkRun:
        ld bc,TS_DMACTR
        out (c),a
.wait:
        in a,(c)
        rla
        jr c,.wait
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ret

; Отдать в ESP через DMA LINK_BULK байтов по HL (чётный адрес в окне
; #8000..#FFFF). Вход: A — их XOR. Накопленные байты обмена уходят в запросе
; LINK_F_PUT, раньше блока. Если ESP сейчас принять блок не может (в её очереди
; меньше места, чем блок) либо он трижды не сошёлся, те же байты уходят
; обычными обменами.
; Выход: CF=0 — отдан; CF=1 — ESP не отвечает (накопленное сброшено, как при
; отказе SpiLink_Push). Портит AF, BC, DE, HL.
SpiLink_PutBlock:
        ld (SpiPutXor),a
        ld (SpiBulkBuf),hl
        ld e,LINK_PUT_TRIES
.ask:
        ld a,(SpiPutXor)
        ld (SpiRoom),a                  ; байт 4 запроса — XOR блока
        ld hl,SpiRsp+3
        ld (SpiDest),hl                 ; данных в ответ не будет
        ld a,LINK_F_PUT
        push de
        call SpiLink_Turn
        pop de
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_READY
        jr z,.mailbox                   ; места в очереди ESP на блок нет
        ld a,(SpiTxLen)
        or a
        jr nz,.mailbox                  ; накопленное взято не всё: блок за ним не пойдёт
        push de
        call SpiLink_BulkEcho           ; DMA «ОЗУ → SPI»: #03, данные, #07
        ld a,LINK_F_PUT_DONE
        call SpiLink_Send
        pop de
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_OK
        ret nz                          ; принят; CF=0
        dec e                           ; не сошёлся: ещё раз
        jr nz,.ask
.mailbox:
        ; Те же байты — обычными обменами, по 54, следом за накопленным.
        ld hl,(SpiBulkBuf)
        ld bc,LINK_BULK
.byte:
        ld a,(hl)
        call ZiFi_PutChar
        ret c
        cpi                             ; HL+1, BC-1; флаги — по BC
        jp pe,.byte
        or a
        ret

; Забрать блок LINK_BULK байтов через DMA TS-Config по адресу (SpiBulkBuf).
; Портит AF, BC, HL.
SpiLink_BulkRead:
        ld bc,TS_DMADADDRL
        ld a,SPI_CMD_RDDMA
        call SpiLink_BulkOpen
        in a,(ZIFI_SPI_DATA)            ; пошёл первый байт данных: DMA берёт уже принятое
        ld a,TS_DMA_SPI_RAM
        call SpiLink_BulkRun
        ld a,SPI_CMD_RD_DONE
        call SpiLink_BulkDone
        ; DMA пишет в память мимо кэша процессора.
        jp SpiLink_Sweep
        ENDIF

; Выбрать ESP и передать команду A с адресом B. Выход: C — порт данных.
SpiLink_Open:
        ld c,a
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        ld a,c
        ld c,ZIFI_SPI_DATA
        out (c),a
        out (c),b
        ret

; Обмен без приёма данных: только отдать накопленное.
; Вход: A — флаги запроса. Выход — как у SpiLink_Turn.
SpiLink_Send:
        ld hl,SpiRsp+3
        ld (SpiDest),hl                 ; сюда попадёт одна контрольная сумма
        ld hl,SpiRoom
        ld (hl),0
; Один обмен: отдать накопленное и принять до (SpiRoom) байтов по (SpiDest).
; Вход: A — флаги запроса.
; Выход: CF=0 — ответ получен: остаток неотданного в начале SpiTxBuf, число
;        принятых байтов в SpiRsp+1; CF=1 — ESP не ответила, накопленное
;        сброшено.
; Портит AF, BC, DE, HL.
SpiLink_Turn:
        ld hl,SpiReq+2
        ld (hl),a                       ; флаги
        dec hl
        inc (hl)                        ; номер запроса
        dec hl
        ld (hl),LINK_REQUEST
        ; Сумма — дополнение суммы заголовка и данных до #FF.
        ld a,(SpiTxLen)
        add a,LINK_HEADER
        ld b,a
        xor a
        call SpiLink_Sum
        cpl
        ld (hl),a                       ; сразу за данными
        ld a,LINK_ATTEMPTS
.send:
        ld (SpiAttempts),a
        ld a,SPI_CMD_WRBUF
        ld b,0
        call SpiLink_Open               ; запись с адреса 0
        out (c),b                       ; пустой байт
        ld hl,SpiReq
        ld a,(SpiTxLen)
        add a,LINK_HEADER+1
        ld b,a
        otir
        call SpiLink_Close
        ld de,LINK_POLLS
.poll:
        ld b,0
        call SpiLink_OpenRead           ; байт 0: признак
        cp LINK_RESPONSE
        jr nz,.wait                     ; там ещё наш запрос
        in a,(c)                        ; байт 1: номер
        ld hl,SpiReq+1
        cp (hl)
        jr nz,.wait
        ; Ответ на наш запрос. Остальные байты ESP записала раньше признака.
        ld hl,SpiRsp
        ini                             ; статус
        ini                             ; длина данных
        ini                             ; сколько байтов запроса взято
        ld a,(SpiRoom)
        ld hl,SpiRsp+1
        cp (hl)
        jr c,.wait                      ; данных больше, чем просили: мусор
        ld b,(hl)
        inc b                           ; данные и контрольная сумма
        ld hl,(SpiDest)
        inir
        call SpiLink_Close
        ; Сумма всех байтов ответа вместе с контрольным равна #FF.
        ld hl,SpiRsp
        ld b,3
        ld a,(SpiReq+1)
        add a,LINK_RESPONSE
        call SpiLink_Sum
        ld c,a
        ld a,(SpiRsp+1)
        inc a
        ld b,a
        ld a,c
        ld hl,(SpiDest)
        call SpiLink_Sum
        inc a
        jr nz,.next                     ; прочитано, пока ESP ещё писала: перечитать

        ; --- ответ принят -----------------------------------------------------
        ld a,(SpiRsp)
        IFDEF ZIFI_SPI_DMA
        and LINK_S_MORE|LINK_S_BULK     ; выставленный блок — тоже данные для Z80
        ELSE
        and LINK_S_MORE
        ENDIF
        ld (SpiMore),a
        ; Невзятый остаток сдвигается в начало и уйдёт следующим обменом.
        ld hl,SpiRsp+2
        ld c,(hl)
        ld a,(SpiTxLen)
        sub c
        jr nc,.keep
        xor a                           ; взято больше, чем послано: не бывает
.keep:
        ld (SpiTxLen),a
        ret z                           ; всё взято; CF=0
        ld b,0
        ld hl,SpiTxBuf
        ld d,h
        ld e,l
        add hl,bc
        ld c,a
        ldir
        or a
        ret

.wait:
        call SpiLink_Close
.next:
        dec de
        ld a,d
        or e
        jp nz,.poll
        ld a,(SpiAttempts)
        dec a
        jp nz,.send
        ; ESP не ответила. Накопленное теряется вместе со своим кадром: иначе
        ; его обрывок ушёл бы перед следующим кадром и испортил оба.
        ld (SpiTxLen),a
        ld (SpiMore),a
        scf
        ret

; A += сумма B байтов по HL. Выход: HL — за последним байтом.
SpiLink_Sum:
        add a,(hl)
        inc hl
        djnz SpiLink_Sum
        ret

; Данные драйвера стоят во второй части: в первой у плагина SMB места в обрез.
SpiMore:        db 0                    ; ESP сообщила, что данные ещё есть
        IFDEF ZIFI_SPI_DMA
SpiBulkBuf:     dw 0                    ; где блок DMA: куда его принять либо откуда отдать
SpiPutCan:      db 0                    ; прошивка принимает блоки от Z80
SpiPutXor:      db 0
        ENDIF
SpiAttempts:    db 0
SpiDest:        dw 0                    ; куда принять данные ответа
SpiRsp:         ds 4                    ; [статус][длина][принято] и место суммы
; Запрос лежит одним куском и уходит одной командой OTIR. Поля заголовка
; служат и переменными: номер запроса, длина накопленного, место под ответ.
SpiReq:         ds LINK_HEADER
SpiTxLen        equ SpiReq+3
SpiRoom         equ SpiReq+4
SpiTxBuf:       ds LINK_CHUNK
                db 0                    ; место контрольной суммы полного куска

        ENDIF                           ; ZIFI_SPI_HEAD_ONLY
