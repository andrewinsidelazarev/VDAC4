#pragma once
/* Заглушка sys/param.h (в MSVC её нет) для стенда diskio.c: ffconf.h берёт отсюда MIN и MAX */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
