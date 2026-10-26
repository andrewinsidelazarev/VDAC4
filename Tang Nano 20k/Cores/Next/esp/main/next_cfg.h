/* Файл /CORES/<имя ядра>.cfg принадлежит приложению Next.
 * До загрузки FPGA читаем карту; перед выходом снимаем выбор, после core_back пишем карту.
 * Никаких настроек в NVS/flash ESP и никаких новых API настроек.
 */
#include "core_cfg.h"
static core_cfg_t next_cfg;
static int next_cfg_returned, next_cfg_capture_failed;

static int next_choice_read(unsigned *value)
{
    uint32_t before=0,after=0,a=0,b=0;
    if (core_version<0x03010011u || (core_version>>16)!=0x0301u ||
        link_rd(4,&before)!=ZM_OK || before!=core_version) return 0;
    for(unsigned retry=0;retry<3;++retry) {
        if(link_rd(0x80,&a)!=ZM_OK || link_rd(0x80,&b)!=ZM_OK) return 0;
        if(a==b) {
            unsigned v=(a>>8)&255u;
            if((a>>16)!=0x5350u || v>7 || (a&255u)!=(v^255u)) return 0;
            if(link_rd(4,&after)!=ZM_OK || after!=before) return 0;
            *value=v; return 1;
        }
    }
    return 0;
}

static int next_cfg_prepare(void)
{
    next_cfg_returned=next_cfg_capture_failed=0;
    if(core_cfg_load(&next_cfg,core,core_name,&running)!=ZM_OK) {
        fail("Cannot read core .cfg; file kept intact"); return 0;
    }
    unsigned value=0;
    if(core_cfg_integer(&next_cfg,"cpu_speed",3,&value,NULL,NULL)<0) {
        fail("Invalid cpu_speed in core .cfg; file kept intact"); return 0;
    }
    return 1;
}

static int next_cfg_apply(void)
{
    unsigned value=0,got=0;
    if(!next_choice_read(&got)) return 0;
    int found=core_cfg_integer(&next_cfg,"cpu_speed",3,&value,NULL,NULL);
    if(found<0)return 0;
    if(!found)return 1;
    return link_wr(0x80,0x53500004u|value)==ZM_OK && next_choice_read(&got) && got==(4u|value);
}

static int next_cfg_capture(void)
{
    unsigned value=0;
    if(!next_choice_read(&value))return 0;
    return !(value&4) || core_cfg_set(&next_cfg,"cpu_speed",value&3,3);
}

static void next_cfg_finish(void)
{
    if(next_cfg_capture_failed) {
        strcpy(error_text,"Cannot read CPU choice; old .cfg kept. Esc exits");state=7;return;
    }
    if(core_cfg_save(&next_cfg,core,&running)==ZM_OK) {
        error_text[0]=0; state=3;
    } else {
        strcpy(error_text,"Core .cfg write failed. Enter retries; Esc discards");
        state=7;
    }
}

static int next_cfg_retry(uint8_t cmd,const uint8_t *p,uint16_t n)
{
    uint8_t ok=0;
    if(cmd!=0x7e)return ZM_ERROR;
    if(n==1 && state==7 && next_cfg_returned) {
        if(p[0]==0){state=3;ok=1;}
        else if(p[0]==2){state=5;ok=1;core->signal_give(wake);}
    }
    return core->reply(0xfe,&ok,1);
}
