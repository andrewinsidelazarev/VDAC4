/* SPDX-License-Identifier: GPL-3.0-or-later
 * F11 -> SETUP: picture size and the compatibility mode. No guest key is intercepted here.
 *
 * 8 ноября 2026 (слой 0.25): пункт ASPECT RATIO убран — флаг формата AVI в ядре 0.13.3 всегда 16:9 (монитор
 * пользователя его всё равно не учитывал). VIDEO.CFG прежних слоёв читается, формат в нём не используется.
 * Режим совместимости (MODE.CFG) — для старых демок и игр под A500: 68000 (кадры исключений 68000, адрес 24 бита),
 * без fast RAM, без турбо. Ядро берёт его только при сбросе Amiga: ESP пишет его до загрузки ROM (процессор ещё в
 * сбросе), а после смены в меню спрашивает перезапуск сеанса (ядро и ROM заново, app.c restart_req).
 */
#define VIDEO_CFG "/AMIGA/VIDEO.CFG"
#define MODE_CFG "/AMIGA/MODE.CFG"
#define R_VIDEO 0x1A0u
#define R_VIDEO_CAP 0x1A4u
#define R_MODE 0x1ACu       /* бит 0 — режим совместимости (AMIGA.BIN 0.13.3+) */
#define R_MODE_CAP 0x1B0u   /* "AMC1": ядро знает R_MODE */
static uint8_t video_config;              /* размер: 0 — 100 %, 1 — 95 %, 2 — 90 % */
static uint8_t mode_compat, mode_supported;
static const char *const video_names[]={"PICTURE SIZE","COMPATIBILITY"};
enum { VIDEO_COUNT=sizeof(video_names)/sizeof(video_names[0]) };
/* "AMIGA-VIDEO 1 <формат 0|1> <размер 0..2>": формат — от прежних слоёв, не используется */
static int video_parse(const uint8_t *p,size_t n) {
    static const char prefix[]="AMIGA-VIDEO 1 ";
    unsigned v[2];size_t at=sizeof prefix-1;
    if(n<at+3 || memcmp(p,prefix,at))return 0;
    for(unsigned i=0;i<2;i++) {
        if(at>=n || p[at]<'0' || p[at]>'2')return 0;
        v[i]=p[at++]-'0';
        if(i<1 && (at>=n || p[at++]!=' '))return 0;
    }
    while(at<n && (p[at]=='\r' || p[at]=='\n'))at++;
    if(at!=n || v[0]>1 || v[1]>2)return 0;
    video_config=(uint8_t)v[1];
    return 1;
}
static int video_apply(void) {
    uint32_t cap=0,back=0;
    return rd(R_VIDEO_CAP,&cap) && cap==0x41565331u &&
        wr(R_VIDEO,video_config) && rd(R_VIDEO,&back) && back==video_config;
}
static int video_read_config(void) {
    uint8_t *text=NULL;size_t n=0;video_config=0;
    int r=core->sd_read(VIDEO_CFG,&text,&n,64,&running);
    if(r==ZM_OK && !video_parse(text,n))core->log("amiga: %s ignored, bad contents",VIDEO_CFG);
    else if(r!=ZM_OK && r!=ZM_NOT_FOUND)core->log("amiga: cannot read %s: %d",VIDEO_CFG,r);
    if(text)core->free(text);
    return video_apply();
}
static int video_save_config(void) {
    char text[48];
    int n=snprintf(text,sizeof text,"AMIGA-VIDEO 1 0 %u\n",(unsigned)(video_config&3));
    return core->sd_write(VIDEO_CFG,(const uint8_t *)text,(size_t)n,&running)==ZM_OK;
}

/* ---- режим совместимости ---- */
/* "AMIGA-MODE 1 <0|1>" */
static int mode_parse(const uint8_t *p,size_t n) {
    static const char prefix[]="AMIGA-MODE 1 ";
    size_t at=sizeof prefix-1;
    if(n<at+1 || memcmp(p,prefix,at) || (p[at]!='0' && p[at]!='1'))return 0;
    unsigned v=(unsigned)(p[at++]-'0');
    while(at<n && (p[at]=='\r' || p[at]=='\n'))at++;
    if(at!=n)return 0;
    mode_compat=(uint8_t)v;
    return 1;
}
/* В ядро. 1 — ядро режим знает и значение приняло; mode_supported — знает ли ядро режим вообще. */
static int mode_apply(void) {
    uint32_t cap=0,back=0;
    mode_supported=(uint8_t)(rd(R_MODE_CAP,&cap) && cap==0x414D4331u);
    return mode_supported && wr(R_MODE,mode_compat) && rd(R_MODE,&back) && (back&1u)==mode_compat;
}
/* Перед загрузкой ROM, пока процессор в сбросе: ядро берёт режим сразу. Ядро без режима — не ошибка. */
static void mode_read_config(void) {
    uint8_t *text=NULL;size_t n=0;mode_compat=0;
    int r=core->sd_read(MODE_CFG,&text,&n,64,&running);
    if(r==ZM_OK && !mode_parse(text,n))core->log("amiga: %s ignored, bad contents",MODE_CFG);
    else if(r!=ZM_OK && r!=ZM_NOT_FOUND)core->log("amiga: cannot read %s: %d",MODE_CFG,r);
    if(text)core->free(text);
    if(!mode_apply() && mode_compat) { core->log("amiga: compatibility mode needs AMIGA.BIN 0.13.3 or newer"); mode_compat=0; }
    if(mode_compat)core->log("amiga: compatibility mode: 68000, no fast RAM, no turbo");
}
static int mode_save_config(void) {
    char text[32];
    int n=snprintf(text,sizeof text,"AMIGA-MODE 1 %u\n",(unsigned)mode_compat);
    return core->sd_write(MODE_CFG,(const uint8_t *)text,(size_t)n,&running)==ZM_OK;
}

/* Enter на строке (defaults — Del: размер 100 %, режим выключен). Новый режим ядро возьмёт при сбросе — после
 * смены меню спрашивает сброс сейчас (Enter — да, Esc — позже: режим вступит при следующем сбросе). */
static void video_change(int defaults,unsigned row) {
    uint8_t before=video_config, mode_before=mode_compat;
    int saved=1;
    if(defaults) { video_config=0; if(mode_supported)mode_compat=0; }
    else if(row==0)video_config=(uint8_t)((video_config+1u)%3u);
    else if(row==1) {
        if(!mode_supported) { menu_set_status("UPDATE AMIGA.BIN FOR THE COMPATIBILITY MODE",C_YELLOW); return; }
        mode_compat^=1u;
    }
    else return;
    if(defaults || video_config!=before) {
        if(!video_apply()) {
            video_config=before; mode_compat=mode_before;
            menu_set_status("VIDEO CHANGE FAILED, TRY AGAIN",C_YELLOW);
            return;
        }
        saved&=video_save_config();
    }
    if(mode_compat!=mode_before) {
        if(!mode_apply()) { mode_compat=mode_before; menu_set_status("MODE CHANGE FAILED, TRY AGAIN",C_YELLOW); return; }
        saved&=mode_save_config();
        if(saved) {
            menu_set_status(mode_compat?"RESTART AMIGA NOW AS 68000 WITHOUT FAST RAM?":"RESTART AMIGA NOW AS 68020 WITH FAST RAM?",C_YELLOW);
            menu_mode=MODE_RESTART;   /* полный перезапуск сеанса, не тёплый сброс */
            return;
        }
    }
    if(!saved)menu_set_status("NOT SAVED: TANG SD, WORKS UNTIL EXIT",C_YELLOW);
    else menu_status[0]=0;
}
static const char *video_value(unsigned row) {
    if(row==0)return (video_config&3)==1 ? "95%":(video_config&3)==2 ? "90%":"100%";
    if(row==1)return !mode_supported ? "N/A" : mode_compat ? "68000, NO FAST RAM" : "OFF";
    return "";
}
