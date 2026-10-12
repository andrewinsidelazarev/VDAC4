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
#define ZM_APP_ABI_MINOR 2   /* 1: temperature в zm_core_t; 2: net_* (поток HTTP/HTTPS) */

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
};

#ifdef __cplusplus
}
#endif

#endif /* ZM_APP_H */
