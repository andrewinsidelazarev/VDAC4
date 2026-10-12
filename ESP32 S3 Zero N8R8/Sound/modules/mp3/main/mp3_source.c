/* MP3.ELF — потоковый декодер плеера VDAC4: MP3 (Helix: MPEG-1/2/2.5 Layer III)
 * и WAV.
 *
 * Поток: байты берёт сам из очереди плеера (zm_host_t::input_read). Что в нём,
 * видно по началу: «RIFF....WAVE» — WAV, иначе MP3.
 *
 * MP3: метку ID3v2 в начале пропускает, кадр ищет по слову синхронизации, кадр
 * с ошибкой считает и пропускает. Отдаёт float на частоте файла, моно — как
 * есть. Заголовок Xing/Info (или VBRI) первого кадра даёт длительность,
 * средний битрейт VBR и оглавление для перемотки; разбирается, когда кадр весь
 * в буфере (длинную метку ID3 плагин пропускает сам, и первым блоком приходит
 * хвост её сектора — бывает в несколько байтов), сам кадр звука не несёт и
 * пропускается. После перемотки первого кадра в потоке нет: заголовок — тот,
 * что разобран с начала этого же файла.
 *
 * WAV: PCM 8, 16, 24, 32 бита, float 32 и 64, A-law и µ-law, в том числе
 * WAVE_FORMAT_EXTENSIBLE; больше двух каналов — сведение в стерео; частота
 * выше 48 кГц — прореживание в 2, 4 или 8 раз (среднее кадров группы): частота
 * в записи состояния для Z80 — 16 бит, а 44,1 кГц делает ядро. Метки — из
 * LIST INFO перед данными. После перемотки (AUDIO_FLUSH) заголовка в потоке
 * нет: формат — от начала этого же файла, а по месту потока в файле
 * (zm_host_t::stream_offset) декодер встаёт на начало отсчёта.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mp3dec.h"
#include "zm_app.h"
#include "zm_source.h"

#define INPUT_BYTES 4096
#define MAX_SAMPLES (1152 * 2)

enum { MODE_UNKNOWN = 0, MODE_MP3, MODE_WAV };
enum { WAV_PCM = 1, WAV_FLOAT = 3, WAV_ALAW = 6, WAV_MULAW = 7, WAV_EXTENSIBLE = 0xFFFE };

/* Формат WAV: из «fmt » и «data». */
typedef struct {
  uint16_t format;          /* WAV_PCM, WAV_FLOAT, WAV_ALAW, WAV_MULAW */
  uint16_t channels;
  uint32_t rate;
  uint32_t byte_rate;
  uint16_t block;           /* байт на кадр всех каналов */
  uint16_t bits;
  uint32_t data_start;      /* место данных в файле */
  uint32_t data_size;       /* 0 — до конца файла */
  int valid;
} wav_format_t;

/* Формат WAV, который открывался последним: после перемотки в потоке нет
 * заголовка — только данные с нового места. С начала файла (место 0) — сброс. */
static wav_format_t wav_last;

/* Заголовок Xing/VBRI MP3, что открывался последним (то же для перемотки). */
static struct {
  uint32_t frames;
  uint32_t bytes;
  uint8_t toc[100];
  int have_toc;
  int valid;
} xing_last;

typedef struct {
  HMP3Decoder decoder;
  unsigned char in[INPUT_BYTES];
  size_t in_length;
  short pcm[MAX_SAMPLES];
  size_t pcm_frames;        /* кадров звука в pcm */
  size_t pcm_used;          /* из них уже отдано */
  int id3_checked;
  size_t skip;              /* сколько байтов метки ещё пропустить */
  uint32_t file_size;
  uint32_t audio_start;     /* где в файле начался звук (после метки) */
  uint32_t consumed;        /* байтов файла пройдено */
  uint32_t rate;
  uint8_t channels;
  uint16_t bitrate;         /* кбит/с последнего кадра */
  uint64_t frames;          /* отдано кадров звука */
  uint32_t errors;
  /* Xing/Info */
  uint32_t xing_frames;
  uint32_t xing_bytes;
  uint8_t xing_toc[100];
  int have_toc;
  int first_frame;
  /* WAV */
  int mode;
  uint32_t stream_offset;   /* место первого байта потока в файле */
  int wav_header;           /* 0 — RIFF, 1 — блоки, 2 — данные */
  wav_format_t wav;
  uint32_t wav_left;        /* байтов данных осталось (если длина известна) */
  int wav_sized;
  uint8_t decimate;         /* кадров файла на кадр вывода: 1, 2, 4, 8 */
  char title[64];
  char artist[64];
  char album[64];
  uint16_t year;
} mp3_t;

static const zm_host_t* host;
static zm_source_t table;

static void consume(mp3_t* m, size_t count) {
  if (count > m->in_length) {
    count = m->in_length;
  }
  memmove(m->in, m->in + count, m->in_length - count);
  m->in_length -= count;
  m->consumed += (uint32_t)count;
}

static uint32_t be32(const unsigned char* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Длина кадра Layer III по его заголовку, байт; 0 — не заголовок. */
static size_t frame_bytes(const unsigned char* h) {
  static const uint16_t kbps[2][16] = {
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},        /* MPEG-2, 2.5 */
      {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}};   /* MPEG-1 */
  static const uint16_t rates[4][3] = {
      {11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0 || (h[1] & 0x06) != 0x02) {
    return 0;
  }
  const int version = (h[1] >> 3) & 3;
  const int index = h[2] >> 4;
  const int rate = (h[2] >> 2) & 3;
  if (version == 1 || index == 0 || index == 15 || rate == 3) {
    return 0;
  }
  const uint32_t bits = kbps[version == 3][index] * 1000u;
  return (size_t)((version == 3 ? 144u : 72u) * bits / rates[version][rate] + ((h[2] >> 1) & 1));
}

/* Заголовок Xing/Info или VBRI в первом кадре (кадр целиком, length байт):
 * число кадров, байтов и оглавление. 1 — заголовок есть (кадр без звука). */
static int parse_xing(mp3_t* m, const unsigned char* frame, size_t length) {
  /* Смещение — за побочной информацией: MPEG-1 32/17, MPEG-2 17/9 байт. */
  const int mpeg1 = (frame[1] & 0x08) != 0;
  const int mono = (frame[3] & 0xC0) == 0xC0;
  const size_t offset = 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
  if (length >= 36 + 18 && memcmp(frame + 36, "VBRI", 4) == 0) {
    m->xing_bytes = be32(frame + 36 + 10);
    m->xing_frames = be32(frame + 36 + 14);
    return 1;
  }
  if (length < offset + 8) {
    return 0;
  }
  const unsigned char* x = frame + offset;
  if (memcmp(x, "Xing", 4) != 0 && memcmp(x, "Info", 4) != 0) {
    return 0;
  }
  const uint32_t flags = be32(x + 4);
  size_t at = offset + 8;
  if ((flags & 1) && at + 4 <= length) {
    m->xing_frames = be32(frame + at);
    at += 4;
  }
  if ((flags & 2) && at + 4 <= length) {
    m->xing_bytes = be32(frame + at);
    at += 4;
  }
  if ((flags & 4) && at + 100 <= length) {
    memcpy(m->xing_toc, frame + at, 100);
    m->have_toc = 1;
  }
  return 1;
}

static uint32_t le32(const unsigned char* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int probe(const unsigned char* head, size_t length) {
  if (length >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) {
    return 100;
  }
  if (length >= 3 && memcmp(head, "ID3", 3) == 0) {
    return 80;
  }
  if (length >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0 && (head[1] & 0x06) == 0x02) {
    return 60;                       /* слово синхронизации и Layer III */
  }
  return 0;
}

static void* open_source(uint32_t file_size, const unsigned char* file) {
  (void)file;
  mp3_t* m = (mp3_t*)host->alloc(sizeof(mp3_t), ZM_MEM_INTERNAL);
  if (m == NULL) {
    return NULL;
  }
  memset(m, 0, sizeof(*m));
  m->decoder = MP3InitDecoder();
  if (m->decoder == NULL) {
    host->free(m);
    return NULL;
  }
  m->file_size = file_size;
  m->first_frame = 1;
  m->stream_offset = 0;
  if (host->size >= offsetof(zm_host_t, stream_offset) + sizeof(host->stream_offset)) {
    m->stream_offset = host->stream_offset;
  }
  if (m->stream_offset == 0) {
    wav_last.valid = 0;                 /* новый файл с начала */
    xing_last.valid = 0;
  } else if (xing_last.valid) {
    /* После перемотки: длительность и оглавление — от начала этого файла. */
    m->xing_frames = xing_last.frames;
    m->xing_bytes = xing_last.bytes;
    memcpy(m->xing_toc, xing_last.toc, sizeof(m->xing_toc));
    m->have_toc = xing_last.have_toc;
    m->first_frame = 0;
  }
  return m;
}

/* --- WAV ------------------------------------------------------------------------ */

static void copy_tag(char* to, size_t size, const unsigned char* from, size_t length) {
  size_t n = 0;
  for (size_t i = 0; i < length && from[i] != 0 && n + 1 < size; ++i) {
    to[n++] = (char)from[i];
  }
  while (n > 0 && to[n - 1] == ' ') {
    --n;
  }
  to[n] = 0;
}

/* LIST INFO: INAM — название, IART — исполнитель, IPRD — альбом, ICRD — год. */
static void wav_info(mp3_t* m, const unsigned char* p, size_t length) {
  if (length < 4 || memcmp(p, "INFO", 4) != 0) {
    return;
  }
  for (size_t at = 4; at + 8 <= length;) {
    const uint32_t size = le32(p + at + 4);
    const unsigned char* value = p + at + 8;
    if (size > length - at - 8) {
      break;
    }
    if (memcmp(p + at, "INAM", 4) == 0) {
      copy_tag(m->title, sizeof(m->title), value, size);
    } else if (memcmp(p + at, "IART", 4) == 0) {
      copy_tag(m->artist, sizeof(m->artist), value, size);
    } else if (memcmp(p + at, "IPRD", 4) == 0) {
      copy_tag(m->album, sizeof(m->album), value, size);
    } else if (memcmp(p + at, "ICRD", 4) == 0 && size >= 4) {
      for (uint32_t i = 0; i + 4 <= size; ++i) {
        if (value[i] >= '0' && value[i] <= '9' && value[i + 1] >= '0' && value[i + 1] <= '9' &&
            value[i + 2] >= '0' && value[i + 2] <= '9' && value[i + 3] >= '0' && value[i + 3] <= '9') {
          m->year = (uint16_t)((value[i] - '0') * 1000 + (value[i + 1] - '0') * 100 +
                               (value[i + 2] - '0') * 10 + (value[i + 3] - '0'));
          break;
        }
      }
    }
    at += 8 + size + (size & 1);
  }
}

/* «fmt »: 1 — понятный формат, 0 — нет. */
static int wav_fmt(mp3_t* m, const unsigned char* p, size_t length) {
  if (length < 16) {
    return 0;
  }
  wav_format_t* w = &m->wav;
  w->format = le16(p);
  w->channels = le16(p + 2);
  w->rate = le32(p + 4);
  w->byte_rate = le32(p + 8);
  w->block = le16(p + 12);
  w->bits = le16(p + 14);
  if (w->format == WAV_EXTENSIBLE && length >= 26) {
    w->format = le16(p + 24);           /* первые байты GUID подформата */
  }
  const int bits_ok = w->format == WAV_PCM ? (w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32)
                    : w->format == WAV_FLOAT ? (w->bits == 32 || w->bits == 64)
                    : (w->format == WAV_ALAW || w->format == WAV_MULAW) ? w->bits == 8 : 0;
  if (!bits_ok || w->channels == 0 || w->channels > 8 || w->rate < 1000 || w->rate > 384000 ||
      w->block != w->channels * (w->bits / 8)) {
    host->log("mp3: WAV format %u, %u bits, %u channels, %u Hz is not supported", (unsigned)le16(p),
              (unsigned)w->bits, (unsigned)w->channels, (unsigned)w->rate);
    return 0;
  }
  w->byte_rate = w->rate * w->block;
  return 1;
}

/* Данные WAV начались: формат запомнить, длину считать. */
static void wav_begin(mp3_t* m, uint32_t data_start, uint32_t data_size) {
  m->wav.data_start = data_start;
  m->wav.data_size = data_size;
  m->wav.valid = 1;
  wav_last = m->wav;
  m->wav_sized = data_size != 0 && data_size != 0xFFFFFFFFu &&
                 (m->file_size == 0 || data_start + data_size <= m->file_size + 1);
  m->wav_left = m->wav_sized ? data_size : 0;
  m->decimate = 1;
  while (m->wav.rate / m->decimate > 48000) {
    m->decimate = (uint8_t)(m->decimate * 2);
  }
  m->rate = m->wav.rate / m->decimate;
  m->channels = (uint8_t)(m->wav.channels >= 2 ? 2 : 1);
  m->bitrate = (uint16_t)(m->wav.byte_rate * 8 / 1000);
  m->audio_start = data_start;
  m->wav_header = 2;
}

/* Заголовок WAV из потока: 1 — данные начались, 0 — нужен ввод, −1 — не WAV. */
static int wav_header(mp3_t* m) {
  for (;;) {
    if (m->in_length < INPUT_BYTES) {
      m->in_length += host->input_read(m->in + m->in_length, INPUT_BYTES - m->in_length);
    }
    const int ended = host->input_ended() && host->input_available() == 0;
    if (m->skip != 0) {                 /* блок, который не нужен */
      const size_t part = m->skip < m->in_length ? m->skip : m->in_length;
      consume(m, part);
      m->skip -= part;
      if (part == 0) {
        return ended ? -1 : 0;
      }
      continue;
    }
    if (m->wav_header == 0) {
      if (m->in_length < 12) {
        return ended ? -1 : 0;
      }
      if (memcmp(m->in, "RIFF", 4) != 0 || memcmp(m->in + 8, "WAVE", 4) != 0) {
        return -1;
      }
      consume(m, 12);
      m->wav_header = 1;
      continue;
    }
    if (m->in_length < 8) {
      return ended ? -1 : 0;
    }
    const uint32_t size = le32(m->in + 4);
    if (memcmp(m->in, "data", 4) == 0) {
      if (!m->wav.valid) {
        host->log("mp3: WAV data before its format");
        return -1;
      }
      consume(m, 8);
      wav_begin(m, m->stream_offset + m->consumed, size);
      return 1;
    }
    /* С выравниванием до чётного — в 64 битах: у 0xFFFFFFFF прежняя сумма давала ноль,
     * проверка вместимости проходила, а разбор получал всю огромную длину. */
    const uint64_t padded = (uint64_t)size + (size & 1);
    if ((memcmp(m->in, "fmt ", 4) == 0 || memcmp(m->in, "LIST", 4) == 0) && padded <= INPUT_BYTES - 8) {
      if (m->in_length < 8 + padded) {
        if (ended) {
          return -1;
        }
        return 0;
      }
      if (memcmp(m->in, "fmt ", 4) == 0) {
        if (!wav_fmt(m, m->in + 8, size)) {
          return -1;
        }
        m->wav.valid = 1;
      } else {
        wav_info(m, m->in + 8, size);
      }
      consume(m, 8 + padded);
      continue;
    }
    consume(m, 8);                      /* иной блок (fact, bext, JUNK, id3 …) — мимо */
    m->skip = padded > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)padded;
  }
}

/* G.711. */
static int alaw_sample(uint8_t a) {
  a ^= 0x55;
  int value = (a & 0x0F) << 4;
  const int segment = (a & 0x70) >> 4;
  if (segment == 0) {
    value += 8;
  } else {
    value = (value + 0x108) << (segment - 1);
  }
  return (a & 0x80) ? value : -value;
}

static int mulaw_sample(uint8_t u) {
  u = (uint8_t)~u;
  int value = (((u & 0x0F) << 3) + 0x84) << ((u & 0x70) >> 4);
  return (u & 0x80) ? (0x84 - value) : (value - 0x84);
}

/* Отсчёт канала из кадра — float −1..1. */
static float wav_sample(const wav_format_t* w, const unsigned char* p) {
  switch (w->format) {
    case WAV_PCM:
      switch (w->bits) {
        case 8: return ((int)p[0] - 128) * (1.0f / 128.0f);
        case 16: return (int16_t)le16(p) * (1.0f / 32768.0f);
        case 24: {
          const int32_t value = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
          return (float)(value >> 8) * (1.0f / 8388608.0f);
        }
        default: return (float)(int32_t)le32(p) * (1.0f / 2147483648.0f);
      }
    case WAV_FLOAT:
      if (w->bits == 32) {
        uint32_t u = le32(p);
        float f;
        memcpy(&f, &u, 4);
        return f;
      } else {
        uint64_t u = (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
        double d;
        memcpy(&d, &u, 8);
        return (float)d;
      }
    case WAV_ALAW: return alaw_sample(p[0]) * (1.0f / 32768.0f);
    default: return mulaw_sample(p[0]) * (1.0f / 32768.0f);
  }
}

/* Кадры WAV из очереди: 1 — есть, 0 — нужен ввод, −1 — конец. */
static int wav_render(mp3_t* m, float* out, uint32_t max_frames, uint32_t* frames) {
  if (m->wav_header != 2) {
    const int got = wav_header(m);
    if (got <= 0) {
      return got;
    }
  }
  if (m->in_length < INPUT_BYTES) {
    m->in_length += host->input_read(m->in + m->in_length, INPUT_BYTES - m->in_length);
  }
  const int ended = host->input_ended() && host->input_available() == 0;
  if (m->skip != 0) {                   /* после перемотки — до начала кадра */
    const size_t part = m->skip < m->in_length ? m->skip : m->in_length;
    consume(m, part);
    m->skip -= part;
    if (m->skip != 0) {
      return ended ? -1 : 0;
    }
  }
  const wav_format_t* w = &m->wav;
  const uint32_t group = (uint32_t)w->block * m->decimate;    /* байт на кадр вывода */
  uint32_t count = (uint32_t)(m->in_length / group);
  if (m->wav_sized && count > m->wav_left / group) {
    count = m->wav_left / group;
  }
  if (count > max_frames) {
    count = max_frames;
  }
  if (count == 0) {
    if (ended || (m->wav_sized && m->wav_left < group)) {
      return -1;
    }
    return 0;
  }
  const unsigned char* frame = m->in;
  const uint16_t step = (uint16_t)(w->bits / 8);
  const float scale = 1.0f / (float)m->decimate;
  for (uint32_t i = 0; i < count; ++i) {
    float left = 0.0f;
    float right = 0.0f;
    for (uint32_t k = 0; k < m->decimate; ++k, frame += w->block) {
      if (w->channels == 1) {
        left += wav_sample(w, frame);
      } else if (w->channels == 2) {
        left += wav_sample(w, frame);
        right += wav_sample(w, frame + step);
      } else {
        /* Сведение: левый и правый, центр и тыл на −3 дБ (LFE — мимо). */
        float l = wav_sample(w, frame);
        float r = wav_sample(w, frame + step);
        float weight = 1.0f;
        if (w->channels >= 3) {
          const float center = wav_sample(w, frame + 2 * step) * 0.7071f;
          l += center;
          r += center;
          weight += 0.7071f;
        }
        if (w->channels >= 6) {
          l += wav_sample(w, frame + 4 * step) * 0.7071f;
          r += wav_sample(w, frame + 5 * step) * 0.7071f;
          weight += 0.7071f;
        }
        left += l / weight;
        right += r / weight;
      }
    }
    if (m->channels == 1) {
      out[i] = left * scale;
    } else {
      out[2 * i] = left * scale;
      out[2 * i + 1] = right * scale;
    }
  }
  consume(m, (size_t)count * group);
  if (m->wav_sized) {
    m->wav_left -= count * group;
  }
  m->frames += count;
  *frames = count;
  return 1;
}

/* Начало потока: WAV (с заголовком или, после перемотки, без него) или MP3. */
static void choose_mode(mp3_t* m) {
  if (m->in_length < INPUT_BYTES) {
    m->in_length += host->input_read(m->in + m->in_length, INPUT_BYTES - m->in_length);
  }
  if (m->in_length < 4 && !(host->input_ended() && host->input_available() == 0)) {
    return;                             /* подождать начала */
  }
  if (m->in_length >= 4 && memcmp(m->in, "RIFF", 4) == 0) {
    m->mode = MODE_WAV;
    return;
  }
  if (m->stream_offset != 0 && m->stream_offset != UINT32_MAX && wav_last.valid &&
      m->stream_offset >= wav_last.data_start) {
    /* После перемотки WAV: формат — прежний, встать на начало кадра. */
    m->mode = MODE_WAV;
    m->wav = wav_last;
    const uint32_t into = m->stream_offset - wav_last.data_start;
    const uint32_t rest = into % wav_last.block;
    m->skip = rest != 0 ? wav_last.block - rest : 0;
    wav_begin(m, wav_last.data_start, wav_last.data_size);
    const uint32_t done = into + (uint32_t)m->skip;
    if (m->wav_sized) {
      m->wav_left = done < wav_last.data_size ? wav_last.data_size - done : 0;
    }
    m->frames = done / wav_last.block / m->decimate;
    return;
  }
  m->mode = MODE_MP3;
}

/* Декодировать следующий кадр в pcm. 1 — есть звук, 0 — нужен ввод, −1 — конец. */
static int decode_frame(mp3_t* m) {
  for (;;) {
    if (m->in_length < INPUT_BYTES) {
      m->in_length += host->input_read(m->in + m->in_length, INPUT_BYTES - m->in_length);
    }
    const int ended = host->input_ended() && host->input_available() == 0;
    if (!m->id3_checked) {
      if (m->in_length < 10) {
        return ended ? -1 : 0;
      }
      if (memcmp(m->in, "ID3", 3) == 0) {
        m->skip = 10 + (((size_t)(m->in[6] & 0x7F) << 21) | ((size_t)(m->in[7] & 0x7F) << 14) |
                        ((size_t)(m->in[8] & 0x7F) << 7) | (size_t)(m->in[9] & 0x7F));
        if (m->in[5] & 0x10) {
          m->skip += 10;
        }
      }
      m->id3_checked = 1;
    }
    if (m->skip != 0) {
      const size_t part = m->skip < m->in_length ? m->skip : m->in_length;
      consume(m, part);
      m->skip -= part;
      if (part == 0) {
        return ended ? -1 : 0;
      }
      continue;
    }
    const int offset = MP3FindSyncWord(m->in, (int)m->in_length);
    if (offset < 0) {
      if (m->in_length > 1) {
        consume(m, m->in_length - 1);
      }
      if (ended) {
        return -1;
      }
      if (host->input_available() == 0) {
        return 0;
      }
      continue;
    }
    if (offset > 0) {
      consume(m, (size_t)offset);
    }
    if (m->first_frame && m->in_length >= 4) {
      const size_t length = frame_bytes(m->in);
      if (length != 0) {
        if (length > m->in_length && !ended) {
          if (host->input_available() == 0) {
            return 0;                   /* остаток первого кадра ещё в пути */
          }
          continue;                     /* дочитать */
        }
        m->first_frame = 0;
        m->audio_start = m->consumed;
        if (length <= m->in_length && parse_xing(m, m->in, length)) {
          xing_last.frames = m->xing_frames;
          xing_last.bytes = m->xing_bytes;
          memcpy(xing_last.toc, m->xing_toc, sizeof(xing_last.toc));
          xing_last.have_toc = m->have_toc;
          xing_last.valid = 1;
          consume(m, length);           /* кадр Xing/Info/VBRI звука не несёт */
          continue;
        }
      }
    }
    unsigned char* cursor = m->in;
    int left = (int)m->in_length;
    const int result = MP3Decode(m->decoder, &cursor, &left, m->pcm, 0);
    const size_t used = m->in_length - (size_t)left;
    if (result == ERR_MP3_INDATA_UNDERFLOW) {
      if (m->in_length == INPUT_BYTES) {
        consume(m, 1);              /* ложное начало кадра */
        continue;
      }
      if (ended) {
        return -1;
      }
      return 0;
    }
    if (result == ERR_MP3_MAINDATA_UNDERFLOW) {
      consume(m, used != 0 ? used : 1);
      continue;
    }
    if (result != ERR_MP3_NONE) {
      m->errors++;
      consume(m, 1);
      continue;
    }
    consume(m, used);
    MP3FrameInfo info;
    MP3GetLastFrameInfo(m->decoder, &info);
    if (info.outputSamps <= 0 || info.samprate <= 0 || (info.nChans != 1 && info.nChans != 2) ||
        info.outputSamps > MAX_SAMPLES) {
      m->errors++;
      continue;
    }
    m->rate = (uint32_t)info.samprate;
    m->channels = (uint8_t)info.nChans;
    m->bitrate = (uint16_t)(info.bitrate / 1000);
    m->pcm_frames = (size_t)info.outputSamps / (size_t)info.nChans;
    m->pcm_used = 0;
    return 1;
  }
}

static int render(void* instance, float* out, uint32_t max_frames, uint32_t* frames) {
  mp3_t* m = (mp3_t*)instance;
  *frames = 0;
  if (m->mode == MODE_UNKNOWN) {
    choose_mode(m);
    if (m->mode == MODE_UNKNOWN) {
      return ZM_RENDER_NEED_INPUT;
    }
  }
  if (m->mode == MODE_WAV) {
    const int got = wav_render(m, out, max_frames, frames);
    return got > 0 ? ZM_RENDER_OK : (got == 0 ? ZM_RENDER_NEED_INPUT : ZM_RENDER_END);
  }
  if (m->pcm_used >= m->pcm_frames) {
    const int got = decode_frame(m);
    if (got <= 0) {
      return got == 0 ? ZM_RENDER_NEED_INPUT : ZM_RENDER_END;
    }
  }
  size_t count = m->pcm_frames - m->pcm_used;
  if (count > max_frames) {
    count = max_frames;
  }
  const short* source = m->pcm + m->pcm_used * m->channels;
  const size_t samples = count * m->channels;
  for (size_t i = 0; i < samples; ++i) {
    out[i] = source[i] * (1.0f / 32768.0f);
  }
  m->pcm_used += count;
  m->frames += count;
  *frames = (uint32_t)count;
  return ZM_RENDER_OK;
}

/* Перемотка: новое место в файле по оглавлению Xing (VBR) или по битрейту
 * (CBR); декодер начинает заново, данные с нового места подаёт плеер. */
static int seek(void* instance, int32_t delta_ms, uint32_t* file_offset) {
  mp3_t* m = (mp3_t*)instance;
  if (m->mode != MODE_MP3 || m->rate == 0 || m->bitrate == 0) {
    return ZM_ERROR;                    /* WAV перематывает плагин (AUDIO_FLUSH) */
  }
  int64_t now = (int64_t)(m->frames * 1000 / m->rate);
  int64_t target = now + delta_ms;
  if (target < 0) {
    target = 0;
  }
  const uint32_t audio_bytes = m->xing_bytes != 0 ? m->xing_bytes
                               : (m->file_size > m->audio_start ? m->file_size - m->audio_start : 0);
  uint32_t duration = 0;
  if (m->xing_frames != 0) {
    duration = (uint32_t)((uint64_t)m->xing_frames * (m->rate <= 24000 ? 576 : 1152) * 1000 / m->rate);
  } else if (m->bitrate != 0) {
    duration = (uint32_t)((uint64_t)audio_bytes * 8 / m->bitrate);
  }
  if (duration != 0 && target >= duration) {
    return ZM_ERROR;                  /* за концом: плеер уходит на следующий файл */
  }
  uint32_t offset;
  if (m->have_toc && duration != 0 && audio_bytes != 0) {
    const float percent = (float)target * 100.0f / (float)duration;
    int index = (int)percent;
    if (index > 99) {
      index = 99;
    }
    const float a = m->xing_toc[index];
    const float b = index < 99 ? m->xing_toc[index + 1] : 256.0f;
    const float fraction = a + (b - a) * (percent - (float)index);
    offset = m->audio_start + (uint32_t)(fraction / 256.0f * (float)audio_bytes);
  } else {
    offset = m->audio_start + (uint32_t)((uint64_t)target * m->bitrate / 8);
  }
  /* Декодер с чистого листа: резервуар бит ссылается на прежние кадры. */
  MP3FreeDecoder(m->decoder);
  m->decoder = MP3InitDecoder();
  m->in_length = 0;
  m->pcm_frames = m->pcm_used = 0;
  m->id3_checked = 1;
  m->skip = 0;
  m->consumed = offset;
  m->frames = (uint64_t)target * m->rate / 1000;
  *file_offset = offset;
  return m->decoder != NULL ? ZM_OK : ZM_ERROR;
}

static void info(const void* instance, zm_info_t* out) {
  const mp3_t* m = (const mp3_t*)instance;
  memset(out, 0, sizeof(*out));
  if (m->mode == MODE_WAV) {
    /* Формат известен с блока fmt, а частота вывода — только с блока data: до него
     * (большой LIST/JUNK между ними, ввод ещё не пришёл) сведений нет. Прежде здесь
     * делилось на нулевую частоту. */
    if (!m->wav.valid || m->rate == 0) {
      return;
    }
    const wav_format_t* w = &m->wav;
    snprintf(out->format, sizeof(out->format), "WAV %u-bit%s", (unsigned)w->bits,
             w->format == WAV_FLOAT ? " float" : w->format == WAV_ALAW ? " A-law"
             : w->format == WAV_MULAW ? " u-law" : "");
    out->rate = m->rate;
    out->channels = m->channels;
    out->position_ms = (uint32_t)(m->frames * 1000 / m->rate);
    uint32_t bytes = w->data_size;
    if (bytes == 0 || bytes == 0xFFFFFFFFu || (m->file_size != 0 && w->data_start + bytes > m->file_size + 1)) {
      bytes = m->file_size > w->data_start ? m->file_size - w->data_start : 0;
    }
    out->duration_ms = (uint32_t)((uint64_t)bytes * 1000 / w->byte_rate);
    out->voices = (uint8_t)w->channels;
    out->bitrate = m->bitrate;
    strcpy(out->title, m->title);
    strcpy(out->artist, m->artist);
    strcpy(out->album, m->album);
    out->year = m->year;
    return;
  }
  /* VBR с заголовком: средний битрейт (у кадров он скачет; по нему же плагин
   * считает место перемотки). */
  uint16_t kbps = m->bitrate;
  if (m->rate != 0) {
    out->position_ms = (uint32_t)(m->frames * 1000 / m->rate);
    if (m->xing_frames != 0) {
      out->duration_ms = (uint32_t)((uint64_t)m->xing_frames * (m->rate <= 24000 ? 576 : 1152) *
                                    1000 / m->rate);
      if (m->xing_bytes != 0 && out->duration_ms != 0) {
        kbps = (uint16_t)((uint64_t)m->xing_bytes * 8 / out->duration_ms);
      }
    } else if (m->bitrate != 0 && m->file_size > m->audio_start) {
      out->duration_ms = (uint32_t)((uint64_t)(m->file_size - m->audio_start) * 8 / m->bitrate);
    }
  }
  if (kbps != 0) {
    snprintf(out->format, sizeof(out->format), "MP3 %u kbps", (unsigned)kbps);
  } else {
    strcpy(out->format, "MP3");
  }
  out->rate = m->rate;
  out->channels = m->channels;
  out->voices = m->channels;
  out->bitrate = kbps;
  out->errors = m->errors;
}

static void close_source(void* instance) {
  mp3_t* m = (mp3_t*)instance;
  if (m->decoder != NULL) {
    MP3FreeDecoder(m->decoder);
  }
  host->free(m);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    return 0;
  }
  host = (const zm_host_t*)argv[1];
  table.size = sizeof(table);
  table.abi_major = ZM_SOURCE_ABI_MAJOR;
  table.abi_minor = ZM_SOURCE_ABI_MINOR;
  table.name = "mp3";
  table.version = "1.3";
  table.extensions = "MP3 WAV";
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
