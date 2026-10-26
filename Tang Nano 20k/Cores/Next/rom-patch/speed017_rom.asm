; Локальный прототип для NextZXOS 2.09: частота пользователя и частота меню.
; Применяется только после проверки полного SHA-256 исходного ROM.
; Никаких обращений к SD и никаких диагностических журналов.
    DEVICE ZXSPECTRUMNEXT
    ORG $1052

read_preference:
    push de
    ld bc,$243b
    in d,(c)
    ld a,$f1
    out (c),a
    inc b
    in a,(c)
    dec b
    out (c),d
    inc b
    pop de
    ret

; Вместо LD A,(D5BB) в процедуре отображения частоты.
display_preference:
    push bc
    call read_preference
    ld b,a
    and $3c
    cp $28
    jr nz,.original
    bit 7,b
    jr z,.first
    ld a,b
    and 3
    ld ($d5bb),a
.first:
    ld a,($d5bb)
    and 3
    or $c0
    nextreg $f1,a
.original:
    pop bc
    ld a,($d5bb)
    ret

; Стрелки меняют постоянный выбор; рабочая частота меню остаётся прежней.
store_preference:
    ld ($d5bb),a
    push af
    or $c0
    nextreg $f1,a
    pop af
    ret

; Общая процедура входа/выхода меню. Штатный обмен частотами сохраняется.
swap_preference:
    call $3d96
    push af
    push bc
    call read_preference
    xor $40
    and $40
    nextreg $f1,a
    pop bc
    pop af
    ret

; Вместо HALT / LD HL,5C3B в ожидании клавиши.
; Проверка раз в кадр: F8 обновляет тот же выбор, что стрелки.
poll_preference:
    halt
    push af
    push bc
    push de
    push hl
    call read_preference
    ld b,a
    and $fc
    cp $e8
    jr nz,.done
    ld a,b
    and 3
    ld hl,$d5bb
    cp (hl)
    jr z,.done
    ld (hl),a
    push ix
    push iy
    ex af,af'
    push af
    ex af,af'
    exx
    push bc
    push de
    push hl
    exx
    call $3d52
    exx
    pop hl
    pop de
    pop bc
    exx
    ex af,af'
    pop af
    ex af,af'
    pop iy
    pop ix
.done:
    pop hl
    pop de
    pop bc
    pop af
    ld hl,$5c3b
    jp $0c93

; NMI пользуется тем же выбором. В свободном бите 6 снимка NR07
; запоминается, было ли прервано меню. На возврате это различает
; временные 28 МГц меню и пользовательскую частоту программы.
nmi_preference_enter:
    push bc
    ld d,a
    call read_preference
    ld b,a
    and $3c
    cp $28
    jr nz,.original
    ld a,b
    and $40
    or d
    ld d,a
    ld a,d
    bit 7,b
    jr z,.initialize
    ld a,b
.initialize:
    and 3
    or $c0
    nextreg $f1,a
    and 3
    pop bc
    ret
.original:
    ld a,d
    pop bc
    ret

nmi_preference_leave:
    push bc
    push de
    ld d,a
    bit 6,d
    jr nz,.menu
    and 3
    or $80
    nextreg $f1,a
    and 3
    jr .done
.menu:
    nextreg $f1,$40
    ld a,3
.done:
    pop de
    pop bc
    ret

end_helpers:
    ASSERT end_helpers <= $15fe
    SAVEBIN "speed017_helpers.bin",$1052,end_helpers-$1052

; Два входа в свободных 43 байтах enNextMf.rom.
; RST 10 — штатный межбанковый вызов ROM 0 NextZXOS.
; Выход вызывается в 0402 до DI/восстановления памяти: RST 10 включает
; прерывания, поэтому вызывать его из критической секции 04E0 нельзя.
    ORG $1fd5
nmi_enter_stub:
    push de
    ld a,($3f50)
    rst $10
    dw nmi_preference_enter
    push af
    ld a,d
    ld ($3f50),a
    pop af
    pop de
    ret
nmi_store_stub:
    push hl
    ld hl,$3f50
    bit 6,(hl)
    jr z,.store
    or $40
.store:
    rst $10
    dw nmi_preference_leave
    ld (hl),a
    pop hl
    ret
end_nmi:
    ASSERT end_nmi <= $2000
    SAVEBIN "speed017_nmi.bin",$1fd5,end_nmi-$1fd5
    LABELSLIST "speed017_helpers.labels"
