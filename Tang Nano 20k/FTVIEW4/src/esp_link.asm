; Обмен с ESP платы VDAC4 для банка ядра видео FTVIEW4 (страница 11, core.c). Протокол и
; канал — proto.asm и zifi_spi.asm сетевых плагинов VDAC4 (Network/shared/z80) без
; изменений: кадры [#5A][CMD][LEN LE16][данные][XOR] через общий буфер SPI ведомого ESP
; (порты #57/#77, как у SD-карты; без DMA). Код — sjasmplus; build.py собирает его с адреса
; LINK_ORG (-DLINK_ORG=..., не дальше LINK_LIMIT) и кладёт в страницу банка поверх хвоста
; кода C (ESP_LINK в core.c).
;
; Вход из C — переходы в начале блока, параметры — в ячейках за ними (EspArgs, раскладку
; повторяет core.c, сверяет build.py по символам); итог — A: 0 удачно, 1 нет.
;   +0  Link_Init — найти ESP и начать сеанс;
;   +3  Link_Call — послать кадр EspCmd/EspData/EspLen; EspRepeat > 0 — ждать ответ EspResp
;       не дольше EspRepeat × EspPolls опросов (ответ — в ProtoBuf, длина — ProtoRxLen);
;   +6  адрес ProtoBuf, +8 адрес ProtoRxLen, +10 — EspArgs.
; IX и IY вызывающего сохраняются (их держит SDCC).

        DEVICE NOSLOT64K
        ORG LINK_ORG

        jp Link_Init
        jp Link_Call
        dw ProtoBuf
        dw ProtoRxLen
EspArgs:
EspCmd:         db 0
EspResp:        db 0
EspData:        dw 0
EspLen:         dw 0
EspPolls:       dw 0
EspRepeat:      db 0

Link_Init:
        push ix
        push iy
        call ZiFi_Init
        pop iy
        pop ix
        ld a,0
        ret nc
        inc a
        ret

Link_Call:
        push ix
        push iy
        ld a,(EspCmd)
        ld hl,(EspData)
        ld bc,(EspLen)
        call Proto_Send
        jr c,.fail
        ld a,(EspRepeat)
        or a
        jr z,.ok
        ld b,a
.wait:
        push bc
        ld a,(EspResp)
        ld de,(EspPolls)
        call Proto_WaitCmd
        pop bc
        jr nc,.ok
        djnz .wait
.fail:
        ld a,1
        jr .out
.ok:
        xor a
.out:
        pop iy
        pop ix
        ret

        INCLUDE "proto.asm"
        INCLUDE "zifi_link.asm"
LinkEnd:
        ASSERT LinkEnd <= LINK_LIMIT, блок обмена с ESP не влез до DATA банка
        SAVEBIN "esp_link.bin", LINK_ORG, LinkEnd - LINK_ORG
