; Заголовок .WMF для Wild Commander, формат #11 (WC Improved v1.11i от
; 11 октября 2026 и новее: boot.$C и WC/PLM.WMF этого выпуска; старое ядро
; плагин с неизвестной версией формата отвергает).
; Плагину нужны две страницы: 0 — основной код и буферы (запускается в #8000),
; 1 — код, который работает в окне #C000 (main.asm, Page1_Call).
; Заголовок физически занимает 512 байтов. С отступа 512 — список расширений
; (WC_EXTENSION_LIST, дополнен нулями до границы сектора), за ним с границы
; сектора — код. Секторы списка не входят ни в число страниц, ни в пары блоков.

        ds 16
        db "WildCommanderMDL"
        db #11                        ; формат: список расширений переменной длины
        db 0
        db 2                          ; всего страниц
        db 0                          ; страница 0 подключается по адресу #8000

        ; Пары [страница, число 512-байтных секторов с округлением вверх]: блоки
        ; кода по порядку, каждый — с начала своей страницы. Пара с нулём
        ; секторов кончает список.
        db 0,(mainEnd-mainStart+511)/512
        db 1,(page1End-page1Start+511)/512
        db 0,0
        db 0,0
        db 0,0
        db 0,0

        ; +48: описание списка расширений формата #11.
        dw WcExtensionsEnd-WcExtensions ; значащие байты, без добивки
        dw WC_EXTENSION_COUNT         ; число записей
        dw 0                          ; в файле ноль: WC ставит сюда адрес копии
        db 1                          ; схема записи: [длина][код][имя]
        ds 8
        db 0                          ; +63: расширения срабатывают и по Enter, и по F3
        ds 96                         ; +64: у формата #11 не используется
        db 0
        dd #FFFFFFFF                  ; файл любого размера: он не грузится в память

.plugin_name
        db "VDAC4 Media Player v1.18"
        ds 32-($-.plugin_name),' '
        ; Тип #04 — меню выбора просмотрщика (Shift+F3). В меню плагинов F10
        ; проигрывателя нет: без файла ему играть нечего. По расширениям плагин
        ; вызывается при любом типе.
        db #04
        ds 6
.viewer_name
        db "Play on VDAC4"
        ds 24-($-.viewer_name),' '
        ds 32                         ; резерв заголовка

; Список расширений: запись [длина имени][код обработчика][имя без точки].
; WC сравнивает без учёта регистра часть полного имени после последней точки,
; поэтому годятся и имена длиннее трёх букв (FLAC, MIDI). Код обработчика WC
; кладёт в A' — здесь это вид файла (KIND_*), но плеер всё равно узнаёт вид по
; имени (Mp3_PickKind): так же он ведёт себя при переходе к соседнему файлу.
; Файл без точки в имени формат #11 плагину не отдаёт.
WC_EXTENSION_COUNT_RUN = 0
        MACRO WC_EXTENSION name, handler
        db .end-.start, handler
.start:
        db name
.end:
WC_EXTENSION_COUNT_RUN = WC_EXTENSION_COUNT_RUN + 1
        ENDM

        MACRO WC_EXTENSION_LIST
        ALIGN 512
WcExtensions:
        ; Поток: MP3.ELF, AAC.ELF, OGG.ELF, FLAC.ELF и WAV в прошивке. FLA —
        ; короткое имя FAT 8.3 файла .flac.
        WC_EXTENSION "MP3", KIND_MP3
        WC_EXTENSION "WAV", KIND_MP3
        WC_EXTENSION "AAC", KIND_MP3
        WC_EXTENSION "OGG", KIND_MP3
        WC_EXTENSION "OGA", KIND_MP3
        WC_EXTENSION "FLAC", KIND_MP3
        WC_EXTENSION "FLA", KIND_MP3
        ; Трекерные модули (TRACKER.ELF). MO — MOD с обрезанным расширением
        ; (бывает в сборниках): что внутри, ESP узнаёт по содержимому.
        WC_EXTENSION "MOD", KIND_MODULE
        WC_EXTENSION "XM", KIND_MODULE
        WC_EXTENSION "S3M", KIND_MODULE
        WC_EXTENSION "IT", KIND_MODULE
        WC_EXTENSION "MO", KIND_MODULE
        ; MIDI (MIDI.ELF и банк MIDI.SF2).
        WC_EXTENSION "MID", KIND_MIDI
        WC_EXTENSION "MIDI", KIND_MIDI
        WC_EXTENSION "RMI", KIND_MIDI
        WC_EXTENSION "KAR", KIND_MIDI
        ; Музыка OPL2/OPL3/OPL4 (OPL.ELF).
        WC_EXTENSION "VGM", KIND_OPL
        WC_EXTENSION "VGZ", KIND_OPL
        WC_EXTENSION "DRO", KIND_OPL
        WC_EXTENSION "IMF", KIND_OPL
        WC_EXTENSION "WLF", KIND_OPL
        WC_EXTENSION "MWM", KIND_OPL
        WC_EXTENSION "MFM", KIND_OPL
        ; MWK — набор волн песен MWM: не музыка; плагин его пропускает (дальше в
        ; ту же сторону), а без записи WC при переходе на набор открывал бы меню
        ; просмотрщиков.
        WC_EXTENSION "MWK", KIND_KIT
        ; Плейлист интернет-радио: станции качает и играет ESP.
        WC_EXTENSION "M3U", KIND_RADIO
        ; E-Tracker для SAA1099 (SAA.ELF).
        WC_EXTENSION "SAA", KIND_SAA
        WC_EXTENSION "COP", KIND_SAA
        ; Компилированная музыка AY и TurboSound, TFC на двух YM2203 (TSFM.ELF).
        ; Прежде — отдельный плагин VDAC4TSF.WMF: в старом заголовке было лишь
        ; 32 места под расширения.
        WC_EXTENSION "AS0", KIND_TSFM
        WC_EXTENSION "ASC", KIND_TSFM
        WC_EXTENSION "FTC", KIND_TSFM
        WC_EXTENSION "GTR", KIND_TSFM
        WC_EXTENSION "PSC", KIND_TSFM
        WC_EXTENSION "PSM", KIND_TSFM
        WC_EXTENSION "PT1", KIND_TSFM
        WC_EXTENSION "PT2", KIND_TSFM
        WC_EXTENSION "PT3", KIND_TSFM
        WC_EXTENSION "SQT", KIND_TSFM
        WC_EXTENSION "ST3", KIND_TSFM
        WC_EXTENSION "STC", KIND_TSFM
        WC_EXTENSION "STP", KIND_TSFM
        WC_EXTENSION "TS", KIND_TSFM
        WC_EXTENSION "TFC", KIND_TSFM
        ; TurboFM Dump (дамп регистров двух YM2203) и Multitrack Container ZXTune
        ; (AY, TFC, TFD дорожками одновременно) — тоже TSFM.ELF.
        WC_EXTENSION "TFD", KIND_TSFM
        WC_EXTENSION "MTC", KIND_TSFM
WcExtensionsEnd:
WC_EXTENSION_COUNT equ WC_EXTENSION_COUNT_RUN
        ALIGN 512                     ; код — с границы сектора после списка
        ENDM
