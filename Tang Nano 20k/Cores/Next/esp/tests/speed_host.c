
#define main old_main
#include "app_host.c"
#undef main
static float audio_captured[32];static unsigned audio_captured_n;
static int bad_preference_read(uint32_t a,uint32_t*w,uint32_t n){
 int r=f_read(a,w,n);if(a==0x80)*w^=1u;return r;
}
static int audio_boundary_read(uint32_t a,uint32_t*w,uint32_t n){
 if(a>=W_AUDIO){for(uint32_t i=0;i<n;i++){
  uint32_t raw=4088u+(a-W_AUDIO)/4u+i;
  w[i]=(raw<<19)|(raw<<3);
 }return ZM_OK;}
 return f_read(a,w,n);
}
static int audio_boundary_write(const float*frames,uint32_t n){
 uint32_t take=n>5?5:n;
 for(uint32_t i=0;i<take*2;i++)audio_captured[2*audio_captured_n+i]=frames[i];
 audio_captured_n+=take;return (int)take;
}
int main(void){
 if(old_main())return 1;
 /* Проверяем преобразование реального app.c по границе знака старого кода.
    Частичный pcm_write не должен повторно усиливать остаток блока. */
 fake.link_read=audio_boundary_read;fake.pcm_write=audio_boundary_write;
 running=session_live=1;audio_feed=0;audio_wp=16;audio_captured_n=0;
 next_audio_normalizer_reset(48000);
 audio_pos_t boundary_pos={0,now_ms,0};
 CHECK(audio_step(&boundary_pos)==16 && audio_captured_n==16,"actual audio_step and partial output");
 for(unsigned i=0;i<16;i++){
  CHECK(audio_captured[2*i]==audio_captured[2*i+1],"stereo unpack");
  if(i)CHECK(audio_captured[2*i]>audio_captured[2*i-2] && audio_captured[2*i]-audio_captured[2*i-2]<0.002f,"4095/4096 has no sign discontinuity");
 }
 fake.link_read=f_read;fake.pcm_write=f_pcm_write;
 /* Кольцевые счётчики Evo: обе оси, оба знака, переход 255/0, максимум шага. */
 for(int delta=-128;delta<=127;delta++){
  ps2_hid_t kb;ps2_hid_init(&kb);uint8_t js=0,bs=0;
  load_core();nlog=0;
  const uint8_t first[]={0,255,250,5};input(first,4);input_step(&kb,&js,&bs);
  const uint8_t moved[]={0,255,(uint8_t)(250+delta),(uint8_t)(5+delta)};
  input(moved,4);int sx=0,sy=0;nlog=0;
  for(unsigned piece=0;piece<3;piece++)input_step(&kb,&js,&bs);
  for(int i=0;i<nlog;i++)if(wlog[i].a==R_MOUSE){
   sx+=(int8_t)wlog[i].v;sy+=(int8_t)(wlog[i].v>>8);
  }
  CHECK(sx==2*delta && sy==2*delta && mouse_dx==0 && mouse_dy==0,"2x mouse without wrap/clipping");
 }
 /* Действительный путь загрузка → выбор → F12 → APP_STOP → новый запуск. */
 strict_files=1;tsconfig=1;back_result=ZM_OK;fail_writes=0;
 for(unsigned speed=0;speed<4;++speed){
  CHECK(load_core() && state==2,"load core with .cfg");hw_choice=4|speed;
  leave_core();CHECK(state==3 && tsconfig,"F12 committed .cfg");
  memset(&next_cfg,0,sizeof next_cfg);next_cfg_returned=0;hw_choice=0;
  CHECK(load_core() && hw_choice==(4|speed),"reloaded ELF and FPGA restore from SD");
  leave_core();
 }
 CHECK(load_core(),"load before failure");hw_choice=5;cfg_write_error=1;
 leave_core();CHECK(state==7 && tsconfig && next_cfg.dirty,"failed file write retains retry state");
 uint8_t retry=2;command(0x7e,&retry,1);CHECK(state==5 && replies[0],"retry command accepted");
 cfg_write_error=0;next_cfg_finish();CHECK(state==3 && !next_cfg.dirty,"retry writes pending cfg");
 cfg_read_error=1;CHECK(!load_core() && tsconfig && state==255,"read failure cannot load/reset preference");cfg_read_error=0;
 strcpy(cfg_disk,"version=1\ncpu_speed=7\n");
 CHECK(!load_core() && tsconfig,"invalid speed leaves .cfg intact");
 strcpy(cfg_disk,"version=1\ncpu_speed=2\n");CHECK(load_core(),"load before bad LINK snapshot");
 const int writes_before_bad=cfg_writes;fake.link_read=bad_preference_read;
 leave_core();CHECK(state==7 && next_cfg_capture_failed && cfg_writes==writes_before_bad,"bad LINK does not overwrite SD choice");
 command(0x7e,&retry,1);next_cfg_finish();CHECK(state==7 && cfg_writes==writes_before_bad,"unavailable snapshot cannot become success");
 retry=0;command(0x7e,&retry,1);CHECK(state==3 && strstr(cfg_disk,"cpu_speed=2"),"explicit exit keeps old cfg");
 fake.link_read=f_read;
 printf("NEXT_INPUT_AUDIO_CFG checks=%d errors=%d\n",checks,bad);
 return bad!=0;
}
