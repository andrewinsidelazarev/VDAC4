/* helix_aac_port.h — Helix AAC в модуле VDAC4 (AAC.ELF, modules/aac).
 *
 * Вместо ConfigHelix.h и utils/helix_pgm.h arduino-libhelix: таблицы — обычные
 * константы (у ESP32-S3 они и так читаются из flash/PSRAM), память декодера —
 * функции модуля helix_malloc/helix_free (PSRAM через zm_host_t::alloc), SBR
 * (HE-AAC v1) включён. Параметрического стерео (HE-AAC v2) Helix не умеет:
 * такой поток звучит моно. */
#ifndef HELIX_AAC_PORT_H
#define HELIX_AAC_PORT_H

#define PROGMEM
#define pgm_read_byte(addr) (*(const unsigned char*)(addr))
#define pgm_read_word(addr) (*(const unsigned short*)(addr))

#define AAC_ENABLE_SBR

void* helix_malloc(int size);
void helix_free(void* pointer);

#endif /* HELIX_AAC_PORT_H */
