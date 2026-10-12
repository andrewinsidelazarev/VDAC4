/* it_stream.h — IT потоком для libxmp (TRACKER.ELF).
 *
 * Файл приходит от плеера по порядку (input_read), а libxmp читает его через
 * обратные вызовы (xmp_load_module_from_callbacks). Загрузчик IT libxmp читает
 * отсчёты каждого сэмпла один раз и по номерам сэмплов, а заголовки, узоры
 * (их он проходит дважды) и сообщение — по указателям вразнобой. План: отсчёты
 * сэмплов, идущие в файле по возрастанию номеров, текут насквозь, а всё прочее
 * хранится, пока libxmp не кончит (обычно это первые десятки килобайтов).
 * Отсчёты не в своём порядке — тоже хранятся.
 *
 * По заголовкам план ещё и оценивает память libxmp (сэмплы, дорожки узоров,
 * инструменты, проигрыватель) без сжатия отсчётов, со стерео в моно и с моно
 * в 8 бит — TRACKER выбирает первое, что помещается (make_room).
 */
#ifndef IT_STREAM_H
#define IT_STREAM_H

#include <stdint.h>

#include "zm_source.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { IT_FULL = 0, IT_MONO = 1, IT_MONO_8BIT = 2, IT_LEVELS = 3 };
#define IT_SCRATCH 1024u                /* пропуск потока — кусками такой длины */

typedef struct {
  uint32_t start, end;                 /* [start, end) файла — хранить */
  uint8_t* data;                       /* NULL — поток сюда ещё не дошёл */
} it_keep_t;

typedef struct {
  const zm_host_t* host;
  uint32_t size;                       /* длина файла */
  uint32_t pos;                        /* где читает libxmp */
  uint32_t stream;                     /* байтов потока принято (место следующего) */
  uint8_t* head;                       /* начало файла: [0, head_length) */
  uint32_t head_length;
  uint32_t head_capacity;
  it_keep_t* keep;                     /* участки за головой, по возрастанию */
  int keeps;
  int failed;                          /* поток оборвался или чтение мимо хранимого */
  uint32_t memory[IT_LEVELS];          /* оценка памяти libxmp, байтов */
  uint32_t kept;                       /* байтов хранить (голова и участки) */
  int channels;                        /* каналов в узорах (как у libxmp) */
  int stereo;                          /* сэмплов-стерео */
  int sixteen;                         /* 16-битных сэмплов */
  uint8_t scratch[IT_SCRATCH];         /* пропуск потока (не на стеке задачи плеера) */
} it_stream_t;

/* Принять голову файла из потока и составить план. 0 — готово; −1 — не IT или
 * план невозможен (поток цел — it_stream_rest), а если ещё и s->failed — поток
 * оборвался или нет памяти. s — в куче (scratch). */
int it_stream_begin(it_stream_t* s, const zm_host_t* host, uint32_t size);

/* План не вышел: весь файл в out (size байтов) — голова и остаток потока. 0 —
 * готово. */
int it_stream_rest(it_stream_t* s, uint8_t* out);

/* Обратные вызовы libxmp (struct xmp_callbacks), priv — it_stream_t*. */
unsigned long it_stream_read(void* dest, unsigned long length, unsigned long count, void* priv);
int it_stream_seek(void* priv, long offset, int whence);
long it_stream_tell(void* priv);
int it_stream_close(void* priv);

/* Освободить голову и участки. */
void it_stream_end(it_stream_t* s);

#ifdef __cplusplus
}
#endif

#endif /* IT_STREAM_H */
