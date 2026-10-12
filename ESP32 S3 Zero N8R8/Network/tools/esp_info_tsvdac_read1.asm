; Standalone READ1 experiment. Does not change the regular SNA or SPI sources.
; Run sjasmplus from this directory. The result is esp_info_tsvdac_read1.sna.
; Base: esp_info_tsvdac.sna, built 2026-10-02, SpiLink_OpenRead at #8663.
; The caller expects OpenRead to return the first payload byte in A.

        DEVICE ZXSPECTRUM48
        ORG #4000-27
        INCBIN "esp_info_tsvdac.sna"
        ASSERT $ == #10000, "Expected a 48K SNA (27-byte header + 49152 bytes)"

; Refuse to patch another layout. Expected: LD A,2 / CALL #8679 / IN A,(C).
        ASSERT {b #8663} == #3E && {b #8664} == #02
        ASSERT {b #8665} == #CD && {b #8666} == #79 && {b #8667} == #86
        ASSERT {b #8668} == #ED && {b #8669} == #78

; Exactly one discarded input, then read the first useful byte and return.
; No automatic marker search. All addresses and the original SNA header stay.
        ORG #8663
        LD A,#02
        CALL #8679
        IN A,(C)                       ; discard one byte
        IN A,(C)                       ; return the first useful byte
        RET

; Make this experimental snapshot recognizable on screen, without moving code.
        ASSERT {b #8363} == 'T' && {b #8364} == 'S'
        ORG #8363
        DB "TS-VDAC ESP32-S3 READ1 TEST",13,0
        ASSERT $ <= #8381

        SAVEBIN "esp_info_tsvdac_read1.sna",#4000-27,49179
