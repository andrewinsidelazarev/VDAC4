/* Compare the new independent-voice path to the previous oversampling loop. */
#define main sid_module_main
#include "../modules/sid/main/sid_source.c"
#undef main
static uint32_t seed=0x831ab65u;
static unsigned rnd_voice(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(void) {
  unsigned accepted=0;
  for(unsigned trial=0;trial<200000;++trial) {
    static const uint8_t kinds[]={WV_HOLD,WV_ZERO,WV_DIGI,WV_PULSE,WV_SAW,WV_TRI,WV_NOISE};
    Voice x={0};x.kind=kinds[trial%7];x.step=(rnd_voice()&65535)*CYCLES;
    x.pw=rnd_voice()&65535;x.env=rnd_voice()&255;x.dest=rnd_voice()%3;
    x.test=(trial&8)!=0;x.digi_out=rnd_voice()&65535;
    VoiceState before={rnd_voice()&0xFFFFFF,rnd_voice()&0xFFFFFF,rnd_voice()&65535,
                       rnd_voice()&0x7FFFFF,(int)(rnd_voice()&255)-128};
    VoiceState old=before,got=before;
    const unsigned steps=1+trial%STEPS_MAX;const int real=trial&1;
    uint32_t ring[STEPS_MAX],wave[STEPS_MAX],input[STEPS_MAX]={0};
    uint8_t rise[STEPS_MAX],rise_in[STEPS_MAX]={0},got_rise=93;
    uint32_t got_ring=0x123456;
    int want_n[STEPS_MAX],want_f[STEPS_MAX],got_n[STEPS_MAX],got_f[STEPS_MAX];
    for(unsigned i=0;i<STEPS_MAX;++i) {
      want_n[i]=got_n[i]=(int)(rnd_voice()%10000)-5000;
      want_f[i]=got_f[i]=(int)(rnd_voice()%10000)-5000;
    }
    voice_pass(&x,&old,real,steps,input,rise_in,ring,rise,wave);
    if(x.dest)for(unsigned i=0;i<steps;++i)
      (x.dest==2?want_f:want_n)[i]+=((int)wave[i]-WAVE_MID)*x.env/ENV_DIV;
    const int fast=voice_flat(&x,&got,real,steps,got_n,got_f,&got_ring,&got_rise);
    if(!fast) {
      if(memcmp(&before,&got,sizeof(got)))return 2;
      continue;
    }
    if(memcmp(&old,&got,sizeof(got))||memcmp(want_n,got_n,sizeof(want_n))||
       memcmp(want_f,got_f,sizeof(want_f))||got_ring!=ring[steps-1]||got_rise!=rise[steps-1]) {
      printf("voice mismatch trial=%u kind=%u steps=%u test=%u real=%d\n",
             trial,x.kind,steps,x.test,real);return 1;
    }
    ++accepted;
  }
  printf("voice_cases=200000 fast_voice_cases=%u\n",accepted);return 0;
}
