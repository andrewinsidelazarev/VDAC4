; Двоичный обмен с нативной прошивкой ESP32-S3.
;
; Формат пакета одинаков в обе стороны:
;   [SYNC=#5A][CMD][LEN_L][LEN_H][DATA…][CSUM]  — поля пакета по порядку
;   CSUM = XOR байта CMD со всеми последующими байтами (LEN_L, LEN_H, DATA).
;
; В старых текстовых протоколах асинхронные строки и двоичные данные легко
; рассинхронизировали Z80. Здесь длина известна заранее, экранирование не нужно,
; а CR/LF и нули внутри payload безопасны сами по себе.
;
; ESP не пишет в линию по своей инициативе: он либо отвечает на наш пакет, либо
; присылает файловый запрос, когда обслуживает FTP-клиента.

PROTO_SYNC      equ #5A
PROTO_MAX       equ 1024                ; предел payload, как и на стороне ESP
PROTO_ERR_TEXT  equ 64                  ; буфер текста доклада об ошибке

CMD_ECHO        equ #00
CMD_WIFI_INI    equ #03                 ; отдать zifi.ini целиком, разбирает ESP
CMD_PING        equ #04
CMD_GET_STEP    equ #05
CMD_FTP_START   equ #06
CMD_FTP_STOP    equ #07
CMD_UPDATE_START equ #08
CMD_UPDATE_STOP equ #09
CMD_SMB_START   equ #0B
CMD_SMB_STOP    equ #0C
CMD_ONLINE_UPDATE equ #0D              ; GitHub SHA-256 + firmware.bin -> OTA
CMD_ONLINE_UPDATE_CHECK equ #0E        ; только версия и SHA из manifest
CMD_SYS_RESET   equ #0F
CMD_SYS_INFO    equ #02
CMD_NET_IPCONFIG equ #21               ; текущие IP, маска, шлюз, DNS
CMD_NET_NTP     equ #22                ; точное время; ESP уже учла time: из ini
CMD_NET_PROXY_STATUS equ #23           ; статус HTTP-прокси (0=выкл, 1=вкл, 2=недоступен)
CMD_WEATHER_GET equ #24                ; запись погоды для заставки, без payload
; Проверка и обновление Wild Commander с GitHub (плагин WC Update).
; START: репозиторий,0,ветка,0,каталог,0,защищённые пути,0...,0 — ESP сама
; сверяет SHA и шлёт события; APPLY: номера строк списка по байту.
CMD_WCU_START   equ #25
CMD_WCU_APPLY   equ #26
CMD_WCU_STOP    equ #27
CMD_WCU_SYNC    equ #28                ; выдать заново список и состояние
CMD_SYS_CLOCK   equ #29                ; часы ESP: [год LE16][месяц][число][ч][мин][с], местное
; Звук платы VDAC4 (I2S -> PCM5102). Ответ приходит сразу, без ACK.
; Форматы — Docs/VDAC4.md. Прошивка переходника ZiFi этих команд не знает.
CMD_AUDIO_OPEN  equ #30                ; начать поток MP3
CMD_AUDIO_DATA  equ #31                ; блок MP3, до 1024 байт
CMD_AUDIO_CLOSE equ #32                ; [0] оборвать, [1] доиграть накопленное
CMD_AUDIO_STATUS equ #33
CMD_AUDIO_TONE  equ #34                ; [каналы][частота LE16][мс LE16][уровень]
CMD_AUDIO_VOLUME equ #35               ; [0..255]
CMD_AUDIO_PAUSE equ #36                ; [1] пауза, [0] продолжить
CMD_AUDIO_PROBE equ #37                ; тест линий ЦАП
CMD_AUDIO_SEEK  equ #38                ; модуль: [шаг по таблице порядка со знаком]
CMD_AUDIO_FLUSH equ #39                ; MP3 после перемотки: [время, мс LE32]
CMD_AUDIO_INFO  equ #3A                ; формат, каналы, длина, название
CMD_AUDIO_COVER equ #3B                ; кусок метки ID3: [смещение LE32][данные]
CMD_AUDIO_THUMB equ #3C                ; миниатюра обложки: [часть 0..3]
CMD_AUDIO_PICTURE equ #3D              ; картинка файлом: [длина LE32][смещение LE32][данные]
CMD_AUDIO_VIEW  equ #2F                ; окно: спектр или уровни каналов, позиция, длительность (#3E — LINK_PROBE)
CMD_AUDIO_TAGS  equ #3F                ; метки: название, исполнитель, альбом, год, номер
; Модули ESP (ELF из /WC/ESP/): доставка в кэш flash ESP и запуск приложения.
CMD_MOD_QUERY   equ #69                ; [флаги][длина LE32][время FAT LE32][имя, 0]
CMD_MOD_PUT     equ #6A                ; [смещение LE32][данные]
CMD_MOD_COMMIT  equ #6B
CMD_APP_START   equ #6C                ; [имя, 0]
CMD_APP_STOP    equ #6D
; TCP-клиент: подключиться, отправить, забрать порцию, закрыть. Данные тянет
; Z80 — приёмная очередь ZiFi всего 256 байт, и непрошеный поток её переполнит.
CMD_NET_OPEN    equ #10                ; адрес,0,порт LE16 (без порта — 80)
CMD_NET_SEND    equ #11                ; байты как есть, обычно HTTP-запрос
CMD_NET_RECV    equ #12                ; сколько байт максимум, LE16
CMD_NET_CLOSE   equ #13
; Готовый запрос GET: ESP подключается, отправляет запрос и отбрасывает
; заголовок ответа. Z80 получает только тело — ни склейки текста запроса, ни
; посимвольного поиска CR/LF/CR/LF на нём больше нет.
CMD_NET_HTTP_GET equ #14               ; адрес,0,порт LE16,путь,0
CMD_NET_PING    equ #20                ; ICMP-эхо: имя узла с нулём на конце
EVT_FTP_CLIENT  equ #60
EVT_FTP_COMMAND equ #61
EVT_SMB_CLIENT  equ #62
EVT_SMB_COMMAND equ #63
EVT_SMB_PROGRESS equ #64               ; готовая строка хода передачи файла
EVT_ONLINE_UPDATE_PROGRESS equ #65     ; [этап][проценты]
EVT_WIFI_SIGNAL equ #66                ; готовая ASCII-шкала для Status
EVT_WCU_STATE   equ #67                ; [этап][тек LE16][всего LE16][%][текст]
EVT_WCU_ENTRY   equ #68                ; [№][состояние][флаги][SD LE24][GitHub LE24][путь]

RESP_SYS_INFO   equ #82
RESP_WIFI_INI   equ #83
RESP_GET_STEP   equ #85                 ; метка шага и текст последней ошибки
RESP_FTP_START  equ #86
RESP_FTP_STOP   equ #87
RESP_UPDATE_START equ #88
RESP_UPDATE_STOP equ #89
RESP_SMB_START  equ #8B
RESP_SMB_STOP   equ #8C
RESP_ONLINE_UPDATE equ #8D             ; [успех][SHA-256 32 байта]
RESP_ONLINE_UPDATE_CHECK equ #8E       ; [успех][relation][версия ASCII]
ONLINE_UPDATE_SAME      equ 0
ONLINE_UPDATE_NEWER     equ 1
ONLINE_UPDATE_OLDER     equ 2
ONLINE_UPDATE_DIFFERENT equ 3
RESP_NET_IPCONFIG equ #A0
RESP_NET_PING   equ #A1
RESP_NET_NTP    equ #A2                ; ГГГГММДДЧЧММСС, 14 символов ASCII
RESP_NET_PROXY_STATUS equ #A3          ; [status(1B)][endpoint(ASCII)]
RESP_WEATHER    equ #A4                 ; запись погоды (см. Weather screensaver/src/weather.asm)
RESP_WCU_START  equ #A5                 ; [1 — проверка запущена]
RESP_WCU_APPLY  equ #A6                 ; [1 — список принят]
RESP_WCU_STOP   equ #A7                 ; [1 — задача остановлена]
RESP_WCU_SYNC   equ #A8                 ; [1 — повтор поставлен в очередь]
RESP_SYS_CLOCK  equ #A9                 ; [1 — часы ESP выставлены, 0 — время негодное]
RESP_AUDIO_OPEN equ #B0                 ; [1 — открыт][свободно LE32]
RESP_AUDIO_DATA equ #B1                 ; [1 принят / 0 нет места / 2 закрыт][свободно LE32]
RESP_AUDIO_CLOSE equ #B2
RESP_AUDIO_STATUS equ #B3               ; запись состояния, 32 байта (до 0.1.20 — 24)
RESP_AUDIO_TONE equ #B4                 ; [1 — тон запущен]
RESP_AUDIO_VOLUME equ #B5
RESP_AUDIO_PAUSE equ #B6
RESP_AUDIO_PROBE equ #B7                ; [1 — измерено][24 байта, мВ/20]
RESP_AUDIO_SEEK equ #B8                 ; [1 — перешёл][позиция LE16]
RESP_AUDIO_FLUSH equ #B9                ; [1 — очередь пуста][свободно LE32]
RESP_AUDIO_INFO equ #BA                 ; [формат][каналы][длина LE16][название, 0]
RESP_AUDIO_COVER equ #BB                ; [0 шли / 1 собрана / 2 нет][место LE32][свободно LE32]
RESP_AUDIO_THUMB equ #BC                ; [0 сводится / 1 вот / 2 нет][часть][512 байт]
RESP_AUDIO_PICTURE equ #BD              ; [0 шли / 1 собрана / 2 нет][принято LE32][свободно LE32]
RESP_AUDIO_VIEW equ #AF                 ; [вид][число][длительность с LE16][позиция с LE16][значения]
RESP_AUDIO_TAGS equ #BF                 ; [название,0][исполнитель,0][альбом,0][год,0][номер,0]
RESP_MOD_QUERY  equ #E9                 ; [0 есть / 1 шли / 2 нет места / 3 плохое имя][свободно LE32]
RESP_MOD_PUT    equ #EA                 ; [1 принято / 0 не то место][принято всего LE32]
RESP_MOD_COMMIT equ #EB                 ; [1 положен / 0 нет]
RESP_APP_START  equ #EC                 ; [итог int8][имя, 0][версия, 0]
RESP_APP_STOP   equ #ED
RESP_NET_OPEN   equ #90
RESP_NET_SEND   equ #91
; Первый байт: 0 — порция (пустая значит «данных пока нет, спроси снова»),
; 1 — сервер закрыл соединение. Путать нельзя: файл оборвётся на середине.
RESP_NET_RECV   equ #92
RESP_NET_CLOSE  equ #93
; Признак, код ответа HTTP (LE16) и длина тела (LE32, ноль — сервер не назвал).
RESP_NET_HTTP_GET equ #94
RESP_READY      equ #F0
RESP_ERROR      equ #EE                 ; доклад ESP об исключении
RESP_ACK        equ #FE

; --- передача ---------------------------------------------------------------

; Отправить пакет: A=CMD, HL=данные, BC=длина.
; Разрушает A/BC/DE/HL. Тайм-аут очереди передачи обрабатывает ZiFi_PutChar.
Proto_Send:
        IFDEF ZIFI_SPI_DMA
        ld de,0
        ld (ProtoTailLen),de
        ld e,a
        xor a
        ld (ProtoHold),a
        ld a,e
; Отправить пакет из двух частей: A=CMD, HL/BC — первая, (ProtoTail) и
; (ProtoTailLen) — вторая. Так данные окна уходят прямо со страницы, где они
; лежат, без копии в буфер кадра. (ProtoHold)<>0 — сумму кадра не досылать: она
; уйдёт следующим обменом, обычно в запросе блока следующего кадра.
Proto_Send2:
        ENDIF
        ld (ProtoCmd),a
        ld (ProtoPtr),hl
        ld (ProtoLen),bc
        xor a
        ld (ProtoCsum),a

        ld a,PROTO_SYNC                 ; SYNC в контрольную сумму не входит
        call ZiFi_PutChar
        ret c
        ld a,(ProtoCmd)
        call Proto_PutSum
        ret c
        ld hl,(ProtoLen)
        IFDEF ZIFI_SPI_DMA
        ld de,(ProtoTailLen)
        add hl,de
        ENDIF
        ld a,l
        call Proto_PutSum
        ret c
        ld a,h
        call Proto_PutSum
        ret c

        ld hl,(ProtoPtr)
        ld bc,(ProtoLen)
        call Proto_Body
        ret c
        IFDEF ZIFI_SPI_DMA
        ld hl,(ProtoTail)
        ld bc,(ProtoTailLen)
        call Proto_Body
        ret c
        ENDIF
        ld a,(ProtoCsum)
        call ZiFi_PutChar
        ret c
        IFDEF ZIFI_SPI_DMA
        ld a,(ProtoHold)
        or a
        ret nz                          ; CF=0
        ENDIF
        ; Кадр целиком в канале: на SPI это досылка накопленных байтов, на UART
        ; процедура пуста.
        jp ZiFi_Flush

; Отправить BC байтов по HL, учитывая их в сумме кадра.
; Выход: CF=1 — отказ канала. Разрушает A/BC/DE/HL.
Proto_Body:
        IFDEF ZIFI_SPI
        ; По SPI байт стоит десятки тактов, и побайтовая передача (вызов на
        ; каждый байт) съедала бы скорость канала. Данные кладутся в накопитель
        ; обмена кусками, сколько в нём есть места; сумма считается заодно.
.body:
        ld a,b
        or c
        ret z                           ; CF=0
        IFDEF ZIFI_SPI_DMA
        ; Данных на блок DMA и больше: блок уходит прямо отсюда, без копии —
        ; DMA читает его по адресу (чётному, в окне #8000 или #C000).
        ld a,(SpiPutCan)
        or a
        jr z,.mailbox
        bit 7,h
        jr z,.mailbox                   ; ниже #8000 страниц DMA не знает
        ld a,b
        cp high LINK_BULK
        jr c,.mailbox                   ; меньше блока
        bit 0,l
        jr z,.direct
        ; Нечётный адрес: байт обычным путём, тогда блок — с чётного.
        cp high LINK_BULK
        jr nz,.one
        ld a,c
        or a
        jr z,.mailbox                   ; ровно блок: без первого байта его не набрать
.one:
        ld a,(hl)
        call Proto_PutSum
        ret c
        inc hl
        dec bc
        jr .body
.direct:
        push bc
        push hl
        ld bc,LINK_BULK
        xor a
        call Proto_XorRun               ; A — XOR блока, HL — за ним
        ld e,a
        ld a,(ProtoCsum)
        xor e
        ld (ProtoCsum),a
        ex (sp),hl                      ; HL — блок, на стеке — за ним
        ld a,e
        call SpiLink_PutBlock
        pop hl
        pop bc
        ret c
        push hl
        ld hl,-LINK_BULK
        add hl,bc
        ld b,h
        ld c,l
        pop hl
        jr .body
.mailbox:
        ENDIF
        ld a,(SpiTxLen)
        cp LINK_CHUNK
        jr c,.room
        push hl
        push bc
        ld c,LINK_CHUNK
        call SpiLink_Push               ; накопитель полон: обмен с ESP
        pop bc
        pop hl
        ret c
        jr .body
.room:
        ld e,a
        ld d,0
        push hl
        ld hl,SpiTxBuf
        add hl,de
        ex de,hl                        ; DE — свободное место, L — занято
        ld a,LINK_CHUNK
        sub l                           ; A — сколько места
        pop hl
        inc b
        dec b
        jr nz,.take                     ; осталось больше 255 байтов
        cp c
        jr c,.take
        ld a,c
.take:
        push bc
        ld c,a
        ld b,0
        push bc                         ; длина куска
        ld a,(ProtoCsum)
.copy:
        xor (hl)
        ldi
        jp pe,.copy
        ld (ProtoCsum),a
        pop bc
        ld a,(SpiTxLen)
        add a,c
        ld (SpiTxLen),a
        ex (sp),hl                      ; HL — сколько оставалось
        or a
        sbc hl,bc
        ld b,h
        ld c,l
        pop hl
        jp .body
        ELSE
.body:
        ld a,b
        or c
        ret z                           ; CF=0
        ld a,(hl)
        push hl
        push bc
        call Proto_PutSum
        pop bc
        pop hl
        ret c
        inc hl
        dec bc
        jr .body
        ENDIF

        IFDEF ZIFI_SPI_DMA
; A ^= XOR BC байтов по HL (BC — не больше 2040). Проход только читает: 14,6
; такта на байт против 33 у копирования с суммой.
; Выход: HL — за ними. Сохраняет BC, DE.
Proto_XorRun:
        push bc
        push de
        ld d,a
        ld a,c
        and 7
        ld e,a                          ; остаток от восьмёрок
        srl b
        rr c
        srl b
        rr c
        srl b
        rr c
        ld b,c                          ; восьмёрок
        ld a,d
        inc b
        dec b
        jr z,.tail
.eight:
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        djnz .eight
.tail:
        ld b,e
        inc b
        dec b
        jr z,.done
.one:
        xor (hl)
        inc hl
        djnz .one
.done:
        pop de
        pop bc
        ret
        ENDIF

; Отправить байт из A и учесть его в контрольной сумме.
Proto_PutSum:
        ld e,a
        ld a,(ProtoCsum)
        xor e
        ld (ProtoCsum),a
        ld a,e
        jp ZiFi_PutChar

; Отправить пакет без данных: A=CMD.
Proto_SendEmpty:
        ld hl,0
        ld bc,0
        jp Proto_Send

; --- приём ------------------------------------------------------------------

; Неблокирующий разбор: ZiFi читается через INIR-порции до #BF байт, после чего
; пакет собирается из RAM. Пакет отдаётся только целиком и только с верной
; контрольной суммой; битый или слишком длинный молча отбрасывается.
; Выход: CF=1 и A=CMD — пакет готов (payload в ProtoBuf, длина в ProtoRxLen);
;        CF=0 — данных пока недостаточно.
Proto_Poll:
        IFDEF ZIFI_SPI
        ; Данные кадра — всей принятой порцией сразу: копирование и сумма в
        ; одном коротком цикле. Побайтовый разбор ниже остаётся для заголовка.
        ld a,(ProtoState)
        cp 4
        jp nz,.byte
        ld bc,(ProtoBurstLeft)
        ld a,b
        or c
        jr nz,.block
        ld hl,ProtoBurstBuf
        ld (ProtoBurstPtr),hl
        call ZiFi_ReadBurst
        ret z                           ; данных пока нет; CF=0
        ld (ProtoBurstLeft),bc
.block:
        ; BC — байтов в порции (она же в ProtoBurstLeft).
        ld hl,(ProtoRxLen)
        ld de,(ProtoRxCount)
        or a
        sbc hl,de                       ; сколько данных кадру не хватает
        or a
        sbc hl,bc
        jr nc,.take                     ; порция уходит кадру целиком
        add hl,bc
        ld b,h
        ld c,l                          ; кадр кончается внутри порции
.take:
        ; BC — сколько берём.
        ld hl,(ProtoBurstLeft)
        or a
        sbc hl,bc
        ld (ProtoBurstLeft),hl
        ex de,hl
        add hl,bc
        ld (ProtoRxCount),hl
        ld hl,(ProtoBurstPtr)
        ld de,(ProtoRxPtr)
        IFDEF ZIFI_SPI_DMA
        ; Длинный кусок: сумма — отдельным проходом, перенос — DMA «ОЗУ → ОЗУ»,
        ; если адреса одной чётности (прошивка дополняет кадры до чётной длины,
        ; чтобы так и было).
        ld a,b
        or a
        jr nz,.long
        ld a,c
        cp PROTO_DMA_MIN
        jr c,.short
.long:
        ld a,l
        xor e
        rra
        jr c,.short
        ld a,(ProtoCsumTmp)
        call Proto_XorRun
        ld (ProtoCsumTmp),a
        ld hl,(ProtoBurstPtr)
        call SpiLink_Copy
        ld a,1
        ld (ProtoDmaUsed),a
        jr .copied
.short:
        ENDIF
        ld a,(ProtoCsumTmp)
.copy:
        xor (hl)
        ldi
        jp pe,.copy
        ld (ProtoCsumTmp),a
.copied:
        ld (ProtoBurstPtr),hl
        ld (ProtoRxPtr),de
        ld hl,(ProtoRxCount)
        ld de,(ProtoRxLen)
        or a
        sbc hl,de
        jp nz,Proto_Poll                ; кадр не кончился: за следующей порцией
        IFDEF ZIFI_SPI_DMA
        ; Перенесённое DMA могло остаться в кэше процессора прежним.
        ld a,(ProtoDmaUsed)
        or a
        jr z,.fresh
        xor a
        ld (ProtoDmaUsed),a
        call SpiLink_Sweep
.fresh:
        ENDIF
        ld a,5
        ld (ProtoState),a
.byte:
        ENDIF
        call Proto_GetByte
        ret nc                          ; очередь пуста — пакета нет
        ld e,a                          ; E = принятый байт
        ld a,(ProtoState)

        or a
        jr nz,.st1
        ld a,e                          ; ждём SYNC, мусор вне пакета пропускаем
        cp PROTO_SYNC
        jp nz,Proto_Poll
        ld a,1
        ld (ProtoState),a
        jp Proto_Poll

.st1:
        dec a
        jr nz,.st2
        ld a,e                          ; команда
        ld (ProtoRxCmd),a
        xor a
        ld (ProtoCsumTmp),a
        ld a,(ProtoRxCmd)
        ld (ProtoCsumTmp),a             ; сумма начинается с CMD
        ld a,2
        ld (ProtoState),a
        jp Proto_Poll

.st2:
        dec a
        jr nz,.st3
        ld a,e                          ; младший байт длины
        ld (ProtoRxLen),a
        call Proto_AddCsum
        ld a,3
        ld (ProtoState),a
        jp Proto_Poll

.st3:
        dec a
        jr nz,.st4
        ld a,e                          ; старший байт длины
        ld (ProtoRxLen+1),a
        call Proto_AddCsum
        ld hl,ProtoBuf
        ld (ProtoRxPtr),hl
        ld hl,0
        ld (ProtoRxCount),hl
        ; Длиннее буфера пакет быть не может: ESP держит тот же предел.
        ld hl,(ProtoRxLen)
        ld bc,PROTO_MAX
        or a
        sbc hl,bc
        jr z,.len_ok
        jr c,.len_ok
        call Proto_ResetRx              ; невозможная длина — пакет игнорируем
        jp Proto_Poll
.len_ok:
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        ld a,4
        jr nz,.set
        ld a,5                          ; данных нет — сразу сумма
.set:
        ld (ProtoState),a
        jp Proto_Poll

.st4:
        IFNDEF ZIFI_SPI                 ; по SPI данные принимает блочный путь выше
        dec a
        jr nz,.st5
        ld hl,(ProtoRxPtr)              ; байт данных
        ld (hl),e
        inc hl
        ld (ProtoRxPtr),hl
        ld a,e
        call Proto_AddCsum
        ld hl,(ProtoRxCount)
        inc hl
        ld (ProtoRxCount),hl
        ld bc,(ProtoRxLen)
        ld a,h
        cp b
        jp nz,Proto_Poll
        ld a,l
        cp c
        jp nz,Proto_Poll
        ld a,5
        ld (ProtoState),a
        jp Proto_Poll
        ENDIF

.st5:
        ld a,(ProtoCsumTmp)             ; сравнить накопленную сумму
        ld d,a
        call Proto_ResetRx
        ld a,e
        cp d
        jp nz,Proto_Poll                ; битый пакет — ищем следующий SYNC
        ld a,(ProtoRxCmd)
        scf
        ret

; Вернуть следующий байт из локальной INIR-порции. Новое обращение к ZIFR
; выполняется только после исчерпания уже принятой порции.
; Выход: CF=1 и A=байт либо CF=0, если очередь ZiFi пуста.
Proto_GetByte:
        IFDEF ZIFI_SPI
        ; У SPI порция бывает длиннее 255 байтов (блок DMA): счётчик — слово.
        ld hl,(ProtoBurstLeft)
        ld a,h
        or l
        jr nz,.have
        ld hl,ProtoBurstBuf
        ld (ProtoBurstPtr),hl
        call ZiFi_ReadBurst
        ret z                           ; CF=0
        ld h,b
        ld l,c
.have:
        dec hl
        ld (ProtoBurstLeft),hl
        ld hl,(ProtoBurstPtr)
        ld a,(hl)
        inc hl
        ld (ProtoBurstPtr),hl
        scf
        ret
        ELSE
        ld a,(ProtoBurstLeft)
        or a
        jr nz,.have
        ld hl,ProtoBurstBuf
        call ZiFi_ReadBurst
        or a
        ret z
        ld (ProtoBurstLeft),a
        ld hl,ProtoBurstBuf
        ld (ProtoBurstPtr),hl
.have:
        ld hl,(ProtoBurstPtr)
        ld a,(hl)
        inc hl
        ld (ProtoBurstPtr),hl
        ld hl,ProtoBurstLeft
        dec (hl)
        scf
        ret
        ENDIF

; Учесть байт A в накопленной контрольной сумме приёма.
Proto_AddCsum:
        ld hl,ProtoCsumTmp
        xor (hl)
        ld (hl),a
        ret

; Сбросить состояние приёмника.
Proto_ResetRx:
        xor a
        ld (ProtoState),a
        ld hl,0
        ld (ProtoRxCount),hl
        ret

; --- ожидание конкретного ответа --------------------------------------------

; Ждать пакет с командой A не дольше DE попыток опроса.
; Доклад об ошибке (#EE) сохраняется в ProtoErr и не считается ответом.
; Выход: CF=0 — пакет получен, CF=1 — тайм-аут.
Proto_WaitCmd:
        ld (ProtoWant),a
.loop:
        push de
        call Proto_Poll
        pop de
        jr nc,.idle
        ld hl,ProtoWant
        cp (hl)
        jr z,.got
        cp RESP_ERROR
        jr nz,.idle
        ld a,1
        ld (ProtoErr),a
        call Proto_SaveErr              ; сохранить текст: 0x83 затрёт ProtoBuf
.idle:
        dec de
        ld a,d
        or e
        jr nz,.loop
        scf
        ret
.got:
        or a
        ret

; Скопировать payload доклада об ошибке в отдельный буфер с нулём на конце.
Proto_SaveErr:
        ld hl,ProtoBuf
        ld de,ProtoErrText
        ld bc,(ProtoRxLen)
        ; Длину обязательно ограничиваем: ESP вправе прислать доклад длиной до
        ; PROTO_MAX, а буфер здесь 64 байта. Без ограничения хвост затирал
        ; ProtoBurstBuf и ProtoBuf, то есть приёмные буферы самого протокола —
        ; обмен разрушался прямо во время разбора пакета.
        ld a,b
        or a
        jr nz,.clamp
        ld a,c
        cp PROTO_ERR_TEXT-1
        jr c,.check
.clamp:
        ld bc,PROTO_ERR_TEXT-1
.check:
        ld a,b
        or c
        jr z,.term
.copy:
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,.copy
.term:
        xor a
        ld (de),a
        ret

; Запросить статус HTTP-прокси.
; Возвращает: CF=0 если ответ получен (A=статус: 0-выкл, 1-вкл, 2-недоступен;
; DE=указатель на строку адреса ProtoBuf+1 с нулём); CF=1 при тайм-ауте.
Net_ProxyStatus:
        ld hl,0
        ld bc,0
        ld a,CMD_NET_PROXY_STATUS
        call Proto_Send
        ld de,10000                     ; 10 000 итераций опроса (достаточно для ответа ESP)
        ld a,RESP_NET_PROXY_STATUS
        call Proto_WaitCmd
        ret c
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        scf
        ret z
        ld bc,ProtoBuf
        add hl,bc
        ld (hl),0                       ; гарантируем завершающий ноль
        ld a,(ProtoBuf)                 ; статус (0, 1, 2)
        ld de,ProtoBuf+1                ; адрес "ip:port"
        or a
        ret

; --- данные -----------------------------------------------------------------

ProtoCmd:       db 0
ProtoPtr:       dw 0
ProtoLen:       dw 0
ProtoCsum:      db 0
        IFDEF ZIFI_SPI_DMA
ProtoTail:      dw 0                    ; вторая часть кадра (Proto_Send2)
ProtoTailLen:   dw 0
ProtoHold:      db 0                    ; <>0 — сумму кадра не досылать
ProtoDmaUsed:   db 0                    ; данные кадра переносил DMA: перед разбором — SpiLink_Sweep
; С какой длины кусок приёма переносит DMA: его настройка и чтения для кэша в
; конце кадра стоят около 3700 тактов.
PROTO_DMA_MIN   equ 64
        ENDIF

ProtoState:     db 0                    ; 0..5 — состояние приёмника
ProtoRxCmd:     db 0
ProtoRxLen:     dw 0
ProtoRxPtr:     dw 0
ProtoRxCount:   dw 0
ProtoCsumTmp:   db 0
ProtoWant:      db 0
ProtoErr:       db 0                    ; 1 — ESP присылал доклад об ошибке
ProtoBurstPtr:  dw ProtoBurstBuf
        IFDEF ZIFI_SPI
ProtoBurstLeft: dw 0                    ; у SPI порция — до 512 байтов
        ELSE
ProtoBurstLeft: db 0
        ENDIF
ProtoErrText:   ds PROTO_ERR_TEXT       ; сохранённый текст последнего доклада
; Одна порция приёма. У UART ZiFi это безопасная INIR-порция (#BF байтов), у
; SPI платы VDAC4 — 54 байта одного обмена и контрольная сумма следом
; (ZIFI_BURST_SIZE в zifi_spi.asm).
        IFDEF ZIFI_SPI
        IFDEF ZIFI_SPI_DMA
        ALIGN 2                         ; DMA TS-Config адресует память словами
        ENDIF
ProtoBurstBuf:  ds ZIFI_BURST_SIZE
        ELSE
ProtoBurstBuf:  ds #BF
        ENDIF
; Приёмник хранит не больше PROTO_MAX байтов. Ещё один байт зарезервирован под
; завершающий ноль: текстовый пакет длиной ровно 1024 байта тогда заканчивается
; внутри буфера, а не портит первую команду расположенного следом VFS-кода.
; С ключом ZIFI_SPI_DMA адрес чётный (порция выше — 512 байтов с чётного): в
; буфер кадра переносит DMA.
ProtoBuf:       ds PROTO_MAX+1          ; payload и безопасный текстовый ноль
