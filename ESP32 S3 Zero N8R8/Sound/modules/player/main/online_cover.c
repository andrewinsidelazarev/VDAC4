/* Обложка из сети (online_cover.h): Deezer API. */
#include "online_cover.h"

#include <stdio.h>
#include <string.h>

#define SEARCH_HOST "https://api.deezer.com/search?limit=5&q="
#define JSON_MAX (24u * 1024u)             /* ответ поиска на 5 записей — ~8 КБ */
#define OPEN_MS 8000
#define READ_MS 4000
#define NAME_MAX 96                        /* знаков имени исполнителя для сверки */

/* --- текст ------------------------------------------------------------------------- */

/* Знак CP866 → код Юникода (кириллица; псевдографика и прочее — пробел). */
static uint16_t cp866_code(uint8_t c) {
  if (c < 0x80) return c;
  if (c < 0xB0) return (uint16_t)(0x0410 + (c - 0x80));          /* А…Я, а…п */
  if (c >= 0xE0 && c < 0xF0) return (uint16_t)(0x0440 + (c - 0xE0));  /* р…я */
  if (c == 0xF0) return 0x0401;                                    /* Ё */
  if (c == 0xF1) return 0x0451;                                    /* ё */
  return ' ';
}

static size_t put_utf8(uint16_t code, char* out) {
  if (code < 0x80) {
    out[0] = (char)code;
    return 1;
  }
  out[0] = (char)(0xC0 | (code >> 6));
  out[1] = (char)(0x80 | (code & 0x3F));
  return 2;
}

/* Знак для сверки имён: строчная буква или цифра; прочее — 0 (пропустить). */
static uint16_t fold(uint16_t code) {
  if (code >= 'A' && code <= 'Z') return (uint16_t)(code - 'A' + 'a');
  if ((code >= 'a' && code <= 'z') || (code >= '0' && code <= '9')) return code;
  if (code >= 0x0410 && code <= 0x042F) return (uint16_t)(code + 0x20);   /* А…Я → а…я */
  if (code == 0x0401 || code == 0x0451) return 0x0435;                     /* ё → е */
  if (code >= 0x0430 && code <= 0x044F) return code;
  if (code >= 0xC0 && code <= 0x24F) return code;   /* латиница с диакритикой — как есть */
  return 0;
}

static size_t fold_cp866(const char* text, uint16_t* out, size_t max) {
  size_t n = 0;
  for (; *text != 0 && n < max; ++text) {
    const uint16_t c = fold(cp866_code((uint8_t)*text));
    if (c != 0) out[n++] = c;
  }
  return n;
}

/* «a» входит в «b» (или наоборот): «Robert Miles» и «Robert Miles feat. …». */
static int names_match(const uint16_t* a, size_t na, const uint16_t* b, size_t nb) {
  if (na < 2 || nb < 2) return 0;
  if (na > nb) {
    const uint16_t* t = a;
    a = b;
    b = t;
    const size_t tn = na;
    na = nb;
    nb = tn;
  }
  for (size_t at = 0; at + na <= nb; ++at) {
    if (memcmp(a, b + at, na * sizeof(uint16_t)) == 0) return 1;
  }
  return 0;
}

static size_t append(char* url, size_t used, size_t size, const char* text) {
  const size_t n = strlen(text);
  if (used + n >= size) return size;
  memcpy(url + used, text, n + 1);
  return used + n;
}

/* Текст CP866 → UTF-8 → в запрос (%XX для всего, кроме букв, цифр и -_.~). */
static size_t append_query(char* url, size_t used, size_t size, const char* text) {
  static const char hex[] = "0123456789ABCDEF";
  for (; *text != 0 && used < size; ++text) {
    char utf8[2];
    const size_t n = put_utf8(cp866_code((uint8_t)*text), utf8);
    for (size_t i = 0; i < n; ++i) {
      const unsigned char c = (unsigned char)utf8[i];
      const int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_' || c == '.' || c == '~';
      if (plain) {
        if (used + 1 >= size) return size;
        url[used++] = (char)c;
      } else {
        if (used + 3 >= size) return size;
        url[used++] = '%';
        url[used++] = hex[c >> 4];
        url[used++] = hex[c & 15];
      }
    }
  }
  if (used < size) url[used] = 0;
  return used;
}

size_t online_cover_search_url(const char* artist, const char* title, char* url, size_t size) {
  if (artist == NULL || title == NULL || artist[0] == 0 || title[0] == 0 || size == 0) return 0;
  size_t used = append(url, 0, size, SEARCH_HOST);
  used = append_query(url, used, size, artist);
  used = append(url, used, size, "%20");
  used = append_query(url, used, size, title);
  if (used >= size) {
    url[0] = 0;
    return 0;
  }
  return used;
}

/* --- разбор ответа ----------------------------------------------------------------- */

static const char* find(const char* from, const char* end, const char* key) {
  const size_t n = strlen(key);
  for (const char* p = from; p + n <= end; ++p) {
    if (memcmp(p, key, n) == 0) return p + n;
  }
  return NULL;
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Строка JSON с p (за кавычкой) до кавычки: коды знаков (\uXXXX, \/, \\ и UTF-8) — в out,
 * до max; возвращает конец строки (за кавычкой) или NULL. */
static const char* json_codes(const char* p, const char* end, uint16_t* out, size_t max, size_t* n) {
  *n = 0;
  while (p < end && *p != '"') {
    uint32_t code = 0;
    if (*p == '\\' && p + 1 < end) {
      ++p;
      if (*p == 'u' && p + 4 < end) {
        for (int i = 1; i <= 4; ++i) {
          const int d = hex_digit(p[i]);
          if (d < 0) return NULL;
          code = (code << 4) | (uint32_t)d;
        }
        p += 5;
      } else {
        code = (unsigned char)(*p == 'n' ? '\n' : *p == 't' ? '\t' : *p);
        ++p;
      }
    } else {
      const unsigned char c = (unsigned char)*p;
      if (c < 0x80) {
        code = c;
        ++p;
      } else if ((c & 0xE0) == 0xC0 && p + 1 < end) {
        code = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
        p += 2;
      } else if ((c & 0xF0) == 0xE0 && p + 2 < end) {
        code = ((uint32_t)(c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) |
               ((unsigned char)p[2] & 0x3F);
        p += 3;
      } else {
        code = ' ';
        ++p;
      }
    }
    if (out != NULL && *n < max) out[(*n)++] = (uint16_t)(code > 0xFFFF ? ' ' : code);
  }
  return p < end ? p + 1 : NULL;
}

/* Строка JSON адреса (ASCII, \/ → /) в url. */
static int json_url(const char* p, const char* end, char* url, size_t size) {
  size_t n = 0;
  while (p < end && *p != '"') {
    char c = *p++;
    if (c == '\\' && p < end) c = *p++;
    if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7E || n + 1 >= size) return 0;
    url[n++] = c;
  }
  url[n] = 0;
  return p < end && n > 0;
}

int online_cover_pick(const char* json, size_t length, const char* artist, char* url, size_t size) {
  if (json == NULL || artist == NULL || url == NULL || size == 0) return 0;
  url[0] = 0;
  uint16_t want[NAME_MAX];
  const size_t wanted = fold_cp866(artist, want, NAME_MAX);
  const char* end = json + length;
  const char* p = find(json, end, "\"data\":[");
  while (p != NULL) {
    /* Запись: …"artist":{…"name":"…"…},"album":{…"cover_big":"…"…}… */
    const char* a = find(p, end, "\"artist\":{");
    if (a == NULL) break;
    const char* name = find(a, end, "\"name\":\"");
    const char* album = find(a, end, "\"album\":{");
    if (name == NULL || album == NULL || name > album) break;
    uint16_t codes[NAME_MAX];
    size_t count = 0;
    const char* after = json_codes(name, end, codes, NAME_MAX, &count);
    if (after == NULL) break;
    uint16_t folded[NAME_MAX];
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
      const uint16_t c = fold(codes[i]);
      if (c != 0) folded[n++] = c;
    }
    const char* cover = find(album, end, "\"cover_big\":\"");
    const char* next = find(album, end, "\"artist\":{");
    if (cover != NULL && (next == NULL || cover < next) && names_match(want, wanted, folded, n) &&
        json_url(cover, end, url, size) && strstr(url, "/cover//") == NULL &&
        strncmp(url, "https://", 8) == 0) {
      return 1;
    }
    url[0] = 0;
    p = album;
  }
  return 0;
}

/* --- сеть -------------------------------------------------------------------------- */

static int net_ready(const zm_core_t* core) {
  return core->size >= offsetof(zm_core_t, net_close) + sizeof(core->net_close) &&
         core->net_open != NULL && core->net_read != NULL && core->net_close != NULL;
}

/* Тело ответа целиком в память (растёт до max). NULL — обрыв, пусто, отмена. */
static uint8_t* read_body(const zm_core_t* core, void* stream, size_t max, size_t* size,
                          const volatile int* cancel) {
  size_t capacity = 16384;
  const char* length = core->net_header != NULL ? core->net_header(stream, "Content-Length") : NULL;
  if (length != NULL) {
    /* sscanf ядро модулям не даёт */
    size_t declared = 0;
    while (*length == ' ') ++length;
    for (; *length >= '0' && *length <= '9' && declared <= max; ++length) {
      declared = declared * 10 + (size_t)(*length - '0');
    }
    if (declared > max) return NULL;
    if (declared > 0) capacity = declared + 1;
  }
  uint8_t* data = (uint8_t*)core->alloc(capacity, ZM_MEM_PSRAM);
  size_t used = 0;
  int idle = 0;
  while (data != NULL && !*cancel) {
    if (used + 1 >= capacity) {
      if (capacity >= max) break;
      const size_t grown = capacity * 2 < max ? capacity * 2 : max;
      uint8_t* bigger = (uint8_t*)core->realloc(data, grown);
      if (bigger == NULL) break;
      data = bigger;
      capacity = grown;
    }
    const int got = core->net_read(stream, data + used, (uint32_t)(capacity - 1 - used), READ_MS);
    if (got < 0) break;                     /* конец */
    if (got == 0) {
      if (++idle >= 3) break;               /* молчит 12 с */
      continue;
    }
    idle = 0;
    used += (size_t)got;
  }
  if (data != NULL && (used == 0 || *cancel)) {
    core->free(data);
    return NULL;
  }
  if (data != NULL) data[used] = 0;
  *size = used;
  return data;
}

uint8_t* online_cover_fetch(const zm_core_t* core, const char* artist, const char* title,
                            size_t* size, const volatile int* cancel) {
  *size = 0;
  if (!net_ready(core)) return NULL;
  static char url[640];
  if (online_cover_search_url(artist, title, url, sizeof(url)) == 0) return NULL;
  const uint32_t started = core->millis();
  int status = 0;
  void* stream = core->net_open(url, "Accept: application/json\r\n", OPEN_MS, &status);
  if (stream == NULL) {
    core->log("player: online cover: search failed (HTTP %d)", status);
    return NULL;
  }
  size_t json_size = 0;
  char* json = (char*)read_body(core, stream, JSON_MAX, &json_size, cancel);
  core->net_close(stream);
  if (json == NULL) {
    core->log("player: online cover: no search answer");
    return NULL;
  }
  const int found = online_cover_pick(json, json_size, artist, url, sizeof(url));
  core->free(json);
  if (!found) {
    core->log("player: online cover: not found (%u ms)", (unsigned)(core->millis() - started));
    return NULL;
  }
  stream = core->net_open(url, NULL, OPEN_MS, &status);
  if (stream == NULL) {
    core->log("player: online cover: picture failed (HTTP %d)", status);
    return NULL;
  }
  size_t picture_size = 0;
  uint8_t* picture = read_body(core, stream, ONLINE_COVER_MAX, &picture_size, cancel);
  core->net_close(stream);
  if (picture != NULL && (picture_size < 4 || picture[0] != 0xFF || picture[1] != 0xD8)) {
    core->free(picture);
    picture = NULL;
  }
  core->log("player: online cover: %s, %u KB in %u ms", picture != NULL ? "JPEG" : "not a JPEG",
            (unsigned)(picture_size / 1024), (unsigned)(core->millis() - started));
  *size = picture != NULL ? picture_size : 0;
  return picture;
}
