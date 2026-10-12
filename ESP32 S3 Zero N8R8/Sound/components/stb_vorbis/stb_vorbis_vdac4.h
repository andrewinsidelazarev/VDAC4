/* stb_vorbis_vdac4.h — объявления stb_vorbis (v1.22, public domain) для OGG.ELF
 * (modules/ogg) с настройками модуля VDAC4: только pushdata (поток кусками),
 * без stdio и целочисленного вывода, до 8 каналов. Те же настройки — у
 * stb_vorbis_vdac4.c, где собирается сам декодер. */
#ifndef STB_VORBIS_VDAC4_H
#define STB_VORBIS_VDAC4_H

#define STB_VORBIS_NO_STDIO 1
#define STB_VORBIS_NO_PULLDATA_API 1
#define STB_VORBIS_NO_INTEGER_CONVERSION 1
#define STB_VORBIS_MAX_CHANNELS 8

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#endif /* STB_VORBIS_VDAC4_H */
