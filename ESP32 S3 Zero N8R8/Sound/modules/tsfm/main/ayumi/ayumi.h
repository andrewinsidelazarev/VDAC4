/* Author: Peter Sovietov */

#ifndef AYUMI_H
#define AYUMI_H

enum {
  TONE_CHANNELS = 3,
  DECIMATE_FACTOR = 8,
  FIR_SIZE = 192,
  DC_FILTER_SIZE = 1024
};

struct tone_channel {
  int tone_period;
  int tone_counter;
  int tone;
  int t_off;
  int n_off;
  int e_on;
  int volume;
  float pan_left;
  float pan_right;
};

struct interpolator {
  float c[4];
  float y[4];
};

/* VDAC4 TSFM.ELF: однополюсный фильтр постоянной составляющей (срез 10 Гц, как
 * разделительный конденсатор выхода) вместо скользящего среднего на
 * DC_FILTER_SIZE отсчётов: на 96 кГц среднее на 1024 отсчёта срезало бы бас
 * ниже ~90 Гц, а на каждый AY занимало 8 КБ. */
struct dc_filter {
  float x1;
  float y1;
};

struct ayumi {
  struct tone_channel channels[TONE_CHANNELS];
  int noise_period;
  int noise_counter;
  int noise;
  int envelope_counter;
  int envelope_period;
  int envelope_shape;
  int envelope_segment;
  int envelope;
  const float* dac_table;
  float step;
  float x;
  struct interpolator interpolator_left;
  struct interpolator interpolator_right;
  float fir_left[FIR_SIZE * 2];
  float fir_right[FIR_SIZE * 2];
  int fir_index;
  struct dc_filter dc_left;
  struct dc_filter dc_right;
  int dc_index;
  float dc_coef;
  float left;
  float right;
  /* VDAC4 TSFM.ELF: быстрый вывод ayumi_process_fast — шагов счётчиков
   * (такт/8) на выходной отсчёт. */
  float ticks;
  /* VDAC4 TSFM.ELF: ступеньки с ограниченной полосой (ayumi_process_blep):
   * будущие импульсы — кольцо из AYUMI_BLEP_TAPS, накопитель и текущий уровень. */
  float blep_left[32];
  float blep_right[32];
  float blep_sum_left;
  float blep_sum_right;
  float level_left;
  float level_right;
  int blep_pos;
  /* Ленивое расписание ayumi_process_blep: событие — через next шагов (такт/8)
   * от последней сверки; passed — целых шагов, пройденных с неё (в счётчики ещё
   * не внесены), x — доля текущего шага. С TSFM.ELF 1.12 время — целое число
   * шагов и доля меньше 1: до события бывают тысячи шагов, и float-остаток
   * (прежний until) на таком расстоянии терял точность на каждом отсчёте. */
  int next;
  int passed;
  int dirty;
  /* TSFM.ELF 1.11: панорама ровно ABC (A — влево, B — поровну, C — вправо):
   * уровень считается короткой формулой (fast_level). */
  int abc;
  /* Замер: событий счётчиков и ступенек (смен уровня) — для журнала TSFM. */
  unsigned events;
  unsigned steps;
  /* TSFM.ELF 1.12: источник, которого сейчас не слышно (тон выключенного или
   * молчащего канала, шум без слушателей, огибающая без пользователей или на
   * удержании), выпадает из расписания событий: его счётчик стоит, а tick —
   * общий счёт шагов (такт/8). Когда источник снова нужен или меняют его
   * регистр, он догоняется арифметикой за tick - *_since шагов (lazy_wake_*). */
  unsigned tick;
  int skip_tone[TONE_CHANNELS];
  int skip_noise;
  int skip_envelope;
  unsigned tone_since[TONE_CHANNELS];
  unsigned noise_since;
  unsigned envelope_since;
};

int ayumi_configure(struct ayumi* ay, int is_ym, float clock_rate, int sr);
void ayumi_set_pan(struct ayumi* ay, int index, float pan, int is_eqp);
void ayumi_set_tone(struct ayumi* ay, int index, int period);
void ayumi_set_noise(struct ayumi* ay, int period);
void ayumi_set_mixer(struct ayumi* ay, int index, int t_off, int n_off, int e_on);
void ayumi_set_volume(struct ayumi* ay, int index, int volume);
void ayumi_set_envelope(struct ayumi* ay, int period);
void ayumi_set_envelope_shape(struct ayumi* ay, int shape);
void ayumi_process(struct ayumi* ay);
/* VDAC4 TSFM.ELF: тот же генератор, отсчёт — точное среднее уровня за свой
 * интервал (прямоугольное окно), без восьмикратной передискретизации и
 * 192-точечного фильтра ayumi_process; в 4-5 раз дешевле. */
void ayumi_process_fast(struct ayumi* ay);
/* VDAC4 TSFM.ELF: тот же генератор, каждое изменение уровня внутри отсчёта —
 * ступенька с ограниченной полосой (импульс sinc с окном Блэкмана, 32 точки,
 * срез 19,8 кГц) в точный момент события. Ровно примерно до 18 кГц, без
 * зеркальных частот; задержка — 16 отсчётов. ayumi_process_fast (среднее за
 * отсчёт) терял верха: -1,7 дБ на 15 кГц, -3,2 дБ на 20 кГц. */
void ayumi_process_blep(struct ayumi* ay);
void ayumi_remove_dc(struct ayumi* ay);

#endif
