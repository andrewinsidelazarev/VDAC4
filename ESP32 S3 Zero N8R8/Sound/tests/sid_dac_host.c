/* A calibrated 1 kHz sine encoded in all eight D418 bits. The old 4-bit volume
 * split fails this test with a nonlinear output and DC. Also tests that a 2SID
 * sample in the left DAC does not leak into a silent right DAC. */
#define main sid_module_main
#include "../modules/sid/main/sid_source.c"
#undef main
#include <math.h>

int main(void) {
  cRSID_init(RATE,0);
  cRSID_C64.RealSIDmode=1;
  memset(&synth,0,sizeof(synth));
  synth.chips=2;synth.stereo=CRSID_CHANNELMODE_NARROW;
  synth.attenuation=16;synth.sample_clock_ratio=357;
  synth.volume_mul=4096;synth.lfsr=0x55A5AA;
  Snap snap={0};
  for(unsigned n=1;n<=2;++n) {
    synth.sid[n]=cRSID_C64.SID[1];
    synth.sid[n].ChipModel=8580;
    synth.sid[n].Channel=n==1?CRSID_CHANNEL_LEFT:CRSID_CHANNEL_RIGHT;
    synth.sid[n].BasePtrRD=synth.rd[n];
    uint8_t* r=snap.chip[n-1];
    r[4]=r[11]=r[18]=0x49;r[6]=r[13]=r[20]=0xFF;
    r[21]=r[22]=0xFF;r[23]=3;
    r[SNAP_ENV]=r[SNAP_ENV+1]=r[SNAP_ENV+2]=255;
    r[24]=0x17;
  }
  double sum=0,energy=0,si=0,co=0,right=0;
  const unsigned pre=RATE/4,nframes=RATE;
  for(unsigned i=0;i<pre+nframes;++i) {
    double phase=2*3.141592653589793*1000*i/RATE;
    double target=0.8*sin(phase);
    unsigned nearest=0;double distance=1e20;
    for(unsigned code=0;code<256;++code) {
      double err=fabs(mahoney_dac_8580[code]/16384.0-target);
      if(err<distance){distance=err;nearest=code;}
    }
    snap.chip[0][24]=nearest;
    int16_t pcm[2];synth_sample(&snap,pcm);
    if(i<pre)continue;
    double x=pcm[0]/32768.0,y=pcm[1]/32768.0;
    sum+=x;energy+=x*x;si+=x*sin(phase);co+=x*cos(phase);right+=y*y;
  }
  const double signal=2*(si*si+co*co)/nframes;
  const double residual=energy-signal;
  const double snr=10*log10(signal/residual);
  printf("dc=%f snr_db=%.2f right_rms=%f\n",sum/nframes,snr,sqrt(right/nframes));
  /* The measured ladder has nonuniform steps: effective precision is below
   * an ideal 8-bit DAC. Its sine still must exceed 30 dB SNR. */
  if(fabs(sum/nframes)>0.001 || snr<30 || sqrt(right/nframes)>0.0001)return 1;
  /* Ordinary SID configurations must stay on their existing output path. */
  synth.sid[1].BasePtr=snap.chip[0];
  snap.chip[0][23]=7;
  if(mahoney_mode(&synth.sid[1],snap.chip[0]))return 2;
  snap.chip[0][23]=3;synth.sid[1].ChipModel=6581;
  if(mahoney_mode(&synth.sid[1],snap.chip[0]))return 3;
  return 0;
}
