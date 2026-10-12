# libxmp

Плеер трекерной музыки Extended Module Player, полная сборка (все загрузчики и
ProWizard), лицензия MIT — `LICENSE`.

Источник: https://github.com/libxmp/libxmp, коммит
`372272995bc67323a6e59a907ebdd093970c7875` (2026-10-02, тот же, что у
`components/libxmp_lite`). Взяты без изменений `include/xmp.h` и `src/` без
`src/depackers` (распаковщикам нужны временные файлы и popen), `src/bitrot`
(старый неиспользуемый код) и обёрток `src/lite`. Изменено (всё — под
`LIBXMP_ESP_MODULE`):

- `src/rng.c` — в модуле ESP нет `time()`, зерно случайных чисел постоянное (как
  в `components/libxmp_lite`);
- `src/load.c` — MD5 модуля не считается: это второй проход по файлу с начала, а
  IT приходит потоком (TRACKER, `xmp_load_module_from_callbacks`);
- `src/filetype.c` — файловой системы у модуля нет (`libxmp_get_filetype` —
  «нет файла», без `stat`);
- `src/loaders/sample.c` — сжатие отсчётов при загрузке по флагам
  `xmp_esp_reduce` (`esp/xmp_esp.h`): стерео сводится в моно, 16 бит — в 8;
  TRACKER ставит их, когда копия отсчётов не помещается в PSRAM.

Собирается с `LIBXMP_STATIC`, `LIBXMP_NO_DEPACKERS` и `LIBXMP_ESP_MODULE`.

В TRACKER.ELF (`modules/tracker`) libxmp играет IT и всё, что не берёт
`components/tracker` (MOD, XM и S3M играет он): MED и OctaMED, MultiTracker,
Scream Tracker 2, 669, Ultra Tracker, Oktalyzer, Soundtracker на 15 сэмплов,
упакованные форматы Amiga (ProWizard) и прочие форматы libxmp. Модуль грузится
из памяти или потоком (IT); `esp/xmp_esp_shim.c` — «файлы» в памяти (временный
файл ProWizard), таблица ctype и `getenv`, которых ядро прошивки модулям не
даёт, память libxmp в PSRAM (`xmp_malloc` и прочие: обычный malloc отдал бы
сотни мелких блоков дорожек узоров из внутренней ОЗУ) и флаги сжатия отсчётов.
Имена libc в исходниках libxmp переименованы ключами `-D` (`CMakeLists.txt`;
то же у стендов ПК `tests/test_player_host.py` и
`tools/unreal/build_vdac4_esp_host.py`). У стенда `player_host` память libxmp
идёт в счёт его PSRAM (`XMP_ESP_HOST_BUDGET`).
