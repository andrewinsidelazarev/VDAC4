; VDAC4 Media Player — страница 1 (MODULE P1, окно #C000): перемотка потока и файл на SD2.
; С плагина 1.31 (19.11.2026) здесь: страница 0 кончилась (код упирался в окно FMADDR #B200).
; Входы — Mp3_Seek, Mp3_Sd2Probe, Mp3_Sd2Send, Mp3_EspReads в player.asm (Page1_Call).

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
        ld a,(Mp3EspFile)
        or a
        jr nz,.esp_file                  ; файл читает ESP: с нового места она читает сама
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
.esp_file:
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

;
; --- файл на SD2: читает ESP (плагин 1.31, PLAYER 1.47, 19.11.2026) --------------------------
; Через Z80 поток с SD2 идёт дважды (ESP отдаёт сектор тома плагину, плагин — тот же поток
; обратно): FLAC 24 бит 1871 кбит/с с флешки получал 181 КБ/с из ~235 и рвался. Файл на SD2 под
; VDAC4 VFS (флешка ESP, карта Tang) ESP находит сама по первому кластеру — как видео FTVIEW4.

; Mp3Sd2 = 1 и Sd2Cluster — первый кластер файла, если SD2 сейчас — VDAC4 VFS (переходник стоит:
; JP #4FE8 по #501C, LD A,I по #4FE8) и плагин вызван с панели устройства 6 (SD2). Как sd2_vfs у
; FTVIEW4: поток 0 — на начале файла, его текущий кластер ядро держит в CUHL/CUDE (#38A8,
; общий блок CORE32 в окне #0000). Звать до чтения файла.
Mp3_Sd2Probe:
        xor a
        ld (Mp3Sd2),a
        ld a,(#501C)
        cp #C3
        ret nz
        ld hl,(#501D)
        ld de,#4FE8
        or a
        sbc hl,de
        ret nz
        ld hl,(#4FE8)
        ld de,#57ED                      ; ED 57 — LD A,I
        or a
        sbc hl,de
        ret nz
        ld hl,(Mp3PanelIx)
        ld de,#1D
        add hl,de
        ld a,(hl)
        cp 6
        ret nz
        ld hl,(#38A8)
        ld (Sd2Cluster),hl
        ld hl,(#38AA)
        ld (Sd2Cluster+2),hl
        ld a,1
        ld (Mp3Sd2),a
        ret

; AUDIO_OPEN с файлом на SD2: ModOpen (9 байтов, страница 0) с битом 3 флагов и за ним
; [первый кластер][длина файла][начало звука — длина пропущенной метки][n][первые n (до 16)
; байтов звука — из первого блока, сверка у ESP]. Тело собирается здесь, в странице 1: она
; подключена, пока Proto_Send его отправляет. Выход — как у Proto_Send.
Mp3_Sd2Send:
        ld hl,ModOpen
        ld de,Sd2Body
        ld bc,9
        ldir
        ld a,(Sd2Body+5)
        or 8
        ld (Sd2Body+5),a
        ld hl,Sd2Cluster
        ld bc,4
        ldir
        ld hl,Mp3FileSize
        ld bc,4
        ldir
        ld hl,Mp3TagLen
        ld bc,4
        ldir
        ld hl,(Mp3BlockLen)
        ld a,h
        or a
        ld a,16
        jr nz,.n
        ld a,l
        cp 16
        jr c,.n
        ld a,16
.n:
        ld (de),a                        ; Sd2Body+21
        inc de
        ld c,a
        ld b,0
        push bc
        or a
        jr z,.empty
        ld hl,(Mp3BlockPtr)
        ldir
.empty:
        pop hl
        ld bc,22
        add hl,bc
        ld b,h
        ld c,l
        ld a,CMD_AUDIO_OPEN
        ld hl,Sd2Body
        jp Proto_Send

; ESP ответила флагом OPEN_REPLY_SD2: файл читает она. Плагину отдавать нечего — остаток в
; ноль; перемотка — её делом (AUDIO_FLUSH с местом, плагин файл не трогает).
Mp3_EspReads:
        ld a,1
        ld (Mp3EspFile),a
        ld (Mp3CanSeek),a
        ld hl,0
        ld (Mp3Left),hl
        ld (Mp3Left+2),hl
        ld (Mp3BlockLen),hl
        ld (Mp3ReadLeft),hl
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

Sd2Cluster:     ds 4                    ; первый кластер файла на SD2 (Mp3_Sd2Probe)
Sd2Body:        ds 9+13+16              ; тело AUDIO_OPEN с файлом на SD2 (Mp3_Sd2Send)
