# libxmp-lite

Плеер трекерной музыки Extended Module Player в урезанной сборке «lite»
(форматы MOD, S3M, XM, IT), лицензия MIT — `LICENSE`.

Источник: https://github.com/libxmp/libxmp, коммит
`372272995bc67323a6e59a907ebdd093970c7875` (2026-10-02). Взяты без изменений:
`include/xmp.h`, все заголовки `src/`, исходники, которые включают обёртки
`src/lite/lite-*.c`, и сами обёртки. Собирается с `LIBXMP_CORE_PLAYER` и
`LIBXMP_STATIC` (как `BUILD_LITE` в их CMake).

В TRACKER.ELF (`modules/tracker`) libxmp играет IT: NNA, огибающие, сжатые
отсчёты IT 2.14/2.15, фильтры. MOD, XM и S3M по-прежнему играет
`components/tracker`.
