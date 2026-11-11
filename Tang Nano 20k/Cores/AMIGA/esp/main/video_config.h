/* SPDX-License-Identifier: GPL-3.0-or-later
 * F11 -> SETUP: picture size and the compatibility mode. No guest key is intercepted here.
 *
 * 8 ноября 2026 (слой 0.25): пункт ASPECT RATIO убран — флаг формата AVI в ядре 0.13.3 всегда 16:9 (монитор
 * пользователя его всё равно не учитывал). VIDEO.CFG прежних слоёв читается, формат в нём не используется.
 * Режим совместимости (MODE.CFG) — для старых демок и игр под A500: 68000 (кадры исключений 68000, адрес 24 бита),
 * без fast RAM, без турбо. Ядро берёт его только при сбросе Amiga: ESP пишет его до загрузки ROM (процессор ещё в
 * сбросе), а после смены в меню спрашивает перезапуск сеанса (ядро и ROM заново, app.c restart_req).
 *
 * 11 ноября 2026 (слой 0.28, ядро 0.13.9): пункт INTERLACE — как выводить чересстрочную картинку. BOTH FIELDS (по
 * умолчанию, как прежде): оба поля склеены — статичный interlace (Workbench hires-lace) стоит ровно. LAST FIELD: только
 * последнее поле, каждая строка дважды — нет гребёнки на анимации и вдвое меньше чтений SDRAM (демо «9 Fingers»:
 * в hires-lace под нагрузкой блиттера захват HDMI не успевал — штрихи в правой половине строки), но статичный
 * interlace мерцает мелкими деталями, как на ЭЛТ. Бит 2 регистра картинки (R_VIDEO); ядро без него — пункт N/A.
 */
#define VIDEO_CFG "/AMIGA/VIDEO.CFG"
#define MODE_CFG "/AMIGA/MODE.CFG"
#define R_VIDEO 0x1A0u
#define R_VIDEO_CAP 0x1A4u
#define R_MODE 0x1ACu       /* бит 0 — режим совместимости (AMIGA.BIN 0.13.3+) */
#define R_MODE_CAP 0x1B0u   /* "AMC1": ядро знает R_MODE */
#define R_LACE_CAP 0x1B4u   /* "ALF1": ядро знает бит 2 R_VIDEO — interlace, последнее поле (AMIGA.BIN 0.13.9+) */
#define VIDEO_LAST_FIELD 4u
static uint8_t video_config;              /* биты 0-1 — размер: 0 — 100 %, 1 — 95 %, 2 — 90 %; бит 2 — LAST FIELD */
static uint8_t mode_compat, mode_supported, lace_supported;
static const char *const video_names[]={"PICTURE SIZE","COMPATIBILITY","INTERLACE"};
enum { VIDEO_COUNT=sizeof(video_names)/sizeof(video_names[0]) };
/* "AMIGA-VIDEO 2 <interlace 0|1> <размер 0..2>"; прежний "AMIGA-VIDEO 1 <формат 0|1> <размер>" — формат не используется */
static int video_parse(const uint8_t *p,size_t n) {
    static const char prefix1[]="AMIGA-VIDEO 1 ", prefix2[]="AMIGA-VIDEO 2 ";
    unsigned v[2];size_t at=sizeof prefix1-1;
    int v2=n>=at && !memcmp(p,prefix2,at);
    if(n<at+3 || (!v2 && memcmp(p,prefix1,at)))return 0;
    for(unsigned i=0;i<2;i++) {
        if(at>=n || p[at]<'0' || p[at]>'2')return 0;
        v[i]=p[at++]-'0';
        if(i<1 && (at>=n || p[at++]!=' '))return 0;
    }
    while(at<n && (p[at]=='\r' || p[at]=='\n'))at++;
    if(at!=n || v[0]>1 || v[1]>2)return 0;
    video_config=(uint8_t)(v[1] | (v2 && v[0] ? VIDEO_LAST_FIELD:0u));
    return 1;
}
/* Ядро без бита 2 (до 0.13.9) — бит не пишется: прежде он был форматом 16:9 */
static int video_apply(void) {
    uint32_t cap=0,back=0,lace=0;
    lace_supported=(uint8_t)(rd(R_LACE_CAP,&lace) && lace==0x414C4631u);
    uint32_t v=lace_supported ? video_config : (video_config&3u);
    return rd(R_VIDEO_CAP,&cap) && cap==0x41565331u &&
        wr(R_VIDEO,v) && rd(R_VIDEO,&back) && back==v;
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
    int n=snprintf(text,sizeof text,"AMIGA-VIDEO 2 %u %u\n",(unsigned)((video_config&VIDEO_LAST_FIELD)!=0),
                   (unsigned)(video_config&3));
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
    else if(row==0)video_config=(uint8_t)(((video_config&3u)+1u)%3u | (video_config&VIDEO_LAST_FIELD));
    else if(row==1) {
        if(!mode_supported) { menu_set_status("UPDATE AMIGA.BIN FOR THE COMPATIBILITY MODE",C_YELLOW); return; }
        mode_compat^=1u;
    }
    else if(row==2) {
        if(!lace_supported) { menu_set_status("UPDATE AMIGA.BIN FOR THE INTERLACE CHOICE",C_YELLOW); return; }
        video_config^=VIDEO_LAST_FIELD;   /* сразу, без сброса: ядро берёт бит с начала кадра вывода */
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
    if(row==2)return !lace_supported ? "N/A" : (video_config&VIDEO_LAST_FIELD) ? "LAST FIELD" : "BOTH FIELDS";
    return "";
}
