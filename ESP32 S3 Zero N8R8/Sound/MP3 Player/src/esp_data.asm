; Модули ESP: то, что доставке (esp_modules.asm, код — во второй странице
; плагина, окно #C000) нужно в странице 0. Тексты, имена и буферы читает и
; пишет WC (FENTRY, TENTRY, TXTPR), а он отображает в #C000 свои страницы; код
; страницы 0 видит только её. Здесь же входы в доставку для кода страницы 0.

ESP_CHUNK       equ 512                 ; данных в кадре MOD_PUT: сектор карты
ESP_APP_OK      equ 0
ESP_SHOW_EVERY  equ 8                   ; секторов между показами полосы

; Ответы, которых ESP заставляет ждать (Esp_WaitLong): кадров по 1/50 с.
; MOD_COMMIT — запись модуля в LittleFS: стирание секторов flash, секунды на
; сотню КБ (на плате OPL.ELF записался, а ответа 12000 опросов MP3_WAIT плагин
; не дождался); APP_START — смена приложения и загрузка его ELF из кэша.
ESP_STORE_FRAMES equ 60*50
ESP_START_FRAMES equ 15*50
; MOD_QUERY: места под банк MIDI или ПЗУ OPL4 нет — ESP (0.1.35) освобождает его
; сама: перезапуск приложения, ресурсы прочь, ожидание временных буферов — до 3 с.
; С флешкой в разъёме USB модуля (прошивка 0.1.43) ресурс ESP до ответа грузит с
; неё: банк в 6 МБ — секунды, на медленной флешке — больше десятка.
ESP_QUERY_FRAMES equ 60*50
ESP_FRAME_POLLS equ 256                 ; опросов за кадр ожидания (быстрый ответ — без паузы)

; Почему не вышла доставка (EspWhy).
WHY_OTHER       equ 0
WHY_MISSING     equ 1                   ; нет файла в /WC/ESP
WHY_ROOM        equ 2                   ; ESP: нет места
WHY_ESC         equ 3                   ; нажат Esc

; Входы в доставку (страница 1, Page1_Call); что на входе и выходе — у самих
; процедур в esp_modules.asm.
Esp_Prepare:
        call Page1_Call
        dw P1.Esp_Prepare
Esp_LoadRom:
        call Page1_Call
        dw P1.Esp_LoadRom
Esp_Restart:
        call Page1_Call
        dw P1.Esp_Restart
Track_Count:
        call Page1_Call
        dw P1.Track_Count
; Входы в окно плагина и экран Tang (screen.asm).
Ui_Probe:
        call Page1_Call
        dw P1.Ui_Probe
Ui_Screen:
        call Page1_Call
        dw P1.Ui_Screen
Ui_Track:
        call Page1_Call
        dw P1.Ui_Track
Ui_Show:
        call Page1_Call
        dw P1.Ui_Show

; Полоса загрузки — снова у файла плеера (Mod_Play).
Esp_LoadDone:
        xor a
        ld (UiLoading),a
        ld hl,Mp3Sent
        ld (UiLoadSent),hl
        ld hl,Mp3Total
        ld (UiLoadTotal),hl
        ret

EspAppRunning:  db 0                    ; приложение PLAYER.ELF запущено
EspNeedsApp:    db 0                    ; файлу нужно приложение (AAC, Ogg): без него — ошибка
EspReopened:    db 0                    ; свой файл найден заново (перемотка, обложка)
EspFlags:       db 0                    ; флаги MOD_QUERY: 1 — ресурс
EspReleased:    db 0                    ; Esp_Resource освобождал ресурсы ESP
EspWhy:         db 0                    ; WHY_*: почему не вышла доставка
EspWaitCode:    db 0                    ; Esp_WaitLong: код ответа
EspWaitLeft:    dw 0                    ; Esp_WaitLong: сколько кадров ещё ждать
EspName:        dw 0
EspStateText:   ds 22+13                ; «Sending to the ESP: » и имя
EspErrorText:   ds 32+13                ; текст ошибки и имя
EspSize:        ds 4
EspOffset:      ds 4
EspLeft:        dw 0
EspChunk:       dw 0
EspEntry:       ds 32
EspFentry:      db #00
                ds 13
EspQuery:       ds 9+13
EspDirWc:       db #10,"wc",0
EspDirEsp:      db #10,"esp",0
EspPlayer:      db "PLAYER.ELF",0
EspMp3:         db "MP3.ELF",0
                ds 3                    ; имена копируются по 13 байтов
EspAac:         db "AAC.ELF",0
                ds 5
EspFlac:        db "FLAC.ELF",0
                ds 4
EspOgg:         db "OGG.ELF",0
                ds 5
EspTracker:     db "TRACKER.ELF",0
                ds 1
EspMidi:        db "MIDI.ELF",0
                ds 4
EspBank:        db "MIDI.SF2",0
                ds 4
EspOpl:         db "OPL.ELF",0
                ds 5
EspTsfm:        db "TSFM.ELF",0
EspSaa:         db "SAA.ELF",0
                ds 5
EspRom:         db "YRW801.ROM",0
                ds 2
EspKit:         ds 13                   ; набор волн MWM: «ИМЯ.MWK»
