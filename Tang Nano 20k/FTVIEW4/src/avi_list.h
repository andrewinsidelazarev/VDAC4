// Блок параметров банка списка роликов (страница 10 WMF). Лежит в окне #C000,
// в первой странице буфера за кэшем EVO (#C000..#FDFF): это окно одно и то же
// во всех банках. Вызов — far_call(LIST_PAGE, LIST_FAR) из видеобанка. Текущий
// ролик (кластер) и размер кадра банк берёт из копии полей AVI в AVI_FAR,
// выбранный ролик (кластер и размер файла) кладёт туда же.

// Кадр на экране: растяжение и матрица. В видеобанке это переменные HUD
// (avi_controls.c) — они копируются в блок одним присваиванием.
typedef struct
{
  u16 w, h;           // размер на экране
  u32 ta, te;         // BITMAP_TRANSFORM_A/E
} LIST_VIEW;

typedef struct
{
  u8 chosen;          // выход: 1 — выбран ролик (Enter), в том числе текущий
  u8 op;              // вход: LIST_OP_*
  u32 address;        // кадр на экране (RAM_G), под окном списка
  LIST_VIEW view;
  u8 code;            // вход: место отказа плеера для окна сообщения
  u8 count, sel;      // LIST_OP_READ: роликов в таблице и место текущего
  u8 ok;              // LIST_OP_READ: 0 — каталог не прочитался
} LIST_SHARED;

#define LIST_FAR ((LIST_SHARED*)0xFE00)
#define LIST_PAGE 10

// Таблица роликов каталога — в первой странице буфера (#CC00…#F2BF): банк
// ядра видео читает её после LIST_OP_READ.
#define LIST_MAX 80
// Имя в таблице — до NAME_MAX символов, при обрезке ещё «...» и ноль. Строка
// окна вмещает ~72 строчных или ~57 заглавных букв: обычное имя упирается в
// её ширину, а не в таблицу.
#define NAME_MAX 96
typedef struct
{
  u32 cluster, size;
  u8 key[16];         // ключ сортировки: латиница заглавными, кириллица — #80+номер
  char name[NAME_MAX + 4];  // имя на экране: ASCII, кириллица транслитом (LIST_OP_READ — CP866)
} LIST_ENTRY;
#define LIST_E ((LIST_ENTRY*)0xCC00)

// Что делает банк списка. Кроме самого списка он показывает окна сообщений
// плеера: в видеобанке на них не осталось места.
#define LIST_OP_PICK 0    // окно списка роликов
#define LIST_OP_CODEC 1   // «Unsupported video codec»
#define LIST_OP_FILE 2    // «Cannot display file»
#define LIST_OP_MJPEG 3   // кодек по началу файла в fs_buf: chosen 0 — чужой
#define LIST_OP_NOFT 4    // «No VDAC2 detected!»
#define LIST_OP_VDAC4 5   // chosen 1 — VDAC4 (RAM_G 2 МБ), 0 — FT812; для банка изображений
#define LIST_OP_READ 6    // каталог в таблицу без окна, имена в CP866: банк ядра видео (список рисует ESP)
#define LIST_OP_BG_START 7  // фоновая таблица перемотки ролика из AVI_FAR (core_bg.c): с начала
#define LIST_OP_BG_STEP 8   // один короткий шаг её построения

// Фоновая таблица перемотки банка ядра видео (core_bg.c): банк списка строит её по idx1 мелкими
// шагами, пока ролик идёт (банк ядра зовёт шаг, когда кэш потока полон). Записи — в страницах
// 12…15, как у index_build. Блок — за LIST_FAR в той же странице 1.
typedef struct
{
  u8 state;           // BG_*
  u8 percent;         // пройдено idx1, %
  u16 step;           // кадров на запись таблицы (0 — idx1 ещё не найден)
  u16 count;          // готовых записей (k < count — запись есть)
  u32 avg;            // вход START: байтов звука в секунду (core_avg; 0 — звука нет)
} BG_SHARED;
#define BG_FAR ((BG_SHARED*)0xFE40)
#define BG_IDLE 0     // не начата
#define BG_RUN 1      // строится
#define BG_DONE 2     // готова целиком
#define BG_NONE 3     // таблицы не будет (нет idx1, ошибка чтения)
