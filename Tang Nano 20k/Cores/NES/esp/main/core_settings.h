/* Настройки NES — /CORES/nes.cfg на карте с NES.BIN. Игровые .sav — отдельно.
 * aspect_wide — пропорции (F7 в ядре, ESP только сохраняет); picture_size (0.8, ядро 0.9) — размер картинки
 * против вылета за край на старых HDMI-приёмниках: 0 — AUTO (как SIZE в TS-Config, слот 0), 1 — 100 %,
 * 2 — 95 %, 3 — 90 %. AUTO читает регистр CTRL слота 0 (LINK 0x40, биты 16:15) перед загрузкой ядра NES. */
#include "core_cfg.h"
static core_cfg_t nes_cfg;
static int nes_cfg_pending, nes_cfg_capture_failed;
#define R_TS_CTRL   0x40u         /* CTRL TS-Config / FT812: SIZE — биты 16:15 (0 — 100 %, 1 — 95 %, 2 — 90 %) */
#define R_NES_SIZE  0x84u         /* ядро NES 0.9: W/R {535A, size[1:0]} */
#define NES_SIZE_CORE 0x04010009u
static unsigned nes_size_cfg;     /* picture_size из .cfg: 0 — AUTO, 1…3 — 100 / 95 / 90 % */
static unsigned slot0_size;       /* SIZE слота 0: 0…2 */
static int slot0_size_known;

/* SIZE ядра слота 0 — пока оно на LINK (до core_load): TS-Config (личность 01) или FT812 (02) */
static void slot0_size_read(void)
{
    uint32_t v = 0, w = 0, ctrl = 0;
    slot0_size_known = 0;
    if (link_rd(4, &v) != ZM_OK || link_rd(4, &w) != ZM_OK || v != w) return;
    if ((v >> 24) != 0x01 && (v >> 24) != 0x02) return;
    if (link_rd(R_TS_CTRL, &ctrl) != ZM_OK) return;
    slot0_size = (ctrl >> 15) & 3;
    if (slot0_size == 3) slot0_size = 0;
    slot0_size_known = 1;
}

static unsigned nes_size_effective(void) { return nes_size_cfg ? nes_size_cfg - 1 : slot0_size; }
static unsigned nes_size_percent(unsigned s) { return s == 1 ? 95 : s == 2 ? 90 : 100; }

/* Размер — в ядро (0.9+); 1 — записан и прочитан обратно */
static int nes_size_apply(void)
{
    if (core_version < NES_SIZE_CORE) return 0;
    const unsigned s = nes_size_effective();
    uint32_t r = 0;
    return link_wr(R_NES_SIZE, 0x535A0000u | s) == ZM_OK && link_rd(R_NES_SIZE, &r) == ZM_OK &&
           (r >> 16) == 0x535A && (r & 3) == s;
}

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
    unsigned size=0;
    nes_size_cfg=core_cfg_integer(&nes_cfg,"picture_size",3,&size,NULL,NULL)==1 ? size : 0;   /* плохая строка — AUTO */
    return 1;
}
static int nes_cfg_apply(void)
{
    unsigned value=0,got=0;
    if(core_cfg_integer(&nes_cfg,"aspect_wide",1,&value,NULL,NULL)<0)return 0;
    if(!nes_aspect_read(&got))return 0;
    if(link_wr(0x80,0x41530000u|value)!=ZM_OK || !nes_aspect_read(&got) || got!=value) return 0;
    /* размер — не повод не стартовать: ядро старше 0.9 его не знает, сбой — в журнал */
    if(core_version>=NES_SIZE_CORE) {
        const unsigned s=nes_size_effective();
        if(nes_size_apply())
            core->log("nes: picture size %u%% (%s)",nes_size_percent(s),
                      nes_size_cfg ? "nes.cfg" : slot0_size_known ? "AUTO: slot 0" : "AUTO: slot 0 unknown");
        else core->log("nes: picture size was not applied");
    }
    return 1;
}
static void nes_cfg_capture(void)
{
    unsigned value=0;
    nes_cfg_capture_failed=!nes_aspect_read(&value) || !core_cfg_set(&nes_cfg,"aspect_wide",value,1) ||
        !core_cfg_set(&nes_cfg,"picture_size",nes_size_cfg,3);
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
