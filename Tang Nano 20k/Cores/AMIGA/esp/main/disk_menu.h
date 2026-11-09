/* SPDX-License-Identifier: GPL-3.0-or-later
 * Меню F11 поверх Amiga: выбор дискеты DF0 на microSD Tang и Evo и в сети (net_disks.h), выбор системного
 * диска HD0 (вкладка HDD), новые пустые образы на карте Tang (INS). F1…F10 заняты программами Amiga,
 * клавиши F11 на клавиатуре Amiga нет. Машина продолжает работать, клавиши и мышь идут меню.
 * Образ диска работает там, где лежит; смена диска — с подтверждением, Amiga перезапускается.
 * Вкладка KEYS: клавиатура как джойстик порта 2 (включение и назначение клавиш, /AMIGA/KEYS.CFG).
 * Выбор и отрисовка — как у меню NES (Cores/NES/esp/main/game_menu.h), окно 56 × 24 знака.
 * Разметка: 0 — шапка, 1 — путь и страница; 2, 18, 20 — разделители; 3…16 — список; 17 — полное
 * имя выбранного, когда оно не влезло в строку списка; 19 — строка состояния; 21…23 — подсказки. */
#define R_OSD_CTRL  0xC0u
#define R_OSD_BAND  0xD0u
#define R_OSD_RULE  0xD4u
#define R_OSD_BAR   0xD8u
#define W_OSD       0x3000u
#define OSD_COLS    56
#define OSD_ROWS    24
#define ROW_TITLE   0
#define ROW_PATH    1
#define MENU_TOP    3
#define MENU_LINES  14
#define ROW_TAIL    17
#define ROW_STATUS  19
#define ROW_HELP    21
#define MENU_MAX_ITEMS 6000u
#define HID_ENTER 40
#define HID_ESC   41
#define HID_BKSP  42
#define HID_TAB   43
#define HID_MINUS 45
#define HID_F4    61
#define HID_F5    62
#define HID_F11   68
#define HID_F12   69
#define HID_INS   73
#define HID_HOME  74
#define HID_PGUP  75
#define HID_DEL   76
#define HID_END   77
#define HID_PGDN  78
#define HID_RIGHT 79
#define HID_LEFT  80
#define HID_DOWN  81
#define HID_UP    82
#define G_ELLIPSIS '"'
#define G_MARK     '*'
#define G_FOLDER   '\\'
enum { C_WHITE=0, C_YELLOW=1, C_CYAN=2, C_GRAY=3 };

#define MI_DIR    1u
#define MI_PARENT 2u
#define MI_SIZE   4u
typedef struct {
    char name[256];
    uint32_t size;
    uint16_t index;     /* номер записи в каталоге: файл с карты Evo плагин отдаёт по номеру */
    uint8_t flags;
} menu_item_t;
static uint8_t osd_shadow[OSD_ROWS][64];
static uint32_t osd_band_rows, osd_rule_rows;
enum { CARD_TANG=0, CARD_EVO=1, CARD_NET=2, CARD_HDD=3, CARD_KEYS=4, CARD_VIDEO=5, CARD_COUNT=6 };
enum { MODE_LIST=0, MODE_NEW, MODE_USE, MODE_REMOVE, MODE_RESET, MODE_KEY, MODE_RESTART };   /* RESTART: слой 0.25 */
#define NEW_NAME_MAX 24u
static const uint16_t new_sizes_mb[]={32,64,128,256,512,1024,2048};
#define NEW_SIZE_COUNT (sizeof new_sizes_mb/sizeof new_sizes_mb[0])
static int menu_open, menu_card, menu_mode;
static char menu_path[CARD_COUNT][512]={"/AMIGA","/","/","/AMIGA","/","/"};
static char menu_new_name[NEW_NAME_MAX+1], menu_pick[256];
static unsigned menu_new_size=3;
static char menu_status[OSD_COLS+1];
static int menu_status_color;
static menu_item_t *menu_items;
static unsigned menu_item_n, menu_item_cap, menu_cursor, menu_disks, menu_dirs;
static int menu_truncated;

static void osd_clear(void) { memset(osd_shadow,0,sizeof osd_shadow); osd_band_rows=osd_rule_rows=0; }
static void osd_glyph(unsigned row,unsigned col,char ch,int color) {
    if (row<OSD_ROWS && col<OSD_COLS) osd_shadow[row][col]=(uint8_t)(color<<6|((unsigned)ch-0x20));
}
static unsigned osd_put(unsigned row,unsigned col,const char *text,int color,unsigned width) {
    if (row>=OSD_ROWS || col>=OSD_COLS) return 0;
    if (col+width>OSD_COLS) width=OSD_COLS-col;
    unsigned n=0; const uint8_t *p=(const uint8_t *)text;
    while (*p && n<width) {
        unsigned ch=*p++;
        if (ch>=0x80) { while ((*p&0xc0)==0x80) ++p; ch='?'; }
        if (ch>='a' && ch<='z') ch-=32;
        else if (ch=='`') ch='\'';
        else if (ch=='{') ch='(';
        else if (ch=='}') ch=')';
        else if (ch=='|') ch='!';
        else if (ch=='~') ch='-';
        else if (ch=='"' || ch=='*' || ch=='\\') ch='\'';   /* их места — значки окна */
        else if (ch<0x20 || ch>0x5f) ch='?';
        if (n+1==width && *p) ch=G_ELLIPSIS;
        osd_shadow[row][col+n++]=(uint8_t)(color<<6|(ch-0x20));
    }
    return n;
}
/* '~' переключает цвет: клавиша голубым, действие серым */
static void osd_help(unsigned row,const char *text) {
    char part[OSD_COLS+1]; unsigned col=1,k=0; int key=0;
    for (const char *p=text;;p++) {
        if (*p=='~' || !*p) {
            part[k]=0;
            if (col<OSD_COLS-1) col+=osd_put(row,col,part,key?C_CYAN:C_GRAY,OSD_COLS-1-col);
            k=0;
            if (!*p) break;
            key=!key;
        } else if (k+1<sizeof part) part[k++]=*p;
    }
}
static void osd_put_right(unsigned row,const char *text,int color) {
    unsigned n=(unsigned)strlen(text);
    if (n>OSD_COLS-2) n=OSD_COLS-2;
    osd_put(row,OSD_COLS-1-n,text,color,n);
}
static int osd_row_flush(unsigned r) {
    uint32_t words[16];
    for (unsigned w=0;w<16;w++) words[w]=little32(osd_shadow[r]+4*w);
    return core->link_write(W_OSD+r*64u,words,16)==ZM_OK;
}
static void menu_set_status(const char *text,int color) {
    strncpy(menu_status,text,OSD_COLS); menu_status[OSD_COLS]=0; menu_status_color=color;
}

/* ---- клавиатура как джойстик порта 2 ----
 * Назначенные клавиши в режиме не доходят до Amiga, остальные идут как обычно. Биты — порядок NanoMig (hid_packet 3).
 * Модификаторы HID E0…E7 здесь 104…111 (ps2_hid.h). */
#define KEYS_CFG "/AMIGA/KEYS.CFG"
/* Номер строки вкладки равен номеру действия: строка 0 — включение. Индекс без вычитаний: адрес «таблица минус 1» лёг бы
 * вне секций модуля, загрузчик ELF обнулил бы его (проверка check_module_symbols). */
enum { JA_SWITCH, JA_UP, JA_DOWN, JA_LEFT, JA_RIGHT, JA_FIRE1, JA_FIRE2, JA_COUNT };
static const uint8_t ja_bit[JA_COUNT]={0,8,4,2,1,16,32};
static const char ja_name[JA_COUNT][21]={"KEYBOARD AS JOYSTICK","UP","DOWN","LEFT","RIGHT","FIRE 1","FIRE 2"};
static const uint8_t ja_default[JA_COUNT]={0,HID_UP,HID_DOWN,HID_LEFT,HID_RIGHT,104,106};
static uint8_t ja_key[JA_COUNT]={0,HID_UP,HID_DOWN,HID_LEFT,HID_RIGHT,104,106};
static uint8_t ja_held[128], ja_mask;
static int ja_on;
static unsigned ja_pick;
/* Пока ждём новую клавишу, её значение в строке мигает: видно, что меню ждёт ввода (menu_blink_step). */
#define KEYS_BLINK_MS 250u
static uint32_t keys_blink_at;
static int keys_blink_on;

static int ja_action(unsigned code) {
    for (int a=JA_UP;a<JA_COUNT;a++) if (ja_key[a]==code) return a;
    return -1;
}
/* Что держали нажатым, больше не держат: меню открылось, очередь клавиш сброшена. */
static void ja_release_all(void) { memset(ja_held,0,sizeof ja_held); ja_mask=0; }
/* Клавиша режима: 1 — съедена, Amiga её не получит. Отпускание нажатой в меню тоже съедается. */
static int ja_input(uint16_t ev) {
    int a=ja_on?ja_action(ev&0x7fu):-1;
    if (a<0) return 0;
    if (ev&0x80u) { ja_held[ev&0x7fu]=0; ja_mask&=(uint8_t)~ja_bit[a]; }
    else { ja_held[ev&0x7fu]=1; ja_mask|=ja_bit[a]; }
    return 1;
}
/* Отдельная функция: иначе компилятор сложит low[code-40] в литерал «адрес минус смещение», см. выше. */
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline,noclone))
#endif
static const char *name_at(const char *table,unsigned width,unsigned index) { return table+width*index; }
static void key_name(unsigned code,char *out,size_t cap) {
    /* Массивы знаков, не указателей: загрузчик ELF кладёт .rodata отдельно, лишние перемещения модулю ни к чему. */
    static const char low[][10]={"ENTER","ESC","BKSP","TAB","SPACE","-","=","[","]","BACKSLASH","#",";","'","BACKQUOTE",",",".","/","CAPS"};
    static const char nav[][10]={"PRTSC","SCROLL","PAUSE","INS","HOME","PGUP","DEL","END","PGDN","RIGHT","LEFT","DOWN","UP","NUM",
                                 "PAD /","PAD *","PAD -","PAD +","PAD ENTER"};
    static const char mod[][8]={"L CTRL","L SHIFT","L ALT","L WIN","R CTRL","R SHIFT","R ALT","R WIN"};
    if (code>=4 && code<=29) snprintf(out,cap,"%c",'A'+code-4);
    else if (code>=30 && code<=38) snprintf(out,cap,"%c",'1'+code-30);
    else if (code==39) snprintf(out,cap,"0");
    else if (code>=40 && code<=57) snprintf(out,cap,"%s",name_at(low[0],sizeof low[0],code-40));
    else if (code>=58 && code<=69) snprintf(out,cap,"F%u",code-57);
    else if (code>=70 && code<=88) snprintf(out,cap,"%s",name_at(nav[0],sizeof nav[0],code-70));
    else if (code>=89 && code<=97) snprintf(out,cap,"PAD %u",code-88);
    else if (code==98) snprintf(out,cap,"PAD 0");
    else if (code==99) snprintf(out,cap,"PAD .");
    else if (code>=104 && code<=111) snprintf(out,cap,"%s",name_at(mod[0],sizeof mod[0],code-104));
    else snprintf(out,cap,"KEY %u",code);
}
/* Текст одной строкой: «вкл код код код код код код». Чужой или порченый файл — значения по умолчанию. */
static int keys_parse(const uint8_t *text,size_t size) {
    unsigned v[JA_COUNT]; size_t at=0;
    for (unsigned i=0;i<JA_COUNT;i++) {
        unsigned n=0, digits=0;
        while (at<size && text[at]==' ') at++;
        while (at<size && text[at]>='0' && text[at]<='9' && digits<3) { n=n*10+(text[at++]-'0'); digits++; }
        if (!digits) return 0;
        v[i]=n;
    }
    if (v[0]>1) return 0;
    for (unsigned i=JA_UP;i<JA_COUNT;i++) {
        if (!v[i] || v[i]>127 || v[i]==HID_F11 || v[i]==HID_F12 || v[i]==HID_ESC) return 0;
        for (unsigned k=JA_UP;k<i;k++) if (v[k]==v[i]) return 0;
    }
    ja_on=(int)v[0];
    for (unsigned i=JA_UP;i<JA_COUNT;i++) ja_key[i]=(uint8_t)v[i];
    return 1;
}
static void keys_read_config(void) {
    uint8_t *text=NULL; size_t size=0;
    ja_on=0; memcpy(ja_key,ja_default,sizeof ja_key); ja_release_all();
    int r=core->sd_read(KEYS_CFG,&text,&size,64,&running);
    for (unsigned tries=0;r==ZM_BUSY && tries<20u && running;tries++) {
        core->delay(100u);
        r=core->sd_read(KEYS_CFG,&text,&size,64,&running);
    }
    if (r==ZM_OK && !keys_parse(text,size)) core->log("amiga: %s ignored, bad contents",KEYS_CFG);
    else if (r!=ZM_OK && r!=ZM_NOT_FOUND) core->log("amiga: cannot read %s: %d",KEYS_CFG,r);
    if (text) core->free(text);
}
static int keys_save_config(void) {
    char text[64];
    int n=snprintf(text,sizeof text,"%d %u %u %u %u %u %u\n",ja_on,ja_key[JA_UP],ja_key[JA_DOWN],ja_key[JA_LEFT],ja_key[JA_RIGHT],ja_key[JA_FIRE1],ja_key[JA_FIRE2]);
    return core->sd_write(KEYS_CFG,(const uint8_t *)text,(size_t)n,&running)==ZM_OK;
}
/* Настройка действует сразу; если не записалась, то только до выхода из ядра, и это надо сказать. */
static void keys_saved(const char *done) {
    if (!keys_save_config()) menu_set_status("NOT SAVED: TANG SD, WORKS UNTIL EXIT",C_YELLOW);
    else if (done) menu_set_status(done,C_WHITE);
    else menu_status[0]=0;
}
#include "video_config.h"

static void keys_assign(unsigned code) {
    char text[OSD_COLS+1]="";
    for (unsigned a=JA_UP;a<JA_COUNT;a++) if (ja_key[a]==code && a!=ja_pick) {
        ja_key[a]=ja_key[ja_pick];     /* клавиша одна на одно действие: занятая меняется местами */
        snprintf(text,sizeof text,"SWAPPED WITH %s",ja_name[a]);
    }
    ja_key[ja_pick]=(uint8_t)code;
    keys_saved(text[0]?text:NULL);
}

/* ---- карта Evo: запросы плагину (в ответе на ВВОД), как у меню NES ----
 *   1 LIST [пропуск LE16][до 8][путь, 0] -> 0x6E [итог][ещё][число][флаги, имя, 0]…
 *   2 READ [номер LE16][путь, 0] -> 0x77…0x79 или 0x6F [код] */
#define EVO_LIST 1
#define EVO_READ 2
#define EVO_BATCH 8
#define EVO_WAIT_MS 3000u
static uint8_t evo_req[1+3+512];
static uint16_t evo_req_n;
static volatile int evo_req_pending, evo_done, evo_status, evo_more;
static volatile unsigned evo_got;
static zm_sd_entry_t *evo_out;
static unsigned evo_cap;

static void evo_reply_input(uint8_t ok) {
    uint8_t reply[1+sizeof evo_req]; uint16_t n=1;
    reply[0]=ok;
    core->mutex_lock(lock);
    if (ok && evo_req_pending) {
        memcpy(reply+1,evo_req,evo_req_n);
        n=(uint16_t)(1+evo_req_n);
        evo_req_pending=0;
    }
    core->mutex_unlock(lock);
    core->reply(0xF1,reply,n);
}
static int evo_command(uint8_t cmd,const uint8_t *p,uint16_t n) {
    const uint8_t ok=1;
    core->mutex_lock(lock);
    input_ms=core->millis();
    if (cmd==0x6E && n>=3) {
        evo_status=p[0]; evo_more=p[1];
        unsigned at=3,got=0;
        for (unsigned i=0;i<p[2] && got<evo_cap && at<n;i++) {
            zm_sd_entry_t *e=&evo_out[got];
            memset(e,0,sizeof *e);
            e->flags=p[at++]&1;
            unsigned k=0;
            while (at<n && p[at] && k+1<sizeof e->name) e->name[k++]=(char)p[at++];
            while (at<n && p[at]) at++;
            at++; got++;
        }
        evo_got=got; evo_done=1;
    } else if (cmd==0x6F && n>=1) {
        evo_status=p[0]?p[0]:255; evo_done=1;
    }
    core->mutex_unlock(lock);
    core->reply(cmd|0x80,&ok,1);
    core->signal_give(wake);
    return ZM_OK;
}
static int evo_request(const uint8_t *req,uint16_t n,uint32_t wait_ms) {
    core->mutex_lock(lock);
    memcpy(evo_req,req,n); evo_req_n=n; evo_done=0; evo_got=0; evo_status=0; evo_req_pending=1;
    core->mutex_unlock(lock);
    uint32_t start=core->millis(), seen=0;
    while (running && !exit_req && !evo_done) {
        if (rx_receiving && rx_received!=seen) { seen=rx_received; start=core->millis(); }
        if (rx_receiving && rx_ready) return 1;
        if (core->millis()-start>wait_ms) break;
        core->delay(5);
    }
    core->mutex_lock(lock); evo_req_pending=0; core->mutex_unlock(lock);
    return evo_done;
}

/* ---- список папки ---- */
static int menu_fetch(uint32_t skip,zm_sd_entry_t *out,unsigned want,unsigned *count,int *more) {
    *count=0; *more=0;
    if (menu_card!=CARD_EVO) {
        uint32_t n=0;
        int r=core->sd_list(menu_path[menu_card],menu_card==CARD_HDD?".hdf":".adf",skip,out,want,&n,more,&running);
        if (r!=ZM_OK) {
            menu_set_status(r==ZM_BUSY?"TANG SD IS BUSY, TRY AGAIN":r==ZM_NOT_FOUND?"FOLDER NOT FOUND":"CANNOT READ TANG SD",C_YELLOW);
            return 0;
        }
        *count=n; return 1;
    }
    uint8_t req[sizeof evo_req];
    req[0]=EVO_LIST; req[1]=(uint8_t)skip; req[2]=(uint8_t)(skip>>8); req[3]=(uint8_t)(want>EVO_BATCH?EVO_BATCH:want);
    size_t len=strlen(menu_path[1])+1;
    memcpy(req+4,menu_path[1],len);
    evo_out=out; evo_cap=req[3];
    if (!evo_request(req,(uint16_t)(4+len),EVO_WAIT_MS)) { menu_set_status("EVO SD: NO ANSWER, UPDATE VDAC4COR.WMF",C_YELLOW); return 0; }
    if (evo_status) { menu_set_status("FOLDER NOT FOUND ON EVO SD",C_YELLOW); return 0; }
    *count=evo_got; *more=evo_more;
    return 1;
}
static int menu_reserve(unsigned want) {
    if (want<=menu_item_cap) return 1;
    unsigned cap=menu_item_cap?menu_item_cap*2:64;
    while (cap<want) cap*=2;
    if (cap>MENU_MAX_ITEMS+1) cap=MENU_MAX_ITEMS+1;
    if (cap<want) return 0;
    menu_item_t *v=core->alloc(cap*sizeof *v,ZM_MEM_PSRAM);
    if (!v) return 0;
    if (menu_items) { memcpy(v,menu_items,menu_item_n*sizeof *v); core->free(menu_items); }
    menu_items=v; menu_item_cap=cap;
    return 1;
}
static void menu_items_free(void) {
    if (menu_items) core->free(menu_items);
    menu_items=NULL; menu_item_n=menu_item_cap=menu_cursor=menu_disks=menu_dirs=0;
}
static unsigned menu_fold(unsigned char c) { return c>='a' && c<='z'?c-32u:c; }
static int menu_item_less(const menu_item_t *a,const menu_item_t *b) {
    if ((a->flags&MI_DIR)!=(b->flags&MI_DIR)) return (a->flags&MI_DIR)!=0;
    const unsigned char *x=(const unsigned char *)a->name,*y=(const unsigned char *)b->name;
    for (;;x++,y++) {
        unsigned p=menu_fold(*x),q=menu_fold(*y);
        if (p!=q) return p<q;
        if (!p) return a->index<b->index;
    }
}
static void menu_sort(menu_item_t *v,unsigned n) {
    static const unsigned gaps[]={701,301,132,57,23,10,4,1};
    static menu_item_t t;
    for (unsigned g=0;g<sizeof gaps/sizeof gaps[0];g++) {
        unsigned gap=gaps[g];
        for (unsigned i=gap;i<n;i++) {
            t=v[i];
            unsigned j=i;
            while (j>=gap && menu_item_less(&t,&v[j-gap])) { v[j]=v[j-gap]; j-=gap; }
            v[j]=t;
        }
    }
}
static int menu_root(void) { return !strcmp(menu_path[menu_card],"/"); }
static void menu_status_flush(void);
#include "net_disks.h"
static void menu_read_dir(void) {
    static zm_sd_entry_t batch[16];
    menu_item_n=menu_cursor=menu_disks=menu_dirs=0; menu_truncated=0;
    if (!menu_root() && menu_reserve(1)) {
        memset(&menu_items[0],0,sizeof menu_items[0]);
        strcpy(menu_items[0].name,".."); menu_items[0].flags=MI_DIR|MI_PARENT;
        menu_item_n=1;
    }
    unsigned first=menu_item_n; uint32_t skip=0; int ok=1;
    if(menu_card==CARD_VIDEO) {
        menu_status[0]=0;
        if(!menu_reserve(VIDEO_COUNT))return;
        for(unsigned i=0;i<VIDEO_COUNT;i++){memset(&menu_items[i],0,sizeof menu_items[i]);strcpy(menu_items[i].name,video_names[i]);}
        menu_item_n=VIDEO_COUNT;return;
    }
    if (menu_card==CARD_KEYS) {
        menu_status[0]=0;
        if (!menu_reserve(JA_COUNT)) return;
        for (unsigned i=0;i<JA_COUNT;i++) {
            menu_item_t *it=&menu_items[i];
            memset(it,0,sizeof *it);
            strcpy(it->name,ja_name[i]);
        }
        menu_item_n=JA_COUNT;
        return;
    }
    if (menu_card==CARD_NET) {   /* 8.11.2026, слой 0.24: разделы -> буквы -> дискеты (net_disks.h) */
        menu_status[0]=0;
        (void)first;
        net_read_dir();
        return;
    }
    for (;;) {
        unsigned count=0; int more=0;
        if (!menu_fetch(skip,batch,menu_card==CARD_EVO?EVO_BATCH:16,&count,&more)) { ok=0; break; }
        for (unsigned i=0;i<count;i++) {
            if (menu_item_n-first>=MENU_MAX_ITEMS || !menu_reserve(menu_item_n+1)) { menu_truncated=1; break; }
            menu_item_t *it=&menu_items[menu_item_n++];
            memcpy(it->name,batch[i].name,sizeof it->name); it->name[sizeof it->name-1]=0;
            it->size=batch[i].size; it->index=(uint16_t)(skip+i);
            it->flags=(uint8_t)((batch[i].flags&1?MI_DIR:0)|(menu_card!=CARD_EVO?MI_SIZE:0));
            if (it->flags&MI_DIR) menu_dirs++; else menu_disks++;
        }
        skip+=count;
        if (menu_truncated || !more || !count) break;
    }
    menu_sort(menu_items+first,menu_item_n-first);
    if (ok) menu_status[0]=0;
}
static void menu_select(const char *name) {
    menu_cursor=menu_item_n>1 && (menu_items[0].flags&MI_PARENT)?1:0;
    if (!name) return;
    for (unsigned i=0;i<menu_item_n;i++) if (!strcmp(menu_items[i].name,name)) { menu_cursor=i; return; }
}

/* ---- отрисовка ---- */
static void menu_put_status(void) {
    memset(osd_shadow[ROW_STATUS],0,sizeof osd_shadow[ROW_STATUS]);
    if (menu_mode==MODE_NEW) {
        unsigned n=osd_put(ROW_STATUS,1,"NAME: ",C_GRAY,6);
        n+=osd_put(ROW_STATUS,1+n,menu_new_name,C_WHITE,NEW_NAME_MAX);
        osd_glyph(ROW_STATUS,1+n,'_',C_YELLOW);
        osd_put(ROW_STATUS,2+n,menu_card==CARD_HDD?".HDF":".ADF",C_GRAY,4);
        return;
    }
    if (menu_status[0]) { osd_put(ROW_STATUS,1,menu_status,menu_status_color,OSD_COLS-2); return; }
    /* 8.11.2026, слой 0.25: вкладка SETUP (была VIDEO) — размер и режим совместимости */
    if(menu_card==CARD_VIDEO) {osd_put(ROW_STATUS,1,menu_cursor==1?"FOR OLD A500 DEMOS AND GAMES: 68000, NO FAST RAM, NO TURBO":"SETTINGS ARE SAVED ON TANG SD",C_GRAY,OSD_COLS-2);return;}
    if (menu_card==CARD_KEYS) {
        unsigned n=osd_put(ROW_STATUS,1,"JOYSTICK PORT 2, KEYBOARD: ",C_GRAY,OSD_COLS-2);
        osd_put(ROW_STATUS,1+n,ja_on?"ON":"OFF",ja_on?C_YELLOW:C_WHITE,3);
        return;
    }
    if (menu_card==CARD_HDD) {
        unsigned n=osd_put(ROW_STATUS,1,"HD0: ",C_GRAY,5);
        osd_put(ROW_STATUS,1+n,hd0_name[0]?hd0_name:"NONE",hd0_name[0]?C_WHITE:C_GRAY,OSD_COLS-2-n);
        return;
    }
    const int locked=df0_name[0] && df0_ro;
    unsigned n=osd_put(ROW_STATUS,1,"DF0: ",C_GRAY,5);
    n+=osd_put(ROW_STATUS,1+n,df0_name[0]?df0_name:"EMPTY",df0_name[0]?C_WHITE:C_GRAY,OSD_COLS-2-n-(locked?12:0));
    if (locked) osd_put(ROW_STATUS,2+n,"(READ ONLY)",C_GRAY,11);
}
static void menu_status_flush(void) { menu_put_status(); osd_row_flush(ROW_STATUS); }
/* Строка вкладки KEYS: действие и клавиша справа. Назначаемая — жёлтая, её клавиша мигает. */
static void keys_row_put(unsigned row,unsigned line) {
    char key[16];
    const int picking=menu_mode==MODE_KEY && line==ja_pick;
    if (line) key_name(ja_key[line],key,sizeof key);
    else strcpy(key,ja_on?"ON":"OFF");
    osd_put(row,3,ja_name[line],picking?C_YELLOW:C_WHITE,OSD_COLS-16);
    if (!picking || keys_blink_on) osd_put_right(row,key,picking?C_YELLOW:line?C_CYAN:ja_on?C_YELLOW:C_GRAY);
}
static void menu_draw(void) {
    osd_clear();
    osd_band_rows=1u<<ROW_TITLE|1u<<ROW_STATUS;
    osd_rule_rows=1u<<2|1u<<(ROW_TAIL+1)|1u<<(ROW_STATUS+1);   /* линия идёт посреди своей строки: ROW_TAIL занята именем файла */
    osd_put(ROW_TITLE,1,"AMIGA",C_YELLOW,5);
    osd_put(ROW_TITLE,7,menu_card==CARD_VIDEO?"SET":menu_card==CARD_KEYS?"JOY":menu_card==CARD_HDD?"HD0":"DF0",C_WHITE,3);
    {
        static const uint8_t tab_at[CARD_COUNT]={17,23,28,33,38,44};
        static const char tab_name[CARD_COUNT][6]={"TANG","EVO","NET","HDD","KEYS","SETUP"};
        osd_glyph(ROW_TITLE,tab_at[menu_card]-1u,G_MARK,C_YELLOW);
        for (int t=0;t<CARD_COUNT;t++) osd_put(ROW_TITLE,tab_at[t],tab_name[t],t==menu_card?C_YELLOW:C_GRAY,t==CARD_VIDEO?5:4);
    }
    unsigned pages=menu_item_n?(menu_item_n+MENU_LINES-1)/MENU_LINES:1, page=menu_cursor/MENU_LINES;
    char where[24];
    snprintf(where,sizeof where,"%u/%u",page+1,pages);
    const char *path=menu_card==CARD_VIDEO?"AMIGA SETTINGS":menu_card==CARD_KEYS?"GAME CONTROLS":menu_path[menu_card];
    size_t n=strlen(path), room=OSD_COLS-4-strlen(where);
    if (n>room) { osd_glyph(ROW_PATH,1,G_ELLIPSIS,C_GRAY); osd_put(ROW_PATH,2,path+n-(room-1),C_GRAY,(unsigned)room); }
    else osd_put(ROW_PATH,1,path,C_GRAY,(unsigned)room);
    osd_put_right(ROW_PATH,where,C_GRAY);
    unsigned first=page*MENU_LINES;
    for (unsigned i=0;i<MENU_LINES && first+i<menu_item_n;i++) {
        const menu_item_t *it=&menu_items[first+i];
        unsigned row=MENU_TOP+i;
        if (first+i==menu_cursor) osd_glyph(row,1,G_MARK,C_YELLOW);
        if(menu_card==CARD_VIDEO) {
            osd_put(row,3,video_names[first+i],C_WHITE,OSD_COLS-18);
            osd_put_right(row,video_value(first+i),C_CYAN);
        } else if (menu_card==CARD_KEYS) keys_row_put(row,first+i);
        else if (it->flags&MI_DIR) {
            osd_glyph(row,3,G_FOLDER,C_CYAN);
            osd_put(row,5,it->name,C_CYAN,OSD_COLS-6);
        } else if (menu_card==CARD_HDD) {
            char mb[16];
            snprintf(mb,sizeof mb,"%uM",(unsigned)(it->size>>20));
            osd_put(row,3,it->name,C_WHITE,OSD_COLS-11);
            osd_put_right(row,mb,C_GRAY);
        } else osd_put(row,3,it->name,C_WHITE,OSD_COLS-4);
    }
    if (menu_mode==MODE_NEW) {
        if (menu_card==CARD_HDD) {
            char text[OSD_COLS+1];
            snprintf(text,sizeof text,"NEW HARD DISK  < %u MB >",(unsigned)new_sizes_mb[menu_new_size]);
            osd_put(ROW_TAIL,1,text,C_CYAN,OSD_COLS-2);
        } else osd_put(ROW_TAIL,1,"NEW FLOPPY  880 KB",C_CYAN,OSD_COLS-2);
    } else if (menu_cursor<menu_item_n) {
        const char *name=menu_items[menu_cursor].name;
        size_t len=strlen(name);
        if (len>OSD_COLS-(menu_items[menu_cursor].flags&MI_DIR?6u:menu_card==CARD_HDD?11u:4u)) {
            osd_glyph(ROW_TAIL,1,G_ELLIPSIS,C_GRAY);
            osd_put(ROW_TAIL,2,name+(len>OSD_COLS-4u?len-(OSD_COLS-4u):0),C_CYAN,OSD_COLS-3);
        }
    }
    menu_put_status();
    if (menu_mode==MODE_NEW) {
        osd_help(ROW_HELP,"~A-Z 0-9 -~ NAME  ~BKSP~ ERASE");
        if (menu_card==CARD_HDD) osd_help(ROW_HELP+1,"~LEFT/RIGHT~ SIZE");
        osd_help(ROW_HELP+2,"~ENTER~ CREATE  ~ESC~ CANCEL");
    } else if (menu_mode==MODE_KEY) {
        osd_help(ROW_HELP,"ANY KEY EXCEPT F11 AND F12");
        osd_help(ROW_HELP+2,"~ESC~ CANCEL");
    } else if (menu_mode!=MODE_LIST) {
        osd_help(ROW_HELP,"AMIGA RESTARTS, UNSAVED WORK IS LOST");
        osd_help(ROW_HELP+2,"~ENTER~ YES  ~ESC~ NO");
    } else {
        osd_help(ROW_HELP,menu_card==CARD_VIDEO?"~ENTER~ CHANGE  ~DEL~ DEFAULTS":menu_card==CARD_KEYS?(menu_cursor?"~ENTER~ NEW KEY  ~DEL~ DEFAULT KEYS":"~ENTER~ ON/OFF  ~DEL~ DEFAULT KEYS"):
                          menu_card==CARD_HDD?"~ENTER~ USE  ~DEL~ REMOVE  ~INS~ NEW":
                          menu_card==CARD_TANG?"~ENTER~ INSERT  ~DEL~ EJECT  ~INS~ NEW":
                          menu_card==CARD_NET?"~ENTER~ INSERT  ~DEL~ EJECT  ~F5~ RELOAD LIST":"~ENTER~ INSERT  ~DEL~ EJECT");
        osd_help(ROW_HELP+1,(menu_card==CARD_VIDEO || menu_card==CARD_KEYS)?"~TAB~ NEXT TAB  ~UP/DOWN~ LINE":"~TAB~ SOURCE  ~BKSP~ BACK  ~PGUP/PGDN~ PAGE");
        osd_help(ROW_HELP+2,"~ESC~ CLOSE  ~F12~ EXIT TO WC  ~F4 / CTRL+ALT+F4~ RESET");
    }
    for (unsigned r=0;r<OSD_ROWS;r++) if (!osd_row_flush(r)) return;
    wr(R_OSD_BAND,osd_band_rows); wr(R_OSD_RULE,osd_rule_rows); wr(R_OSD_BAR,0);
    unsigned sel=menu_item_n?MENU_TOP+menu_cursor%MENU_LINES:63;
    wr(R_OSD_CTRL,sel<<8|1u);
}
/* Мигание назначаемой клавиши: зовётся из input_step, в одном потоке с меню. Перерисовывается одна строка.
 * Курсор в этом режиме стоит на назначаемой строке, поэтому она на текущей странице. 0 — сбой LINK. */
static int menu_blink_step(void) {
    if (!menu_open || menu_mode!=MODE_KEY) return 1;
    uint32_t now=core->millis();
    if (now-keys_blink_at<KEYS_BLINK_MS) return 1;
    keys_blink_at=now; keys_blink_on=!keys_blink_on;
    unsigned row=MENU_TOP+ja_pick%MENU_LINES;
    memset(&osd_shadow[row][3],0,OSD_COLS-3);
    keys_row_put(row,ja_pick);
    return osd_row_flush(row);
}
static void menu_enter_dir(const char *name) {
    char *path=menu_path[menu_card]; char back[256]="";
    if (!strcmp(name,"..")) {
        char *slash=strrchr(path,'/');
        if (slash) { strncpy(back,slash+1,sizeof back-1); back[sizeof back-1]=0; }
        if (slash==path) path[1]=0; else if (slash) *slash=0;
    } else {
        size_t a=strlen(path),b=strlen(name);
        if (a+b+2>=sizeof menu_path[0]) { menu_set_status("PATH IS TOO LONG",C_YELLOW); return; }
        if (a>1) path[a]='/';
        memcpy(path+(a>1?a+1:a),name,b+1);
    }
    menu_set_status("READING FOLDER...",C_WHITE);
    menu_status_flush();
    menu_read_dir();
    menu_select(back[0]?back:NULL);
}
static void menu_close(void) { wr(R_OSD_CTRL,63u<<8); menu_open=0; menu_mode=MODE_LIST; }
static void menu_show(void) {
    menu_open=1;
    char keep[256]="";
    if (menu_item_n && menu_cursor<menu_item_n) strcpy(keep,menu_items[menu_cursor].name);
    menu_set_status("READING FOLDER...",C_WHITE);
    menu_item_n=0;
    menu_draw();
    menu_read_dir();
    menu_select(keep[0]?keep:NULL);
    menu_draw();
}
/* Полный путь файла в текущей папке вкладки. */
static int menu_file_path(char *out,size_t cap,const char *name) {
    const char *dir=menu_path[menu_card];
    size_t a=strlen(dir),b=strlen(name);
    if (a+b+2>=cap) return 0;
    memcpy(out,dir,a);
    if (a>1) out[a]='/';
    memcpy(out+(a>1?a+1:a),name,b+1);
    return 1;
}
/* Выбранный файл: с карты Tang — чтение и подключение, с карты Evo — приём от плагина. */
static void menu_load(const menu_item_t *it) {
    char text[OSD_COLS+1]="READING ";
    size_t room=OSD_COLS-8, len=strlen(it->name);
    memcpy(text+8,it->name,len<room?len:room);
    text[8+(len<room?len:room)]=0;
    menu_set_status(text,C_WHITE);
    menu_status_flush();
    const char *problem;
    if (menu_card==CARD_NET) problem=net_load(it);
    else if (menu_card==CARD_TANG) {
        char path[512];
        problem=menu_file_path(path,sizeof path,it->name)?df0_insert_tang(path,it->name):"PATH IS TOO LONG";
    } else {
        uint8_t req[sizeof evo_req]; size_t n=strlen(menu_path[1])+1;
        req[0]=EVO_READ; req[1]=(uint8_t)it->index; req[2]=(uint8_t)(it->index>>8);
        memcpy(req+3,menu_path[1],n);
        rx_discard();
        rx_receiving=1;
        int answered=evo_request(req,(uint16_t)(3+n),EVO_WAIT_MS);
        rx_receiving=0;
        if (!rx_ready) {
            rx_discard();
            problem=!answered?"EVO SD: NO ANSWER, UPDATE VDAC4COR.WMF":
                    evo_status==1?"FILE NOT FOUND ON EVO SD":"CANNOT READ THE FILE FROM EVO SD";
        } else problem=df0_insert_received(it->name);
    }
    if (problem) { menu_set_status(problem,C_YELLOW); menu_draw(); return; }
    menu_status[0]=0;
    menu_close();
}
/* ---- новые образы ---- */
static uint8_t fs_buffer[FS_BLOCK];
static int menu_name_taken(const char *name) {
    for (unsigned i=0;i<menu_item_n;i++) {
        const unsigned char *x=(const unsigned char *)menu_items[i].name,*y=(const unsigned char *)name;
        while (*y && menu_fold(*x)==menu_fold(*y)) { x++; y++; }
        if (!*y && x[0]=='.') return 1;
    }
    return 0;
}
static void menu_new_begin(void) {
    const char *base=menu_card==CARD_HDD?"HD":"DISK";
    for (unsigned n=1;n<100;n++) {
        snprintf(menu_new_name,sizeof menu_new_name,"%s%02u",base,n);
        if (!menu_name_taken(menu_new_name)) break;
    }
    menu_status[0]=0;
    menu_mode=MODE_NEW;
    menu_draw();
}
/* Файла быть не должно: дискету sd_write заменил бы молча. */
static const char *image_check_free(const char *path) {
    void *file=NULL; uint32_t sectors=0;
    int r=core->sd_open(path,0,&sectors,&file);
    if (file) core->sd_close(file);
    if (r==ZM_NOT_FOUND) return NULL;
    return r==ZM_BUSY?"TANG SD IS BUSY, TRY AGAIN":"FILE ALREADY EXISTS";
}
static const char *adf_create(const char *path,const char *name) {
    const char *problem=image_check_free(path);
    if (problem) return problem;
    uint8_t *image=core->alloc(ADF_BYTES,ZM_MEM_PSRAM);
    if (!image) return "NOT ENOUGH MEMORY";
    memset(image,0,ADF_BYTES);
    fs_layout_t l;
    fs_layout(&l,ADF_BLOCKS,FS_DOS0,name,fs_stamp(evo_time));
    fs_make_block(&l,0,image);
    for (uint32_t n=l.root;n<=fs_last(&l);n++) fs_make_block(&l,n,image+(size_t)n*FS_BLOCK);
    int r=core->sd_write(path,image,ADF_BYTES,&running);
    core->free(image);
    return r==ZM_OK?NULL:r==ZM_BUSY?"TANG SD IS BUSY, TRY AGAIN":"CANNOT WRITE THE IMAGE";
}
/* RDB пишется последним: прерванный образ ядро не примет. Место раздела не стирается: карта
 * занятости считает его свободным, а что там лежало, том не читает. */
static const char *hdf_create(const char *path,const char *name,unsigned mb) {
    const char *problem=image_check_free(path);
    if (problem) return problem;
    uint32_t sectors=mb*2048u, cylinders=sectors/HDF_CYL_SECTORS;
    int r=core->sd_create(path,sectors);
    if (r!=ZM_OK) return r==ZM_BUSY?"TANG SD IS BUSY, TRY AGAIN":r==ZM_NO_MEMORY?"NOT ENOUGH SPACE ON TANG SD":"CANNOT CREATE THE FILE";
    void *file=NULL; uint32_t got=0;
    if (core->sd_open(path,1,&got,&file)!=ZM_OK || got!=sectors) {
        if (file) core->sd_close(file);
        return "CANNOT OPEN THE NEW FILE";
    }
    fs_layout_t l;
    fs_layout(&l,(cylinders-HDF_RDB_CYLS)*HDF_CYL_SECTORS,FS_DOS1,name,fs_stamp(evo_time));
    memset(fs_buffer,0,sizeof fs_buffer);
    int ok=1;
    for (uint32_t n=0;ok && n<16;n++) ok=sd_retry_write(file,n,fs_buffer);  /* Kickstart ищет RDB здесь */
    for (uint32_t n=0;ok && n<FS_RESERVED;n++) {
        fs_make_block(&l,n,fs_buffer);
        ok=sd_retry_write(file,HDF_START+n,fs_buffer);
    }
    for (uint32_t n=l.root;ok && n<=fs_last(&l);n++) {
        fs_make_block(&l,n,fs_buffer);
        ok=sd_retry_write(file,HDF_START+n,fs_buffer);
    }
    if (ok) { hdf_part_block(cylinders,fs_buffer); ok=sd_retry_write(file,1,fs_buffer); }
    if (ok) { hdf_rdb_block(cylinders,fs_buffer); ok=sd_retry_write(file,0,fs_buffer); }
    core->sd_close(file);
    return ok?NULL:"CANNOT WRITE THE IMAGE";
}
static void menu_new_commit(void) {
    char name[NEW_NAME_MAX+5], path[512];
    if (!menu_new_name[0]) return;
    snprintf(name,sizeof name,"%s%s",menu_new_name,menu_card==CARD_HDD?".HDF":".ADF");
    menu_mode=MODE_LIST;
    const char *problem="PATH IS TOO LONG";
    if (menu_file_path(path,sizeof path,name)) {
        menu_set_status("CREATING THE IMAGE...",C_WHITE);
        menu_draw();
        problem=menu_card==CARD_HDD?hdf_create(path,menu_new_name,new_sizes_mb[menu_new_size]):adf_create(path,menu_new_name);
    }
    if (problem) { menu_set_status(problem,C_YELLOW); menu_draw(); return; }
    menu_read_dir();
    menu_select(name);
    char text[OSD_COLS+1];
    snprintf(text,sizeof text,"CREATED %s",name);
    menu_set_status(text,C_WHITE);
    menu_draw();
}
/* ---- системный диск ---- */
static void menu_hdd_ask(int mode,const char *name) {
    char text[OSD_COLS+1];
    const char *verb=mode==MODE_USE?"BOOT FROM ":"REMOVE ";
    size_t room=sizeof text-1, n=strlen(verb), len=strlen(name);
    memcpy(text,verb,n);
    if (len>room-n) len=room-n;
    memcpy(text+n,name,len); text[n+len]=0;
    menu_pick[0]=0;
    if (mode==MODE_USE) { strncpy(menu_pick,name,sizeof menu_pick-1); menu_pick[sizeof menu_pick-1]=0; }
    menu_set_status(text,C_YELLOW);
    menu_mode=mode;
    menu_draw();
}
/* Выбранный диск должен загрузить систему: дискета загрузилась бы раньше него. */
static void menu_hdd_use(void) {
    char path[512];
    menu_mode=MODE_LIST;
    const char *problem=menu_file_path(path,sizeof path,menu_pick)?NULL:"PATH IS TOO LONG";
    if (!problem && hd0_file && !strcmp(path,hd0_path)) problem="THIS DISK IS ALREADY IN USE";
    if (!problem) problem=hd0_attach(path);
    if (problem) { menu_set_status(problem,C_YELLOW); menu_draw(); return; }
    if (!hd0_save_config(path)) core->log("amiga: cannot save %s",HD0_CFG);
    df0_eject();
    menu_status[0]=0;
    menu_close();
    amiga_restart();
}
static void menu_hdd_remove(void) {
    hd0_detach();
    hd0_path[0]=0;
    if (!hd0_save_config("")) core->log("amiga: cannot save %s",HD0_CFG);
    menu_status[0]=0;
    menu_close();
    amiga_restart();
}
/* Вопрос или ввод имени: остальные клавиши меню не работают. */
static void menu_dialog_key(unsigned code) {
    if (code==HID_ESC || code==HID_F11) {
        menu_mode=MODE_LIST; menu_status[0]=0; menu_draw();
        return;
    }
    if (menu_mode==MODE_KEY) {
        if (code==HID_F12) { menu_set_status("F12 EXITS TO WC, PRESS ANOTHER KEY",C_YELLOW); menu_status_flush(); return; }
        menu_mode=MODE_LIST;
        keys_assign(code);
        menu_draw();
        return;
    }
    if (menu_mode!=MODE_NEW) {
        if (code==HID_ENTER) {
            if (menu_mode==MODE_USE) menu_hdd_use();
            else if (menu_mode==MODE_REMOVE) menu_hdd_remove();
            else if (menu_mode==MODE_RESTART) { menu_mode=MODE_LIST; menu_status[0]=0; menu_close(); restart_req=1; core->signal_give(wake); }
            else { menu_mode=MODE_LIST; menu_status[0]=0; menu_close(); amiga_restart(); }
        }
        return;
    }
    size_t n=strlen(menu_new_name);
    char ch=0;
    if (code>=4 && code<=29) ch=(char)('A'+code-4);
    else if (code>=30 && code<=38) ch=(char)('1'+code-30);
    else if (code==39) ch='0';
    else if (code==HID_MINUS) ch='-';
    if (ch) { if (n<NEW_NAME_MAX) { menu_new_name[n]=ch; menu_new_name[n+1]=0; } }
    else if (code==HID_BKSP) { if (n) menu_new_name[n-1]=0; }
    else if (code==HID_LEFT && menu_card==CARD_HDD) { if (menu_new_size) menu_new_size--; }
    else if (code==HID_RIGHT && menu_card==CARD_HDD) { if (menu_new_size+1<NEW_SIZE_COUNT) menu_new_size++; }
    else if (code==HID_ENTER) { menu_new_commit(); return; }
    else return;
    menu_draw();
}
/* Клавиша (HID: код [6:0], отпускание — бит 7). 1 — съедена меню, Amiga её не получит. */
static int menu_key(uint16_t ev) {
    unsigned code=ev&0x7f, up=(ev&0x80)!=0;
    if (!menu_open) {
        if (code==HID_F11) {
            if (!up && menu_available) menu_show();
            return 1;
        }
        return 0;
    }
    if (up) return 1;
    if (menu_mode!=MODE_LIST) { menu_dialog_key(code); return 1; }
    switch (code) {
    case HID_F11: case HID_ESC: menu_close(); break;
    case HID_UP: if (menu_cursor) menu_cursor--; menu_draw(); break;
    case HID_DOWN: if (menu_cursor+1<menu_item_n) menu_cursor++; menu_draw(); break;
    case HID_PGUP: menu_cursor=menu_cursor>=MENU_LINES?menu_cursor-MENU_LINES:0; menu_draw(); break;
    case HID_PGDN: if (menu_item_n) menu_cursor=menu_cursor+MENU_LINES<menu_item_n?menu_cursor+MENU_LINES:menu_item_n-1; menu_draw(); break;
    case HID_HOME: menu_cursor=0; menu_draw(); break;
    case HID_END: if (menu_item_n) menu_cursor=menu_item_n-1; menu_draw(); break;
    case HID_BKSP: if (!menu_root()) { menu_enter_dir(".."); menu_draw(); } break;
    case HID_DEL:
        if(menu_card==CARD_VIDEO){video_change(1,0);menu_draw();}
        else if (menu_card==CARD_KEYS) { memcpy(ja_key,ja_default,sizeof ja_key); keys_saved("DEFAULT KEYS"); menu_draw(); }
        else if (menu_card!=CARD_HDD) { df0_eject(); menu_status[0]=0; menu_draw(); }
        else if (hd0_file) menu_hdd_ask(MODE_REMOVE,hd0_name);
        else { menu_set_status("NO HARD DISK",C_YELLOW); menu_draw(); }
        break;
    case HID_INS:
        if (menu_card==CARD_TANG || menu_card==CARD_HDD) menu_new_begin();
        break;
    case HID_F4:   /* при открытом меню Ctrl+Alt+F4 до сброса не доходит: клавиши у меню */
        menu_set_status("RESET AMIGA?",C_YELLOW);
        menu_mode=MODE_RESET;
        menu_draw();
        break;
    case HID_F5:
        if (menu_card==CARD_NET && net_list_here()) {
            char keep[256]="";
            if (menu_cursor<menu_item_n) strcpy(keep,menu_items[menu_cursor].name);
            net_reload=1;
            menu_set_status("READING LIST...",C_WHITE);
            menu_status_flush();
            menu_read_dir();
            menu_select(keep[0]?keep:NULL);
            menu_draw();
        }
        break;
    case HID_TAB: case HID_LEFT: case HID_RIGHT:
        menu_card=(menu_card+(code==HID_LEFT?CARD_COUNT-1:1))%CARD_COUNT;
        menu_set_status("READING FOLDER...",C_WHITE);
        menu_item_n=0;
        menu_draw();
        menu_read_dir();
        menu_select(NULL);
        menu_draw();
        break;
    case HID_ENTER:
        if(menu_card==CARD_VIDEO){video_change(0,menu_cursor);menu_draw();}
        else if (menu_card==CARD_KEYS) {
            if (!menu_cursor) { ja_on=!ja_on; keys_saved(NULL); }
            else {
                char text[OSD_COLS+1];
                ja_pick=menu_cursor;
                snprintf(text,sizeof text,"PRESS THE KEY FOR %s",ja_name[ja_pick]);
                menu_set_status(text,C_YELLOW);
                menu_mode=MODE_KEY;
                keys_blink_on=1; keys_blink_at=core->millis();
            }
            menu_draw();
        } else if (menu_cursor<menu_item_n) {
            const menu_item_t *it=&menu_items[menu_cursor];
            if (it->flags&MI_DIR) { char name[256]; strcpy(name,it->name); menu_enter_dir(name); menu_draw(); }
            else if (menu_card==CARD_HDD) menu_hdd_ask(MODE_USE,it->name);
            else menu_load(it);
        }
        break;
    default: break;
    }
    return 1;
}
