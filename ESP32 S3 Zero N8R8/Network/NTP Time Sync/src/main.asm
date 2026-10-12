; Синхронизация часов ZX Evolution по NTP через ZiFi.
;
; Плагин Wild Commander типа 1: запускается из меню и сразу отдаёт управление.
; Точное время получает ESP32-S3 — модуль сам ходит к серверам NTP и учитывает
; часовой пояс из zifi.ini, поэтому Z80 остаётся спросить строку, сравнить с
; часами Mr.Gluk и при расхождении их поправить.
;
; ВЫПОЛНЕНИЯ ПРИ СТАРТЕ WC ТИП 1 НЕ ДАЁТ. Это выяснено по загрузчику WC
; (WCVW.ASM, метка ALPL): при разборе секции [PLUGINS] в C кладётся 1, и строка
; CP C: RET Z намеренно отсекает именно тип 1 — единственный, который доходит
; до выполнения через CALL Z,RAJT. Плагины из wc.ini только регистрируются.
; Автозапуск требует отдельной секции [AUTOEXEC] и второго прохода уже ПОСЛЕ
; инициализации WC: во время разбора ini выполнять чужой код нельзя, он уводит
; текущий поток и каталог, по которым WC сразу настраивает панели.
;
; Правило поведения: при ЛЮБОЙ неудаче — нет ESP, нет сети, не пришёл ответ,
; ответ негодный — плагин молча выходит. Ему нечего показывать пользователю и
; незачем задерживать работу, если синхронизировать нечего. Если ESP на связи,
; а время по NTP получить не вышло, часы ESP перед выходом выставляются по
; часам Evo (SYS_CLOCK): по ним прошивка ставит время файлов на флешке.

        DEVICE ZXSPECTRUM128
        INCLUDE "wc_api.inc"

startCode:
        ORG #0000
        INCLUDE "wc_header.inc"

        ALIGN 512                         ; код начинается после сектора заголовка
        DISP #8000
mainStart:

; Число попыток и пределы ожидания. После SYS_RESET ESP перезагружается и
; поднимает канал SPI: попыток PING столько же, сколько у FTP и SMB после их сброса.
NTP_PING_TRIES  equ 30
NTP_WAIT        equ 12000                 ; обычный ответ: ACK, READY
NTP_LONG_WAIT   equ 12000                 ; подключение к сети и запрос NTP

Start:
        call ZiFi_Init                    ; CF=1 — модуля нет
        ret c

        ; Плагин выполняется при каждом запуске WC, а он тождественен сбросу Evo.
        ; RESET ZX саму ESP не трогает, поэтому её перезапускаем здесь, как FTP и
        ; SMB при своём запуске: прошивка начинает с чистого состояния, даже если
        ; прошлый сеанс оставил её занятой. Ответ не проверяем: ESP может быть уже
        ; в сбросе — готовность устанавливает цикл PING ниже.
        ld a,CMD_SYS_RESET
        call Proto_SendEmpty

        ; Готовность прошивки: PING до первого READY.
        ld b,NTP_PING_TRIES
.ping:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        ld a,RESP_READY
        ld de,NTP_WAIT
        call Proto_WaitCmd
        pop bc
        jr nc,.ready
        djnz .ping
        ret

.ready:
        ; Wi-Fi поднимает ESP: отдаём zifi.ini целиком, разбор в native firmware.
        ; Если модуль уже в сети, команда просто подтверждает адрес. Поясом из
        ; time: ESP пользуется и без сети — для часов по RTC ниже.
        call Config_Load                  ; CF=1 — файл не найден
        jr c,ClockFromRtc
        ld a,CMD_WIFI_INI
        ld hl,IniBuffer
        ld bc,(IniLength)
        call Proto_Send
        ld a,RESP_ACK
        ld de,NTP_WAIT
        call Proto_WaitCmd
        jr c,ClockFromRtc
        ld a,RESP_WIFI_INI
        ld de,NTP_LONG_WAIT
        call Proto_WaitCmd
        jr c,ClockFromRtc
        ld a,(ProtoBuf)
        or a
        jr z,ClockFromRtc                 ; сеть не поднялась

        ; Запрос точного времени. При удаче ESP по тому же ответу ставит и свои часы.
        ld a,CMD_NET_NTP
        call Proto_SendEmpty
        ld a,RESP_ACK
        ld de,NTP_WAIT
        call Proto_WaitCmd
        jr c,ClockFromRtc
        ld a,RESP_NET_NTP
        ld de,NTP_LONG_WAIT
        call Proto_WaitCmd
        jr c,ClockFromRtc

        call Ntp_Parse                    ; CF=1 — ответ негодный
        jr c,ClockFromRtc
        call Rtc_Read
        call Ntp_NeedUpdate               ; Z=1 — расхождение меньше минуты
        jr z,Finish
        call Rtc_Write
        jr Finish

; NTP не ответил: часы ESP — по часам Evo. Ответ ESP ничего не меняет (старая
; прошивка команду не знает), поэтому только дожидаемся его.
ClockFromRtc:
        call Rtc_Read
        ld a,(RtcTime)                    ; год двумя цифрами — XXI век
        call Bcd_ToBin
        ld l,a
        ld h,0
        ld bc,2000
        add hl,bc
        ld (ClockMsg),hl
        ld hl,RtcTime+1                   ; месяц, число, часы, минуты, секунды
        ld de,ClockMsg+2
        ld b,RTC_FIELDS-1
.field:
        ld a,(hl)
        push bc
        call Bcd_ToBin
        pop bc
        ld (de),a
        inc hl
        inc de
        djnz .field
        ld a,CMD_SYS_CLOCK
        ld hl,ClockMsg
        ld bc,7
        call Proto_Send
        jr c,Finish
        ld a,RESP_SYS_CLOCK
        ld de,NTP_WAIT
        call Proto_WaitCmd
        ; Дальше идёт общий выход: файловое состояние надо вернуть в любом
        ; случае, независимо от того, потребовалась правка часов или нет.

; Единственная точка выхода. Config_Load ищет zifi.ini по томам и оставляет
; потоки там, где нашёл; вернуть их обязан тот, кто сдвинул. При запуске из
; меню это незаметно — WC после возврата плагина сам перечитывает панели, — но
; для автозапуска состояние достанется продолжающейся инициализации WC.
Finish:
        call Config_RestoreRoot
        ret

; Байт BCD в A — в двоичное число в A. Портит B, C.
Bcd_ToBin:
        ld c,a
        and #F0
        rrca                              ; десятки × 8
        ld b,a
        rrca
        rrca                              ; десятки × 2
        add a,b                           ; десятки × 10
        ld b,a
        ld a,c
        and #0F
        add a,b
        ret

; SYS_CLOCK: [год LE16][месяц][число][часы][минуты][секунды] — поля после года
; в порядке RtcTime.
ClockMsg:
        ds 2+RTC_FIELDS-1

        INCLUDE "ntp.asm"
        INCLUDE "rtc.asm"
        INCLUDE "config.asm"
        INCLUDE "proto.asm"
        INCLUDE "zifi_link.asm"

; Переходники API Wild Commander. Номер функции передаётся в A и выполняется
; общим входом WC_API. Для функций, где вызывающий код уже занимает A под
; параметр, значение переносится в альтернативный AF через EX AF,AF'.
WC_STREAM:
        ld a,FN_STREAM
        jp WC_API
WC_FENTRY:
        ld a,FN_FENTRY
        jp WC_API
WC_GFILE:
        ld a,FN_GFILE
        jp WC_API
WC_GDIR:
        ld a,FN_GDIR
        jp WC_API
WC_LOAD512:
        ld a,FN_LOAD512
        jp WC_API
WC_ADIR:
        ex af,af'
        ld a,FN_ADIR
        jp WC_API
WC_FINDNEXT:
        ex af,af'
        ld a,FN_FINDNEXT
        jp WC_API

mainEnd:
        ; Код и данные обязаны поместиться в окно #8000..#BFFF.
        ASSERT mainEnd <= #C000, "plugin code exceeds the #8000 page"
        ENT
endCode:
        IFDEF ZIFI_SPI
        SAVEBIN "../build/vdac4/VDAC4NTP.WMF",startCode,endCode-startCode
        ELSE
        SAVEBIN "../build/NTPTIME.WMF",startCode,endCode-startCode
        ENDIF
