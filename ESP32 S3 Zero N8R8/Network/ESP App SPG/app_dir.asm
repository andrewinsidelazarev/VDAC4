; Папка приложения на SD-карте Evo: та, где лежат копия ИМЯ.ELF и файлы данных.
; Собирается только с файлами данных (HAS_DATA, build.bat ИМЯ.ELF … ДАННЫЕ).
;
; Происхождение. Поиск каталога по секторам (LocateApp и процедуры Scan*) и путь в ESP
; (PathCfgLoad, PathCfgSave) перенесены из DOOM VDAC4 (launcher/directory.asm,
; launcher/pathcfg.asm), а туда поиск пришёл из загрузчика R-Type VDAC2+ (rtype_loader.asm).
; Автор — Andrew Lazarev, лицензия MIT (тот же текст — ../shared/fat32/LICENSE). Изменено:
; вместо DOOM.ELF и IWAD ищутся копия ИМЯ.ELF и первый файл данных приложения; нет экрана
; загрузки и текстов ошибок; рабочая память — своя страница (SCRATCH_PAGE в окне #C000).
;
; Как находится папка — два пути:
;   1. ESP помнит папки программ на картах (path.cfg во flash ESP, команды PATH_GET и
;      PATH_SET, прошивка 0.1.83+). Ключ — имя приложения (имя ELF без «.ELF») и серийный
;      номер тома FAT32: у каждой карты своя запись. Папку, которую назвала ESP, программа
;      проверяет: в ней должны быть ИМЯ.ELF и все файлы данных.
;   2. Записи нет или папка не годится (её удалили, карта другая) — поиск по всей карте:
;      первая папка, где есть ИМЯ.ELF и первый файл данных. Найденную папку программа
;      отдаёт ESP (PATH_SET), и в следующий раз поиска нет.
; На карту ничего не пишется — ни в корень, ни куда-либо ещё.
;
; Почему поиск свой, а не через драйвер: драйвер FAT32 умеет найти файл в текущем
; каталоге (FAT32_FIND), но обходить дерево каталогов ему пришлось бы, меняя свой текущий
; каталог. Здесь сектор каталога читается целиком в буфер, и 32-байтовые записи в нём
; разбираются прямо в памяти. Возвращаться к родительскому каталогу помогает стек
; «кадров» в памяти (не стек Z80): кадр — 16 байтов с положением в родителе.
;
; Память (страница SCRATCH_PAGE в окне #C000): сектор каталога — #C000, сектор FAT — #C200,
; кадры родителей — #C400…#FFFF (960 штук: глубже 960 вложенных папок поиск не идёт).

CMD_PATH_GET    equ #2A                 ; [серийный LE32][имя, 0] -> AA [1 есть / 0 нет][кластер LE32]
RESP_PATH_GET   equ #AA
CMD_PATH_SET    equ #2B                 ; [серийный LE32][кластер LE32][имя, 0] -> AB [1 / 0]
RESP_PATH_SET   equ #AB

; Закреплённые адреса рабочей страницы драйвера (как в DOOM VDAC4; их сверяет тест по
; shared/fat32/build/fat32.sym): запись каталога, которую нашёл FAT32_FIND, первый сектор
; тома и адрес процедуры «прочитать сектор» в таблице порта.
FAT32_ENTRY     equ #3868               ; WDOS.ENTRY: 32 байта записи каталога
FAT32_ADDTOP    equ #389E               ; WDOS.ADDTOP: номер первого сектора тома (LE32)
FAT32_PORT      equ #3900               ; таблица порта SD: INIT, READ, WRITE, SYNC
FAT32_READ512   equ FAT32_PORT+2        ; READ: BC — буфер, DE:HL — номер сектора

ScanBuffer      equ #C000               ; сектор каталога (и BPB)
ScanFatBuffer   equ #C200               ; сектор FAT
ScanFrames      equ #C400               ; кадры родительских каталогов, до #FFFF

; ---------------------------------------------------------------------------
; Папка из ESP (PATH_GET). Выход: NZ — подошла: AppDir и текущий каталог драйвера — она;
; Z — записи нет, папка не годится или ESP команды не знает: искать (LocateApp).
PathCfgLoad:
        xor a
        ld (PathEsp),a
        call ScanGeometry               ; BPB тома — в ScanBuffer
        jp nz,.no
        ; серийный номер тома — BPB +67 (поле FAT32 «Volume ID»)
        ld hl,ScanBuffer+67
        ld de,PathGetMsg
        ld bc,4
        ldir
        ld hl,ScanBuffer+67
        ld de,PathSetMsg
        ld bc,4
        ldir
        ld hl,PathGetMsg
        ld bc,PathGetEnd-PathGetMsg
        ld a,CMD_PATH_GET
        call Proto_Send
        ld a,RESP_PATH_GET
        call PathWait
        jp c,.no                        ; прошивка без path.cfg
        ld a,1
        ld (PathEsp),a                  ; ESP команды знает: найденное поиском — записать
        ld a,(ProtoBuf)
        or a
        jp z,.no                        ; записи нет
        ld hl,ProtoBuf+1
        ld de,PathDir
        call ScanCopy32
        ; Кластер должен быть внутри тома (2…, сектор — до конца тома), как у каталогов
        ; поиска: запись могла остаться от другой карты с тем же серийным номером.
        ld hl,PathDir
        ld de,ScanCluster
        call ScanCopy32
        xor a
        ld (ScanSector),a
        call ScanClusterLba
        jr c,.no
        ld hl,PathDir
        call FAT32_SET_DIR_CLUSTER
        call CheckAppDir
        jr z,.no
        ld hl,PathDir
        ld de,AppDir
        call ScanCopy32
        ld a,1
        or a                            ; NZ
        ret
.no:
        xor a                           ; Z
        ret

; После поиска: папка AppDir — в ESP. Отказ записи приложению не мешает: в следующий раз
; программа просто снова поищет.
PathCfgSave:
        ld a,(PathEsp)
        or a
        ret z
        ld hl,AppDir
        ld de,PathSetMsg+4
        call ScanCopy32
        ld hl,PathSetMsg
        ld bc,PathSetEnd-PathSetMsg
        ld a,CMD_PATH_SET
        call Proto_Send
        ld a,RESP_PATH_SET
        jp PathWait

; Ждать ответ A от ESP (около 10 с). Выход: CF=0 — пришёл (тело — в ProtoBuf); CF=1 — ESP
; ответила докладом об ошибке (команды не знает) или молчала.
PathWait:
        ld (PathWant),a
        ld b,8
.outer:
        ld de,#FFFF
.loop:
        push bc
        push de
        call Proto_Poll
        pop de
        pop bc
        jr nc,.idle                     ; кадра нет
        ld hl,PathWant
        cp (hl)
        ret z                           ; тот ответ; CF=0
        cp RESP_ERROR
        scf
        ret z
.idle:
        dec de
        ld a,d
        or e
        jr nz,.loop
        djnz .outer
        scf
        ret

; Есть ли в текущем каталоге драйвера копия ИМЯ.ELF и все файлы данных.
; Выход: NZ — все есть, Z — какого-то нет.
CheckAppDir:
        ld hl,AppName
        call FindName
        ret z
        ld hl,DataNames
        ld b,DATA_COUNT
.next:
        push bc
        call FindName                   ; HL — уже за именем
        pop bc
        ret z
        djnz .next
        ret                             ; NZ остался от FindName

; Найти файл HL (ИМЯ.EXT, 0) в текущем каталоге драйвера (FAT32_FIND; драйвер заодно
; проверяет всю цепочку кластеров файла). Выход: NZ — есть, его запись — в FAT32_ENTRY;
; Z — нет или ошибка чтения. HL — за нулём имени: так удобно идти по списку имён.
FindName:
        ld de,FindSpec+1                ; FindSpec: [атрибут 0 — файл][имя, 0]
.copy:
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        or a
        jr nz,.copy
        push hl
        ld hl,FindSpec
        call FAT32_FIND                 ; CF=1 — ошибка, Z — нет, NZ — есть
        pop hl
        jr c,.none
        ret nz
.none:
        xor a
        ret

; ---------------------------------------------------------------------------
; Поиск папки по всей карте: первая, где есть копия ИМЯ.ELF и первый файл данных.
; Выход: CF=0 — нашлась: AppDir и текущий каталог драйвера — она; CF=1 — нет (или карта
; испорчена, или не читается).
LocateApp:
        xor a
        ld (ScanFatValid),a
        ld hl,ScanFrames
        ld (ScanStack),hl
        ld hl,65535                     ; не больше 65535 каталогов на карту
        ld (DirBudget),hl
        call ScanGeometry
        jp nz,ScanFail
        ld hl,ScanRoot                  ; начать с корня
        ld de,ScanChild
        call ScanCopy32
.enter:                                 ; войти в каталог ScanChild
        ld hl,(DirBudget)
        ld a,h
        or l
        jp z,ScanFail
        dec hl
        ld (DirBudget),hl
        ld hl,ScanChild
        ld de,ScanDir
        call ScanCopy32
        ld hl,ScanChild
        ld de,ScanCluster
        call ScanCopy32
        xor a
        ld (ScanSector),a
        ld (ScanEntry),a
        ld (ScanFiles),a
        ld hl,1
        ld (ScanSteps),hl
.read:                                  ; прочитать сектор каталога
        call ScanClusterLba
        jp c,ScanFail
        ld bc,ScanBuffer
        call ScanReadLba
        jp nz,ScanFail
.entry:                                 ; очередная запись сектора (16 по 32 байта)
        ld a,(ScanEntry)
        cp 16
        jp z,.next_sector
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,ScanBuffer
        add hl,de
        push hl
        pop ix                          ; IX — запись
        ld hl,ScanEntry
        inc (hl)                        ; вернуться — к следующей
        ld a,(ix+0)
        or a
        jp z,.done_dir                  ; 0 — записей дальше нет
        cp #E5
        jr z,.entry                     ; удалённая
        ld a,(ix+11)                    ; атрибуты
        bit 3,a                         ; длинное имя (#0F) и метка тома
        jr nz,.entry
        bit 4,a
        jr nz,.directory
        ld de,ScanElfName
        call ScanName
        ld a,1                          ; бит 0 — копия ИМЯ.ELF
        jr z,.file
        ld de,ScanDataName
        call ScanName
        jr nz,.entry
        ld a,2                          ; бит 1 — первый файл данных
.file:
        ld hl,ScanFiles
        or (hl)
        ld (hl),a
        cp 3
        jr nz,.entry
        ; Оба файла в этом каталоге. Остальное (все файлы данных, их цепочки) проверит
        ; FAT32_FIND вызывающего.
        ld hl,ScanDir
        ld de,AppDir
        call ScanCopy32
        ld hl,AppDir
        call FAT32_SET_DIR_CLUSTER      ; у входа 10 нет итога в A/флагах
        or a                            ; CF=0
        ret
.directory:
        ; «.» и «..»; у длинного имени с точкой впереди короткое имя без точки
        ld a,(ix+0)
        cp '.'
        jp z,.entry
        ld l,(ix+26)                    ; первый кластер: младшее слово +26, старшее +20
        ld h,(ix+27)
        ld (ScanChild),hl
        ld l,(ix+20)
        ld h,(ix+21)
        ld a,h
        and #0F
        ld h,a
        ld (ScanChild+2),hl
        call ScanCheckChild
        jp c,ScanFail                   ; ссылка на свой же каталог или предка — петля
        ld de,(ScanStack)
        ld a,d
        or e
        jp z,ScanFail                   ; кадры кончились: слишком глубоко
        ld hl,ScanState                 ; положение в этом каталоге — в кадр
        ld bc,16
        ldir
        ld (ScanStack),de
        jp .enter
.next_sector:
        xor a
        ld (ScanEntry),a
        ld a,(ScanSector)
        inc a
        ld hl,ScanSectorsPerCluster
        cp (hl)
        ld (ScanSector),a
        jr c,.advance
        call ScanNextCluster            ; кластер каталога кончился
        jp c,ScanFail
        jr z,.done_dir                  ; и сам каталог кончился
        xor a
        ld (ScanSector),a
.advance:
        ld hl,(ScanSteps)
        inc hl
        ld (ScanSteps),hl
        ld de,4097                      ; каталог FAT32 — не больше 4096 секторов
        or a
        sbc hl,de
        jp nc,ScanFail
        jp .read
.done_dir:
        ; каталог пройден — вернуться в родителя (кадр со стека в памяти)
        ld hl,(ScanStack)
        ld de,ScanFrames
        or a
        sbc hl,de
        jp z,ScanFail                   ; это был корень: папки нет
        ld hl,(ScanStack)
        ld de,16
        or a
        sbc hl,de
        ld (ScanStack),hl
        ld de,ScanState
        ld bc,16
        ldir
        jp .read                        ; заново прочитать сектор родителя

; Геометрия тома, который уже смонтировал драйвер: BPB — в ScanBuffer; начало FAT,
; начало данных, конец тома, секторов в кластере, кластер корня. Выход: Z — прочитано.
ScanGeometry:
        ld hl,(FAT32_ADDTOP)            ; первый сектор тома
        ld (ScanVolume),hl
        ld (ScanLba),hl
        ld hl,(FAT32_ADDTOP+2)
        ld (ScanVolume+2),hl
        ld (ScanLba+2),hl
        ld bc,ScanBuffer
        call ScanReadLba
        ret nz
        ld a,(ScanBuffer+13)            ; секторов в кластере
        ld (ScanSectorsPerCluster),a
        ld hl,ScanBuffer+44             ; кластер корня
        ld de,ScanRoot
        call ScanCopy32
        ld hl,ScanVolume
        ld de,ScanFatStart
        call ScanCopy32
        ld hl,(ScanBuffer+14)           ; резервных секторов
        ld (ScanTemp),hl
        ld hl,0
        ld (ScanTemp+2),hl
        ld hl,ScanFatStart
        ld de,ScanTemp
        call ScanAdd32
        ld hl,ScanFatStart
        ld de,ScanDataStart
        call ScanCopy32
        ld a,(ScanBuffer+40)            ; флаги FAT: бит 7 — зеркала выключены
        ld c,0
        bit 7,a
        jr z,.mirrored
        and 15
        ld c,a                          ; тогда читать надо ту копию FAT, что активна
.mirrored:
        ld a,(ScanBuffer+16)            ; копий FAT
        ld b,a
.fats:
        ld a,c
        or a
        jr z,.data
        push bc
        ld hl,ScanFatStart
        ld de,ScanBuffer+36             ; секторов в одной FAT
        call ScanAdd32
        pop bc
        dec c
.data:
        push bc
        ld hl,ScanDataStart
        ld de,ScanBuffer+36
        call ScanAdd32
        pop bc
        djnz .fats
        ld hl,ScanBuffer+32             ; секторов в томе
        ld de,ScanEnd
        call ScanCopy32
        ld hl,ScanEnd
        ld de,ScanVolume
        call ScanAdd32
        xor a
        ret

; Номер сектора: ScanLba = начало данных + (ScanCluster − 2) × секторов в кластере +
; ScanSector. Выход: CF=1 — кластер негодный или сектор за концом тома.
ScanClusterLba:
        ld hl,(ScanCluster+2)
        ld a,h
        cp #10
        jr nc,.bad                      ; в FAT32 у кластера 28 разрядов
        ld de,(ScanCluster)
        ld a,h
        or l
        jr nz,.valid
        ld a,d
        or a
        jr nz,.valid
        ld a,e
        cp 2
        jr c,.bad                       ; кластеры 0 и 1 — не данные
.valid:
        ex de,hl                        ; DE:HL — кластер
        ld bc,2
        or a
        sbc hl,bc
        jr nc,.no_borrow
        dec de
.no_borrow:
        ld a,(ScanSectorsPerCluster)    ; степень двойки: умножение — сдвигами
.multiply:
        srl a
        jr c,.multiplied
        add hl,hl
        rl e
        rl d
        jr c,.bad
        jr .multiply
.multiplied:
        ld a,(ScanSector)
        ld c,a
        ld b,0
        add hl,bc
        jr nc,.sector
        inc de
.sector:
        ld (ScanLba),hl
        ld (ScanLba+2),de
        ld hl,ScanLba
        ld de,ScanDataStart
        call ScanAdd32
        jr c,.bad
        ld hl,(ScanLba)
        ld de,(ScanEnd)
        or a
        sbc hl,de
        ld hl,(ScanLba+2)
        ld de,(ScanEnd+2)
        sbc hl,de
        jr nc,.bad
        or a
        ret
.bad:
        scf
        ret

; Следующий кластер цепочки ScanCluster — из активной FAT (сектор FAT кэшируется в
; ScanFatBuffer: подряд идущие кластеры читают его один раз). Выход: CF=1 — ошибка
; чтения или испорченная ссылка; Z — цепочка кончилась; NZ — ScanCluster — следующий.
ScanNextCluster:
        ld hl,(ScanCluster)
        ld de,(ScanCluster+2)
        ld a,l
        and 127
        ld (ScanFatEntry),a             ; номер ссылки в секторе FAT (по 4 байта, 128 штук)
        ld b,7
.shift:
        srl d
        rr e
        rr h
        rr l
        djnz .shift                     ; номер сектора FAT = кластер / 128
        ld (ScanLba),hl
        ld (ScanLba+2),de
        ld hl,ScanLba
        ld de,ScanFatStart
        call ScanAdd32
        ld a,(ScanFatValid)
        or a
        jr z,.read
        ld hl,(ScanLba)
        ld de,(ScanFatCached)
        or a
        sbc hl,de
        jr nz,.read
        ld hl,(ScanLba+2)
        ld de,(ScanFatCached+2)
        sbc hl,de
        jr z,.cached
.read:
        ld bc,ScanFatBuffer
        call ScanReadLba
        jr nz,.error
        ld hl,ScanLba
        ld de,ScanFatCached
        call ScanCopy32
        ld a,1
        ld (ScanFatValid),a
.cached:
        ld a,(ScanFatEntry)
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        ld de,ScanFatBuffer
        add hl,de
        ld de,ScanCluster
        call ScanCopy32
        ld a,(ScanCluster+3)
        and #0F
        ld (ScanCluster+3),a
        ld hl,(ScanCluster)             ; #0FFFFFF8 и выше — конец цепочки
        ld de,#FFF8
        or a
        sbc hl,de
        ld hl,(ScanCluster+2)
        ld de,#0FFF
        sbc hl,de
        jr nc,.end
        ld hl,(ScanCluster)             ; #0FFFFFF0…#0FFFFFF7 — испорченный кластер
        ld de,#FFF0
        or a
        sbc hl,de
        ld hl,(ScanCluster+2)
        ld de,#0FFF
        sbc hl,de
        jr nc,.bad
        ld hl,(ScanCluster+2)
        ld a,h
        or l
        jr nz,.next
        ld hl,(ScanCluster)
        ld de,2
        or a
        sbc hl,de
        jr c,.bad                       ; 0 и 1 — ссылка в никуда
.next:
        xor a
        inc a                           ; NZ, CF=0
        ret
.end:
        xor a                           ; Z, CF=0
        ret
.bad:
        ld a,#FE
.error:
        scf
        ret

; Ребёнок не может указывать на свой каталог или его предка («.» и «..» уже пропущены).
; Выход: CF=1 — указывает (на испорченной карте это была бы бесконечная петля).
ScanCheckChild:
        ld hl,ScanDir
        call .equal
        jr z,.bad
        ld hl,ScanFrames
.frame:
        ld de,(ScanStack)
        or a
        sbc hl,de
        add hl,de
        jr z,.good
        push hl
        call .equal
        pop hl
        jr z,.bad
        ld de,16
        add hl,de
        jr .frame
.equal:
        ld de,ScanChild
        ld b,4
.byte:
        ld a,(de)
        cp (hl)
        ret nz
        inc hl
        inc de
        djnz .byte
        ret
.bad:
        scf
        ret
.good:
        or a
        ret

; Имя записи IX (11 знаков «ИМЯ     EXT») против образца DE; строчные буквы записи —
; как заглавные. Выход: Z — совпало.
ScanName:
        push ix
        pop hl
        ld b,11
.char:
        ld a,(hl)
        cp 'a'
        jr c,.compare
        cp 'z'+1
        jr nc,.compare
        and #DF
.compare:
        ex de,hl
        cp (hl)
        ex de,hl
        ret nz
        inc hl
        inc de
        djnz .char
        ret

; Прочитать сектор ScanLba в буфер BC процедурой порта драйвера (как ReadLba у R-Type).
; Выход: Z — прочитан, NZ — ошибка (A — её код).
ScanReadLba:
        ld hl,(ScanLba)
        ld de,(ScanLba+2)
        push ix
        call .port
        pop ix
        or a
        ret
.port:
        push hl
        ld hl,(FAT32_READ512)
        ex (sp),hl                      ; на стеке — адрес процедуры, HL — как был
        ret                             ; «переход» туда; вернётся она к вызвавшему .port
ScanCopy32:
        ld bc,4
        ldir
        ret
; (HL) += (DE), 32 бита. Выход: CF — перенос из старшего разряда.
ScanAdd32:
        ld b,4
        or a
.byte:
        ld a,(de)
        adc a,(hl)
        ld (hl),a
        inc hl
        inc de
        djnz .byte
        ret

; Ошибка поиска — из тела LocateApp (стек Z80 там не тронут).
ScanFail:
        scf
        ret

; --- переменные поиска --------------------------------------------------------
; Положение в каталоге: эти 16 байтов целиком уходят в кадр, когда поиск спускается
; в подкаталог, и возвращаются из кадра, когда подкаталог пройден.
ScanState:
ScanDir:        dd 0                    ; первый кластер каталога
ScanCluster:    dd 0                    ; текущий кластер
ScanSector:     db 0                    ; сектор в кластере
ScanEntry:      db 0                    ; запись в секторе
ScanFiles:      db 0                    ; что нашлось в каталоге: бит 0 — ELF, бит 1 — данные
                db 0
ScanSteps:      dw 0                    ; прочитано секторов каталога
                dw 0
        ASSERT $-ScanState == 16
ScanStack:      dw ScanFrames           ; вершина стека кадров
DirBudget:      dw 0
ScanSectorsPerCluster: db 0
ScanFatValid:   db 0                    ; ScanFatBuffer хранит сектор ScanFatCached
ScanFatEntry:   db 0
ScanChild:      dd 0
ScanRoot:       dd 0
ScanVolume:     dd 0
ScanFatStart:   dd 0
ScanDataStart:  dd 0
ScanEnd:        dd 0
ScanLba:        dd 0
ScanTemp:       dd 0
ScanFatCached:  dd 0
ScanElfName:    APP_ELF83               ; «ИМЯ     ELF» — как в записи каталога
ScanDataName:   DATA_FIRST83            ; первый файл данных, так же
AppDir:         dd 0                    ; кластер папки приложения
FindSpec:       db 0                    ; для FAT32_FIND: атрибут и имя
                ds 13
PathGetMsg:     ds 4                    ; [серийный номер тома][имя приложения, 0]
                APP_STEM
PathGetEnd:
PathSetMsg:     ds 8                    ; [серийный][кластер][имя приложения, 0]
                APP_STEM
PathSetEnd:
PathDir:        dd 0
PathEsp:        db 0                    ; ESP знает PATH_GET/PATH_SET
PathWant:       db 0
