#pragma once

/* Для компиляции ff.c (язык C) на ПК: ffconf.h берёт отсюда только шаг тика. Заглушки
 * FreeRTOS в tests/stubs_host — C++, их C-компилятор не прочтёт. */
#define portTICK_PERIOD_MS 1
