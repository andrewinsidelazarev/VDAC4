/* Author: Peter Sovietov */

#include <string.h>
#include <math.h>
#include "ayumi.h"

static const float AY_dac_table[] = {
  0.0f, 0.0f,
  0.00999465934234f, 0.00999465934234f,
  0.0144502937362f, 0.0144502937362f,
  0.0210574502174f, 0.0210574502174f,
  0.0307011520562f, 0.0307011520562f,
  0.0455481803616f, 0.0455481803616f,
  0.0644998855573f, 0.0644998855573f,
  0.107362478065f, 0.107362478065f,
  0.126588845655f, 0.126588845655f,
  0.20498970016f, 0.20498970016f,
  0.292210269322f, 0.292210269322f,
  0.372838941024f, 0.372838941024f,
  0.492530708782f, 0.492530708782f,
  0.635324635691f, 0.635324635691f,
  0.805584802014f, 0.805584802014f,
  1.0f, 1.0f
};

static const float YM_dac_table[] = {
  0.0f, 0.0f,
  0.00465400167849f, 0.00772106507973f,
  0.0109559777218f, 0.0139620050355f,
  0.0169985503929f, 0.0200198367285f,
  0.024368657969f, 0.029694056611f,
  0.0350652323186f, 0.0403906309606f,
  0.0485389486534f, 0.0583352407111f,
  0.0680552376593f, 0.0777752346075f,
  0.0925154497597f, 0.111085679408f,
  0.129747463188f, 0.148485542077f,
  0.17666895552f, 0.211551079576f,
  0.246387426566f, 0.281101701381f,
  0.333730067903f, 0.400427252613f,
  0.467383840696f, 0.53443198291f,
  0.635172045472f, 0.75800717174f,
  0.879926756695f, 1.0f
};

static void reset_segment(struct ayumi* ay);

static int update_tone(struct ayumi* ay, int index) {
  struct tone_channel* ch = &ay->channels[index];
  ch->tone_counter += 1;
  if (ch->tone_counter >= ch->tone_period) {
    ch->tone_counter = 0;
    ch->tone ^= 1;
  }
  return ch->tone;
}

static int update_noise(struct ayumi* ay) {
  int bit0x3;
  ay->noise_counter += 1;
  if (ay->noise_counter >= (ay->noise_period << 1)) {
    ay->noise_counter = 0;
    bit0x3 = ((ay->noise ^ (ay->noise >> 3)) & 1);
    ay->noise = (ay->noise >> 1) | (bit0x3 << 16);
  }
  return ay->noise & 1;
}

static void slide_up(struct ayumi* ay) {
  ay->envelope += 1;
  if (ay->envelope > 31) {
    ay->envelope_segment ^= 1;
    reset_segment(ay);
  }
}

static void slide_down(struct ayumi* ay) {
  ay->envelope -= 1;
  if (ay->envelope < 0) {
    ay->envelope_segment ^= 1;
    reset_segment(ay);
  }
}

static void hold_top(struct ayumi* ay) {
  (void) ay;
}

static void hold_bottom(struct ayumi* ay) {
  (void) ay;
}

static void (* const Envelopes[][2])(struct ayumi*) = {
  {slide_down, hold_bottom},
  {slide_down, hold_bottom},
  {slide_down, hold_bottom},
  {slide_down, hold_bottom},
  {slide_up, hold_bottom},
  {slide_up, hold_bottom},
  {slide_up, hold_bottom},
  {slide_up, hold_bottom},
  {slide_down, slide_down},
  {slide_down, hold_bottom},
  {slide_down, slide_up},
  {slide_down, hold_top},
  {slide_up, slide_up},
  {slide_up, hold_top},
  {slide_up, slide_down},
  {slide_up, hold_bottom}
};

static void reset_segment(struct ayumi* ay) {
  if (Envelopes[ay->envelope_shape][ay->envelope_segment] == slide_down
    || Envelopes[ay->envelope_shape][ay->envelope_segment] == hold_top) {
    ay->envelope = 31;
    return;
  }
  ay->envelope = 0;
}

int update_envelope(struct ayumi* ay) {
  ay->envelope_counter += 1;
  if (ay->envelope_counter >= ay->envelope_period) {
    ay->envelope_counter = 0;
    Envelopes[ay->envelope_shape][ay->envelope_segment](ay);
  }
  return ay->envelope;
}

static void update_mixer(struct ayumi* ay) {
  int i;
  int out;
  int noise = update_noise(ay);
  int envelope = update_envelope(ay);
  ay->left = 0;
  ay->right = 0;
  for (i = 0; i < TONE_CHANNELS; i += 1) {
    out = (update_tone(ay, i) | ay->channels[i].t_off) & (noise | ay->channels[i].n_off);
    out *= ay->channels[i].e_on ? envelope : ay->channels[i].volume * 2 + 1;
    ay->left += ay->dac_table[out] * ay->channels[i].pan_left;
    ay->right += ay->dac_table[out] * ay->channels[i].pan_right;
  }
}

/* VDAC4 TSFM.ELF. Таблица импульсов ступенек: строка p — импульс, сдвинутый на
 * p/AYUMI_BLEP_PHASES отсчёта; сумма строки — 1 (ступенька без смещения уровня).
 * Считается один раз, при первой настройке (до отрисовки, в задаче декодера). */
#define AYUMI_BLEP_TAPS 32
#define AYUMI_BLEP_PHASES 64
static float blep_kernel[AYUMI_BLEP_PHASES + 1][AYUMI_BLEP_TAPS];
static int blep_ready;

static void ayumi_blep_init(void) {
  const float pi = 3.14159265358979f;
  const float cutoff = 0.449f;               /* 19,8 кГц от 44,1 кГц */
  int p, k;
  if (blep_ready) return;
  for (p = 0; p <= AYUMI_BLEP_PHASES; p += 1) {
    const float f = (float)p / AYUMI_BLEP_PHASES;
    float sum = 0;
    for (k = 0; k < AYUMI_BLEP_TAPS; k += 1) {
      /* Импульс в момент f + TAPS/2 от первого отсчёта строки. */
      const float x = (float)k - (float)(AYUMI_BLEP_TAPS / 2) - f;
      const float arg = 2.0f * cutoff * x;
      const float s = fabsf(arg) < 1e-6f ? 1.0f : sinf(pi * arg) / (pi * arg);
      const float w = (x + AYUMI_BLEP_TAPS / 2) / AYUMI_BLEP_TAPS;  /* 0..1 по окну */
      const float window = w <= 0.0f || w >= 1.0f ? 0.0f :
          0.42f - 0.5f * cosf(2.0f * pi * w) + 0.08f * cosf(4.0f * pi * w);
      blep_kernel[p][k] = 2.0f * cutoff * s * window;
      sum += blep_kernel[p][k];
    }
    for (k = 0; k < AYUMI_BLEP_TAPS; k += 1) blep_kernel[p][k] /= sum;
  }
  blep_ready = 1;
}

int ayumi_configure(struct ayumi* ay, int is_ym, float clock_rate, int sr) {
  int i;
  memset(ay, 0, sizeof(struct ayumi));
  ay->step = clock_rate / (sr * 8 * DECIMATE_FACTOR);
  ay->ticks = clock_rate / (sr * 8.0f);
  ay->dc_coef = 1.0f - 2.0f * 3.14159265358979f * 10.0f / (float)sr;
  ay->dirty = 1;
  ayumi_blep_init();
  ay->dac_table = is_ym ? YM_dac_table : AY_dac_table;
  ay->noise = 1;
  ayumi_set_envelope(ay, 1);
  for (i = 0; i < TONE_CHANNELS; i += 1) {
    ayumi_set_tone(ay, i, 1);
  }
  return ay->step < 1;
}

static void lazy_catch_up(struct ayumi* ay);
static void lazy_wake_tone(struct ayumi* ay, int index);
static void lazy_wake_noise(struct ayumi* ay);
static void lazy_wake_envelope(struct ayumi* ay);

void ayumi_set_pan(struct ayumi* ay, int index, float pan, int is_eqp) {
  lazy_catch_up(ay);
  if (is_eqp) {
    ay->channels[index].pan_left = sqrtf(1 - pan);
    ay->channels[index].pan_right = sqrtf(pan);
  } else {
    ay->channels[index].pan_left = 1 - pan;
    ay->channels[index].pan_right = pan;
  }
  /* TSFM.ELF 1.11: панорама ровно ABC — короткий расчёт уровня (fast_level). */
  ay->abc = ay->channels[0].pan_left == 1.0f && ay->channels[0].pan_right == 0.0f &&
            ay->channels[1].pan_left == 0.5f && ay->channels[1].pan_right == 0.5f &&
            ay->channels[2].pan_left == 0.0f && ay->channels[2].pan_right == 1.0f;
}

void ayumi_set_tone(struct ayumi* ay, int index, int period) {
  lazy_catch_up(ay);
  lazy_wake_tone(ay, index);                 /* пропущенные шаги — по старому периоду */
  period &= 0xfff;
  ay->channels[index].tone_period = (period == 0) | period;
}

void ayumi_advance_noise(struct ayumi* ay, unsigned shifts) {
  /* Регистр сдвига AY (17 бит, обратная связь от битов 0 и 3) повторяется через
   * 2^17 - 1 шагов. */
  shifts %= 131071u;
  while (shifts--) {
    const int bit0x3 = ((ay->noise ^ (ay->noise >> 3)) & 1);
    ay->noise = (ay->noise >> 1) | (bit0x3 << 16);
  }
}

void ayumi_set_noise(struct ayumi* ay, int period) {
  lazy_catch_up(ay);
  lazy_wake_noise(ay);
  period &= 0x1f;
  ay->noise_period = (period == 0) | period;
}

void ayumi_set_mixer(struct ayumi* ay, int index, int t_off, int n_off, int e_on) {
  lazy_catch_up(ay);
  ay->channels[index].t_off = t_off & 1;
  ay->channels[index].n_off = n_off & 1;
  ay->channels[index].e_on = e_on;
}

void ayumi_set_volume(struct ayumi* ay, int index, int volume) {
  lazy_catch_up(ay);
  ay->channels[index].volume = volume & 0xf;
}

void ayumi_set_envelope(struct ayumi* ay, int period) {
  lazy_catch_up(ay);
  lazy_wake_envelope(ay);
  period &= 0xffff;
  ay->envelope_period = (period == 0) | period;
}

void ayumi_set_envelope_shape(struct ayumi* ay, int shape) {
  lazy_catch_up(ay);
  lazy_wake_envelope(ay);                    /* дальше форма начинается заново */
  ay->envelope_shape = shape & 0xf;
  ay->envelope_counter = 0;
  ay->envelope_segment = 0;
  reset_segment(ay);
}

static float decimate(float* x) {
  float y = -0.0000046183113992051936f * (x[1] + x[191]) +
    -0.00001117761640887225f * (x[2] + x[190]) +
    -0.000018610264502005432f * (x[3] + x[189]) +
    -0.000025134586135631012f * (x[4] + x[188]) +
    -0.000028494281690666197f * (x[5] + x[187]) +
    -0.000026396828793275159f * (x[6] + x[186]) +
    -0.000017094212558802156f * (x[7] + x[185]) +
    0.000023798193576966866f * (x[9] + x[183]) +
    0.000051281160242202183f * (x[10] + x[182]) +
    0.00007762197826243427f * (x[11] + x[181]) +
    0.000096759426664120416f * (x[12] + x[180]) +
    0.00010240229300393402f * (x[13] + x[179]) +
    0.000089344614218077106f * (x[14] + x[178]) +
    0.000054875700118949183f * (x[15] + x[177]) +
    -0.000069839082210680165f * (x[17] + x[175]) +
    -0.0001447966132360757f * (x[18] + x[174]) +
    -0.00021158452917708308f * (x[19] + x[173]) +
    -0.00025535069106550544f * (x[20] + x[172]) +
    -0.00026228714374322104f * (x[21] + x[171]) +
    -0.00022258805927027799f * (x[22] + x[170]) +
    -0.00013323230495695704f * (x[23] + x[169]) +
    0.00016182578767055206f * (x[25] + x[167]) +
    0.00032846175385096581f * (x[26] + x[166]) +
    0.00047045611576184863f * (x[27] + x[165]) +
    0.00055713851457530944f * (x[28] + x[164]) +
    0.00056212565121518726f * (x[29] + x[163]) +
    0.00046901918553962478f * (x[30] + x[162]) +
    0.00027624866838952986f * (x[31] + x[161]) +
    -0.00032564179486838622f * (x[33] + x[159]) +
    -0.00065182310286710388f * (x[34] + x[158]) +
    -0.00092127787309319298f * (x[35] + x[157]) +
    -0.0010772534348943575f * (x[36] + x[156]) +
    -0.0010737727700273478f * (x[37] + x[155]) +
    -0.00088556645390392634f * (x[38] + x[154]) +
    -0.00051581896090765534f * (x[39] + x[153]) +
    0.00059548767193795277f * (x[41] + x[151]) +
    0.0011803558710661009f * (x[42] + x[150]) +
    0.0016527320270369871f * (x[43] + x[149]) +
    0.0019152679330965555f * (x[44] + x[148]) +
    0.0018927324805381538f * (x[45] + x[147]) +
    0.0015481870327877937f * (x[46] + x[146]) +
    0.00089470695834941306f * (x[47] + x[145]) +
    -0.0010178225878206125f * (x[49] + x[143]) +
    -0.0020037400552054292f * (x[50] + x[142]) +
    -0.0027874356824117317f * (x[51] + x[141]) +
    -0.003210329988021943f * (x[52] + x[140]) +
    -0.0031540624117984395f * (x[53] + x[139]) +
    -0.0025657163651900345f * (x[54] + x[138]) +
    -0.0014750752642111449f * (x[55] + x[137]) +
    0.0016624165446378462f * (x[57] + x[135]) +
    0.0032591192839069179f * (x[58] + x[134]) +
    0.0045165685815867747f * (x[59] + x[133]) +
    0.0051838984346123896f * (x[60] + x[132]) +
    0.0050774264697459933f * (x[61] + x[131]) +
    0.0041192521414141585f * (x[62] + x[130]) +
    0.0023628575417966491f * (x[63] + x[129]) +
    -0.0026543507866759182f * (x[65] + x[127]) +
    -0.0051990251084333425f * (x[66] + x[126]) +
    -0.0072020238234656924f * (x[67] + x[125]) +
    -0.0082672928192007358f * (x[68] + x[124]) +
    -0.0081033739572956287f * (x[69] + x[123]) +
    -0.006583111539570221f * (x[70] + x[122]) +
    -0.0037839040415292386f * (x[71] + x[121]) +
    0.0042781252851152507f * (x[73] + x[119]) +
    0.0084176358598320178f * (x[74] + x[118]) +
    0.01172566057463055f * (x[75] + x[117]) +
    0.013550476647788672f * (x[76] + x[116]) +
    0.013388189369997496f * (x[77] + x[115]) +
    0.010979501242341259f * (x[78] + x[114]) +
    0.006381274941685413f * (x[79] + x[113]) +
    -0.007421229604153888f * (x[81] + x[111]) +
    -0.01486456304340213f * (x[82] + x[110]) +
    -0.021143584622178104f * (x[83] + x[109]) +
    -0.02504275058758609f * (x[84] + x[108]) +
    -0.025473530942547201f * (x[85] + x[107]) +
    -0.021627310017882196f * (x[86] + x[106]) +
    -0.013104323383225543f * (x[87] + x[105]) +
    0.017065133989980476f * (x[89] + x[103]) +
    0.036978919264451952f * (x[90] + x[102]) +
    0.05823318062093958f * (x[91] + x[101]) +
    0.079072012081405949f * (x[92] + x[100]) +
    0.097675998716952317f * (x[93] + x[99]) +
    0.11236045936950932f * (x[94] + x[98]) +
    0.12176343577287731f * (x[95] + x[97]) +
    0.125f * x[96];
  memcpy(&x[FIR_SIZE - DECIMATE_FACTOR], x, DECIMATE_FACTOR * sizeof(float));
  return y;
}

void ayumi_process(struct ayumi* ay) {
  int i;
  float y1;
  float* c_left = ay->interpolator_left.c;
  float* y_left = ay->interpolator_left.y;
  float* c_right = ay->interpolator_right.c;
  float* y_right = ay->interpolator_right.y;
  float* fir_left = &ay->fir_left[FIR_SIZE - ay->fir_index * DECIMATE_FACTOR];
  float* fir_right = &ay->fir_right[FIR_SIZE - ay->fir_index * DECIMATE_FACTOR];
  ay->fir_index = (ay->fir_index + 1) % (FIR_SIZE / DECIMATE_FACTOR - 1);
  for (i = DECIMATE_FACTOR - 1; i >= 0; i -= 1) {
    ay->x += ay->step;
    if (ay->x >= 1) {
      ay->x -= 1;
      y_left[0] = y_left[1];
      y_left[1] = y_left[2];
      y_left[2] = y_left[3];
      y_right[0] = y_right[1];
      y_right[1] = y_right[2];
      y_right[2] = y_right[3];
      update_mixer(ay);
      y_left[3] = ay->left;
      y_right[3] = ay->right;
      y1 = y_left[2] - y_left[0];
      c_left[0] = 0.5f * y_left[1] + 0.25f * (y_left[0] + y_left[2]);
      c_left[1] = 0.5f * y1;
      c_left[2] = 0.25f * (y_left[3] - y_left[1] - y1);
      y1 = y_right[2] - y_right[0];
      c_right[0] = 0.5f * y_right[1] + 0.25f * (y_right[0] + y_right[2]);
      c_right[1] = 0.5f * y1;
      c_right[2] = 0.25f * (y_right[3] - y_right[1] - y1);
    }
    fir_left[i] = (c_left[2] * ay->x + c_left[1]) * ay->x + c_left[0];
    fir_right[i] = (c_right[2] * ay->x + c_right[1]) * ay->x + c_right[0];
  }
  ay->left = decimate(fir_left);
  ay->right = decimate(fir_right);
}

/* VDAC4 TSFM.ELF. Счётчики шагают с частотой такт/8, как в update_mixer:
 * шаг увеличивает счётчик, при достижении периода — событие (переключение тона,
 * шаг шума или огибающей) и сброс в 0. Между событиями уровень постоянен,
 * поэтому выходной отсчёт — точное среднее по времени: время до ближайшего
 * события берётся сразу, без пошагового счёта. Уровень пересчитывается в начале
 * отсчёта (громкость и микшер могли смениться между кадрами) и после события. */

/* Шагов до события счётчика: событие, когда counter + 1 >= period, то есть
 * max(1, period - counter) (TSFM.ELF 1.11: без ветвления вокруг сравнения). */
static int ticks_to_event(int counter, int period) {
  const int d = period - counter;
  return d > 1 ? d : 1;
}

static void fast_level(struct ayumi* ay, float* left, float* right) {
  int i;
  int out;
  const int noise = ay->noise & 1;
  float l = 0;
  float r = 0;
  if (ay->abc) {
    /* TSFM.ELF 1.11: при ABC веса 1/0,5/0 — произведения точные, и
     * left = a + b/2, right = b/2 + c дают тот же float, что и цикл ниже (сверено
     * на всех 32³ сочетаниях уровней), без шести весов и умножений на ноль. */
    float v[TONE_CHANNELS];
    for (i = 0; i < TONE_CHANNELS; i += 1) {
      const struct tone_channel* ch = &ay->channels[i];
      out = (ch->tone | ch->t_off) & (noise | ch->n_off);
      out *= ch->e_on ? ay->envelope : ch->volume * 2 + 1;
      v[i] = ay->dac_table[out];
    }
    {
      const float b_half = v[1] * 0.5f;
      *left = v[0] + b_half;
      *right = b_half + v[2];
    }
    return;
  }
  for (i = 0; i < TONE_CHANNELS; i += 1) {
    const struct tone_channel* ch = &ay->channels[i];
    out = (ch->tone | ch->t_off) & (noise | ch->n_off);
    out *= ch->e_on ? ay->envelope : ch->volume * 2 + 1;
    l += ay->dac_table[out] * ch->pan_left;
    r += ay->dac_table[out] * ch->pan_right;
  }
  *left = l;
  *right = r;
}

/* Сдвинуть все счётчики на k шагов (k не больше шагов до ближайшего события);
 * у кого событие на k-м шаге — выполнить его. */
static void fast_advance(struct ayumi* ay, int k) {
  /* TSFM.ELF 1.11: k не больше шагов до ближайшего события, поэтому событие на
   * k-м шаге — ровно когда counter + k >= period (и при уменьшенном периоде, когда
   * счётчик уже за ним: тогда k = 1). Прежде расстояние считалось ещё раз. */
  /* TSFM.ELF 1.12: пропущенные (неслышимые) источники стоят — их догонит
   * lazy_wake_*; tick — общий счёт шагов. */
  int i;
  ay->tick += (unsigned)k;
  for (i = 0; i < TONE_CHANNELS; i += 1) {
    struct tone_channel* ch = &ay->channels[i];
    if (ay->skip_tone[i]) continue;
    ch->tone_counter += k;
    if (ch->tone_counter >= ch->tone_period) {
      ch->tone_counter = 0;
      ch->tone ^= 1;
    }
  }
  if (!ay->skip_noise) {
    ay->noise_counter += k;
    if (ay->noise_counter >= ay->noise_period << 1) {
      int bit0x3 = ((ay->noise ^ (ay->noise >> 3)) & 1);
      ay->noise_counter = 0;
      ay->noise = (ay->noise >> 1) | (bit0x3 << 16);
    }
  }
  if (!ay->skip_envelope) {
    ay->envelope_counter += k;
    if (ay->envelope_counter >= ay->envelope_period) {
      ay->envelope_counter = 0;
      Envelopes[ay->envelope_shape][ay->envelope_segment](ay);
    }
  }
}

void ayumi_process_fast(struct ayumi* ay) {
  float remaining = ay->ticks;
  float sum_left = 0;
  float sum_right = 0;
  float left;
  float right;
  int i;
  fast_level(ay, &left, &right);
  for (;;) {
    int next = ticks_to_event(ay->noise_counter, ay->noise_period << 1);
    int e = ticks_to_event(ay->envelope_counter, ay->envelope_period);
    float until;
    if (e < next) next = e;
    for (i = 0; i < TONE_CHANNELS; i += 1) {
      e = ticks_to_event(ay->channels[i].tone_counter, ay->channels[i].tone_period);
      if (e < next) next = e;
    }
    /* До границы шага с ближайшим событием: остаток текущего шага и целые. */
    until = (1.0f - ay->x) + (float)(next - 1);
    if (remaining < until) {
      /* Событий в этом отсчёте больше нет; пройденные границы шагов — без них. */
      float end = ay->x + remaining;
      int crossed = (int)end;
      sum_left += left * remaining;
      sum_right += right * remaining;
      if (crossed > 0) {
        for (i = 0; i < TONE_CHANNELS; i += 1) ay->channels[i].tone_counter += crossed;
        ay->noise_counter += crossed;
        ay->envelope_counter += crossed;
      }
      ay->x = end - (float)crossed;
      break;
    }
    sum_left += left * until;
    sum_right += right * until;
    remaining -= until;
    fast_advance(ay, next);
    ay->x = 0;
    fast_level(ay, &left, &right);
  }
  ay->left = sum_left / ay->ticks;
  ay->right = sum_right / ay->ticks;
}

/* Изменение уровня (dl, dr) в момент f отсчёта: импульс ступеньки — в кольцо. */
static void blep_add(struct ayumi* ay, float f, float dl, float dr) {
  int p = (int)(f * AYUMI_BLEP_PHASES + 0.5f);
  int k;
  const float* kernel;
  if (p < 0) p = 0;
  if (p > AYUMI_BLEP_PHASES) p = AYUMI_BLEP_PHASES;
  kernel = blep_kernel[p];
  /* VDAC4 TSFM.ELF 1.10: коэффициент читается один раз на точку (прежде — дважды,
   * для каждой стороны), кольцо проходится двумя непрерывными кусками — от
   * blep_pos до конца и с начала — без маски индекса на каждой точке. Каждая
   * ячейка по-прежнему получает одно прибавление dl·h (dr·h) — звук бит в бит. */
  {
    const int tail = AYUMI_BLEP_TAPS - ay->blep_pos;   /* точек до конца кольца */
    float* left = ay->blep_left + ay->blep_pos;
    float* right = ay->blep_right + ay->blep_pos;
    int j;
    for (k = 0; k < tail; k += 1) {
      const float h = kernel[k];
      left[k] += dl * h;
      right[k] += dr * h;
    }
    for (j = 0; k < AYUMI_BLEP_TAPS; k += 1, j += 1) {
      const float h = kernel[k];
      ay->blep_left[j] += dl * h;
      ay->blep_right[j] += dr * h;
    }
  }
}

/* Ленивое расписание (TSFM.ELF 1.3): на 96 кГц пересчёт уровня и поиск события
 * у пяти счётчиков на каждом отсчёте удваивали цену AY. Теперь на отсчёт —
 * вычитание и накопление; счётчики сдвигаются только при событии
 * (fast_advance на весь промежуток сразу) или перед записью регистра. */

/* Ближайшее событие: next — шагов до него от последней сверки (passed = 0). */
/* --- TSFM.ELF 1.12: неслышимые источники вне расписания ----------------------
 * Канал молчит, если он без огибающей и с громкостью 0: ЦАП AY даёт 0 и на
 * ступени 0, и на ступени 1, так что ни тон, ни шум его не меняют. Тон слышен,
 * если он не выключен в микшере у неслышащего канала; шум — если его слушает
 * хотя бы один звучащий канал; огибающая — если её берёт хотя бы один канал и
 * она не на удержании (там события ничего не меняют). Состояние пропущенного
 * источника догоняется за tick - since шагов: число событий счётчика с периодом
 * period считается так же, как шли бы шаги по одному (событие, когда
 * counter + 1 >= period). Звук логически тот же; время событий во float
 * считается по меньшему числу событий, поэтому фаза ступеньки изредка может
 * сдвинуться на соседнюю (1/64 отсчёта). */

/* Шагов n со счётчика counter при периоде period: сколько событий и новый счётчик. */
static unsigned skip_events(int* counter, int period, unsigned n) {
  const unsigned p = period > 1 ? (unsigned)period : 1u;
  const unsigned first = ticks_to_event(*counter, period);
  unsigned events;
  if (n < first) {
    *counter += (int)n;
    return 0;
  }
  events = 1 + (n - first) / p;
  *counter = (int)((n - first) % p);
  return events;
}

static int channel_silent(const struct tone_channel* ch) {
  return !ch->e_on && ch->volume == 0;
}

static int envelope_holds(const struct ayumi* ay) {
  void (*const step)(struct ayumi*) = Envelopes[ay->envelope_shape][ay->envelope_segment];
  return step == hold_top || step == hold_bottom;
}

static void lazy_wake_tone(struct ayumi* ay, int index) {
  struct tone_channel* ch = &ay->channels[index];
  if (!ay->skip_tone[index]) return;
  ay->skip_tone[index] = 0;
  ch->tone ^= (int)(skip_events(&ch->tone_counter, ch->tone_period,
                                ay->tick - ay->tone_since[index]) & 1u);
}

static void lazy_wake_noise(struct ayumi* ay) {
  unsigned events;
  if (!ay->skip_noise) return;
  ay->skip_noise = 0;
  events = skip_events(&ay->noise_counter, ay->noise_period << 1, ay->tick - ay->noise_since);
  /* Регистр сдвига AY (17 бит, обратная связь от битов 0 и 3) повторяется через
   * 2^17 - 1 шагов: длинный пропуск — не больше одного полного круга. */
  events %= 131071u;
  while (events--) {
    const int bit0x3 = ((ay->noise ^ (ay->noise >> 3)) & 1);
    ay->noise = (ay->noise >> 1) | (bit0x3 << 16);
  }
}

static void lazy_wake_envelope(struct ayumi* ay) {
  unsigned events;
  void (*const* shape)(struct ayumi*);
  if (!ay->skip_envelope) return;
  ay->skip_envelope = 0;
  events = skip_events(&ay->envelope_counter, ay->envelope_period,
                       ay->tick - ay->envelope_since);
  shape = Envelopes[ay->envelope_shape];
  /* Повторяющиеся формы (оба отрезка — подъём или спад) возвращаются в то же
   * состояние через 64 шага; у остальных за 64 шага огибающая доходит до
   * удержания, дальше шаги ничего не меняют. */
  if ((shape[0] == slide_up || shape[0] == slide_down) &&
      (shape[1] == slide_up || shape[1] == slide_down)) {
    events %= 64u;
  } else if (events > 64u) {
    events = 64u;
  }
  while (events--) {
    Envelopes[ay->envelope_shape][ay->envelope_segment](ay);
  }
}

/* Какие источники слышны сейчас: ставших слышными — догнать, переставших — снять. */
static void lazy_audibility(struct ayumi* ay) {
  int noise_heard = 0;
  int envelope_heard = 0;
  int i;
  for (i = 0; i < TONE_CHANNELS; i += 1) {
    const struct tone_channel* ch = &ay->channels[i];
    const int silent = channel_silent(ch);
    const int tone_heard = !ch->t_off && !silent;
    if (!ch->n_off && !silent) noise_heard = 1;
    if (ch->e_on) envelope_heard = 1;
    if (tone_heard) {
      lazy_wake_tone(ay, i);
    } else if (!ay->skip_tone[i]) {
      ay->skip_tone[i] = 1;
      ay->tone_since[i] = ay->tick;
    }
  }
  if (noise_heard) {
    lazy_wake_noise(ay);
  } else if (!ay->skip_noise) {
    ay->skip_noise = 1;
    ay->noise_since = ay->tick;
  }
  if (envelope_heard && !envelope_holds(ay)) {
    lazy_wake_envelope(ay);
  } else if (!ay->skip_envelope) {
    ay->skip_envelope = 1;
    ay->envelope_since = ay->tick;
  }
}

static void lazy_schedule(struct ayumi* ay) {
  /* Потолок — 65536 шагов, как у самого долгого периода (огибающая): дальше
   * время во float теряло бы долю шага. */
  int next = 65536;
  int e;
  int i;
  lazy_audibility(ay);
  if (!ay->skip_noise) {
    e = ticks_to_event(ay->noise_counter, ay->noise_period << 1);
    if (e < next) next = e;
  }
  if (!ay->skip_envelope) {
    e = ticks_to_event(ay->envelope_counter, ay->envelope_period);
    if (e < next) next = e;
  }
  for (i = 0; i < TONE_CHANNELS; i += 1) {
    if (ay->skip_tone[i]) continue;
    e = ticks_to_event(ay->channels[i].tone_counter, ay->channels[i].tone_period);
    if (e < next) next = e;
  }
  ay->next = next;
  ay->passed = 0;
}

/* Перед записью регистра: пройденные с последней сверки целые шаги — в счётчики
 * (событий среди них нет: их меньше next), доля шага x остаётся. Повторный вызов
 * (несколько регистров подряд) ничего не добавляет. */
static void lazy_catch_up(struct ayumi* ay) {
  const int crossed = ay->passed;
  int i;
  if (crossed > 0) {
    /* 1.12: пропущенные источники стоят — их догонит lazy_wake_*. */
    ay->tick += (unsigned)crossed;
    for (i = 0; i < TONE_CHANNELS; i += 1) {
      if (!ay->skip_tone[i]) ay->channels[i].tone_counter += crossed;
    }
    if (!ay->skip_noise) ay->noise_counter += crossed;
    if (!ay->skip_envelope) ay->envelope_counter += crossed;
    ay->next -= crossed;
    ay->passed = 0;
  }
  ay->dirty = 1;
}

static void lazy_level(struct ayumi* ay, float f) {
  float left, right;
  fast_level(ay, &left, &right);
  if (left != ay->level_left || right != ay->level_right) {
    blep_add(ay, f, left - ay->level_left, right - ay->level_right);
    ay->level_left = left;
    ay->level_right = right;
  }
}

/* То же для события внутри отсчёта (TSFM.ELF 1.9): доля отсчёта
 * (ticks - left) / ticks считается, только если уровень сменился. Деление float
 * на ESP32-S3 — библиотечный вызов (__divsf3), а у большинства событий (шум без
 * смены бита, удержанная огибающая, выключенный тон) ступеньки нет. Выражение
 * то же, что прежде, — звук бит в бит прежний. */
static void lazy_level_at(struct ayumi* ay, float ticks, float left_in_sample) {
  float left, right;
  fast_level(ay, &left, &right);
  if (left != ay->level_left || right != ay->level_right) {
    ay->steps += 1;
    blep_add(ay, (ticks - left_in_sample) / ticks, left - ay->level_left,
             right - ay->level_right);
    ay->level_left = left;
    ay->level_right = right;
  }
}

void ayumi_process_blep(struct ayumi* ay) {
  const float ticks = ay->ticks;
  float left_in_sample = ticks;
  if (ay->dirty) {
    /* Регистры сменились между кадрами: новое расписание и ступенька в начале. */
    ay->dirty = 0;
    lazy_schedule(ay);
    lazy_level(ay, 0.0f);
  }
  for (;;) {
    /* До события: целые шаги (точно) минус доля текущего шага (меньше 1). */
    const float until = (float)(ay->next - ay->passed) - ay->x;
    if (until > left_in_sample) break;
    left_in_sample -= until;
    ay->events += 1;
    fast_advance(ay, ay->next);
    ay->passed = 0;
    ay->x = 0;
    lazy_level_at(ay, ticks, left_in_sample);
    lazy_schedule(ay);
  }
  {
    /* Остаток отсчёта: целые шаги — в passed, доля — в x. */
    const float pos = ay->x + left_in_sample;
    const int whole = (int)pos;
    ay->passed += whole;
    ay->x = pos - (float)whole;
  }
  /* Выход — накопленные ступеньки (с утечкой: ошибка округления не копится). */
  ay->blep_sum_left = ay->blep_sum_left * 0.99999f + ay->blep_left[ay->blep_pos];
  ay->blep_sum_right = ay->blep_sum_right * 0.99999f + ay->blep_right[ay->blep_pos];
  ay->blep_left[ay->blep_pos] = 0;
  ay->blep_right[ay->blep_pos] = 0;
  ay->blep_pos = (ay->blep_pos + 1) & (AYUMI_BLEP_TAPS - 1);
  ay->left = ay->blep_sum_left;
  ay->right = ay->blep_sum_right;
}

static float dc_filter(struct dc_filter* dc, float coef, float x) {
  const float y = x - dc->x1 + coef * dc->y1;
  dc->x1 = x;
  dc->y1 = y;
  return y;
}

void ayumi_remove_dc(struct ayumi* ay) {
  ay->left = dc_filter(&ay->dc_left, ay->dc_coef, ay->left);
  ay->right = dc_filter(&ay->dc_right, ay->dc_coef, ay->right);
}
