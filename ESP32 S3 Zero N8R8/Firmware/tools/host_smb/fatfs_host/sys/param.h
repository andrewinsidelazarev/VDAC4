#pragma once

// sys/param.h для MSVC: ffconf.h берёт из него MIN и MAX.
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
