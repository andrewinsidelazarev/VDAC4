/* OGG.ELF — потоковый декодер плеера VDAC4: Ogg Vorbis (stb_vorbis v1.22,
 * components/stb_vorbis).
 *
 * Поток: байты берёт сам из очереди плеера (zm_host_t::input_read) — файл .OGG
 * (.OGA) с карты или поток интернет-радио (audio/ogg, application/ogg). Страницы
 * Ogg разбирает сам: начало потока — страница BOS с заголовком Vorbis; там
 * декодер открывается (заголовки целиком в буфере: буфер растёт, если они не
 * влезают — комментарии с обложкой бывают по сотне КБ). Цепочка потоков —
 * у интернет-радио каждая песня — новый логический поток со своими
 * заголовками: на следующей странице BOS декодер открывается заново, название,
 * исполнитель и альбом — из комментариев Vorbis новой песни. Обложка — тоже
 * из комментариев: METADATA_BLOCK_PICTURE (base64 блока картинки FLAC; лицевая,
 * тип 3, важнее прочих) или старый COVERART (base64 самой картинки) — плееру
 * (zm_host_t::cover), миниатюру сводит он. Ogg с Opus, FLAC или Speex внутри —
 * отказ с названием кодека (Opus is not supported).
 *
 * Звук — float на частоте потока: моно как есть, стерео как есть, больше
 * каналов — сведение в стерео (порядок каналов Vorbis).
 *
 * stb_vorbis получает заголовки только целиком (все три пакета в буфере): с
 * оборванным комментарием он читает длины из пустоты и пишет мимо памяти.
 *
 * Перемотка — как у MP3: место в файле считает плагин (время × битрейт),
 * декодер, открытый заново с этого места (AUDIO_FLUSH), берёт заголовки,
 * сохранённые с начала файла (заголовок и таблицы Vorbis, комментарий — пустой),
 * находит следующую страницу (сверяет её CRC) и продолжает с неё как обычно —
 * без stb_vorbis_flush_pushdata: тот пропускает найденную страницу целиком, а у
 * файлов ffmpeg страница — около секунды. Битрейт — средний по пройденному
 * звуку, пока его мало — номинальный из заголовка.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "stb_vorbis_vdac4.h"
#include "zm_app.h"
#include "zm_source.h"

#define INPUT_START (64u * 1024u)        /* буфер потока; растёт, пока заголовки не влезут */
#define INPUT_MAX (4u * 1024u * 1024u)
#define ARENA_START (256u * 1024u)       /* память stb_vorbis: таблицы и комментарии */
#define ARENA_MAX (4u * 1024u * 1024u)
#define SAVED_MAX (72u * 1024u)          /* заголовки для перемотки (без комментариев) */
#define PAGE_HEADER 27
#define MEASURE_MS 2000                  /* звука меньше — битрейт номинальный */

typedef struct {
  stb_vorbis* v;
  uint8_t* in;
  size_t in_size;
  size_t in_length;
  size_t bos_scan;            /* до этого места буфер уже просмотрен на BOS */
  uint8_t* arena;
  size_t arena_size;
  float** out;                /* кадр stb_vorbis: каналы отдельно */
  int out_samples;
  int out_used;
  int out_channels;
  int resume;                 /* открыт с середины файла: заголовки — сохранённые */
  uint32_t file_size;
  uint32_t consumed;          /* байтов потока пройдено */
  uint32_t audio_start;       /* где кончились заголовки */
  uint32_t rate;
  uint8_t channels;           /* каналов вывода: 1 или 2 */
  uint8_t source_channels;
  uint32_t nominal_kbps;
  uint64_t frames;            /* отдано кадров звука */
  uint64_t audio_bytes;       /* байтов потока, давших measured_frames */
  uint64_t measured_frames;
  uint32_t errors;
  const char* refused;        /* кодек, которого нет ("Opus"), — отказ */
  char title[64];
  char artist[64];
  char album[64];
  uint16_t year;
} ogg_t;

/* Заголовки файла для перемотки: страницы Ogg с заголовком и таблицами Vorbis,
 * комментарий пустой. В .bss модуля — уходят вместе с ним. */
static struct {
  uint8_t pages[SAVED_MAX];
  size_t length;
  uint32_t audio_start;
  uint32_t nominal_kbps;
  char title[64];
  char artist[64];
  char album[64];
  uint16_t year;
  uint64_t audio_bytes;       /* счёт битрейта по файлу (и после перемотки) */
  uint64_t measured_frames;
  uint32_t rate;
  int valid;
} saved;

static const zm_host_t* host;
static zm_source_t table;

static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void consume(ogg_t* o, size_t count) {
  if (count > o->in_length) {
    count = o->in_length;
  }
  memmove(o->in, o->in + count, o->in_length - count);
  o->in_length -= count;
  o->consumed += (uint32_t)count;
  o->bos_scan = o->bos_scan > count ? o->bos_scan - count : 0;
}

static void fill(ogg_t* o) {
  if (o->in_length < o->in_size) {
    o->in_length += host->input_read(o->in + o->in_length, o->in_size - o->in_length);
  }
}

/* Буфер вдвое (не больше max); 0 — некуда. Содержимое сохраняется. */
static int grow(uint8_t** buffer, size_t* size, size_t keep, size_t max) {
  if (*size >= max) {
    return 0;
  }
  const size_t bigger = *size * 2 > max ? max : *size * 2;
  uint8_t* next = (uint8_t*)host->alloc(bigger, ZM_MEM_PSRAM);
  if (next == NULL) {
    return 0;
  }
  memcpy(next, *buffer, keep);
  host->free(*buffer);
  *buffer = next;
  *size = bigger;
  return 1;
}

/* Что за кодек в первом пакете страницы BOS: "Vorbis", "Opus"…; NULL — не поток. */
static const char* bos_codec(const uint8_t* body, size_t length) {
  if (length >= 7 && memcmp(body, "\x01vorbis", 7) == 0) return "Vorbis";
  if (length >= 8 && memcmp(body, "OpusHead", 8) == 0) return "Opus";
  if (length >= 5 && memcmp(body, "\x7F" "FLAC", 5) == 0) return "FLAC";
  if (length >= 5 && memcmp(body, "Speex", 5) == 0) return "Speex";
  return NULL;
}

/* Длина заголовка страницы по месту at (0 — его ещё нет в буфере) или (size_t)-1 —
 * это не страница. */
static size_t page_header(const ogg_t* o, size_t at) {
  if (at + PAGE_HEADER > o->in_length) {
    return 0;
  }
  const uint8_t* p = o->in + at;
  if (memcmp(p, "OggS", 4) != 0 || p[4] != 0 || (p[5] & ~0x07) != 0) {
    return (size_t)-1;
  }
  const size_t header = PAGE_HEADER + p[26];
  return at + header <= o->in_length ? header : 0;
}

/* Следующая страница BOS известного кодека с места from: её место или -1. */
static long find_bos(ogg_t* o, size_t from) {
  size_t at = from > o->bos_scan ? from : o->bos_scan;
  for (; at + PAGE_HEADER + 8 <= o->in_length; ++at) {
    if (o->in[at] != 'O' || o->in[at + 1] != 'g' || (o->in[at + 5] & 0x02) == 0) {
      continue;
    }
    const size_t header = page_header(o, at);
    if (header == (size_t)-1) {
      continue;
    }
    if (header == 0 || at + header + 8 > o->in_length) {
      break;                        /* заголовок или начало пакета ещё в пути */
    }
    if (bos_codec(o->in + at + header, o->in_length - at - header) != NULL) {
      o->bos_scan = at;
      return (long)at;
    }
  }
  o->bos_scan = at;
  return -1;
}

/* Длина тела страницы по таблице сегментов (заголовок страницы — в буфере). */
static size_t page_body(const uint8_t* page) {
  size_t body = 0;
  for (size_t s = 0; s < page[26]; ++s) {
    body += page[PAGE_HEADER + s];
  }
  return body;
}

/* Три заголовочных пакета Vorbis целиком в буфере с места 0? Длина страниц с
 * ними; 0 — ещё нет (или это не страницы). */
static size_t headers_length(const ogg_t* o) {
  size_t at = 0;
  int packets = 0;
  while (packets < 3) {
    const size_t header = page_header(o, at);
    if (header == 0 || header == (size_t)-1) {
      return 0;
    }
    const uint8_t* page = o->in + at;
    const size_t body = page_body(page);
    if (at + header + body > o->in_length) {
      return 0;
    }
    for (size_t s = 0; s < page[26]; ++s) {
      if (page[PAGE_HEADER + s] < 255) {
        ++packets;
      }
    }
    at += header + body;
  }
  return at;
}

static uint32_t crc_step(uint32_t crc, uint8_t byte) {
  crc ^= (uint32_t)byte << 24;
  for (int bit = 0; bit < 8; ++bit) {
    crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
  }
  return crc;
}

/* CRC страницы длиной length (поле CRC считается нулями) верна? */
static int page_crc_ok(const uint8_t* page, size_t length) {
  uint32_t crc = 0;
  for (size_t i = 0; i < length; ++i) {
    crc = crc_step(crc, (i >= 22 && i < 26) ? 0 : page[i]);
  }
  return crc == le32(page + 22);
}

/* --- заголовки для перемотки ----------------------------------------------------- */

/* Страница Ogg из пакетов (каждый — целиком на ней). Возвращает длину, 0 — не влезла. */
static size_t write_page(uint8_t* out, size_t room, uint8_t flags, uint32_t serial,
                         uint32_t sequence, const uint8_t* const* packets,
                         const size_t* lengths, int count) {
  size_t segments = 0;
  size_t body = 0;
  for (int i = 0; i < count; ++i) {
    segments += lengths[i] / 255 + 1;
    body += lengths[i];
  }
  const size_t total = PAGE_HEADER + segments + body;
  if (segments > 255 || total > room) {
    return 0;
  }
  memcpy(out, "OggS", 4);
  out[4] = 0;
  out[5] = flags;
  memset(out + 6, 0, 8);            /* granule: у страниц заголовков 0 */
  put32(out + 14, serial);
  put32(out + 18, sequence);
  put32(out + 22, 0);
  out[26] = (uint8_t)segments;
  uint8_t* lacing = out + PAGE_HEADER;
  uint8_t* data = lacing + segments;
  for (int i = 0; i < count; ++i) {
    for (size_t left = lengths[i]; ; left -= 255) {
      *lacing++ = (uint8_t)(left >= 255 ? 255 : left);
      if (left < 255) {
        break;
      }
    }
    memcpy(data, packets[i], lengths[i]);
    data += lengths[i];
  }
  uint32_t crc = 0;
  for (size_t i = 0; i < total; ++i) {
    crc = crc_step(crc, out[i]);
  }
  put32(out + 22, crc);
  return total;
}

/* Заголовки потока (in[0..used) — его первые страницы) — в saved: заголовок
 * Vorbis, пустой комментарий, таблицы. Таблицы больше страницы — не сохраняются
 * (перемотка тогда не звучит до конца файла; таких файлов почти нет). */
static void save_headers(ogg_t* o, size_t used) {
  saved.valid = 0;
  uint8_t* scratch = (uint8_t*)host->alloc(used, ZM_MEM_PSRAM);   /* пакеты подряд */
  if (scratch == NULL) {
    return;
  }
  size_t start[3] = {0, 0, 0};
  size_t length[3] = {0, 0, 0};
  size_t filled = 0;
  int index = 0;
  uint32_t serial = 0;
  size_t at = 0;
  while (index < 3) {
    if (at + PAGE_HEADER > used || memcmp(o->in + at, "OggS", 4) != 0) {
      break;
    }
    const uint8_t* page = o->in + at;
    const size_t count = page[26];
    const uint8_t* lacing = page + PAGE_HEADER;
    size_t body = at + PAGE_HEADER + count;
    serial = le32(page + 14);
    size_t page_body = 0;
    for (size_t s = 0; s < count; ++s) {
      page_body += lacing[s];
    }
    for (size_t s = 0; s < count && index < 3; ++s) {
      if (body + lacing[s] > used) {
        break;                      /* пакет оборван: заголовки не целиком */
      }
      memcpy(scratch + filled, o->in + body, lacing[s]);
      filled += lacing[s];
      length[index] += lacing[s];
      body += lacing[s];
      if (lacing[s] < 255 && ++index < 3) {
        start[index] = filled;
      }
    }
    at += PAGE_HEADER + count + page_body;   /* следующая страница */
  }
  if (index == 3 && length[0] >= 30 && length[2] != 0) {
    /* Пустой комментарий: [3]"vorbis"[длина имени кодировщика = 0][число = 0][1]. */
    static const uint8_t comment[] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const uint8_t* first[1] = {scratch + start[0]};
    const size_t first_length[1] = {length[0]};
    const uint8_t* second[2] = {comment, scratch + start[2]};
    const size_t second_length[2] = {sizeof(comment), length[2]};
    const size_t one = write_page(saved.pages, SAVED_MAX, 0x02, serial, 0, first, first_length, 1);
    const size_t two = one != 0 ? write_page(saved.pages + one, SAVED_MAX - one, 0x00, serial, 1,
                                             second, second_length, 2)
                                : 0;
    if (two != 0) {
      saved.length = one + two;
      saved.audio_start = o->audio_start;
      saved.nominal_kbps = o->nominal_kbps;
      memcpy(saved.title, o->title, sizeof(saved.title));
      memcpy(saved.artist, o->artist, sizeof(saved.artist));
      memcpy(saved.album, o->album, sizeof(saved.album));
      saved.year = o->year;
      saved.audio_bytes = 0;
      saved.measured_frames = 0;
      saved.rate = o->rate;
      saved.valid = 1;
    }
  }
  host->free(scratch);
}

/* Перемотка: метки, битрейт и начало звука — сохранённые с начала файла. */
static void resume_tags(ogg_t* o) {
  o->audio_start = saved.audio_start;
  o->nominal_kbps = saved.nominal_kbps;
  o->audio_bytes = saved.audio_bytes;
  o->measured_frames = saved.measured_frames;
  memcpy(o->title, saved.title, sizeof(o->title));
  memcpy(o->artist, saved.artist, sizeof(o->artist));
  memcpy(o->album, saved.album, sizeof(o->album));
  o->year = saved.year;
}

/* --- поток Vorbis ---------------------------------------------------------------- */

static void copy_value(char* to, size_t size, const char* value) {
  snprintf(to, size, "%s", value);
}

/* Комментарии Vorbis: TITLE, ARTIST, ALBUM, DATE (год — первые четыре цифры). */
static void read_comments(ogg_t* o) {
  o->title[0] = o->artist[0] = o->album[0] = 0;
  o->year = 0;
  const stb_vorbis_comment c = stb_vorbis_get_comment(o->v);
  for (int i = 0; i < c.comment_list_length; ++i) {
    const char* entry = c.comment_list[i];
    const char* value = strchr(entry, '=');
    if (value == NULL) {
      continue;
    }
    const size_t key = (size_t)(value - entry);
    ++value;
    if (key == 5 && strncasecmp(entry, "TITLE", 5) == 0) {
      copy_value(o->title, sizeof(o->title), value);
    } else if (key == 6 && strncasecmp(entry, "ARTIST", 6) == 0 && o->artist[0] == 0) {
      copy_value(o->artist, sizeof(o->artist), value);
    } else if (key == 5 && strncasecmp(entry, "ALBUM", 5) == 0) {
      copy_value(o->album, sizeof(o->album), value);
    } else if (key == 4 && strncasecmp(entry, "DATE", 4) == 0) {
      int year = 0;
      for (int d = 0; d < 4 && value[d] >= '0' && value[d] <= '9'; ++d) {
        year = year * 10 + (value[d] - '0');
      }
      o->year = year >= 1000 ? (uint16_t)year : 0;
    }
  }
}

/* --- обложка из комментариев ----------------------------------------------------- */

static int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* base64 text → out (места — не меньше 3/4 длины text); прочие знаки (переводы
 * строк, '=') пропускаются. Возвращает число байтов. */
static size_t base64_decode(const char* text, size_t length, uint8_t* out, size_t room) {
  uint32_t bits = 0;
  int count = 0;
  size_t done = 0;
  for (size_t i = 0; i < length && done < room; ++i) {
    const int value = base64_value(text[i]);
    if (value < 0) {
      continue;
    }
    bits = (bits << 6) | (uint32_t)value;
    count += 6;
    if (count >= 8) {
      count -= 8;
      out[done++] = (uint8_t)(bits >> count);
    }
  }
  return done;
}

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Обложка из комментариев — плееру. METADATA_BLOCK_PICTURE: [тип][MIME][описание]
 * [ширина][высота][глубина][цвета][длина][картинка], числа — 32 бита big-endian. */
static void find_cover(ogg_t* o) {
  if (host->cover == NULL) {
    return;
  }
  const stb_vorbis_comment c = stb_vorbis_get_comment(o->v);
  const char* best = NULL;
  int raw = 0;                      /* COVERART: сама картинка, без блока FLAC */
  for (int i = 0; i < c.comment_list_length; ++i) {
    const char* entry = c.comment_list[i];
    if (strncasecmp(entry, "METADATA_BLOCK_PICTURE=", 23) == 0) {
      uint8_t head[6];
      const char* value = entry + 23;
      const size_t got = base64_decode(value, strnlen(value, 8), head, sizeof(head));
      const int front = got >= 4 && be32(head) == 3;
      if (best == NULL || raw || front) {
        best = value;
        raw = 0;
        if (front) {
          break;                    /* лицевая — она и нужна */
        }
      }
    } else if (best == NULL && strncasecmp(entry, "COVERART=", 9) == 0) {
      best = entry + 9;
      raw = 1;
    }
  }
  if (best == NULL) {
    return;
  }
  const size_t length = strlen(best);
  const size_t room = length / 4 * 3 + 3;
  uint8_t* block = (uint8_t*)host->alloc(room, ZM_MEM_PSRAM);
  if (block == NULL) {
    return;
  }
  const size_t size = base64_decode(best, length, block, room);
  const uint8_t* image = block;
  size_t image_size = size;
  if (!raw) {
    /* Поля по очереди; любая длина за концом блока — картинки нет. */
    image = NULL;
    size_t at = 4;                  /* за типом картинки */
    for (int field = 0; field < 2; ++field) {   /* MIME, описание */
      if (at > size || size - at < 4 || be32(block + at) > size - at - 4) {
        at = size;
        break;
      }
      at += 4 + be32(block + at);
    }
    if (size >= at && size - at >= 20) {
      at += 16;                     /* ширина, высота, глубина, цвета */
      const uint32_t bytes = be32(block + at);
      if (bytes <= size - at - 4) {
        image_size = bytes;
        image = block + at + 4;
      }
    }
  }
  if (image != NULL && image_size != 0) {
    host->cover(image, image_size);
  }
  host->free(block);
}

/* stb_vorbis — по заголовкам data[0..length). 1 — открыт (*used — их длина), 0 —
 * заголовки не целиком, −1 — не вышло. Не хватает памяти — арена растёт. */
static int open_vorbis(ogg_t* o, const uint8_t* data, size_t length, int* used) {
  for (;;) {
    stb_vorbis_alloc alloc;
    alloc.alloc_buffer = (char*)o->arena;
    alloc.alloc_buffer_length_in_bytes = (int)o->arena_size;
    int error = 0;
    o->v = stb_vorbis_open_pushdata(data, (int)length, used, &error, &alloc);
    if (o->v != NULL) {
      const stb_vorbis_info info = stb_vorbis_get_info(o->v);
      o->rate = info.sample_rate;
      o->source_channels = (uint8_t)info.channels;
      o->channels = info.channels == 1 ? 1 : 2;
      return 1;
    }
    if (error == VORBIS_need_more_data) {
      return 0;
    }
    if (error == VORBIS_outofmem && grow(&o->arena, &o->arena_size, 0, ARENA_MAX)) {
      continue;
    }
    host->log("ogg: Vorbis headers refused (%d)", error);
    return -1;
  }
}

static void close_vorbis(ogg_t* o) {
  if (o->v != NULL) {
    stb_vorbis_close(o->v);
    o->v = NULL;
  }
  o->out_samples = o->out_used = 0;
}

/* Начало потока: страница BOS (или, после перемотки, любая — с сохранёнными
 * заголовками). 1 — декодер открыт, 0 — нужен ввод, −1 — конец или отказ. */
static int start_stream(ogg_t* o, int ended) {
  for (;;) {
    size_t at = 0;
    while (at + 4 <= o->in_length && memcmp(o->in + at, "OggS", 4) != 0) {
      ++at;
    }
    if (at + 4 > o->in_length) {
      if (o->in_length > 3) {
        consume(o, o->in_length - 3);   /* хвост может быть началом «OggS» */
      }
      return ended ? -1 : 0;
    }
    consume(o, at);
    const size_t header = page_header(o, 0);
    if (header == (size_t)-1) {
      consume(o, 1);
      continue;
    }
    if (header == 0) {
      return ended ? -1 : 0;
    }
    if ((o->in[5] & 0x02) == 0) {
      if (o->resume && saved.valid) {
        /* С середины файла: страница целиком и с верной CRC (иначе «OggS» попался
         * внутри данных), заголовки — сохранённые; stb продолжает с этой страницы
         * (пакет, начатый на прошлой, он отбросит сам). */
        const size_t length = header + page_body(o->in);
        if (length > o->in_length) {
          if (ended) {
            return -1;
          }
          return 0;                 /* страница ещё в пути: буфер её вмещает */
        }
        if (!page_crc_ok(o->in, length)) {
          consume(o, 1);
          continue;
        }
        int used = 0;
        if (open_vorbis(o, saved.pages, saved.length, &used) != 1) {
          return -1;
        }
        resume_tags(o);
        return 1;
      }
      /* Не начало потока, а заголовков нет: страницу — прочь. */
      const size_t body = page_body(o->in);
      if (header + body > o->in_length) {
        if (ended) {
          return -1;
        }
        consume(o, 4);              /* дальше — снова поиск «OggS» */
        continue;
      }
      consume(o, header + body);
      continue;
    }
    if (o->in_length < header + 8) {
      return ended ? -1 : 0;
    }
    const char* codec = bos_codec(o->in + header, o->in_length - header);
    if (codec == NULL) {
      consume(o, 4);                /* поток не звука (скелет и т. п.) — искать дальше */
      continue;
    }
    if (strcmp(codec, "Vorbis") != 0) {
      o->refused = codec;
      host->log("ogg: %s is not supported", codec);
      return -1;
    }
    const uint8_t* ident = o->in + header;
    if (o->in_length >= header + 24) {
      const int32_t nominal = (int32_t)le32(ident + 20);
      o->nominal_kbps = nominal > 0 ? (uint32_t)nominal / 1000u : 0;
    }
    /* Заголовки — только целиком: буфер растёт, пока они не влезут. */
    if (headers_length(o) == 0) {
      if (ended) {
        return -1;
      }
      if (o->in_length == o->in_size && !grow(&o->in, &o->in_size, o->in_length, INPUT_MAX)) {
        host->log("ogg: Vorbis headers longer than %u bytes", (unsigned)INPUT_MAX);
        return -1;
      }
      fill(o);
      if (host->input_available() == 0 && o->in_length < o->in_size) {
        return 0;
      }
      continue;
    }
    int used = 0;
    const int opened = open_vorbis(o, o->in, o->in_length, &used);
    if (opened == 0) {
      o->errors++;                  /* заголовки целиком, а stb хочет ещё: не Vorbis */
      consume(o, 4);
      continue;
    }
    if (opened < 0) {
      o->errors++;
      consume(o, 4);
      continue;
    }
    read_comments(o);
    if (!o->resume) {
      find_cover(o);                /* после перемотки обложка уже на экране */
    }
    o->audio_start = o->consumed + (uint32_t)used;
    o->audio_bytes = 0;
    o->measured_frames = 0;
    if (!o->resume) {
      save_headers(o, (size_t)used);
    }
    consume(o, (size_t)used);
    o->bos_scan = 0;
    return 1;
  }
}

/* Следующий кадр звука. 1 — есть, 0 — нужен ввод, −1 — конец. */
static int decode_packet(ogg_t* o) {
  for (;;) {
    fill(o);
    const int ended = host->input_ended() && host->input_available() == 0;
    if (o->v == NULL) {
      const int started = start_stream(o, ended);
      if (started <= 0) {
        return started;
      }
      continue;
    }
    /* Новая песня цепочки: прежнему потоку — только байты до её страницы BOS. */
    const long bos = find_bos(o, 0);
    const size_t limit = bos >= 0 ? (size_t)bos : o->in_length;
    int channels = 0;
    int samples = 0;
    float** output = NULL;
    const int used = limit != 0 ? stb_vorbis_decode_frame_pushdata(o->v, o->in, (int)limit,
                                                                    &channels, &output, &samples)
                                : 0;
    if (used == 0 && samples == 0) {
      if (bos >= 0) {
        consume(o, limit);          /* хвост прежнего потока */
        close_vorbis(o);
        o->resume = 0;
        continue;                   /* start_stream откроет новый с его заголовков */
      }
      if (o->in_length == o->in_size) {
        if (!grow(&o->in, &o->in_size, o->in_length, INPUT_MAX)) {
          o->errors++;              /* пакет длиннее буфера: прочь, искать страницу */
          consume(o, o->in_length);
          stb_vorbis_flush_pushdata(o->v);
        }
        continue;
      }
      if (ended) {
        return -1;
      }
      if (host->input_available() == 0) {
        return 0;
      }
      continue;
    }
    consume(o, (size_t)used);
    if (samples > 0 && output != NULL && channels > 0) {
      o->out = output;
      o->out_samples = samples;
      o->out_used = 0;
      o->out_channels = channels;
      o->audio_bytes += (uint64_t)used;
      o->measured_frames += (uint64_t)samples;
      if (saved.valid) {
        saved.audio_bytes = o->audio_bytes;      /* для длительности после перемотки */
        saved.measured_frames = o->measured_frames;
      }
      return 1;
    }
    if (samples == 0) {
      o->audio_bytes += (uint64_t)used;
    }
  }
}

static int probe(const uint8_t* head, size_t length) {
  return length >= 4 && memcmp(head, "OggS", 4) == 0 ? 90 : 0;
}

static void* open_source(uint32_t file_size, const uint8_t* file) {
  (void)file;
  ogg_t* o = (ogg_t*)host->alloc(sizeof(ogg_t), ZM_MEM_PSRAM);
  if (o == NULL) {
    return NULL;
  }
  memset(o, 0, sizeof(*o));
  o->in = (uint8_t*)host->alloc(INPUT_START, ZM_MEM_PSRAM);
  o->arena = (uint8_t*)host->alloc(ARENA_START, ZM_MEM_PSRAM);
  if (o->in == NULL || o->arena == NULL) {
    if (o->in != NULL) host->free(o->in);
    if (o->arena != NULL) host->free(o->arena);
    host->free(o);
    return NULL;
  }
  o->in_size = INPUT_START;
  o->arena_size = ARENA_START;
  o->file_size = file_size;
  const uint32_t offset = host->size >= offsetof(zm_host_t, stream_offset) + sizeof(uint32_t)
                              ? host->stream_offset
                              : 0;
  o->resume = offset != 0 && offset != UINT32_MAX;
  o->consumed = o->resume ? offset : 0;
  if (!o->resume) {
    saved.valid = 0;                /* новый файл (или поток радио) с начала */
  } else if (saved.valid) {
    /* После перемотки метки, битрейт и длительность — сразу, до первой страницы. */
    resume_tags(o);
  }
  return o;
}

/* Сведение в стерео: вес каналов (порядок Vorbis) в левом и правом. */
static void mix_weights(int channels, const float** left, const float** right) {
  static const float l3[] = {1, 0.7071f, 0}, r3[] = {0, 0.7071f, 1};
  static const float l4[] = {1, 0, 0.7071f, 0}, r4[] = {0, 1, 0, 0.7071f};
  static const float l5[] = {1, 0.7071f, 0, 0.7071f, 0}, r5[] = {0, 0.7071f, 1, 0, 0.7071f};
  static const float l6[] = {1, 0.7071f, 0, 0.7071f, 0, 0}, r6[] = {0, 0.7071f, 1, 0, 0.7071f, 0};
  static const float l7[] = {1, 0.7071f, 0, 0.7071f, 0, 0.5f, 0};
  static const float r7[] = {0, 0.7071f, 1, 0, 0.7071f, 0.5f, 0};
  static const float l8[] = {1, 0.7071f, 0, 0.7071f, 0, 0.7071f, 0, 0};
  static const float r8[] = {0, 0.7071f, 1, 0, 0.7071f, 0, 0.7071f, 0};
  static const float* const ls[] = {l3, l4, l5, l6, l7, l8};
  static const float* const rs[] = {r3, r4, r5, r6, r7, r8};
  *left = ls[channels - 3];
  *right = rs[channels - 3];
}

static int render(void* instance, float* out, uint32_t max_frames, uint32_t* frames) {
  ogg_t* o = (ogg_t*)instance;
  *frames = 0;
  if (o->refused != NULL) {
    return ZM_RENDER_ERROR;
  }
  if (o->out_used >= o->out_samples) {
    const int got = decode_packet(o);
    if (got <= 0) {
      if (o->refused != NULL) {
        return ZM_RENDER_ERROR;
      }
      return got == 0 ? ZM_RENDER_NEED_INPUT : ZM_RENDER_END;
    }
  }
  uint32_t count = (uint32_t)(o->out_samples - o->out_used);
  if (count > max_frames) {
    count = max_frames;
  }
  const int at = o->out_used;
  const int channels = o->out_channels;
  if (channels == 1) {
    memcpy(out, o->out[0] + at, count * sizeof(float));
  } else if (channels == 2) {
    const float* left = o->out[0] + at;
    const float* right = o->out[1] + at;
    for (uint32_t i = 0; i < count; ++i) {
      out[2 * i] = left[i];
      out[2 * i + 1] = right[i];
    }
  } else {
    const float* wl;
    const float* wr;
    mix_weights(channels > 8 ? 8 : channels, &wl, &wr);
    float scale = 0;
    for (int c = 0; c < channels && c < 8; ++c) {
      scale += wl[c];
    }
    scale = 1.0f / scale;
    for (uint32_t i = 0; i < count; ++i) {
      float l = 0, r = 0;
      for (int c = 0; c < channels && c < 8; ++c) {
        const float s = o->out[c][at + (int)i];
        l += wl[c] * s;
        r += wr[c] * s;
      }
      out[2 * i] = l * scale;
      out[2 * i + 1] = r * scale;
    }
  }
  o->out_used += (int)count;
  o->frames += count;
  *frames = count;
  return ZM_RENDER_OK;
}

/* Перематывает плагин (AUDIO_FLUSH: место в файле — время × битрейт). */
static int seek(void* instance, int32_t delta_ms, uint32_t* file_offset) {
  (void)instance;
  (void)delta_ms;
  (void)file_offset;
  return ZM_ERROR;
}

static uint32_t current_kbps(const ogg_t* o) {
  const uint32_t rate = o->rate != 0 ? o->rate : saved.rate;
  if (rate != 0 && o->measured_frames * 1000u >= (uint64_t)rate * MEASURE_MS) {
    return (uint32_t)(o->audio_bytes * 8u * rate / o->measured_frames / 1000u);
  }
  return o->nominal_kbps;
}

static void info(const void* instance, zm_info_t* out) {
  const ogg_t* o = (const ogg_t*)instance;
  memset(out, 0, sizeof(*out));
  if (o->refused != NULL) {
    snprintf(out->format, sizeof(out->format), "Ogg %s", o->refused);
    snprintf(out->title, sizeof(out->title), "%s is not supported", o->refused);
    return;
  }
  const uint32_t kbps = current_kbps(o);
  if (kbps != 0) {
    snprintf(out->format, sizeof(out->format), "Vorbis %u kbps", (unsigned)kbps);
  } else {
    snprintf(out->format, sizeof(out->format), "Vorbis");
  }
  out->rate = o->rate;
  out->channels = o->channels;
  out->voices = o->source_channels;
  out->bitrate = (uint16_t)kbps;
  out->errors = o->errors;
  memcpy(out->title, o->title, sizeof(out->title));
  memcpy(out->artist, o->artist, sizeof(out->artist));
  memcpy(out->album, o->album, sizeof(out->album));
  out->year = o->year;
  if (o->rate != 0) {
    out->position_ms = (uint32_t)(o->frames * 1000 / o->rate);
  }
  /* Длительность — и до первой страницы после перемотки (битрейт — сохранённый). */
  if (kbps != 0 && o->file_size > o->audio_start) {
    out->duration_ms = (uint32_t)((uint64_t)(o->file_size - o->audio_start) * 8u / kbps);
  }
}

static void close_source(void* instance) {
  ogg_t* o = (ogg_t*)instance;
  close_vorbis(o);
  host->free(o->in);
  host->free(o->arena);
  host->free(o);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    return 0;
  }
  host = (const zm_host_t*)argv[1];
  table.size = sizeof(table);
  table.abi_major = ZM_SOURCE_ABI_MAJOR;
  table.abi_minor = ZM_SOURCE_ABI_MINOR;
  table.name = "ogg";
  table.version = "1.0";
  table.extensions = "OGG OGA";
  table.input = ZM_INPUT_STREAM;
  table.keeps_file = 0;
  table.seek_unit = ZM_SEEK_MS;
  table.resources = NULL;
  table.probe = probe;
  table.open = open_source;
  table.render = render;
  table.seek = seek;
  table.info = info;
  table.levels = NULL;
  table.close = close_source;
  *(const zm_source_t**)argv[2] = &table;
  return ZM_SOURCE_ABI_MAJOR;
}
