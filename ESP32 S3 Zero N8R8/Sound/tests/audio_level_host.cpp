#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "zm_level.h"
static void check(bool ok, const char* what) {
  if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}
int main() {
  for (uint32_t rate : {44100u,49715u,48000u}) {
    zm_level_t level{};
    zm_level_init(&level,rate,0.f,-18.f);
    float block[2048]{};
    for (unsigned n=0;n<rate*10u;n+=1024) {
      zm_level_process(&level,block,1024);
      for(float x:block) check(x==0.f,"silence amplified into noise");
    }
    check(level.blocks==0&&level.gain_db==0.f,"silence changes gain");
    const float expected_filter_gain=level.gain;
    // A steady 1 kHz stereo tone: level must converge to -12 LUFS,
    // with matching stereo gain and bounded, finite output.
    double last_energy=0.;
    unsigned last_count=0;
    for (uint32_t n=0;n<rate*65u;n+=1024) {
      const float previous=level.gain_db;
      for(unsigned i=0;i<1024;++i) {
        float value=.8f*sinf(float(2.0*3.14159265358979323846*1000*(n+i)/rate));
        block[2*i]=block[2*i+1]=value;
      }
      zm_level_process(&level,block,1024);
      check(fabsf(level.gain_db-previous)<=.0501f,"gain jumps faster than 0.5 dB/s");
      for(unsigned i=0;i<1024;++i) {
        check(std::isfinite(block[2*i])&&fabsf(block[2*i])<1.f,"invalid or clipped output");
        check(block[2*i]==block[2*i+1],"stereo balance changes");
        if(n>=rate*60u){last_energy+=block[2*i]*block[2*i];++last_count;}
      }
    }
    check(level.gain_db<0.f,"loud OPL cannot be attenuated");
    check(fabsf(level.song_lufs+level.gain_db+12.f)<.15f,"target not reached");
    // A 1 kHz dual-mono tone has LUFS close to its peak level in dBFS.
    const double output_db=10*log10(2*last_energy/last_count);
    check(fabs(output_db+12)<.5,"incorrect physical output level");
    const float gain=level.gain_db,linear=level.gain;
    const uint32_t blocks=level.blocks;
    zm_level_discontinuity(&level);
    check(level.gain_db==gain&&level.gain==linear&&level.blocks==blocks,"seek loses normalization");
    check(level.step_count==0&&level.step_number==0,"seek keeps partial block");
    memset(block,0,sizeof(block));
    for(unsigned i=0;i<1000;++i)zm_level_process(&level,block,1024);
    check(level.gain_db==gain&&level.blocks==blocks,"silent tail changes level");
    zm_level_init(&level,rate,0.f,-18.f);
    check(level.blocks==0&&level.gain==expected_filter_gain,"new track keeps previous state");
    for(uint32_t n=0;n<rate*45u;n+=1024) {
      for(unsigned i=0;i<1024;++i) {
        block[2*i]=.01f*sinf(float(2.0*3.14159265358979323846*1000*(n+i)/rate));
        block[2*i+1]=0.f;
      }
      zm_level_process(&level,block,1024);
      for(unsigned i=0;i<1024;++i) check(block[2*i+1]==0.f,"silent channel gets noise");
      check(level.gain_db<=18.f,"quiet input exceeds gain ceiling");
    }
    check(fabsf(level.gain_db-18.f)<.001f,"quiet input is not boosted");
    for(unsigned i=0;i<2048;++i)block[i]=(i&1)?-4.f:4.f;
    zm_level_process(&level,block,1024);
    for(float x:block)check(std::isfinite(x)&&fabsf(x)<1.f,"overload is not limited");
    printf("PASS rate=%u, output=%.3f dBFS, gain=%.3f dB, silence/seek/new track\n",rate,output_db,gain);
  }
}
