/* it_stream.c — IT потоком для libxmp (TRACKER.ELF), см. it_stream.h.
 *
 * План строится прогоном того порядка, в каком читает загрузчик IT libxmp
 * (src/loaders/it_load.c): заголовок и таблицы указателей; инструменты;
 * для каждого сэмпла — его заголовок и сразу отсчёты; узоры два прохода
 * (подсчёт каналов, разбор); сообщение. Поток идёт только вперёд: всё, что
 * прогон читает, когда поток уже прошёл это место, хранится (участки keep), а
 * голова файла (заголовки, начало для проб форматов libxmp) — целиком.
 * Длина сжатых отсчётов (IT 2.14) в заголовке не записана — до начала
 * следующей известной структуры файла.
 */
#include "it_stream.h"

#include <string.h>

#include "zm_app.h"

#define HEAD_MIN (64u * 1024u)          /* голова не короче: пробы форматов libxmp читают начало */
#define HEAD_MAX (1024u * 1024u)        /* до первых отсчётов, если их не дальше */
#define WAIT_MS 30000u                  /* ждать байтов: плагин шлёт файл без пауз */
#define BLOCK_OVERHEAD 24u              /* на блок памяти: длина у xmp_malloc и заголовок кучи */
#define INSTRUMENT_BYTES 1200u          /* инструмент libxmp с поддорожками */
#define SAMPLE_BYTES 160u               /* описание сэмпла libxmp */
#define PLAYER_BYTES (320u * 1024u)     /* проигрыватель libxmp (каналы, микшер) */
#define LOAD_BASE (96u * 1024u)         /* контекст и описание модуля libxmp при загрузке */
#define INSTRUMENT_HEADER 554u          /* заголовок инструмента IT 2.xx */
#define SAMPLE_HEADER 0x50u
#define MAX_ROWS 1024u                  /* строк в узоре у OpenMPT (если заголовок узора не в голове) */

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t* p) {
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* [at, at + length) внутри [0, size) — без переполнения. */
static int inside(uint32_t at, uint32_t length, uint32_t size) {
  return at <= size && length <= size - at;
}

/* --- поток ------------------------------------------------------------------------ */

/* Байты потока [at, at + count) — и в хранимые участки, что с ними пересекаются. */
static void keep_store(it_stream_t* s, uint32_t at, const uint8_t* data, uint32_t count) {
  const uint32_t end = at + count;
  for (int i = 0; i < s->keeps; ++i) {
    it_keep_t* k = &s->keep[i];
    if (k->end <= at) {
      continue;
    }
    if (k->start >= end) {
      break;                                           /* участки — по возрастанию */
    }
    if (k->data == NULL) {
      k->data = (uint8_t*)s->host->alloc(k->end - k->start, ZM_MEM_PSRAM);
      if (k->data == NULL) {
        s->failed = 1;
        return;
      }
    }
    const uint32_t from = at > k->start ? at : k->start;
    const uint32_t to = end < k->end ? end : k->end;
    memcpy(k->data + (from - k->start), data + (from - at), to - from);
  }
}

/* Взять из потока до count байтов (ждать хотя бы одного). 0 — поток кончился,
 * оборвался или плеер остановил приём (s->failed). */
static uint32_t take(it_stream_t* s, uint8_t* out, uint32_t count) {
  if (s->failed || s->stream >= s->size) {
    return 0;
  }
  if (count > s->size - s->stream) {
    count = s->size - s->stream;
  }
  size_t available = s->host->input_available();
  if (available == 0) {
    available = s->host->input_wait(count < 4096 ? count : 4096, WAIT_MS);
  }
  if (available == 0) {
    s->failed = 1;
    return 0;
  }
  const uint32_t got = (uint32_t)s->host->input_read(out, available < count ? available : count);
  if (got == 0) {
    s->failed = 1;
    return 0;
  }
  keep_store(s, s->stream, out, got);
  s->stream += got;
  return got;
}

/* Голова: первые length байтов (растёт при разборе плана). */
static int need(it_stream_t* s, uint32_t length) {
  if (length > s->size) {
    return 0;
  }
  if (length > s->head_capacity) {
    uint32_t capacity = s->head_capacity != 0 ? s->head_capacity : HEAD_MIN;
    while (capacity < length) {
      if (capacity > UINT32_MAX / 2) {  /* удвоение за 2 ГБ дало бы ноль и вечный цикл */
        capacity = length;
        break;
      }
      capacity *= 2;
    }
    if (capacity > s->size) {
      capacity = s->size;
    }
    uint8_t* head = (uint8_t*)s->host->alloc(capacity, ZM_MEM_PSRAM);
    if (head == NULL) {
      s->failed = 1;
      return 0;
    }
    if (s->head != NULL) {
      memcpy(head, s->head, s->head_length);
      s->host->free(s->head);
    }
    s->head = head;
    s->head_capacity = capacity;
  }
  while (s->head_length < length) {
    const uint32_t got = take(s, s->head + s->head_length, length - s->head_length);
    if (got == 0) {
      return 0;
    }
    s->head_length += got;
  }
  return 1;
}

static const it_keep_t* find_keep(const it_stream_t* s, uint32_t at) {
  int low = 0;
  int high = s->keeps - 1;
  while (low <= high) {
    const int middle = (low + high) / 2;
    const it_keep_t* k = &s->keep[middle];
    if (at < k->start) {
      high = middle - 1;
    } else if (at >= k->end) {
      low = middle + 1;
    } else {
      return k;
    }
  }
  return NULL;
}

/* До count байтов с места at: голова, хранимое или поток вперёд. 0 — нет. */
static uint32_t fetch(it_stream_t* s, uint32_t at, uint8_t* out, uint32_t count) {
  if (at < s->head_length) {
    const uint32_t n = s->head_length - at < count ? s->head_length - at : count;
    memcpy(out, s->head + at, n);
    return n;
  }
  if (at < s->stream) {
    const it_keep_t* k = find_keep(s, at);         /* поток это место прошёл */
    if (k == NULL || k->data == NULL) {
      return 0;
    }
    const uint32_t filled = s->stream < k->end ? s->stream : k->end;
    const uint32_t n = filled - at < count ? filled - at : count;
    memcpy(out, k->data + (at - k->start), n);
    return n;
  }
  while (s->stream < at) {
    const uint32_t gap = at - s->stream;
    if (take(s, s->scratch, gap < IT_SCRATCH ? gap : IT_SCRATCH) == 0) {
      return 0;
    }
  }
  return take(s, out, count);
}

/* --- обратные вызовы libxmp ---------------------------------------------------- */

unsigned long it_stream_read(void* dest, unsigned long length, unsigned long count, void* priv) {
  it_stream_t* s = (it_stream_t*)priv;
  if (s->failed || length == 0 || count == 0 || s->pos >= s->size) {
    return 0;
  }
  uint64_t want = (uint64_t)length * count;
  if (want > s->size - s->pos) {
    want = s->size - s->pos;
    want -= want % length;
  }
  uint8_t* out = (uint8_t*)dest;
  uint32_t done = 0;
  while (done < want) {
    const uint32_t got = fetch(s, s->pos, out + done, (uint32_t)want - done);
    if (got == 0) {
      s->failed = 1;                                  /* назад мимо хранимого или обрыв */
      break;
    }
    done += got;
    s->pos += got;
  }
  return done / length;
}

int it_stream_seek(void* priv, long offset, int whence) {
  it_stream_t* s = (it_stream_t*)priv;
  const long base = whence == 1 ? (long)s->pos : (whence == 2 ? (long)s->size : 0);  /* CUR, END */
  if (base + offset < 0) {
    return -1;
  }
  s->pos = (uint32_t)(base + offset);
  return 0;
}

long it_stream_tell(void* priv) { return (long)((it_stream_t*)priv)->pos; }

int it_stream_close(void* priv) {
  (void)priv;                                       /* память — it_stream_end */
  return 0;
}

/* --- план -------------------------------------------------------------------------- */

typedef struct {
  uint32_t start, end;
} span_t;

/* Чтение прогона [start, end): то, что поток уже прошёл (за головой), — хранить. */
typedef struct {
  span_t* spans;                                    /* хранить (могут пересекаться) */
  int count;
  int capacity;
  uint32_t head;
  uint32_t stream;
  int failed;
} replay_t;

static void replay_read(replay_t* r, uint32_t start, uint32_t end) {
  if (end <= r->head || start >= end) {
    return;
  }
  if (start < r->head) {
    start = r->head;
  }
  if (start < r->stream) {
    if (r->count == r->capacity) {
      r->failed = 1;
      return;
    }
    r->spans[r->count].start = start;
    r->spans[r->count].end = end < r->stream ? end : r->stream;
    ++r->count;
  }
  if (end > r->stream) {
    r->stream = end;
  }
}

static int span_less(const span_t* a, const span_t* b) {
  return a->start < b->start || (a->start == b->start && a->end < b->end);
}

/* Конец данных с места start: следующая известная структура файла (или его конец). */
static uint32_t next_start(const uint32_t* starts, int count, uint32_t start, uint32_t size) {
  uint32_t next = size;
  for (int i = 0; i < count; ++i) {
    if (starts[i] > start && starts[i] < next) {
      next = starts[i];
    }
  }
  return next;
}

int it_stream_begin(it_stream_t* s, const zm_host_t* host, uint32_t size) {
  memset(s, 0, sizeof(*s));
  s->host = host;
  s->size = size;
  if (!need(s, 0xC0) || memcmp(s->head, "IMPM", 4) != 0) {
    return -1;
  }
  const uint32_t orders = rd16(s->head + 0x20);
  const uint32_t instruments = rd16(s->head + 0x22);
  const uint32_t samples = rd16(s->head + 0x24);
  const uint32_t patterns = rd16(s->head + 0x26);
  const uint16_t flags = rd16(s->head + 0x2C);
  const uint16_t special = rd16(s->head + 0x2E);
  const uint32_t message_length = rd16(s->head + 0x36);
  const uint32_t message = rd32(s->head + 0x38);
  const uint32_t tables = 0xC0 + orders + 4 * (instruments + samples + patterns);
  /* Голова: заголовок, таблицы, заголовки инструментов и сэмплов (по ним план). */
  uint32_t head = tables > HEAD_MIN ? tables : HEAD_MIN;
  if (tables > size || !need(s, tables)) {
    return -1;
  }
  const uint8_t* table_ins = s->head + 0xC0 + orders;
  const uint8_t* table_smp = table_ins + 4 * instruments;
  const uint8_t* table_pat = table_smp + 4 * samples;
  for (uint32_t i = 0; i < instruments; ++i) {
    const uint32_t at = rd32(table_ins + 4 * i);
    if (at != 0 && inside(at, INSTRUMENT_HEADER, size) && at + INSTRUMENT_HEADER > head) {
      head = at + INSTRUMENT_HEADER;
    }
  }
  for (uint32_t i = 0; i < samples; ++i) {
    const uint32_t at = rd32(table_smp + 4 * i);
    if (at != 0 && inside(at, SAMPLE_HEADER, size) && at + SAMPLE_HEADER > head) {
      head = at + SAMPLE_HEADER;
    }
  }
  if (head > size) {
    head = size;
  }
  /* Таблицы — в голове, но она может переехать: места указателей — заново. */
  if (!need(s, head)) {
    return -1;
  }
  table_ins = s->head + 0xC0 + orders;
  table_smp = table_ins + 4 * instruments;
  table_pat = table_smp + 4 * samples;
  /* Всё до первых отсчётов (узоры, сообщение) — тоже в голову, если не слишком
   * много: тогда каналы узоров известны точно и хранить почти нечего. */
  uint32_t first_data = size;
  for (uint32_t i = 0; i < samples; ++i) {
    const uint32_t at = rd32(table_smp + 4 * i);
    if (at != 0 && inside(at, SAMPLE_HEADER, size) && (s->head[at + 0x12] & 1)) {
      const uint32_t data = rd32(s->head + at + 0x48);
      first_data = data != 0 && data < first_data ? data : first_data;
    }
  }
  if (first_data > s->head_length && first_data <= HEAD_MAX) {
    if (!need(s, first_data)) {
      return -1;
    }
    head = first_data;
    table_ins = s->head + 0xC0 + orders;
    table_smp = table_ins + 4 * instruments;
    table_pat = table_smp + 4 * samples;
  }

  /* Известные начала структур: по ним — конец сжатых отсчётов и узоров за головой. */
  const int known = (int)(instruments + 2 * samples + patterns + 1);
  uint32_t* starts = (uint32_t*)host->alloc((size_t)known * sizeof(uint32_t), ZM_MEM_PSRAM);
  span_t* spans = (span_t*)host->alloc((size_t)(known * 2 + 8) * sizeof(span_t), ZM_MEM_PSRAM);
  if (starts == NULL || spans == NULL) {
    host->free(starts);
    host->free(spans);
    return -1;
  }
  int count = 0;
  for (uint32_t i = 0; i < instruments; ++i) {
    starts[count++] = rd32(table_ins + 4 * i);
  }
  for (uint32_t i = 0; i < samples; ++i) {
    const uint32_t at = rd32(table_smp + 4 * i);
    starts[count++] = at;
    starts[count++] = at != 0 && inside(at, SAMPLE_HEADER, size) ? rd32(s->head + at + 0x48) : 0;
  }
  for (uint32_t i = 0; i < patterns; ++i) {
    starts[count++] = rd32(table_pat + 4 * i);
  }
  starts[count++] = (special & 1) && message_length != 0 ? message : 0;

  /* Прогон чтений загрузчика IT и оценка памяти. */
  replay_t r = {spans, 0, known * 2 + 8, head, head, 0};
  replay_read(&r, 0, tables);
  for (uint32_t i = 0; i < instruments; ++i) {
    const uint32_t at = rd32(table_ins + 4 * i);
    if (at != 0 && inside(at, INSTRUMENT_HEADER, size)) {
      replay_read(&r, at, at + INSTRUMENT_HEADER);
    }
  }
  uint32_t sample_memory[IT_LEVELS] = {0, 0, 0};
  uint32_t peak[IT_LEVELS] = {0, 0, 0};               /* переходный пик загрузки сэмпла */
  for (uint32_t i = 0; i < samples; ++i) {
    const uint32_t at = rd32(table_smp + 4 * i);
    if (at == 0 || !inside(at, SAMPLE_HEADER, size)) {
      continue;
    }
    replay_read(&r, at, at + SAMPLE_HEADER);
    const uint8_t* h = s->head + at;
    const uint8_t sample_flags = h[0x12];
    const uint32_t frames = rd32(h + 0x30);
    const uint32_t data = rd32(h + 0x48);
    if (!(sample_flags & 1) || frames <= 1 || frames > 0x10000000u || data >= size) {
      continue;                                       /* как libxmp: без отсчётов */
    }
    const uint32_t width = (sample_flags & 2) ? 2 : 1;
    const uint32_t channels = (sample_flags & 4) ? 2 : 1;
    const uint64_t full = (uint64_t)frames * width * channels;
    uint64_t end = next_start(starts, count, data, size);
    if (!(sample_flags & 8) && data + full < end) {
      end = data + full;                              /* без сжатия длина известна */
    }
    replay_read(&r, data, (uint32_t)end);
    s->stereo += channels == 2;
    s->sixteen += width == 2;
    sample_memory[IT_FULL] += (uint32_t)(full + 4 * width * channels + 4 + BLOCK_OVERHEAD);
    sample_memory[IT_MONO] += (uint32_t)(frames * width + 4 * width + 4 + BLOCK_OVERHEAD);
    sample_memory[IT_MONO_8BIT] += frames + 4 + 4 + BLOCK_OVERHEAD;
    /* Сверх итоговой копии: у сжатых — буфер распаковки, у стерео — буфер
     * чередования (полный размер); сэмпл, который ужимается, ещё и сначала
     * грузится в полный размер (src/loaders/sample.c). */
    const uint64_t buffer = (sample_flags & 8) || channels == 2 ? full : 0;
    const uint64_t transient[IT_LEVELS] = {
        buffer, buffer + (channels == 2 ? full : 0),
        buffer + (channels == 2 || width == 2 ? full : 0)};
    for (int level = 0; level < IT_LEVELS; ++level) {
      if (transient[level] > peak[level]) {
        peak[level] = (uint32_t)transient[level];
      }
    }
  }
  /* Узоры: два прохода; каналы — как считает libxmp (наибольший номер в данных). */
  uint32_t rows = 0;
  int max_channel = 0;
  int exact = 1;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 0; i < patterns; ++i) {
      const uint32_t at = rd32(table_pat + 4 * i);
      if (at == 0) {
        rows += pass == 0 ? 64 : 0;
        continue;
      }
      uint32_t end = next_start(starts, count, at, size);
      if (inside(at, 8, s->head_length)) {
        const uint32_t packed = rd16(s->head + at);
        if ((uint64_t)at + 8 + packed < end) {
          end = at + 8 + packed;
        }
        if (pass == 0) {
          rows += rd16(s->head + at + 2);
        }
        if (pass == 0 && end <= s->head_length) {
          uint8_t mask[64];
          memset(mask, 0, sizeof(mask));
          static const uint8_t event_bytes[16] = {0, 1, 1, 2, 1, 2, 2, 3, 2, 3, 3, 4, 3, 4, 4, 5};
          const uint32_t pattern_rows = rd16(s->head + at + 2);
          uint32_t row = 0;
          for (uint32_t p = at + 8; row < pattern_rows && p < end;) {
            const uint8_t b = s->head[p++];
            if (b == 0) {
              ++row;
              continue;
            }
            const int c = (b - 1) & 63;
            max_channel = c > max_channel ? c : max_channel;
            if (b & 0x80) {
              if (p >= end) {
                break;
              }
              mask[c] = s->head[p++] & 0x0F;
            }
            p += event_bytes[mask[c]];
          }
        } else if (pass == 0) {
          exact = 0;
        }
      } else if (pass == 0) {
        rows += MAX_ROWS;
        exact = 0;
      }
      if (at < size) {
        replay_read(&r, at, end);
      }
    }
  }
  if ((special & 1) && message_length != 0 && message < size) {
    replay_read(&r, message, (uint64_t)message + message_length < size ? message + message_length : size);
  }
  host->free(starts);
  if (r.failed) {
    host->free(spans);
    return -1;
  }

  /* Хранимые участки: слить пересекающиеся. */
  for (int i = 1; i < r.count; ++i) {                /* вставками: участков немного */
    const span_t item = spans[i];
    int j = i - 1;
    while (j >= 0 && span_less(&item, &spans[j])) {
      spans[j + 1] = spans[j];
      --j;
    }
    spans[j + 1] = item;
  }
  int merged = 0;
  for (int i = 0; i < r.count; ++i) {
    if (merged > 0 && spans[i].start <= spans[merged - 1].end) {
      if (spans[i].end > spans[merged - 1].end) {
        spans[merged - 1].end = spans[i].end;
      }
    } else {
      spans[merged++] = spans[i];
    }
  }
  s->kept = s->head_capacity;
  if (merged > 0) {
    s->keep = (it_keep_t*)host->alloc((size_t)merged * sizeof(it_keep_t), ZM_MEM_PSRAM);
    if (s->keep == NULL) {
      host->free(spans);
      return -1;
    }
    for (int i = 0; i < merged; ++i) {
      s->keep[i].start = spans[i].start;
      s->keep[i].end = spans[i].end;
      s->keep[i].data = NULL;
      s->kept += spans[i].end - spans[i].start;
    }
    s->keeps = merged;
  }
  host->free(spans);

  /* Память libxmp: сэмплы, дорожки (узоры × каналы), инструменты, описания;
   * пик — переходные буферы сэмпла с основой libxmp при загрузке или
   * проигрыватель после неё. */
  s->channels = exact ? max_channel + 1 : 64;
  const uint32_t track_blocks = patterns * (uint32_t)s->channels;
  const uint32_t other = rows * (uint32_t)s->channels * 8u + track_blocks * (4u + BLOCK_OVERHEAD) +
                         ((flags & 4) ? instruments : samples) * INSTRUMENT_BYTES +
                         samples * SAMPLE_BYTES +
                         patterns * (16u + 4u * (uint32_t)s->channels + BLOCK_OVERHEAD) + s->kept;
  for (int level = 0; level < IT_LEVELS; ++level) {
    const uint32_t loading = peak[level] + LOAD_BASE;
    s->memory[level] = sample_memory[level] + other + (loading > PLAYER_BYTES ? loading : PLAYER_BYTES);
  }
  return 0;
}

int it_stream_rest(it_stream_t* s, uint8_t* out) {
  if (s->failed || s->stream != s->head_length) {
    return -1;
  }
  memcpy(out, s->head, s->head_length);
  for (uint32_t at = s->head_length; at < s->size;) {
    const uint32_t got = take(s, out + at, s->size - at);
    if (got == 0) {
      return -1;
    }
    at += got;
  }
  return 0;
}

void it_stream_end(it_stream_t* s) {
  if (s->host == NULL) {
    return;
  }
  for (int i = 0; i < s->keeps; ++i) {
    s->host->free(s->keep[i].data);
  }
  s->host->free(s->keep);
  s->host->free(s->head);
  s->keep = NULL;
  s->head = NULL;
  s->keeps = 0;
}
