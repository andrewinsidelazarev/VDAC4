/* Compare OSC3 phases and readback against the original cRSID HQ oscillator. */
#define main sid_module_main
#include "../modules/sid/main/sid_source.c"
#undef main
static uint32_t seed=0x9b83651u;
static unsigned rnd_osc(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(void) {
  cRSID_init(RATE,0);
  for(unsigned trial=0;trial<200000;++trial) {
    uint8_t regs[32],expected_rd[32]={0},actual_rd[32]={0};
    for(unsigned i=0;i<32;++i)regs[i]=rnd_osc();
    regs[4]&=(uint8_t)~2;regs[11]&=(uint8_t)~2;
    regs[18]=(regs[18]&0x0D)|((trial%8)<<4);
    Osc3 o={0};cRSID_SIDinstance sid={0};
    sid.ChipModel=(rnd_osc()&1)?8580:6581;sid.BasePtr=regs;sid.BasePtrRD=expected_rd;
    for(unsigned i=0;i<3;++i) {
      o.acc[i]=sid.PhaseAccu[7*i]=rnd_osc()&0xFFFFFF;
      o.prev[i]=sid.PrevPhaseAccu[7*i]=rnd_osc()&0xFFFFFF;
      sid.EnvelopeCounter[7*i]=rnd_osc()&255;
      sid.NoiseLFSR[7*i]=rnd_osc()&0x7FFFFF;
    }
    o.out3=sid.PrevWavGenOut[14]=rnd_osc()&65535;
    o.lfsr3=sid.NoiseLFSR[14];
    o.digi3=sid.PrevSounDemonDigiWF[14]=(int)(rnd_osc()&255)-128;
    o.ring=sid.RingSourceMSB=(rnd_osc()&1)?ACC_MSB:0;
    o.rise=sid.SyncSourceMSBrise=rnd_osc()&1;
    const unsigned steps=1+(trial/8)%STEPS_MAX;
    const int real=rnd_osc()&1;
    cRSID_C64.RealSIDmode=real;
    for(unsigned i=0;i<steps;++i)cRSID_emulateHQwaves(&sid,CYCLES);
    osc3_chip(&o,actual_rd,regs,steps,sid.ChipModel!=8580,real);
    int mismatch=o.ring!=sid.RingSourceMSB||o.rise!=sid.SyncSourceMSBrise||
      o.out3!=sid.PrevWavGenOut[14]||o.lfsr3!=sid.NoiseLFSR[14]||
      o.digi3!=sid.PrevSounDemonDigiWF[14]||actual_rd[0x1B]!=expected_rd[0x1B];
    for(unsigned i=0;i<3;++i)mismatch|=o.acc[i]!=(uint32_t)sid.PhaseAccu[7*i]||
                                      o.prev[i]!=(uint32_t)sid.PrevPhaseAccu[7*i];
    if(mismatch){printf("OSC3 mismatch trial=%u wf=%02X steps=%u real=%d\n",trial,regs[18],steps,real);return 1;}
  }
  puts("osc3_cases=200000");return 0;
}
