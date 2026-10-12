/* AAC.ELF — потоковый декодер плеера VDAC4: AAC в потоке ADTS (Helix HE-AAC,
 * components/helix_aac): AAC-LC и SBR (HE-AAC v1). Параметрического стерео
 * (HE-AAC v2) Helix не умеет — такой поток звучит моно.
 *
 * Поток: байты берёт сам из очереди плеера (zm_host_t::input_read) — файл .AAC с
 * карты или поток интернет-радио (audio/aac, audio/aacp). Метку ID3v2 в начале
 * пропускает. Кадр ищет по заголовку ADTS (слово синхронизации, слой 0, частота,
 * длина) и, пока не встал на поток, верит ему, только если сразу за кадром
 * начинается следующий; декодирует кадр, только когда он весь в буфере. Кадр с
 * ошибкой считает и пропускает. Отдаёт float на частоте выхода (с SBR — вдвое
 * выше частоты ядра), моно — как есть.
 *
 * Битрейт — средний по кадрам (длины из заголовков ADTS). По нему длительность
 * файла и перемотка: место в файле считает плагин (время × битрейт), как у MP3,
 * а декодер, открытый заново с этого места (AUDIO_FLUSH), ищет следующий кадр;
 * начало звука и счёт битрейта он берёт от этого же файла с начала (aac_last),
 * иначе длительность считалась бы от нового места.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "aacdec.h"
#include "zm_app.h"
#include "zm_source.h"

#define FRAME_MAX 8191              /* длина кадра ADTS — 13 бит */
#define INPUT_BYTES (FRAME_MAX + 9) /* кадр целиком и заголовок следующего */
#define MAX_SAMPLES (AAC_MAX_NCHANS * AAC_MAX_NSAMPS * 2)   /* с SBR — вдвое */
#define ADTS_SAMPLES 1024           /* кадров звука ядра в блоке */

typedef struct {
  HAACDecoder decoder;
  unsigned char in[INPUT_BYTES];
  size_t in_length;
  short pcm[MAX_SAMPLES];
  size_t pcm_frames;        /* кадров звука в pcm */
  size_t pcm_used;          /* из них уже отдано */
  int id3_checked;
  size_t skip;              /* сколько байтов метки ещё пропустить */
  int synced;               /* стоим на начале кадра: прошлый кадр кончился здесь */
  size_t frame_length;      /* кадр в работе (0 — нет): блоков в кадре ADTS бывает до 4, */
  size_t frame_pos;         /* а AACDecode декодирует по одному */
  int frame_block;
  int frame_blocks;
  int started;              /* первый кадр найден */
  uint32_t file_size;
  uint32_t audio_start;     /* где в потоке первый кадр */
  uint32_t consumed;        /* байтов потока пройдено */
  uint32_t rate;            /* частота выхода */
  uint32_t core_rate;       /* частота ядра (без SBR) */
  uint8_t channels;
  int sbr;
  uint64_t frames;          /* отдано кадров звука */
  uint64_t frame_bytes;     /* байтов кадров ADTS всего — для битрейта */
  uint64_t core_samples;    /* кадров звука ядра в них */
  uint32_t errors;
} aac_t;

/* Файл, открытый с начала последним: где начался звук и счёт битрейта. После
 * перемотки поток — с середины, а длительность — по всему файлу. */
static struct {
  uint32_t audio_start;
  uint64_t frame_bytes;
  uint64_t core_samples;
  uint32_t core_rate;
  int valid;
} aac_last;

static const zm_host_t* host;
static zm_source_t table;

/* Память Helix (helix_aac_port.h): состояние декодера и SBR — десятки КБ, в PSRAM. */
void* helix_malloc(int size) {
  void* pointer = host->alloc((size_t)size, ZM_MEM_PSRAM);
  if (pointer != NULL) {
    memset(pointer, 0, (size_t)size);
  }
  return pointer;
}

void helix_free(void* pointer) {
  if (pointer != NULL) {
    host->free(pointer);
  }
}

static void consume(aac_t* a, size_t count) {
  if (count > a->in_length) {
    count = a->in_length;
  }
  memmove(a->in, a->in + count, a->in_length - count);
  a->in_length -= count;
  a->consumed += (uint32_t)count;
}

/* Заголовок ADTS: длина кадра, байт (0 — не заголовок); частота ядра — в *rate,
 * блоков в кадре — в *blocks (можно NULL). */
static size_t adts_frame(const unsigned char* h, uint32_t* rate, int* blocks) {
  static const uint32_t rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                     22050, 16000, 12000, 11025, 8000,  7350};
  if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) {
    return 0;                       /* слово синхронизации 0xFFF и слой 0 */
  }
  const int index = (h[2] >> 2) & 0x0F;
  if (index >= 13) {
    return 0;
  }
  const size_t length = ((size_t)(h[3] & 0x03) << 11) | ((size_t)h[4] << 3) | (h[5] >> 5);
  const size_t header = (h[1] & 0x01) ? 7 : 9;   /* без CRC и с ним */
  if (length <= header) {
    return 0;
  }
  if (rate != NULL) {
    *rate = rates[index];
  }
  if (blocks != NULL) {
    *blocks = (h[6] & 0x03) + 1;
  }
  return length;
}

static int probe(const unsigned char* head, size_t length) {
  if (length >= 7 && adts_frame(head, NULL, NULL) != 0) {
    return 70;
  }
  if (length >= 3 && memcmp(head, "ID3", 3) == 0) {
    return 20;                      /* метка — и у MP3 */
  }
  return 0;
}

static void* open_source(uint32_t file_size, const unsigned char* file) {
  (void)file;
  aac_t* a = (aac_t*)host->alloc(sizeof(aac_t), ZM_MEM_PSRAM);
  if (a == NULL) {
    return NULL;
  }
  memset(a, 0, sizeof(*a));
  a->decoder = AACInitDecoder();
  if (a->decoder == NULL) {
    host->free(a);
    return NULL;
  }
  a->file_size = file_size;
  /* После перемотки (AUDIO_FLUSH) поток — с середины файла: метки там нет. */
  const uint32_t offset = host->size >= offsetof(zm_host_t, stream_offset) + sizeof(uint32_t)
                              ? host->stream_offset
                              : 0;
  a->id3_checked = offset != 0 && offset != UINT32_MAX;
  a->consumed = offset != UINT32_MAX ? offset : 0;
  if (offset == 0) {
    aac_last.valid = 0;             /* новый файл (или поток радио) с начала */
  } else if (aac_last.valid) {
    a->started = 1;
    a->audio_start = aac_last.audio_start;
    a->frame_bytes = aac_last.frame_bytes;
    a->core_samples = aac_last.core_samples;
    a->core_rate = aac_last.core_rate;
  }
  return a;
}

/* Где в буфере начинается кадр, которому можно верить: заголовок ADTS, а за
 * кадром — ещё один той же частоты (или конец потока). Возвращает смещение
 * (длина кадра — в *length) или -1 — кадра нет; *wait = 1 — по смещению кандидат,
 * проверить его нечем: нужен ввод (буфер вмещает кадр и заголовок за ним). */
static long find_frame(const aac_t* a, int ended, size_t* length, int* wait) {
  *wait = 0;
  for (size_t at = 0; at + 7 <= a->in_length; ++at) {
    if (a->in[at] != 0xFF) {
      continue;
    }
    uint32_t rate = 0;
    const size_t size = adts_frame(a->in + at, &rate, NULL);
    if (size == 0) {
      continue;
    }
    if (at + size + 7 > a->in_length) {
      if (!ended) {
        *wait = 1;
        return (long)at;
      }
      if (at + size <= a->in_length) {
        *length = size;             /* последний кадр потока */
        return (long)at;
      }
      continue;
    }
    uint32_t next_rate = 0;
    if (adts_frame(a->in + at + size, &next_rate, NULL) != 0 && next_rate == rate) {
      *length = size;
      return (long)at;
    }
  }
  return -1;
}

/* Нужен ввод: 0 — ждать его, -1 — поток кончился. */
static int need_input(int ended) { return ended ? -1 : 0; }

/* Декодировать следующий блок в pcm. 1 — есть звук, 0 — нужен ввод, −1 — конец. */
static int decode_frame(aac_t* a) {
  for (;;) {
    if (a->frame_length == 0 && a->in_length < INPUT_BYTES) {
      a->in_length += host->input_read(a->in + a->in_length, INPUT_BYTES - a->in_length);
    }
    const int ended = host->input_ended() && host->input_available() == 0;
    if (a->frame_length != 0) {
      /* Следующий блок того же кадра: заголовка у него нет, Helix помнит кадр. */
      unsigned char* cursor = a->in + a->frame_pos;
      int left = (int)(a->frame_length - a->frame_pos);
      const int result = AACDecode(a->decoder, &cursor, &left, a->pcm);
      a->frame_pos = (size_t)(cursor - a->in);
      const int done = ++a->frame_block >= a->frame_blocks || result != ERR_AAC_NONE ||
                       a->frame_pos >= a->frame_length;
      const size_t length = a->frame_length;
      if (done) {
        a->frame_length = 0;
        consume(a, length);
      }
      if (result != ERR_AAC_NONE) {
        /* Испорченный кадр: прочь целиком (заголовок верный), дальше — следующий;
         * декодер — с чистого листа (SBR и перекрытие сбиты). */
        a->errors++;
        AACFlushCodec(a->decoder);
        continue;
      }
      AACFrameInfo info;
      AACGetLastFrameInfo(a->decoder, &info);
      if (info.outputSamps <= 0 || info.sampRateOut <= 0 ||
          (info.nChans != 1 && info.nChans != 2) || info.outputSamps > MAX_SAMPLES) {
        a->errors++;
        continue;
      }
      a->core_samples += ADTS_SAMPLES;
      a->rate = (uint32_t)info.sampRateOut;
      a->core_rate = (uint32_t)info.sampRateCore;
      aac_last.audio_start = a->audio_start;
      aac_last.frame_bytes = a->frame_bytes;
      aac_last.core_samples = a->core_samples;
      aac_last.core_rate = a->core_rate;
      aac_last.valid = 1;
      a->sbr = info.sampRateOut > info.sampRateCore;
      a->channels = (uint8_t)info.nChans;
      a->pcm_frames = (size_t)info.outputSamps / (size_t)info.nChans;
      a->pcm_used = 0;
      return 1;
    }
    if (!a->id3_checked) {
      if (a->in_length < 10) {
        return need_input(ended);
      }
      if (memcmp(a->in, "ID3", 3) == 0) {
        a->skip = 10 + (((size_t)(a->in[6] & 0x7F) << 21) | ((size_t)(a->in[7] & 0x7F) << 14) |
                        ((size_t)(a->in[8] & 0x7F) << 7) | (size_t)(a->in[9] & 0x7F));
        if (a->in[5] & 0x10) {
          a->skip += 10;
        }
      }
      a->id3_checked = 1;
    }
    if (a->skip != 0) {
      const size_t part = a->skip < a->in_length ? a->skip : a->in_length;
      consume(a, part);
      a->skip -= part;
      if (part == 0) {
        return need_input(ended);
      }
      continue;
    }
    size_t length = 0;
    if (a->synced) {
      if (a->in_length < 7) {
        if (ended || host->input_available() == 0) {
          return need_input(ended);
        }
        continue;
      }
      length = adts_frame(a->in, NULL, NULL);
      if (length == 0) {
        a->synced = 0;              /* кадр не там, где кончился прошлый: искать заново */
        a->errors++;
      }
    }
    if (!a->synced) {
      int wait = 0;
      const long at = find_frame(a, ended, &length, &wait);
      if (at < 0) {
        /* Кадра нет: оставить хвост — в нём может начинаться заголовок. */
        if (a->in_length > 6) {
          consume(a, a->in_length - 6);
        }
        if (ended || host->input_available() == 0) {
          return need_input(ended);
        }
        continue;
      }
      consume(a, (size_t)at);
      if (wait) {
        if (host->input_available() == 0) {
          return 0;
        }
        continue;                   /* дочитать и проверить снова */
      }
      a->synced = 1;
    }
    if (length > a->in_length) {
      if (ended || host->input_available() == 0) {
        return need_input(ended);   /* кадр ещё в пути (или оборван в конце) */
      }
      continue;
    }
    if (!a->started) {
      a->started = 1;
      a->audio_start = a->consumed;
    }
    adts_frame(a->in, NULL, &a->frame_blocks);
    a->frame_length = length;
    a->frame_pos = 0;
    a->frame_block = 0;
    a->frame_bytes += length;
  }
}

static int render(void* instance, float* out, uint32_t max_frames, uint32_t* frames) {
  aac_t* a = (aac_t*)instance;
  *frames = 0;
  if (a->pcm_used >= a->pcm_frames) {
    const int got = decode_frame(a);
    if (got <= 0) {
      return got == 0 ? ZM_RENDER_NEED_INPUT : ZM_RENDER_END;
    }
  }
  size_t count = a->pcm_frames - a->pcm_used;
  if (count > max_frames) {
    count = max_frames;
  }
  const short* source = a->pcm + a->pcm_used * a->channels;
  const size_t samples = count * a->channels;
  for (size_t i = 0; i < samples; ++i) {
    out[i] = source[i] * (1.0f / 32768.0f);
  }
  a->pcm_used += count;
  a->frames += count;
  *frames = (uint32_t)count;
  return ZM_RENDER_OK;
}

/* Перематывает плагин (AUDIO_FLUSH: место в файле — время × битрейт). */
static int seek(void* instance, int32_t delta_ms, uint32_t* file_offset) {
  (void)instance;
  (void)delta_ms;
  (void)file_offset;
  return ZM_ERROR;
}

/* Средний битрейт кадров, кбит/с (0 — кадров ещё не было). */
static uint32_t average_kbps(const aac_t* a) {
  if (a->core_samples == 0 || a->core_rate == 0) {
    return 0;
  }
  return (uint32_t)(a->frame_bytes * 8u * a->core_rate / a->core_samples / 1000u);
}

static void info(const void* instance, zm_info_t* out) {
  const aac_t* a = (const aac_t*)instance;
  memset(out, 0, sizeof(*out));
  const uint32_t kbps = average_kbps(a);
  const char* name = a->sbr ? "HE-AAC" : "AAC";
  if (kbps != 0) {
    snprintf(out->format, sizeof(out->format), "%s %u kbps", name, (unsigned)kbps);
  } else {
    snprintf(out->format, sizeof(out->format), "%s", name);
  }
  out->rate = a->rate;
  out->channels = a->channels;
  out->voices = a->channels;
  out->bitrate = (uint16_t)kbps;
  out->errors = a->errors;
  if (a->rate != 0) {
    out->position_ms = (uint32_t)(a->frames * 1000 / a->rate);
  }
  /* Длительность — и до первого кадра после перемотки (битрейт — от aac_last). */
  if (kbps != 0 && a->file_size > a->audio_start) {
    out->duration_ms = (uint32_t)((uint64_t)(a->file_size - a->audio_start) * 8u / kbps);
  }
}

static void close_source(void* instance) {
  aac_t* a = (aac_t*)instance;
  if (a->decoder != NULL) {
    AACFreeDecoder(a->decoder);
  }
  host->free(a);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    return 0;
  }
  host = (const zm_host_t*)argv[1];
  table.size = sizeof(table);
  table.abi_major = ZM_SOURCE_ABI_MAJOR;
  table.abi_minor = ZM_SOURCE_ABI_MINOR;
  table.name = "aac";
  table.version = "1.0";
  table.extensions = "AAC";
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
