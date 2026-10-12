/* Интернет-радио PLAYER.ELF: плейлист M3U и текст станций (radio_list.h). */
#include "radio_list.h"

#include <string.h>

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

static int starts_no_case(const uint8_t* text, size_t length, const char* prefix) {
  size_t i = 0;
  for (; prefix[i] != 0; ++i) {
    if (i >= length || lower(text[i]) != lower((uint8_t)prefix[i])) {
      return 0;
    }
  }
  return 1;
}

static int is_space(uint8_t c) { return c == ' ' || c == '\t'; }

/* Название из #EXTINF: скобки вокруг — прочь, хвост « - www.…» (так в плейлистах
 * станций TAVR: «(Radio ROKS - www.radioroks.ua)») — тоже. */
static void extinf_title(const uint8_t* from, const uint8_t* to, char* out) {
  while (from < to && is_space(*from)) ++from;
  while (to > from && is_space(to[-1])) --to;
  if (to - from >= 2 && from[0] == '(' && to[-1] == ')') {
    ++from;
    --to;
  }
  for (const uint8_t* p = from; p + 7 <= to; ++p) {
    if (memcmp(p, " - www.", 7) == 0 || memcmp(p, " - http", 7) == 0) {
      to = p;
      break;
    }
  }
  while (to > from && is_space(to[-1])) --to;
  size_t length = (size_t)(to - from);
  if (length > RADIO_TITLE - 1) {
    length = RADIO_TITLE - 1;
  }
  memcpy(out, from, length);
  out[length] = 0;
}

int radio_parse_m3u(const uint8_t* text, uint32_t size, radio_station_t* out, int max) {
  uint32_t at = 0;
  if (size >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) {
    at = 3;                                    /* BOM UTF-8 */
  }
  char title[RADIO_TITLE] = "";
  int count = 0;
  while (at < size && count < max) {
    uint32_t end = at;
    while (end < size && text[end] != '\n' && text[end] != '\r') ++end;
    uint32_t a = at, b = end;
    while (a < b && is_space(text[a])) ++a;
    while (b > a && is_space(text[b - 1])) --b;
    const size_t length = b - a;
    if (starts_no_case(text + a, length, "#EXTINF:")) {
      const uint8_t* comma = (const uint8_t*)memchr(text + a + 8, ',', length - 8);
      if (comma != NULL) {
        extinf_title(comma + 1, text + b, title);
      } else {
        title[0] = 0;
      }
    } else if (starts_no_case(text + a, length, "http://") ||
               starts_no_case(text + a, length, "https://")) {
      if (length < RADIO_URL) {
        memcpy(out[count].url, text + a, length);
        out[count].url[length] = 0;
        memcpy(out[count].title, title, sizeof(title));
        ++count;
      }
      title[0] = 0;
    } else if (length != 0 && text[a] != '#') {
      title[0] = 0;                            /* локальный файл: не станция */
    }
    at = end;
    while (at < size && (text[at] == '\n' || text[at] == '\r')) ++at;
  }
  return count;
}

/* CP1251 0x80..0xBF — Unicode (0xC0..0xFF — А..я подряд). */
static const uint16_t cp1251_high[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409,
    0x2039, 0x040A, 0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x003F, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0,
    0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB,
    0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6,
    0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};

/* Знак Unicode — в CP866; 0 — пропустить (мягкий перенос). */
static uint8_t cp866(uint32_t code) {
  if (code >= 0x20 && code < 0x7F) return (uint8_t)code;
  if (code >= 0x410 && code <= 0x43F) return (uint8_t)(0x80 + (code - 0x410));   /* А..п */
  if (code >= 0x440 && code <= 0x44F) return (uint8_t)(0xE0 + (code - 0x440));   /* р..я */
  switch (code) {
    case 0x401: return 0xF0;                   /* Ё */
    case 0x451: return 0xF1;                   /* ё */
    case 0x406: case 0x407: return 'I';        /* І, Ї */
    case 0x456: case 0x457: return 'i';        /* і, ї */
    case 0x404: return 'E';                    /* Є */
    case 0x454: return 'e';                    /* є */
    case 0x490: return 0x83;                   /* Ґ — Г */
    case 0x491: return 0xA3;                   /* ґ — г */
    case 0x40E: return 0x93;                   /* Ў — У */
    case 0x45E: return 0xE3;                   /* ў — у */
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0xAB: case 0xBB: return '"';
    case 0x2026: case 0x2022: case 0xB7: return '.';
    case 0xA0: return ' ';
    case 0x2116: return 'N';
    case 0xAD: return 0;
    default: return '?';
  }
}

/* Правильный ли UTF-8 (и есть ли в нём что-то кроме ASCII — неважно). Знак,
 * оборванный в конце строки, — правильный, если до него был целый (декодер
 * обрезал строку по длине); без целого — это 8-битный текст с буквой в конце. */
static int valid_utf8(const uint8_t* p) {
  int whole = 0;
  while (*p != 0) {
    const uint8_t c = *p++;
    if (c < 0x80) continue;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0 || c == 0xC0 || c == 0xC1) return 0;
    for (; extra > 0; --extra) {
      if (*p == 0) return whole;
      if ((*p++ & 0xC0) != 0x80) return 0;
    }
    whole = 1;
  }
  return 1;
}

/* 8-битная кириллица — CP866? Байты 80..AF — буквы CP866 (А..Я, а..п), в CP1251
 * это редкие знаки; B0..DF и F2..FF — буквы CP1251 (А..Я, т..я), в CP866 —
 * псевдографика и знаки; E0..F1 — буквы в обеих. Чьих больше, та и кодировка;
 * поровну — CP1251. */
static int looks_cp866(const uint8_t* p) {
  int dos = 0, win = 0;
  for (; *p != 0; ++p) {
    if (*p >= 0x80 && *p <= 0xAF) {
      ++dos;
    } else if ((*p >= 0xB0 && *p <= 0xDF) || *p >= 0xF2) {
      ++win;
    }
  }
  return dos > win;
}

/* Байт CP866 (80..FF) — Unicode; псевдографика — '?'. */
static uint32_t cp866_code(uint8_t c) {
  if (c < 0xB0) return 0x410 + (c - 0x80);                       /* А..Я, а..п */
  if (c >= 0xE0 && c <= 0xEF) return 0x440 + (c - 0xE0);         /* р..я */
  switch (c) {
    case 0xF0: return 0x401;                   /* Ё */
    case 0xF1: return 0x451;                   /* ё */
    case 0xF2: return 0x404;                   /* Є */
    case 0xF3: return 0x454;                   /* є */
    case 0xF4: return 0x407;                   /* Ї */
    case 0xF5: return 0x457;                   /* ї */
    case 0xF6: return 0x40E;                   /* Ў */
    case 0xF7: return 0x45E;                   /* ў */
    case 0xFC: return 0x2116;                  /* № */
    case 0xFF: return 0xA0;                    /* неразрывный пробел */
    default: return '?';
  }
}

void text_cp866(const char* text, char* out, size_t size) {
  if (size == 0) {
    return;
  }
  const uint8_t* p = (const uint8_t*)text;
  const int utf8 = valid_utf8(p);
  const int dos = !utf8 && looks_cp866(p);
  size_t used = 0;
  while (*p != 0 && used + 1 < size) {
    uint32_t code = *p++;
    if (code >= 0x80) {
      if (utf8) {
        int extra = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : 1;
        code &= 0x3Fu >> extra;
        for (; extra > 0 && *p != 0; --extra) {
          code = (code << 6) | (*p++ & 0x3Fu);
        }
        if (extra != 0) {
          break;                               /* знак обрезан в конце строки */
        }
      } else if (dos) {
        code = cp866_code((uint8_t)code);
      } else {
        code = code >= 0xC0 ? 0x410 + (code - 0xC0) : cp1251_high[code - 0x80];
      }
    }
    if (code < 0x20) {
      continue;
    }
    const uint8_t c = cp866(code);
    if (c != 0) {
      out[used++] = (char)c;
    }
  }
  out[used] = 0;
}

int radio_stream_title(const char* meta, char* out, size_t size) {
  const char* start = strstr(meta, "StreamTitle='");
  if (start == NULL || size == 0) {
    return 0;
  }
  start += 13;
  const char* end = strstr(start, "';");
  if (end == NULL) {
    end = start + strlen(start);
    if (end > start && end[-1] == '\'') {
      --end;
    }
  }
  size_t length = (size_t)(end - start);
  if (length > size - 1) {
    length = size - 1;
  }
  memcpy(out, start, length);
  out[length] = 0;
  return 1;
}
