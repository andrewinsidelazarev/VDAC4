/* Cache only coefficients; compare all integrator/volume state with cRSID. */
#define main sid_module_main
#include "../modules/sid/main/sid_source.c"
#undef main
static uint32_t seed=0x184ccc65u;
static unsigned rnd_params(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(void) {
  cRSID_init(RATE,0);
  VoiceCache params={0};
  uint8_t regs[SNAP_REGS],env[3];
  unsigned cases=0;
  for(unsigned i=0;i<200000;++i) {
    if(i%3==0)for(unsigned j=0;j<SNAP_REGS;++j)regs[j]=rnd_params();
    for(unsigned j=0;j<3;++j)env[j]=rnd_params();
    cRSID_SIDinstance old={0};
    old.ChipModel=(i&4)?8580:6581;old.BasePtr=regs;
    old.NonFiltedSample=(int)(rnd_params()%64000)-32000;
    old.FilterInputSample=(int)(rnd_params()%64000)-32000;
    old.PrevBandPass=(int)(rnd_params()%64000)-32000;
    old.PrevLowPass=(int)(rnd_params()%64000)-32000;
    old.PrevVolume=rnd_params()%(15*4096+1);old.Digi=(int)(rnd_params()%8192)-4096;
    cRSID_C64.RealSIDmode=i&1;
    cRSID_SIDinstance got=old;
    synth_prepare_params(&params,&got,regs,env,cRSID_C64.RealSIDmode);
    for(unsigned j=0;j<3;++j) {
      unsigned want_env=got.ChipModel==8580?env[j]:cRSID_ADSR_DAC_6581[env[j]];
      if(params.voice[j].env!=(int)want_env)return 2;
    }
    const int expected=cRSID_emulateSIDoutputStage(&old);
    const int actual=synth_cached_output(&got,&params);
    if(actual!=expected||memcmp(&old,&got,sizeof(got))) {
      printf("filter mismatch case=%u model=%u real=%u\n",i,got.ChipModel,cRSID_C64.RealSIDmode);return 1;
    }
    ++cases;
  }
  printf("parameter_filter_cases=%u\n",cases);return 0;
}
