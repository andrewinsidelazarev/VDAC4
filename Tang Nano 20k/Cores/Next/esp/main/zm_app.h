/* zm_app.h — интерфейс «ядро ESP ↔ приложение» платы VDAC4.
 *
 * Прошивка (ядро) — канал с Z80, Wi-Fi, FTP/SMB, обновление, вывод звука,
 * загрузчик модулей. Приложение — файл ELF из /WC/ESP/ на карте Evo (например,
 * PLAYER.ELF): ядро держит его копию в кэше своей flash и грузит в PSRAM, когда
 * плагин WC его запускает. Активно одно приложение; следующее выгружает
 * прежнее.
 *
 * Подключение: точка входа ELF — main(argc, argv). Ядро передаёт
 *   argv[1] — (const zm_core_t*) свою таблицу функций,
 *   argv[2] — (const zm_app_t**) место для указателя на таблицу приложения;
 * main возвращает ZM_APP_ABI_MAJOR. Символы ядра по имени не ищутся: всё, что
 * приложению нужно от ядра, — в zm_core_t, поэтому приложение не зависит от
 * версии ESP-IDF, с которой собрана прошивка.
 *
 * Совместимость: несовпадение ZM_APP_ABI_MAJOR — ядро приложение не запускает.
 * ZM_APP_ABI_MINOR растёт, когда в конец таблиц добавлены поля; поле size
 * каждой таблицы — её размер у той стороны, что её заполнила: читать можно
 * только поля, которые в него помещаются.
 */
#ifndef ZM_APP_H
#define ZM_APP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZM_APP_ABI_MAJOR 1
#define ZM_APP_ABI_MINOR 9   /* 1: temperature в zm_core_t; 2: net_* (поток HTTP/HTTPS); 3: ft_* (FT812);
                                4: core_* (ядра ПЛИС из кэша BL616), link_*, sock_* (TCP/UDP);
                                5: ui_* — экран приложения оверлеем Tang поверх картинки Evo;
                                6: app_done — приложение кончило работу: выгрузить */

/* core->temperature: датчика нет (или ещё не прочитан). */
#define ZM_TEMPERATURE_UNKNOWN (-32768)

/* Итоги вызовов. */
enum {
  ZM_OK = 0,
  ZM_ERROR = -1,
  ZM_NO_MEMORY = -2,
  ZM_NOT_FOUND = -3,   /* модуля или ресурса нет в кэше: его пришлёт плагин */
  ZM_BUSY = -4,
  ZM_TIMEOUT = -5,
};

/* Где выделить память. Всё выделенное приложением и его модулями ядро помнит и
 * освобождает при выгрузке приложения. */
#define ZM_MEM_PSRAM    0x01u   /* по умолчанию: 8 МБ, медленнее */
#define ZM_MEM_INTERNAL 0x02u   /* внутренняя: быстрая, её мало */

/* Формат звука, который приложение отдаёт в вывод. Ядро само пересчитывает его
 * в 44 100 Гц (одна и та же частота — бит в бит), применяет громкость и пишет
 * в ЦАП 24 битами. */
typedef struct {
  uint32_t rate;       /* Гц, 8000..96000 */
  uint8_t channels;    /* 1 или 2 */
} zm_pcm_format_t;

/* Экран приложения оверлеем Tang (с ABI minor 5): если на LINK Tang с ядром TS-Config 1.10+,
 * интерфейс проигрывателя рисует ядро ESP и выводит поверх картинки Evo на HDMI Tang
 * (docs/a3-fpga-framebuffer.md, «Оверлеи ESP»); окно плагина на экране Evo остаётся под ним
 * как было. Данные — снимок zm_ui_model_t: ядро берёт его у приложения (zm_app_t::ui_model)
 * 25–50 раз в секунду. Тексты — CP866 (кодировка WC), с нулём; пустая строка — нет. */
#define ZM_UI_TEXT 64
#define ZM_UI_LEVELS 64
#define ZM_UI_STATIONS 64
#define ZM_UI_STATION_TEXT 48
#define ZM_UI_COVER_MAX 256          /* обложка: квадрат до стольких точек */
#define ZM_UI_TEMPERATURE_UNKNOWN (-128)

enum { ZM_UI_PAGE_MAIN = 0, ZM_UI_PAGE_HELP = 1, ZM_UI_PAGE_INFO = 2 };
enum { ZM_UI_IDLE = 0, ZM_UI_LOADING = 1, ZM_UI_BUFFERING = 2, ZM_UI_PLAYING = 3,
       ZM_UI_PAUSED = 4, ZM_UI_FINISHING = 5, ZM_UI_ERROR = 6 };
enum { ZM_UI_VIEW_NONE = 0, ZM_UI_VIEW_SPECTRUM = 1, ZM_UI_VIEW_CHANNELS = 2,
       ZM_UI_VIEW_STATIONS = 3 };

typedef struct {
  uint16_t size;                  /* sizeof у ядра: приложение пишет только поля, что помещаются */
  uint8_t visible;                /* 1 — показать экран: окно плагина открыто и плагин жив */
  uint8_t page;                   /* ZM_UI_PAGE_*: окно поверх у плагина (H — справка, I — подробности) */
  uint8_t state;                  /* ZM_UI_* */
  uint8_t radio;                  /* 1 — интернет-радио: track/tracks — станция, длительности нет */
  uint8_t view;                   /* ZM_UI_VIEW_* */
  uint8_t count;                  /* полос спектра, каналов или станций в stations */
  char title[ZM_UI_TEXT];         /* радио: песня */
  char artist[ZM_UI_TEXT];
  char album[ZM_UI_TEXT];
  char year[8];
  char file[ZM_UI_TEXT];          /* имя файла (от плагина): нет меток — вместо названия */
  char station[ZM_UI_TEXT];       /* радио: станция */
  char format[ZM_UI_TEXT];        /* «MP3  128 kbps  44.1 kHz  stereo», «XM  8 channels  40 positions» */
  char detail[ZM_UI_TEXT];        /* «Samples: 31  Effects: 12», «Kit POWERKIT.MWK», «YMF262 (OPL3)» */
  char status[ZM_UI_TEXT];        /* строка состояния плагина или ESP: «Buffering», «ERROR: …» */
  uint16_t track;                 /* «Track 3 of 10» (радио — станция); 0 — нет */
  uint16_t tracks;
  uint32_t played_ms;             /* радио — от включения станции */
  uint32_t duration_ms;           /* 0 — неизвестна */
  uint8_t load_percent;           /* ZM_UI_LOADING: 0..100 */
  uint8_t volume;                 /* 0..100 % */
  int8_t temperature;             /* кристалл ESP, °C; ZM_UI_TEMPERATURE_UNKNOWN — нет */
  uint8_t current;                /* ZM_UI_VIEW_STATIONS: текущая станция в stations */
  /* Спектр: 0..255 = −72..0 дБ, полосы по логарифму частоты (~40 Гц … 20 кГц), снимок
   * воспроизводимого места; уровни каналов: 0..255. */
  uint8_t level[ZM_UI_LEVELS];
  uint32_t sent_kb;               /* подробности: отдано ESP, КБ (от плагина) */
  uint32_t queue_bytes;           /* в очереди потока */
  uint16_t faults_decode;         /* кадров с ошибкой */
  uint16_t faults_starve;         /* опустошений очереди */
  uint16_t faults_i2s;            /* отказов вывода в ЦАП */
  uint16_t cover_serial;          /* меняется со сменой обложки; 0 — обложки нет */
  char stations[ZM_UI_STATIONS][ZM_UI_STATION_TEXT];
} zm_ui_model_t;

/* ABI 1.7: UTF-8 имена на microSD Tang; flags bit 0 — каталог. */
typedef struct {
  char name[256];
  uint32_t size;
  uint8_t flags;
  uint8_t reserved[3];
} zm_sd_entry_t;

typedef struct zm_core zm_core_t;
typedef struct zm_app zm_app_t;

typedef void (*zm_task_fn)(void* arg);

struct zm_core {
  uint16_t size;
  uint16_t abi_major;
  uint16_t abi_minor;
  const char* firmware;          /* версия прошивки, "vdac4-…" */

  /* --- память, журнал, время --- */
  void* (*alloc)(size_t size, uint32_t caps);
  void* (*realloc)(void* pointer, size_t size);
  void (*free)(void* pointer);
  void (*log)(const char* format, ...);        /* журнал наладки (строка до 136 знаков) */
  uint32_t (*millis)(void);
  void (*delay)(uint32_t ms);

  /* --- задачи и синхронизация (FreeRTOS под ними; ядро снимает их при выгрузке) --- */
  int (*task_start)(zm_task_fn fn, void* arg, uint32_t stack_bytes, uint8_t priority,
                    int8_t cpu);                /* cpu: 0, 1 или −1 — любое */
  void* (*mutex_create)(void);
  void (*mutex_lock)(void* mutex);
  void (*mutex_unlock)(void* mutex);
  void* (*signal_create)(void);                 /* двоичный семафор */
  void (*signal_give)(void* signal);
  int (*signal_wait)(void* signal, uint32_t timeout_ms);   /* ZM_OK или ZM_TIMEOUT */

  /* --- связь с Z80 ---
   * reply — ответ на текущую команду, только из zm_app_t::command;
   * event — сообщение без запроса (плагин его ждёт), wait_ms — сколько ждать места. */
  int (*reply)(uint8_t response, const void* data, uint16_t length);
  int (*event)(uint8_t code, const void* data, uint16_t length, uint32_t wait_ms);

  /* --- вывод звука ---
   * pcm_write: кадры float −1..1, каналы чередуются; ждёт места в выходной
   * очереди и возвращает число принятых кадров. Вызывать из своей задачи
   * звука. pcm_queued_ms — сколько звука ещё не сыграно. */
  int (*pcm_open)(const zm_pcm_format_t* format);
  int (*pcm_write)(const float* frames, uint32_t count);
  uint32_t (*pcm_queued_ms)(void);
  void (*pcm_flush)(void);                      /* выбросить несыгранное: перемотка, стоп */
  void (*pcm_close)(void);
  void (*pcm_pause)(int paused);                /* 1 — пауза вывода, 0 — дальше */
  void (*set_volume)(uint8_t volume);           /* 0..255, 255 — без ослабления */
  uint8_t (*volume)(void);
  uint16_t (*pcm_faults)(void);                 /* отказы I2S за запуск */

  /* --- модули и ресурсы из кэша ядра (копии файлов /WC/ESP/) ---
   * module_open грузит модуль (декодер) и вызывает его точку входа
   * main(3, {name, arg1, arg2}); её итог — в *entry_result. Символы модуля по
   * имени не ищутся (сборка ELF их скрывает): таблицу функций модуль отдаёт
   * через arg2, как приложение.
   * resource — файл целиком в PSRAM (банк MIDI): живёт до перезапуска ESP или
   * до resource_release. ZM_NOT_FOUND — файла нет: приложение просит плагин
   * прислать его (своей командой), и ядро кладёт его в кэш. */
  int (*module_open)(const char* name, void* arg1, void* arg2, void** handle,
                     int* entry_result);
  void (*module_close)(void* handle);
  int (*resource)(const char* name, const uint8_t** data, size_t* size);
  void (*resource_release)(const char* name);

  /* --- с ABI minor 1 ---
   * Температура кристалла ESP в десятых долях °C (датчик ядра читается раз в
   * секунду); ZM_TEMPERATURE_UNKNOWN — неизвестна. Звать, только если size
   * таблицы вмещает поле. */
  int16_t (*temperature)(void);

  /* --- с ABI minor 2 ---
   * Поток HTTP или HTTPS (интернет-радио; звать, только если size таблицы
   * вмещает поля). net_open — GET по url («http://…», «https://…»;
   * переадресации — ядро) с заголовками extra (строки «Имя: значение\r\n», можно
   * NULL); ответ ждёт до timeout_ms, в *status — код ответа (0 — не
   * соединилось). Возвращает поток (ответ 2xx) или NULL. Запрос — HTTP/1.0: тело
   * без chunked; ответ «ICY 200» (Shoutcast) — как HTTP 200.
   * net_header — значение заголовка ответа (имя без учёта регистра) или NULL;
   * строка живёт до следующего вызова для этого потока.
   * net_read — до count байтов тела: >0 — сколько, 0 — за timeout_ms ничего,
   * −1 — конец или обрыв.
   * net_close — закрыть. Потоков разом — до двух; звать из задачи, открывшей
   * поток; при выгрузке приложения ядро закрывает оставшиеся само. */
  void* (*net_open)(const char* url, const char* extra, uint32_t timeout_ms, int* status);
  const char* (*net_header)(void* stream, const char* name);
  int (*net_read)(void* stream, void* data, uint32_t count, uint32_t timeout_ms);
  void (*net_close)(void* stream);

  /* --- с ABI minor 3 ---
   * FT812 эмулятора Tang как видеокарта приложения: ESP — хост FT812, как Z80 на VDAC2
   * (звать, только если size таблицы вмещает поля). Картинку видно, пока Evo держит
   * экран у FT812 (M_SEL = 1: VCONFIG TS-Conf = #04); при M_SEL = 0 дольше 3 с Tang
   * возвращает ядро TS-Config, и FT812 пропадает (ft_read/ft_write — ZM_ERROR).
   * ft_open — стать хостом: при нужде перевести Tang в ядро FT812 (multiboot, до timeout_ms),
   * отключить SPI Evo к FT812 и запустить его в 1024×768@59 (64 МГц). ZM_OK, ZM_NOT_FOUND —
   * ядра FT812 нет (ядро Tang старше 1.12 или слот 1 пуст), ZM_ERROR — FT812 не ответил.
   * ft_write — слова подряд с address (кратен 4; RAM_G 0x000000, RAM_DL 0x300000, регистры
   * 0x302000…); ft_read — слово; ft_host_cmd — команда хоста (ACTIVE 00, CORERST 68, …);
   * ft_close — вернуть FT812 Evo (ядро само зовёт его при выгрузке приложения). */
  int (*ft_open)(uint32_t timeout_ms);
  int (*ft_write)(uint32_t address, const uint32_t* words, uint32_t count);
  int (*ft_read)(uint32_t address, uint32_t* value);
  int (*ft_host_cmd)(uint8_t command, uint8_t parameter);
  void (*ft_close)(void);

  /* --- с ABI minor 4 ---
   * Ядра ПЛИС Tang, которых нет во flash Tang (ZX Next и другие; docs/next-core.md). Звать,
   * только если size таблицы вмещает поля.
   * core_load — загрузить ядро name из кэша BL616 (есть на microSD Tang /CORES/NAME.BIN —
   * сначала свежий образ оттуда в кэш: новое или изменённое ядро — десятки секунд) и дождаться
   * его на LINK (до 15 с): ZM_OK, *version — VERSION ядра ({личность 3+, протокол LINK,
   * версия}); ZM_NOT_FOUND — нет ни в кэше, ни на SD; ZM_BUSY — загрузка уже идёт; ZM_ERROR —
   * не загрузилось (Tang снова в слоте 0, TS-Config). Грузит задача прошивки: выгрузка
   * приложения посреди загрузки её не прерывает. Загрузившее ядро приложение при выгрузке
   * возвращает Tang в слот 0 (ядро ESP само зовёт core_back).
   * core_back — назад в слот 0: запись BOOT текущему ядру (FT812, ядро из кэша), ждать
   * TS-Config на LINK до timeout_ms. Tang уже в TS-Config — сразу ZM_OK. Ядро приложение
   * отпускает в любом случае: ZM_TIMEOUT — возврат не вышел, ESP повторит BOOT сама через 3 с.
   * link_read / link_write — слова регистров и окон LINK текущего ядра (адрес кратен 4, count —
   * до 16 за вызов): ZM_OK или ZM_ERROR (LINK не работает).
   * sock_open — сокет TCP (udp = 0) или UDP (udp = 1) к host:port (имя или адрес IPv4); TCP ждёт
   * соединения до timeout_ms. Номер сокета ≥ 0 или ZM_ERROR; сокетов разом — до пяти (ESP-AT).
   * sock_send — отправить count байтов (TCP — все, UDP — одной датаграммой): ZM_OK / ZM_ERROR.
   * TCP ждёт места в буфере не дольше 3 с: сервер перестал читать — ZM_ERROR.
   * sock_recv — до count байтов: > 0 — сколько, 0 — за timeout_ms ничего (или пустая датаграмма
   * UDP), −1 — закрыт или обрыв. Датаграмма UDP длиннее count обрезается: брать с запасом.
   * sock_close — закрыть; при выгрузке приложения ядро закрывает оставшиеся само.
   * net_resolve — адрес IPv4 имени («a.b.c.d»): ZM_OK или ZM_NOT_FOUND.
   * wifi_info — Wi-Fi ESP: ZM_OK — подключена (ssid, ip, mac — строки), ZM_ERROR — нет. */
  int (*core_load)(const char* name, uint32_t* version);
  int (*core_back)(uint32_t timeout_ms);
  int (*link_read)(uint32_t address, uint32_t* words, uint32_t count);
  int (*link_write)(uint32_t address, const uint32_t* words, uint32_t count);
  int (*sock_open)(int udp, const char* host, uint16_t port, uint32_t timeout_ms);
  int (*sock_send)(int sock, const void* data, uint32_t count);
  int (*sock_recv)(int sock, void* data, uint32_t count, uint32_t timeout_ms);
  void (*sock_close)(int sock);
  int (*net_resolve)(const char* host, char ip[16]);
  int (*wifi_info)(char ssid[33], char ip[16], char mac[18]);

  /* --- с ABI minor 5 ---
   * ui_available — 1, если экран приложения сейчас можно показать оверлеем Tang (Tang с ядром
   * TS-Config 1.10+ на LINK), иначе 0. По нему приложение решает, готовить ли данные только для
   * экрана (обложку в RGB565). Звать, только если size таблицы вмещает поле. */
  int (*ui_available)(void);

  /* --- с ABI minor 6 ---
   * app_done — работа кончена: программа Z80 ушла (плагин вернулся в Wild Commander). Звать из
   * command: ответив на эту команду, ядро выгружает приложение, как по APP_STOP, — stop, затем
   * вся его память, задачи, модули и ELF. Следующий APP_START запускает его заново. Вне command
   * вызов ничего не делает. Звать, только если size таблицы вмещает поле. */
  void (*app_done)(void);

  /* ABI 1.7. Только чтение microSD Tang, только из своей задачи, пока Tang в
   * TS-Config. Операция держит Session и закрывает все файлы до возврата.
   * path — абсолютный путь внутри карты (UTF-8, до 511 байт, без ..).
   * list: каталоги и файлы с suffix (например ".nes", регистр не важен),
   * skip — число подходящих записей; capacity 1..16; more — есть следующая.
   * read: файл целиком, не длиннее max_size, память tracked PSRAM; free(data)
   * после использования. active: NULL либо флаг, снятие прерывает операцию.
   * При отказе count/size=0, data=NULL; файлов и блокировки не остаётся. */
  int (*sd_list)(const char* path, const char* suffix, uint32_t skip,
                 zm_sd_entry_t* entries, uint32_t capacity, uint32_t* count,
                 int* more, const volatile int* active);
  int (*sd_read)(const char* path, uint8_t** data, size_t* size, size_t max_size,
                 const volatile int* active);

  /* ABI 1.8: sd_write; с ABI 1.9 — произвольный файл, 1..7 MiB, без разбора формата.
   * Только TS-Config; Session удерживается до закрытия и проверки чтением обратно.
   * path.tmp/path.bak — временный файл и предыдущая копия. Приложение решает,
   * какие данные писать и как восстанавливаться из .bak после прерванной замены.
   * ABI 1.8 ограничивал запись форматом NES; новых полей в таблице нет. */
  int (*sd_write)(const char* path, const uint8_t* data, size_t size,
                  const volatile int* active);
};

/* Таблица приложения: заполняет приложение в main. */
struct zm_app {
  uint16_t size;
  uint16_t abi_major;
  uint16_t abi_minor;
  const char* name;              /* "player" */
  const char* version;           /* "1.0" */
  /* Команды Z80 из этого диапазона ядро отдаёт приложению (остальные — свои). */
  uint8_t command_first;
  uint8_t command_last;

  /* После загрузки: завести задачи, память. ZM_OK — приложение работает. */
  int (*start)(const zm_core_t* core);
  /* Перед выгрузкой: остановить свои задачи и вывод. Память освободит ядро. */
  void (*stop)(void);
  /* Команда Z80 (ядро обмена, ядро 1): ответить core->reply сразу, не
   * задерживаясь дольше нескольких миллисекунд. ZM_OK — команда своя. */
  int (*command)(uint8_t command, const uint8_t* data, uint16_t length);
  /* Раз за проход сетевого цикла (ядро 0); может быть NULL. */
  void (*poll)(uint32_t now_ms);

  /* --- с ABI minor 5 (ядро зовёт, только если size таблицы вмещает поля и они не NULL) ---
   * ui_model — снимок экрана в *model (поля, что помещаются в model->size): ядро зовёт его из
   * своей задачи экрана на ядре 1, 25–50 раз в секунду, пока Tang может показать экран, —
   * отвечать сразу, без долгих замков. ZM_OK — снимок есть; иначе экрана нет.
   * ui_cover — обложка, сведённая к side×side точек RGB565 (строками сверху вниз), в pixels:
   * ZM_OK или ZM_NOT_FOUND (нет). Ядро зовёт его, когда сменился cover_serial. */
  int (*ui_model)(zm_ui_model_t* model);
  int (*ui_cover)(uint16_t* pixels, uint16_t side);
};

#ifdef __cplusplus
}
#endif

#endif /* ZM_APP_H */
