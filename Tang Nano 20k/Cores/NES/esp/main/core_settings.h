/* Настройки NES — /CORES/nes.cfg на карте с NES.BIN. Игровые .sav — отдельно. */
#include "core_cfg.h"
static core_cfg_t nes_cfg;
static int nes_cfg_pending, nes_cfg_capture_failed;

static int nes_aspect_read(unsigned *value)
{
    uint32_t a=0,b=0,version=0;
    if (core_version<0x04010005 || (core_version>>16)!=0x0401 ||
        link_rd(4,&version)!=ZM_OK || version!=core_version) return 0;
    for(unsigned i=0;i<3;++i) {
        if(link_rd(0x80,&a)!=ZM_OK || link_rd(0x80,&b)!=ZM_OK) return 0;
        if(a==b) {
            unsigned v=(a>>8)&255;
            if((a>>16)!=0x4153 || v>1 || (a&255)!=(v^255))return 0;
            if(link_rd(4,&version)!=ZM_OK || version!=core_version)return 0;
            *value=v;return 1;
        }
    }
    return 0;
}
static int nes_cfg_prepare(void)
{
    nes_cfg_pending=nes_cfg_capture_failed=0;
    unsigned value=0;
    if(core_cfg_load(&nes_cfg,core,core_name,&running)!=ZM_OK ||
       core_cfg_integer(&nes_cfg,"aspect_wide",1,&value,NULL,NULL)<0) {
        fail("Cannot read core .cfg; file kept intact");return 0;
    }
    return 1;
}
static int nes_cfg_apply(void)
{
    unsigned value=0,got=0;
    if(core_cfg_integer(&nes_cfg,"aspect_wide",1,&value,NULL,NULL)<0)return 0;
    if(!nes_aspect_read(&got))return 0;
    return link_wr(0x80,0x41530000u|value)==ZM_OK && nes_aspect_read(&got) && got==value;
}
static void nes_cfg_capture(void)
{
    unsigned value=0;
    nes_cfg_capture_failed=!nes_aspect_read(&value) || !core_cfg_set(&nes_cfg,"aspect_wide",value,1);
}
static void nes_cfg_finish(void)
{
    if(nes_cfg_capture_failed) {
        strcpy(error_text,"Cannot read aspect; previous .cfg kept. Esc exits");
        nes_cfg_pending=1;state=7;return;
    }
    if(core_cfg_save(&nes_cfg,core,&running)!=ZM_OK) {
        strcpy(error_text,"Core .cfg write failed. Enter retries; Esc discards");
        nes_cfg_pending=1;state=7;return;
    }
    nes_cfg_pending=0;error_text[0]=0;state=3;
}
static void nes_cfg_schedule(void)
{
    nes_cfg_pending=1;state=5;core->signal_give(wake);
}
static int nes_cfg_retry(const uint8_t *p,uint16_t n)
{
    uint8_t ok=0;
    if(n==1 && nes_cfg_pending && state==7) {
        if(p[0]==0){nes_cfg_pending=0;state=3;ok=1;}
        else if(p[0]==2){state=5;ok=1;core->signal_give(wake);}
    }
    return core->reply(0xfe,&ok,1);
}
