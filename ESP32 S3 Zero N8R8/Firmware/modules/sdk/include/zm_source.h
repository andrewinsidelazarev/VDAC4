/* zm_source.h — интерфейс «плеер ↔ декодер» платы VDAC4.
 *
 * Декодер — модуль из /WC/ESP/ (MP3, FLAC, TRACKER, MIDI…), который грузит
 * приложение-плеер через zm_core_t::module_open. Декодер только превращает
 * байты файла в PCM и рассказывает о нём: очередь, частота, громкость, спектр,
 * метки для окна, обложка, связь с Z80 — у плеера и ядра.
 *
 * Подключение: точка входа модуля — main(argc, argv). Плеер грузит его через
 * zm_core_t::module_open и передаёт
 *   argv[1] — (const zm_host_t*) свою таблицу,
 *   argv[2] — (const zm_source_t**) место для таблицы декодера;
 * main возвращает ZM_SOURCE_ABI_MAJOR.
 *
 * Потоки: все функции декодера вызывает одна задача плеера; декодер
 * однопоточный и блокировок не держит. Один вызов render — не больше
 * ZM_RENDER_MAX_FRAMES кадров (сторожевой таймер ESP).
 *
 * Совместимость — как в zm_app.h: ZM_SOURCE_ABI_MAJOR должен совпасть, новые
 * поля только в конец, size — размер таблицы у заполнившей стороны.
 */
#ifndef ZM_SOURCE_H
#define ZM_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZM_SOURCE_ABI_MAJOR 1
#define ZM_SOURCE_ABI_MINOR 8   /* 1: samples и effects в zm_info_t; 2: запас ZM_WHOLE_SLACK;
                                   3: file_type в zm_host_t; 4: stream_offset;
                                   5: файл целиком потоком (streams_whole, make_room,
                                   input_wait); 6: помощник на ядре 1 (worker_start,
                                   worker_wait); 7: уступка ядра (rest); 8: дополнительные метки (tag) */

/* ZM_INPUT_WHOLE: за файлом в буфере ещё ZM_WHOLE_SLACK байтов (хост с
 * abi_minor >= 2); декодер вправе писать в весь буфер — например, распаковать
 * отсчёты на месте, выровняв их. */
#define ZM_WHOLE_SLACK 8192
#define ZM_RENDER_MAX_FRAMES 1024

/* Как декодер получает файл. */
enum {
  ZM_INPUT_STREAM = 0,   /* по мере прихода (MP3, FLAC, WAV): input_read */
  ZM_INPUT_WHOLE = 1,    /* целиком в PSRAM до начала звука (MOD/XM/S3M/IT, MIDI) или,
                            если декодер умеет (streams_whole), потоком в open */
};

/* В чём мерит перемотка seek. */
enum {
  ZM_SEEK_MS = 0,
  ZM_SEEK_POSITION = 1,
};

/* Итоги render. */
enum {
  ZM_RENDER_OK = 0,        /* кадры есть */
  ZM_RENDER_NEED_INPUT = 1,/* поток не успевает: данных для кадра нет, повторить позже */
  ZM_RENDER_END = 2,       /* файл кончился, всё отдано */
  ZM_RENDER_ERROR = 3,
};

/* Что плеер даёт декодеру. */
typedef struct {
  uint16_t size;
  uint16_t abi_major;
  uint16_t abi_minor;
  void* (*alloc)(size_t size, uint32_t caps);    /* caps — ZM_MEM_* из zm_app.h */
  void (*free)(void* pointer);
  void (*log)(const char* format, ...);
  /* Поток: байты по порядку из очереди плеера. read возвращает, сколько
   * скопировано (0 — пока нет). ended — файл отдан целиком. */
  size_t (*input_read)(void* destination, size_t count);
  size_t (*input_available)(void);
  int (*input_ended)(void);
  /* Ресурс целиком (банк MIDI): ZM_NOT_FOUND — плеер попросит плагин прислать. */
  int (*resource)(const char* name, const uint8_t** data, size_t* size);
  /* Картинка обложки, найденная в файле (целиком): миниатюру делает плеер
   * (PLAYER 1.5 — из baseline JPEG; PNG не сводит). Можно не вызывать; зовут
   * из задачи декодера (open, render). */
  void (*cover)(const uint8_t* image, size_t size);
  /* Расширение открытого файла большими буквами ("IMF", "WLF"; "" — неизвестно):
   * подсказка для форматов без подписи. Есть, если size его вмещает. */
  const char* file_type;
  /* Поток: место в файле его первого байта — 0 с начала, после перемотки
   * (AUDIO_FLUSH) — куда встал плагин; UINT32_MAX — неизвестно. По нему
   * декодер без заголовков (WAV) выравнивается на отсчёт. Есть, если size
   * его вмещает. */
  uint32_t stream_offset;
  /* --- с abi_minor 5: файл целиком потоком (open с file == NULL у декодера
   * со streams_whole); есть, если size их вмещает. ---
   * make_room: 1 — bytes байтов PSRAM можно занять (кусками, как берёт
   * libxmp); не хватает — плеер освобождает ресурсы, которые этому файлу не
   * нужны (банк MIDI, ПЗУ OPL4), и пробует снова; 0 — места нет.
   * input_wait: ждать, пока в очереди будет count байтов, файл кончится
   * (input_ended), плеер остановит приём или пройдёт timeout_ms; возвращает,
   * сколько байтов есть. */
  int (*make_room)(size_t bytes);
  size_t (*input_wait)(size_t count, uint32_t timeout_ms);
  /* --- с abi_minor 6: помощник на втором ядре (есть, если size их вмещает и
   * указатели не NULL: задачу помощника плеер мог и не получить) ---
   * worker_start — выполнить fn(arg) на задаче ядра 1 (у плеера она одна и
   * живёт, пока живёт он); ZM_OK — запущено, ZM_BUSY — прежняя работа ещё
   * идёт. worker_wait — дождаться конца работы (сразу, если её нет). Звать
   * только из render, seek и close (задача декодера); до выхода из close
   * декодер дожидается своей работы сам. Так MIDI.ELF считает эффекты
   * прошлого куска, пока ядро 0 синтезирует следующий. */
  int (*worker_start)(void (*fn)(void* arg), void* arg);
  void (*worker_wait)(void);
  /* --- с abi_minor 7 (есть, если size её вмещает и указатель не NULL) ---
   * rest — дать другим задачам 1 мс, если с прошлой уступки прошло больше 100 мс (сторож
   * задач ESP): долгий разбор файла в open, долгая перемотка или пачка событий в render не
   * держат ядро. Звать из задачи декодера (open, render, seek). */
  void (*rest)(void);
} zm_host_t;

/* Сведения о звучащем (для окна плеера). Строки — UTF-8 или байты файла (8-битную
 * кириллицу, CP1251 или CP866, различает PLAYER.ELF с 1.9); пусто — неизвестно. */
typedef struct {
  char format[24];         /* "MP3 320 kbps", "XM", "MIDI" */
  uint32_t rate;           /* частота PCM, который отдаёт render */
  uint8_t channels;
  uint32_t duration_ms;    /* 0 — неизвестна */
  uint32_t position_ms;    /* место в песне за последним отданным кадром; если
                              duration_ms известна, плеер берёт время отсюда */
  char title[64];
  char artist[64];
  char album[64];
  uint16_t year;
  /* Трекер: позиция таблицы порядка, её длина, строка паттерна. */
  uint16_t order;
  uint16_t order_length;
  uint8_t row;
  uint8_t voices;          /* каналов модуля / каналов MIDI с нотами */
  uint16_t bitrate;        /* кбит/с (MP3, FLAC), 0 — нет */
  uint32_t errors;         /* испорченных кадров, пропущенных декодером */
  /* Трекер: сэмплов с данными и разных эффектов в узорах; MIDI: разных
   * инструментов с нотами и дорожек файла (0 — нет или не знает). */
  uint16_t samples;
  uint8_t effects;
} zm_info_t;

typedef struct {
  uint16_t size;
  uint16_t abi_major;
  uint16_t abi_minor;
  const char* name;            /* "mp3" */
  const char* version;
  const char* extensions;      /* "MP3", "MOD XM S3M IT", "MID KAR" */
  uint8_t input;               /* ZM_INPUT_* */
  uint8_t keeps_file;          /* ZM_INPUT_WHOLE: 1 — файл нужен до close, 0 — после
                                  open его можно освободить (декодер взял копию) */
  uint8_t seek_unit;           /* ZM_SEEK_MS или ZM_SEEK_POSITION (трекер: позиция
                                  таблицы порядка) */
  const char* resources;       /* нужные ресурсы через пробел ("GM.SF2") или NULL */

  /* Узнаёт ли декодер файл по его началу: 0 — нет, 1..100 — уверенность. */
  int (*probe)(const uint8_t* head, size_t length);
  /* Начать файл. file_size — полная длина; для ZM_INPUT_WHOLE file — весь
   * файл (живёт до close), для потока — NULL. Возвращает экземпляр или NULL.
   * ZM_INPUT_WHOLE со streams_whole: file == NULL — файл идёт потоком
   * (input_read, input_wait), и open читает из него всё, что нужно, сама (это
   * долго: столько, сколько плагин шлёт файл); остаток потока после open
   * плеер выбрасывает. */
  void* (*open)(uint32_t file_size, const uint8_t* file);
  /* До max_frames кадров float −1..1 в out, каналы чередуются. */
  int (*render)(void* instance, float* out, uint32_t max_frames, uint32_t* frames);
  /* Перемотка на delta (мс или позиций — seek_unit). Поток: в *file_offset — откуда
   * плеер должен подать файл дальше (после чего декодер начнёт с чистого
   * состояния); файл целиком: *file_offset = UINT32_MAX — перемотал сам. */
  int (*seek)(void* instance, int32_t delta, uint32_t* file_offset);
  void (*info)(const void* instance, zm_info_t* out);
  /* Уровни каналов 0..255 (трекер, MIDI); NULL — не умеет. Возвращает число. */
  int (*levels)(const void* instance, uint8_t* out, int max);
  void (*close)(void* instance);
  /* --- с abi_minor 5 (есть, если size его вмещает) ---
   * ZM_INPUT_WHOLE: 1 — open умеет брать файл потоком (file == NULL); плеер
   * так подаёт файлы, которым это выгодно (IT: отсчёты не лежат в памяти
   * дважды). */
  uint8_t streams_whole;
  /* С abi_minor 8, если size вмещает: дополнительные метки UTF-8 (например,
   * "TRACKNUMBER"). NULL — нет. Указатель принадлежит декодеру, копировать
   * до следующего вызова. Не меняет размер zm_info_t у прежних декодеров. */
  const char* (*tag)(const void* instance, const char* name);
} zm_source_t;

#ifdef __cplusplus
}
#endif

#endif /* ZM_SOURCE_H */
