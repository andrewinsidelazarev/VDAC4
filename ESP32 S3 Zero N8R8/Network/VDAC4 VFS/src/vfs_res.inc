; VDAC4 VFS: раскладка страницы резидента VFSRES.WMF и обмен с ним.
;
; Резидент (тип #06) получает одну 16-КиБ страницу на весь сеанс WC; WC её не двигает и не
; вытесняет. В ней — образ драйвера устройства 6, код обмена с ESP и подпись, по которой
; переключатель VDAC4VFS.WMF находит страницу.
;
; Смещения внутри страницы:
;   +#0000 — установка (исполняется в окне #8000 при старте WC, A=#06);
;   +#0100 — подпись и состояние (RES_SIG…);
;   +#0200 — код, который исполняется в окне #C000: RES_SWAP (из переходника DOS_SWP) и RES_IO
;            (из драйвера), протокол канала ESP;
;   +RES_IMG — образ драйвера (логический адрес #3800).
;
; Переходник в CORE32 (WC_TRAMP, 16 байтов перед DOS_SWP) ставит переключатель:
;   LD A,I / DI / PUSH AF / LD BC,#13AF / IN E,(C) / LD A,страница / OUT (C),A / JP RES_SWAP
; а «LD HL,DR2» ветки A=6 (WC_SD2_LDHL) меняет на «JP WC_TRAMP». RES_SWAP кладёт образ драйвера в
; #3800 окна #0000 (WildDOS панели) и уходит в его хвост D_TAIL: прежняя страница #C000, прежние
; прерывания и штатный конец DOS_SWP (WC_SD2_TAIL: SELDEV_INVALIDATE, POP IX, RET).

RES_SIG         equ #0100               ; смещение подписи в странице
RES_CODE        equ #0200               ; начало кода окна #C000
RES_IMG         equ #3000               ; образ драйвера
RES_IMG_MAX     equ #0600               ; не больше: #3800..#3DFF, выше — буфер сектора
RES_WINDOW      equ #C000               ; окно, через которое исполняется резидент

; Поля подписи (смещения от RES_SIG)
SIG_TEXT        equ 0                   ; "VDAC4VFS"
SIG_VERSION     equ 8                   ; версия раскладки
SIG_PAGE        equ 9                   ; физическая страница резидента (ставит установка)
SIG_STATE       equ 10                  ; SD2: STATE_* (ставит переключатель)
SIG_SWAP        equ 11                  ; LE16: адрес RES_SWAP в окне #C000
SIG_IO          equ 13                  ; LE16: адрес RES_IO в окне #C000 (самопроверка переключателя)
; Диагностика (раскладка 2): что было с последней операцией RES_IO — её показывает переключатель
SIG_SWAPS       equ 15                  ; LE16: сколько раз RES_SWAP ставил образ драйвера
SIG_CALLS       equ 17                  ; LE16: вызовов RES_IO
SIG_OP          equ 19                  ; последняя операция: RES_BEGIN / RES_INFO / RES_READ
SIG_WHY         equ 20                  ; её итог: DIAG_*
SIG_RES         equ 21                  ; байт к итогу: итог ESP, флаги INFO или длина данных
SIG_WAITS       equ 22                  ; LE16: вызовов RES_WAIT после неё
SIG_ERR         equ 24                  ; доклад ESP об ошибке (DIAG_ESPERR): текст с нулём
SIG_ERR_LEN     equ 32
SIG_SIZE        equ SIG_ERR+SIG_ERR_LEN
RES_LAYOUT      equ 3                   ; 3: запись сектора (RES_WRITE) и RES_RELEASE

STATE_SD        equ 0                   ; SD2 — штатная вторая карта ZC
STATE_VFS       equ 1                   ; SD2 = том VDAC4 VFS (папки источников, только чтение)
STATE_TANG      equ 2                   ; SD2 = карта Tang целиком, с записью (ESP её не трогает)

DIAG_OK         equ 0
DIAG_LINK       equ 1                   ; ZiFi_Init: ESP не ответила на начало сеанса
DIAG_SEND       equ 2                   ; запрос не ушёл
DIAG_WAIT       equ 3                   ; ответа ещё нет (драйвер ждёт порциями RES_WAIT)
DIAG_ESPERR     equ 4                   ; ESP прислала доклад об ошибке (SIG_ERR)
DIAG_RESULT     equ 5                   ; итог ответа не 0 (SIG_RES)
DIAG_NOTREADY   equ 6                   ; INFO: том не собран (SIG_RES — флаги)
DIAG_LENGTH     equ 7                   ; READ: данных не столько, сколько просили (SIG_RES — длина)

; Операции RES_IO (A на входе). Выход: CF=0 — да; CF=1 — A = RES_PENDING (ответа ещё нет: драйвер
; открывает прерывания и зовёт RES_WAIT) или RES_FAIL.
RES_BEGIN       equ 0                   ; начать сеанс канала (ZiFi_Init)
RES_INFO        equ 1                   ; INFO: том готов?
RES_READ        equ 2                   ; часть сектора D_LBA номер D_PART -> D_BOUNCE
RES_WAIT        equ 3                   ; ждать ответ на прежний запрос дальше
RES_WRITE       equ 4                   ; сектор D_BOUNCE -> D_LBA (только SD2 = карта Tang)
RES_RELEASE     equ 5                   ; MODE 0: ESP снова владеет картой Tang (старт WC)
RES_PENDING     equ 1
RES_FAIL        equ 2
RES_POLLS       equ 2000                ; опросов за один вызов (прерывания закрыты)
RES_TRIES       equ 250                 ; вызовов RES_WAIT на один запрос

; Переменные драйвера в окне #0000 (#3800…), общие с резидентом
D_LBA           equ #38D0               ; BLKNUM: позиция чтения (XPOZI/PROZ)
D_BOUNCE        equ #3E00               ; сектор: чтение — резидент кладёт сюда, драйвер — в HL; запись — наоборот

; Команды прошивки ESP 0.1.97+ (zifi/vdisk_service.hpp); WRITE и MODE — с 0.1.100
CMD_VDISK_INFO  equ #15
CMD_VDISK_READ  equ #16                 ; [lba LE32][смещение LE16][длина <= 248]
CMD_VDISK_RESET equ #17
CMD_VDISK_WRITE equ #18                 ; [lba LE32][данные 512]: кадр Z80 -> ESP до 1024 байт
CMD_VDISK_MODE  equ #19                 ; [0 — том VFS, 1 — карта Tang посекторно]
RESP_VDISK_INFO equ #95                 ; [итог][флаги][поколение LE32][секторов LE32][источников]
RESP_VDISK_READ equ #96                 ; [итог][данные]
RESP_VDISK_RESET equ #97                ; [итог][поколение LE32]
RESP_VDISK_WRITE equ #98                ; [итог]
RESP_VDISK_MODE equ #99                 ; [итог][флаги][секторов LE32]
VDISK_READY     equ #01                 ; флаг INFO: том собран (режим 1 — карта у WC)
VDISK_RAW       equ #40                 ; флаг INFO: режим 1, SD2 = карта Tang
VDISK_CHUNK     equ 248

DEV_SD2         equ 6                   ; DOS_SWP: второй SD ZC — его место и занимает VFS
