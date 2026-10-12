; Page 1. Raw media DMA supported by SPI link version 2 (VDAC4 0.1.42).
; Request control bytes: [command][valid data length LE16][optional offset LE32].
; One DMA transfer, padded to 512 bytes; ESP discards padding. No data XOR/CRC.
Fast_Probe:
        xor a
        ld (FastMedia),a
        ld hl,MP3_BLOCK_SIZE
        ld (Mp3SendSize),hl
        ld a,MP3_STATUS_EVERY-1
        ld (Mp3StatusMask),a
        ld a,(SpiPutCan)
        or a
        ret z
        ld b,LINK_STATE_ADDR
        call SpiLink_OpenRead
        in a,(c)                        ; signature
        cp #D7
        jr nz,.old
        in a,(c)                        ; protocol version
        call SpiLink_Close
        cp 2
        ret c
        ld a,1
        ld (FastMedia),a
        ld (Mp3StatusMask),a             ; two 8 KiB blocks between status polls
        ld hl,MP3_READ_SIZE
        ld (Mp3SendSize),hl
        ret
.old:
        jp SpiLink_Close

Fast_Audio:
        ld a,CMD_AUDIO_DATA
        ld hl,(Mp3BlockPtr)
        ld bc,(Mp3BlockLen)
        jr Fast_Send

Fast_Resource:
        ld a,CMD_MOD_PUT
        ld hl,Mp3ReadBuffer
        ld bc,(EspChunk)

; A command, HL data, BC valid bytes. CF on refusal or timeout.
Fast_Send:
        ld (SpiBulkBuf),hl
        ld (SpiTxBuf),a
        ld (SpiTxBuf+1),bc
        ld hl,3
        cp CMD_MOD_PUT
        jr nz,.control
        ld hl,EspOffset
        ld de,SpiTxBuf+3
        ld bc,4
        ldir
        ld hl,7
.control:
        ld a,l
        ld (FastControlLen),a
        ld hl,(SpiTxBuf+1)
        dec hl
        ld a,h
        srl a
        inc a
        ld (FastSectors),a
        ld a,100                        ; bounded wait for ESP input-ring space
        ld (FastWaitLeft),a
.ask:
        ld a,(FastControlLen)            ; Turn consumes only the control length
        ld (SpiTxLen),a
        ld hl,SpiRsp+3
        ld (SpiDest),hl
        xor a
        ld (SpiRoom),a
        ld a,#10                        ; raw media request, not ordinary PUT
        call SpiLink_Turn
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_READY
        jr nz,.ready
        ld hl,FastWaitLeft
        dec (hl)
        jr z,.fail
        ei                              ; the initial tag probe can leave DI
        halt                            ; let ESP drain its queue, then a new TID
        jr .ask
.ready:
        ld bc,TS_DMASADDRL
        ld a,SPI_CMD_WRDMA
        call SpiLink_BulkOpen
        ld a,(FastSectors)
        dec a
        ld bc,TS_DMANUM
        out (c),a
        ld a,TS_DMA_RAM_SPI
        call SpiLink_BulkRun
        ld a,SPI_CMD_WR_DONE
        call SpiLink_BulkDone
        ld a,LINK_F_PUT_DONE
        call SpiLink_Send
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_OK
        ret nz
.fail:
        scf
        ret
