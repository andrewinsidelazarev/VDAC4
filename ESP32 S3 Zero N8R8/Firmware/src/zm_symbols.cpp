// Стандартные функции, которые ядро даёт модулям ESP (ELF из /WC/ESP/) сверх
// узкой таблицы libc загрузчика (components/elf_loader): строки, печать в
// буфер, математика float и double, помощники libgcc для 64-битных целых и
// double (сопроцессор ESP32-S3 — только одинарной точности) и TJpgDec из ПЗУ.
// Всё прочее модуль получает через zm_core_t (zm_app.h). Нужен модулю символ,
// которого здесь нет, — связывание сообщит его в журнале.

#include "zifi/board.hpp"

#if ZIFI_HAS_AUDIO

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>

#include "esp32s3/rom/tjpgd.h"
#include "esp_elf.h"

extern "C" {
// Помощники libgcc: берётся только адрес.
extern char __adddf3, __subdf3, __muldf3, __divdf3, __negdf2;
extern char __eqdf2, __nedf2, __ltdf2, __ledf2, __gtdf2, __gedf2, __unorddf2;
extern char __fixdfsi, __fixunsdfsi, __floatsidf, __floatunsidf;
extern char __extendsfdf2, __truncdfsf2, __fixdfdi, __floatdidf;
extern char __divdi3, __udivdi3, __moddi3, __umoddi3;
extern char __ashldi3, __ashrdi3, __lshrdi3, __muldi3;
extern char __fixsfdi, __fixunssfdi, __floatdisf, __floatundisf;
extern char __divsf3, __fixunssfsi, __floatunsisf;
}

namespace zifi {
namespace {

#define ZM_SYM(name) {#name, reinterpret_cast<const void*>(&name)}

const struct esp_elfsym kModuleSymbols[] = {
    // string.h
    ZM_SYM(memmove), ZM_SYM(memcmp), ZM_SYM(memchr), ZM_SYM(strncmp), ZM_SYM(strcpy),
    ZM_SYM(strncpy), ZM_SYM(strcat), ZM_SYM(strstr), ZM_SYM(strnlen), ZM_SYM(strcasecmp),
    ZM_SYM(strncasecmp),
    // stdio.h, stdlib.h
    ZM_SYM(snprintf), ZM_SYM(vsnprintf), ZM_SYM(sprintf), ZM_SYM(qsort), ZM_SYM(abs),
    ZM_SYM(labs), ZM_SYM(atoi), ZM_SYM(strtoul), ZM_SYM(rand), ZM_SYM(srand),
    // math.h, float
    ZM_SYM(sinf), ZM_SYM(cosf), ZM_SYM(tanf), ZM_SYM(atanf), ZM_SYM(atan2f), ZM_SYM(powf),
    ZM_SYM(expf), ZM_SYM(exp2f), ZM_SYM(logf), ZM_SYM(log2f), ZM_SYM(log10f), ZM_SYM(sqrtf),
    ZM_SYM(floorf), ZM_SYM(ceilf), ZM_SYM(roundf), ZM_SYM(truncf), ZM_SYM(fmodf),
    ZM_SYM(lrintf), ZM_SYM(lroundf),
    // math.h, double
    ZM_SYM(sin), ZM_SYM(cos), ZM_SYM(tan), ZM_SYM(atan), ZM_SYM(atan2), ZM_SYM(pow),
    ZM_SYM(exp), ZM_SYM(log), ZM_SYM(log10), ZM_SYM(sqrt), ZM_SYM(floor), ZM_SYM(ceil),
    ZM_SYM(fmod), ZM_SYM(fabs), ZM_SYM(ldexp), ZM_SYM(frexp),
    // libgcc: double и 64-битные целые
    ZM_SYM(__adddf3), ZM_SYM(__subdf3), ZM_SYM(__muldf3), ZM_SYM(__negdf2),
    ZM_SYM(__eqdf2), ZM_SYM(__nedf2), ZM_SYM(__ledf2), ZM_SYM(__gedf2), ZM_SYM(__unorddf2),
    ZM_SYM(__fixdfsi), ZM_SYM(__floatsidf), ZM_SYM(__extendsfdf2), ZM_SYM(__truncdfsf2),
    ZM_SYM(__fixdfdi), ZM_SYM(__floatdidf), ZM_SYM(__divdi3), ZM_SYM(__udivdi3),
    ZM_SYM(__moddi3), ZM_SYM(__umoddi3), ZM_SYM(__ashldi3), ZM_SYM(__ashrdi3),
    ZM_SYM(__lshrdi3), ZM_SYM(__muldi3), ZM_SYM(__fixsfdi), ZM_SYM(__fixunssfdi),
    ZM_SYM(__floatdisf), ZM_SYM(__floatundisf), ZM_SYM(__divsf3), ZM_SYM(__fixunssfsi),
    ZM_SYM(__floatunsisf),
    // ESP-IDF: память нужного вида (трекер берёт PSRAM сам)
    ZM_SYM(heap_caps_malloc), ZM_SYM(heap_caps_calloc), ZM_SYM(heap_caps_free),
    // ПЗУ ESP32-S3: TJpgDec (обложки)
    ZM_SYM(jd_prepare), ZM_SYM(jd_decomp),
    {nullptr, nullptr},
};

#undef ZM_SYM

}  // namespace

void registerModuleSymbols() {
  static bool registered = false;
  if (!registered) {
    registered = esp_elf_register_symbol(kModuleSymbols) == 0;
  }
}

}  // namespace zifi

#endif  // ZIFI_HAS_AUDIO
