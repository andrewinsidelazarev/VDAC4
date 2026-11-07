# libwebp 1.5.0 — декодер для сопроцессора BL616 (VDAC4)

Источник: https://github.com/webmproject/libwebp, метка `v1.5.0` (git clone 04.11.2026 в `E:\zx\libwebp`).
Лицензия — BSD (`COPYING`), патенты — `PATENTS`, авторы — `AUTHORS`.

Взяты только файлы декодера, нужные `ft_image.c` (WebP в CMD_LOADIMAGE и CMD_LOADIMAGE_BOX, прошивка
40): `src/dec` (VP8, VP8L, альфа, ввод-вывод), `src/dsp` (только C), `src/utils`, заголовки
`src/webp`; из `src/enc` — два заголовка, которые подключает `src/dsp/lossless.h`. Кодировщика нет.
Что собирается — [`vdac4_sources.txt`](vdac4_sources.txt) (его читают `app/CMakeLists.txt`,
`copro/test/run.sh` и `copro/test/test_task_images.py`).

Сборка: `HAVE_CONFIG_H` (свой пустой `src/webp/config.h`: без SSE2/NEON и потоков — на BL616 и в
стенде на ПК один и тот же код C), `WEBP_REDUCE_SIZE` (без масштабирования), `WEBP_REDUCE_CSP`
(только RGBA-виды точек). Горячие файлы — `-O2`, прочие — `-Os` SDK (`app/CMakeLists.txt`).

## Правки VDAC4

Все помечены «VDAC4» в коде.

1. **Строки VP8L — в обработчик** (`src/dec/vp8l_dec.c`, `vp8li_dec.h`): `ProcessRows` отдаёт готовые
   строки (после обратных преобразований) в `VP8LRowsHookFn`, а не в буфер вывода во всю картинку
   (памяти BL616 на него нет).
2. **VP8L кусками** (там же): `VP8LDecodeImageRows(dec, last_row)` — разбор до строки `last_row` за
   вызов (сопроцессор между кусками ведёт свой главный цикл); `VP8LDecodeImage` — как было. Кэш
   цветов на конце куска полон сам: цикл разбора выходит только на границе строки или после ссылки
   назад, а там libwebp его уже дописывает.
3. **Память — из пула картинки** (`src/utils/utils.c`): `malloc`, `calloc`, `free` → `VDAC4WebPAlloc`,
   `VDAC4WebPFree` (`ft_image.c`: стопкой из рабочей памяти сопроцессора; куча BL616 — ~40 КБ).
4. **Кэш строк VP8 — по настоящему размеру** (`src/dec/frame_dec.c`, `AllocateMemory`): libwebp брал
   `top_size` (32 байта на столбец макроблоков) × `cache_height`, а строкам кэша нужно 16 — вдвое
   больше нужного; у 1280 точек в ширину это 46 КБ.
5. **Разделы токенов VP8 — окнами** (`src/dec/vp8_dec.c` + `vp8i_dec.h`, `src/utils/bit_reader_utils.c`
   + `.h`): `ParsePartitions` отдаёт начало и длину раздела в `VP8PartitionsHookFn` (если он есть), а
   `VP8LoadFinalBytes`, когда окно кончилось, зовёт `VP8BitReaderRefillFn` — следующее окно. Так у
   CMD_LOADIMAGE_BOX файл лежит в RAM_G, а не в памяти BL616. Байты и их порядок те же — разбор бит в
   бит тот же (у libwebp чтение по 8 бит у конца буфера и по `BITS` в середине равносильны).
6. **log2 без `clz`** (`src/utils/utils.h`): на RISC-V без Zbb (E907) `__builtin_clz` — вызов
   `__clzsi2`, а `VP8GetBit` зовёт его на каждый бит; там — таблица `WebPLogTable8bit`, как у
   компиляторов без `clz` (модель BL616: VP8 1280×720 — −7 %).

Проверка — `copro/test/test_task_images.py` (37 случаев WebP против Pillow, бит в бит) и
`copro/test/run.sh` (`host_test`: WebP через кольцо, с файлом в RAM_G).
