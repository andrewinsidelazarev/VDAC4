/* SPDX-License-Identifier: GPL-3.0-or-later
 * Карта Evo — том EVO: в Workbench, только чтение (AMIGA.ELF 0.18, ядро 0.11: второй диск IDE, unit 5).
 * CrossDOS из Workbench 3.1 (40.19) знает только FAT12/16, а карта Evo — FAT32 на десятки ГБ; fat95 через штатный
 * scsi.device дотянулся бы лишь до ~8 ГБ. Поэтому ESP сам читает FAT карты (сектора даёт плагин VDAC4 Cores 0.15+
 * в ответе на ВВОД, как меню F11) и показывает Amiga готовый том FFS (DOS\1), собранный на лету:
 *   диск: цилиндр 0 — RDB (RDSK + PART «EVO», не загрузочный, FFS из Kickstart), дальше раздел ~1,9 ГБ;
 *   том: блоки 0–1 загрузочные, корень посередине, за ним карта занятости — все блоки заняты, места нет;
 *   заголовки каталогов и файлов, блоки расширения и данных получают номера, когда FFS до них доходит, и номер,
 *   раз выданный, не меняется до выхода из ядра; блок данных файла — сектор его кластера на карте.
 * Каталог FAT читается целиком, когда FFS впервые читает его заголовок (для списка — и заголовки подкаталогов).
 * Запись: FFS может переписать свой блок (имя, дата, значок) — блок ложится в память ESP, карта не меняется никогда.
 * Имена: длинное имя FAT; кириллица — латиницей, прочее не ASCII — «_», не длиннее 30 знаков, без повторов в каталоге.
 * Архив .lha/.lzh — ещё и каталог рядом с ним, распакованный на лету (evo_lha.h, 0.19).
 * Ни таблиц указателей, ни индексов «таблица минус N»: загрузчик модулей ESP (tests/check_elf_pointers.py).
 */
#define EV_HEADS 16u
#define EV_SECS 63u
#define EV_CYL (EV_HEADS*EV_SECS)              /* блоков в цилиндре; цилиндр 0 — RDB */
#define EV_CYLS 3901u                          /* раздел меньше 2 ГБ: FFS 40 и scsi.device Kickstart 3.1 — без сюрпризов */
#define EV_DISK_BLOCKS (EV_CYLS*EV_CYL)
#define EV_PART_BLOCKS ((EV_CYLS-1u)*EV_CYL)
#define EV_TABLE 72u                           /* таблица хешей и указателей блока FFS */
#define EV_NAME 30u
#define EV_RAW_NAME 64u
#define EV_BURST 16u                           /* секторов за запрос (AM_SD_MAX плагина) */
#define EV_CACHE 256u                          /* секторов карты в кэше ESP */
#define EV_WAIT_MS 1500u                       /* без нового сектора дольше — запрос заново */
#define EV_PROBE_MS 700u                       /* первый сектор: плагин старше 0.15 молчит */
#define EV_TRIES 3u
#define EV_OVERLAY 1024u                       /* блоков, переписанных Amiga, в памяти ESP */
#define EV_OVERLAY_SLOTS (EV_OVERLAY*2u)
#define EV_PROTECT 5u                          /* rwed без w и d: писать и удалять нельзя */
#define EV_ENTS_MAX 65536u
#define EV_DIRS_MAX 8192u
#define EV_RUNS_MAX 262144u
#define EV_CHAINS 4u
#define EV_SECTOR_OP 7
#define EV_ST_FILE 0xFFFFFFFDu
#define EV_ST_USERDIR 2u
#define EV_T_HEADER 2u
#define EV_T_LIST 16u
#define EV_F_DIR 1u                            /* каталог */
#define EV_F_ARC 2u                            /* каталог-архив: сам файл .lha — соседняя запись */
#define EV_F_MEMBER 4u                         /* файл или каталог внутри архива */

typedef struct {
    uint32_t cluster, size;     /* первый кластер, байты */
    uint32_t header;            /* свой блок */
    uint32_t chain;             /* следующий в той же ячейке хеша (при чтении каталога — номер записи + 1) */
    uint32_t data, ext;         /* файл: первые данные и первый блок расширения, 0 — ещё не выданы */
    uint32_t dir;               /* каталог: номер в ev.dirs + 1, 0 — ещё не прочитан */
    uint32_t arc;               /* архив: номер в lha.arcs + 1; у члена архива cluster — номер члена, а у каталога
                                 * в архиве — член с этим путём, size — длина пути */
    uint16_t date, time;
    uint8_t flags, len;         /* EV_F_… */
    char name[EV_NAME];
} ev_ent_t;
typedef struct {
    uint32_t first, count;      /* записи в ev.ents */
    uint32_t header, parent;    /* свой блок и блок родителя (у корня 0) */
    uint32_t heads;             /* первый блок заголовков записей */
    uint32_t hash[EV_TABLE];
} ev_dir_t;
enum { EV_RUN_HEADS, EV_RUN_DATA, EV_RUN_EXT };
typedef struct {
    uint32_t base, count;       /* блоки тома */
    uint32_t owner;             /* HEADS: каталог; DATA и EXT: запись */
    uint32_t index;             /* DATA: первый блок файла; EXT: номер блока расширения, с 1 */
    uint32_t data, next;        /* EXT: свои данные и следующий блок расширения, 0 — ещё не выданы */
    uint32_t kind;
} ev_run_t;
typedef struct { uint32_t at, cluster, count; } ev_extent_t;   /* кластеры файла: с номера at подряд count */
/* Цепочка кластеров файла читается по мере чтения файла: известно walked кластеров, next — следующий (0 — дальше нет). */
typedef struct { uint32_t ent, count, cap, walked, need, next; ev_extent_t *ext; } ev_chain_t;

static struct {
    int ready, probe;
    uint32_t part, fat, data, root_lba, root_secs, root_cluster, clusters, spc;
    int fat32;
    fs_layout_t layout;
    uint32_t next;              /* следующий свободный блок тома */
    ev_ent_t *ents; uint32_t n_ents, cap_ents;
    ev_dir_t *dirs; uint32_t n_dirs, cap_dirs;
    ev_run_t *runs; uint32_t n_runs, cap_runs;
    ev_chain_t chains[EV_CHAINS]; unsigned chain_turn;
    uint8_t *cache; uint32_t cache_tag[EV_CACHE];
    uint32_t *ov_key; uint16_t *ov_slot; uint8_t *ov_data; uint32_t ov_count; int ov_full;
    uint32_t reads, failed;
} ev;
/* Обмен с плагином: запрос в ответе на ВВОД, сектора — SD_DONE #76 подряд (command, ev_sd_done). */
static uint8_t *ev_rx;
static volatile uint32_t ev_sd_lba, ev_sd_want, ev_sd_got;
static volatile uint8_t ev_sd_tag;
static volatile int ev_sd_fail;

static uint16_t little16(const uint8_t *p) { return (uint16_t)(p[0]|p[1]<<8); }

/* Массив под need элементов; ноль — памяти нет или предел. */
static int ev_room(void **p,uint32_t *cap,uint32_t need,size_t size,uint32_t max) {
    if (need<=*cap) return 1;
    if (need>max) return 0;
    uint32_t n=*cap?*cap:64u;
    while (n<need) n*=2u;
    if (n>max) n=max;
    void *q=core->alloc((size_t)n*size,ZM_MEM_PSRAM);
    if (!q) return 0;
    if (*p) { memcpy(q,*p,(size_t)*cap*size); core->free(*p); }
    *p=q; *cap=n;
    return 1;
}

/* SD_DONE #76 [номер][итог][LBA][512]: сектор текущего запроса — по порядку. Чужой или запоздалый — «не ждём» (0),
 * плагин бросает остаток запроса. */
static int ev_sd_done(const uint8_t *p,uint16_t n) {
    uint8_t ok=0;
    int done=0;
    core->mutex_lock(lock);
    input_ms=core->millis();
    if (n>=6 && ev_rx && ev_sd_got<ev_sd_want && p[0]==ev_sd_tag && little32(p+2)==ev_sd_lba+ev_sd_got) {
        ok=1;
        if (p[1]==0 && n==6+512) {
            memcpy(ev_rx+(size_t)ev_sd_got*512u,p+6,512);
            if (++ev_sd_got==ev_sd_want) done=1;
        } else { ev_sd_fail=1; done=1; }
        if (done) evo_done=1;
    }
    core->mutex_unlock(lock);
    core->reply(0xF6,&ok,1);
    if (done) core->signal_give(wake);
    return ZM_OK;
}
/* count секторов с lba в ev_rx. Итог — сколько пришло подряд. */
static unsigned ev_fetch(uint32_t lba,unsigned count,uint32_t wait_ms) {
    core->mutex_lock(lock);
    ev_sd_tag=(uint8_t)(ev_sd_tag+1u); ev_sd_lba=lba; ev_sd_got=0; ev_sd_fail=0; ev_sd_want=count;
    evo_req[0]=EV_SECTOR_OP; evo_req[1]=ev_sd_tag; put32(evo_req+2,lba); evo_req[6]=(uint8_t)count;
    evo_req_n=7; evo_done=0; evo_req_pending=1;
    core->mutex_unlock(lock);
    uint32_t start=core->millis(), seen=0;
    while (running && !exit_req && !evo_done) {
        uint32_t got=ev_sd_got;
        if (got!=seen) { seen=got; start=core->millis(); }
        if (core->millis()-start>wait_ms) break;
        core->delay(1);
    }
    core->mutex_lock(lock);
    evo_req_pending=0; ev_sd_want=0;
    unsigned got=ev_sd_got;
    core->mutex_unlock(lock);
    return got;
}
/* Сектор карты: из кэша или у плагина, с чтением вперёд ahead секторов. NULL — не прочитать. */
static const uint8_t *ev_sector(uint32_t lba,unsigned ahead) {
    unsigned slot=lba%EV_CACHE;
    if (ev.cache_tag[slot]==lba+1u) return ev.cache+(size_t)slot*512u;
    if (ahead<1u) ahead=1u;
    if (ahead>EV_BURST) ahead=EV_BURST;
    unsigned tries=ev.probe?1u:EV_TRIES;
    for (unsigned t=0;t<tries && running && !exit_req;t++) {
        unsigned got=ev_fetch(lba,ahead,ev.probe?EV_PROBE_MS:EV_WAIT_MS);
        for (unsigned k=0;k<got;k++) {
            unsigned s=(lba+k)%EV_CACHE;
            memcpy(ev.cache+(size_t)s*512u,ev_rx+(size_t)k*512u,512);
            ev.cache_tag[s]=lba+k+1u;
        }
        if (got) { ev.reads+=got; return ev.cache+(size_t)slot*512u; }
        ahead=1u;
    }
    if (ev.failed++<8u) core->log("amiga: EVO: sector %u not read",(unsigned)lba);
    return NULL;
}

/* ---- FAT ---- */
static int ev_cluster_ok(uint32_t c) { return c>=2u && c-2u<ev.clusters; }
static uint32_t ev_cluster_lba(uint32_t c) { return ev.data+(c-2u)*ev.spc; }
static uint32_t ev_fat_next(uint32_t c) {
    uint32_t at=ev.fat32?c*4u:c*2u;
    const uint8_t *s=ev_sector(ev.fat+at/512u,EV_BURST);
    if (!s) return 0;
    at%=512u;
    return ev.fat32?little32(s+at)&0x0FFFFFFFu:little16(s+at);
}
static uint32_t ev_blocks(uint32_t size) { return size/512u+(size%512u?1u:0u); }

/* ---- имена ---- */
static const char ev_cyr[32][4]={"A","B","V","G","D","E","ZH","Z","I","Y","K","L","M","N","O","P",
    "R","S","T","U","F","KH","TS","CH","SH","SCH","","Y","","E","YU","YA"};
/* Знак UTF-16 в имя Amiga: ASCII как есть, кириллица латиницей («Ж» — «Zh», «ж» — «zh»), прочее — «_». */
static unsigned ev_put(char *out,unsigned n,uint32_t c) {
    if (c>=0x20u && c<0x7Fu && c!=':' && c!='/') { if (n<EV_RAW_NAME) out[n++]=(char)c; return n; }
    unsigned idx=32u, lower=0;
    if (c==0x401u) idx=5u;
    else if (c==0x451u) { idx=5u; lower=1; }
    else if (c>=0x410u && c<0x430u) idx=c-0x410u;
    else if (c>=0x430u && c<0x450u) { idx=c-0x430u; lower=1; }
    if (idx>=32u) { if (n<EV_RAW_NAME) out[n++]='_'; return n; }
    for (unsigned k=0;k<4u && ev_cyr[idx][k];k++) {
        char ch=ev_cyr[idx][k];
        if (lower || k) ch=(char)(ch|0x20);
        if (n<EV_RAW_NAME) out[n++]=ch;
    }
    return n;
}
/* Байт короткого имени FAT: кодовая страница 866 (так пишет WC). */
static uint32_t ev_cp866(uint8_t b) {
    if (b<0x80u) return b;
    if (b<0xB0u) return 0x410u+(b-0x80u);
    if (b>=0xE0u && b<0xF0u) return 0x440u+(b-0xE0u);
    if (b==0xF0u) return 0x401u;
    if (b==0xF1u) return 0x451u;
    return 0xFFFDu;
}
/* Не длиннее 30: расширение (до 8 знаков с точкой) остаётся, укорачивается начало; suffix — «~N» перед расширением. */
static unsigned ev_fit(char *dst,const char *src,unsigned n,unsigned suffix) {
    char tail[12]; unsigned t=0, dot=n;
    if (suffix) t=(unsigned)snprintf(tail,sizeof tail,"~%u",suffix);
    for (unsigned i=n;i-->1u;) if (src[i]=='.') { dot=i; break; }
    unsigned ext=n-dot;
    if (ext>8u) { dot=n; ext=0; }
    unsigned room=EV_NAME-t-ext, base=dot<room?dot:room;
    memcpy(dst,src,base); memcpy(dst+base,tail,t); memcpy(dst+base+t,src+dot,ext);
    return base+t+ext;
}
/* Хеш имени FFS (DOS\1, без международного режима: только ASCII). */
static unsigned ev_hash(const char *s,unsigned n) {
    uint32_t h=n;
    for (unsigned i=0;i<n;i++) {
        uint32_t c=(uint8_t)s[i];
        if (c>='a' && c<='z') c-=32u;
        h=(h*13u+c)&0x7FFu;
    }
    return h%EV_TABLE;
}
static int ev_same(const char *a,unsigned an,const char *b,unsigned bn) {
    if (an!=bn) return 0;
    for (unsigned i=0;i<an;i++) {
        unsigned x=(uint8_t)a[i], y=(uint8_t)b[i];
        if (x>='a' && x<='z') x-=32u;
        if (y>='a' && y<='z') y-=32u;
        if (x!=y) return 0;
    }
    return 1;
}

/* ---- каталог FAT -> записи ---- */
typedef struct { uint16_t name[260]; unsigned next, len; uint8_t sum; int have; } ev_lfn_t;
typedef struct { uint32_t first; uint32_t bucket[EV_TABLE]; int full; } ev_load_t;
static const uint8_t ev_lfn_at[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
static ev_load_t ev_ld;                      /* каталог, который сейчас читается (по одному, без вложенности) */

/* Новая запись каталога: имя Amiga из raw, единственное в каталоге без учёта регистра. NULL — места нет. */
static ev_ent_t *ev_new(ev_load_t *ld,const char *raw,unsigned rn) {
    if (ld->full) return NULL;
    if (!ev_room((void **)&ev.ents,&ev.cap_ents,ev.n_ents+1u,sizeof *ev.ents,EV_ENTS_MAX)) {
        ld->full=1; core->log("amiga: EVO: too many entries, directory cut"); return NULL;
    }
    for (unsigned suffix=0;suffix<1000u;suffix++) {
        char nm[EV_NAME]; unsigned nn=ev_fit(nm,raw,rn,suffix), h=ev_hash(nm,nn); int clash=0;
        for (uint32_t j=ld->bucket[h];j;j=ev.ents[ld->first+j-1u].chain) {
            const ev_ent_t *o=&ev.ents[ld->first+j-1u];
            if (ev_same(o->name,o->len,nm,nn)) { clash=1; break; }
        }
        if (clash) continue;
        ev_ent_t *x=&ev.ents[ev.n_ents];
        memset(x,0,sizeof *x);
        memcpy(x->name,nm,nn); x->len=(uint8_t)nn;
        x->chain=ld->bucket[h];
        ld->bucket[h]=ev.n_ents-ld->first+1u;
        ev.n_ents++;
        return x;
    }
    return NULL;
}
static int ev_dir_begin(ev_load_t *ld) {
    if (!ev_room((void **)&ev.dirs,&ev.cap_dirs,ev.n_dirs+1u,sizeof *ev.dirs,EV_DIRS_MAX)) {
        core->log("amiga: EVO: too many directories"); return 0;
    }
    memset(ld,0,sizeof *ld);
    ld->first=ev.n_ents;
    return 1;
}
static uint32_t ev_alloc(uint32_t kind,uint32_t count,uint32_t owner,uint32_t index);
/* Записи с ld->first — каталог: номера их заголовков подряд, ячейки хеша. *out — номер каталога в ev.dirs. */
static void ev_dir_finish(ev_load_t *ld,uint32_t header,uint32_t parent,uint32_t *out) {
    uint32_t index=ev.n_dirs, count=ev.n_ents-ld->first, heads=0;
    if (count) {
        heads=ev_alloc(EV_RUN_HEADS,count,index,0);
        if (!heads) { core->log("amiga: EVO: volume blocks used up"); ev.n_ents=ld->first; count=0; }
    }
    ev_dir_t *d=&ev.dirs[index];
    memset(d,0,sizeof *d);
    d->first=ld->first; d->count=count; d->header=header; d->parent=parent; d->heads=heads;
    for (uint32_t i=0;i<count;i++) {
        ev_ent_t *x=&ev.ents[ld->first+i];
        x->header=heads+i;
        x->chain=x->chain?heads+x->chain-1u:0u;
    }
    for (unsigned h=0;h<EV_TABLE;h++) d->hash[h]=count && ld->bucket[h]?heads+ld->bucket[h]-1u:0u;
    ev.n_dirs++;
    *out=index;
}
#include "evo_lha.h"

static void ev_add(ev_load_t *ld,const uint8_t *e,const char *raw,unsigned rn) {
    ev_ent_t *x=ev_new(ld,raw,rn);
    if (!x) return;
    x->flags=(e[11]&0x10)?EV_F_DIR:0u;
    x->cluster=(ev.fat32?(uint32_t)little16(e+20)<<16:0u)|little16(e+26);
    x->size=x->flags?0u:little32(e+28);
    x->time=little16(e+22); x->date=little16(e+24);
    if (!x->flags) ev_lha_offer(ld,(uint32_t)(x-ev.ents),raw,rn);
}
/* Запись каталога (32 байта). -1 — конец каталога. */
static int ev_dirent(ev_load_t *ld,ev_lfn_t *l,const uint8_t *e) {
    if (e[0]==0) return -1;
    if (e[0]==0xE5u) { l->have=0; return 0; }
    if ((e[11]&0x3Fu)==0x0Fu) {             /* часть длинного имени: с конца, номера по убыванию */
        unsigned n=e[0]&0x3Fu;
        if (e[0]&0x40u) {
            if (n<1u || n>20u) { l->have=0; return 0; }
            l->have=1; l->sum=e[13]; l->len=n*13u;
        } else if (!l->have || n!=l->next || e[13]!=l->sum) { l->have=0; return 0; }
        l->next=n-1u;
        for (unsigned k=0;k<13u;k++) l->name[(n-1u)*13u+k]=little16(e+ev_lfn_at[k]);
        return 0;
    }
    int lfn=l->have && l->next==0;
    l->have=0;
    if (e[11]&0x08u) return 0;               /* метка тома */
    if (e[0]=='.' && (e[1]==' ' || (e[1]=='.' && e[2]==' '))) return 0;
    char raw[EV_RAW_NAME]; unsigned rn=0;
    if (lfn) {
        uint8_t sum=0;
        for (unsigned k=0;k<11u;k++) sum=(uint8_t)(((sum&1u)<<7)+(sum>>1)+e[k]);
        lfn=sum==l->sum;
    }
    if (lfn) {
        for (unsigned k=0;k<l->len && l->name[k] && l->name[k]!=0xFFFFu;k++) rn=ev_put(raw,rn,l->name[k]);
    } else {                                 /* 8.3, строчные по флагам NT */
        unsigned end=8;
        while (end && e[end-1u]==' ') end--;
        for (unsigned k=0;k<end;k++) {
            uint32_t c=ev_cp866(k==0 && e[0]==0x05u?0xE5u:e[k]);
            if ((e[12]&0x08u) && c>='A' && c<='Z') c+=32u;
            rn=ev_put(raw,rn,c);
        }
        unsigned xe=11;
        while (xe>8u && e[xe-1u]==' ') xe--;
        if (xe>8u) {
            rn=ev_put(raw,rn,'.');
            for (unsigned k=8;k<xe;k++) {
                uint32_t c=ev_cp866(e[k]);
                if ((e[12]&0x10u) && c>='A' && c<='Z') c+=32u;
                rn=ev_put(raw,rn,c);
            }
        }
    }
    if (rn) ev_add(ld,e,raw,rn);
    return 0;
}
/* Каталог FAT (кластер 0 — корень FAT16) в ev.dirs. Ноль — нет памяти. */
static int ev_dir_load(uint32_t cluster,uint32_t header,uint32_t parent,uint32_t *out) {
    static ev_lfn_t l;                       /* 0,5 КБ: не на стеке потока */
    if (!ev_dir_begin(&ev_ld)) return 0;
    memset(&l,0,sizeof l);
    int fixed=cluster==0 && !ev.fat32;
    uint32_t c=fixed?0:cluster, sec=0, clusters=0;
    if (!fixed && cluster==0) c=ev.root_cluster;
    for (;;) {
        uint32_t lba; unsigned ahead;
        if (fixed) {
            if (sec>=ev.root_secs) break;
            lba=ev.root_lba+sec; ahead=ev.root_secs-sec;
        } else {
            if (!ev_cluster_ok(c) || clusters>65536u) break;
            lba=ev_cluster_lba(c)+sec; ahead=ev.spc-sec;
        }
        const uint8_t *s=ev_sector(lba,ahead);
        if (!s) break;
        int end=0;
        for (unsigned k=0;k<16u && !end;k++) end=ev_dirent(&ev_ld,&l,s+k*32u)<0;
        if (end) break;
        if (!fixed && ++sec==ev.spc) { sec=0; clusters++; c=ev_fat_next(c); }
        else if (fixed) sec++;
    }
    ev_dir_finish(&ev_ld,header,parent,out);
    return 1;
}

/* ---- номера блоков тома ---- */
static uint32_t ev_alloc(uint32_t kind,uint32_t count,uint32_t owner,uint32_t index) {
    uint32_t base=ev.next;
    if (base<=ev.layout.root && base+count>ev.layout.root) base=fs_last(&ev.layout)+1u;
    if (!count || base+count>ev.layout.blocks || base+count<base) return 0;
    if (!ev_room((void **)&ev.runs,&ev.cap_runs,ev.n_runs+1u,sizeof *ev.runs,EV_RUNS_MAX)) return 0;
    ev_run_t *r=&ev.runs[ev.n_runs++];
    r->base=base; r->count=count; r->owner=owner; r->index=index; r->data=0; r->next=0; r->kind=kind;
    ev.next=base+count;
    return base;
}
/* Выдача номеров идёт по возрастанию: поиск делением пополам. */
static int ev_find(uint32_t b) {
    uint32_t lo=0, hi=ev.n_runs;
    while (lo<hi) { uint32_t mid=(lo+hi)/2u; if (ev.runs[mid].base<=b) lo=mid+1u; else hi=mid; }
    if (!lo) return -1;
    const ev_run_t *r=&ev.runs[lo-1u];
    return b-r->base<r->count?(int)(lo-1u):-1;
}

/* ---- цепочка кластеров файла ---- */
static ev_chain_t *ev_chain(uint32_t ent) {
    for (unsigned i=0;i<EV_CHAINS;i++) if (ev.chains[i].ext && ev.chains[i].ent==ent) return &ev.chains[i];
    ev_chain_t *ch=&ev.chains[ev.chain_turn++%EV_CHAINS];
    if (ch->ext) core->free(ch->ext);
    memset(ch,0,sizeof *ch);
    const uint32_t bytes=ev.spc*512u, size=ev.ents[ent].size;
    ch->cap=16;
    ch->ext=core->alloc(ch->cap*sizeof *ch->ext,ZM_MEM_PSRAM);
    if (!ch->ext) return NULL;
    ch->ent=ent; ch->need=size/bytes+(size%bytes?1u:0u); ch->next=ev.ents[ent].cluster;
    return ch;
}
/* Кластеры с номерами до ci включительно — в цепочку. Ноль — цепочка кончилась раньше или FAT не прочитать. */
static int ev_chain_reach(ev_chain_t *ch,uint32_t ci) {
    while (ch->walked<=ci && ch->walked<ch->need && ev_cluster_ok(ch->next)) {
        uint32_t cl=ch->next;
        ev_extent_t *last=ch->count?&ch->ext[ch->count-1u]:NULL;
        if (last && last->cluster+last->count==cl) last->count++;
        else {
            if (ch->count==ch->cap) {
                ev_extent_t *more=core->alloc(ch->cap*2u*sizeof *more,ZM_MEM_PSRAM);
                if (!more) return 0;
                memcpy(more,ch->ext,ch->cap*sizeof *more); core->free(ch->ext); ch->ext=more; ch->cap*=2u;
            }
            ch->ext[ch->count].at=ch->walked; ch->ext[ch->count].cluster=cl; ch->ext[ch->count].count=1; ch->count++;
        }
        ch->next=++ch->walked<ch->need?ev_fat_next(cl):0u;
    }
    return ch->walked>ci;
}
/* Сектор карты для блока файла; *run — сколько секторов файла подряд с него (из уже прочитанной цепочки). Ноль — не найти. */
static uint32_t ev_file_lba(uint32_t ent,uint32_t block,uint32_t *run) {
    ev_chain_t *ch=ev_chain(ent);
    uint32_t ci=block/ev.spc, off=block%ev.spc, lo=0, hi;
    if (!ch) return 0;
    ev_chain_reach(ch,ci+EV_BURST/ev.spc);  /* и на окно чтения вперёд: соседние кластеры обычно рядом в той же FAT */
    if (ch->walked<=ci) return 0;
    hi=ch->count;
    while (lo<hi) { uint32_t mid=(lo+hi)/2u; if (ch->ext[mid].at<=ci) lo=mid+1u; else hi=mid; }
    if (!lo) return 0;
    const ev_extent_t *x=&ch->ext[lo-1u];
    if (ci-x->at>=x->count) return 0;
    *run=(x->count-(ci-x->at))*ev.spc-off;
    return ev_cluster_lba(x->cluster+(ci-x->at))+off;
}

/* n байтов файла FAT (запись ent) с позиции at. Ноль — не прочитать. */
static int ev_file_bytes(uint32_t ent,uint32_t at,uint8_t *out,uint32_t n) {
    while (n) {
        uint32_t run=0, lba=ev_file_lba(ent,at/512u,&run);
        const uint8_t *s=lba?ev_sector(lba,run):NULL;
        if (!s) return 0;
        uint32_t off=at%512u, k=512u-off;
        if (k>n) k=n;
        memcpy(out,s+off,k); out+=k; at+=k; n-=k;
    }
    return 1;
}

/* ---- блоки тома FFS ---- */
static void ev_stamp(uint8_t *b,uint16_t date,uint16_t time) {
    static const uint16_t before[12]={0,31,59,90,120,151,181,212,243,273,304,334};
    unsigned year=1980u+(date>>9), month=(date>>5)&15u, day=date&31u;
    if (month<1u || month>12u || day<1u) return;
    uint32_t days=(year-1978u)*365u+before[month-1u]+day-1u;
    for (unsigned y=1980;y<year;y+=4u) days++;
    if (year%4u==0 && month>2u) days++;
    be32(b,days); be32(b+4,(time>>11)*60u+((time>>5)&63u)); be32(b+8,(time&31u)*2u*50u);
}
static void ev_head_common(uint8_t *o,const ev_ent_t *e,uint32_t parent,uint32_t sec_type) {
    be32(o,EV_T_HEADER); be32(o+4,e->header);
    be32(o+80*4,EV_PROTECT);
    ev_stamp(o+105*4,e->date,e->time);
    o[0x1B0]=e->len; memcpy(o+0x1B1,e->name,e->len);
    be32(o+124*4,e->chain); be32(o+125*4,parent); be32(o+127*4,sec_type);
}
/* Заголовок записи i каталога d. Подкаталог читается с карты здесь, при первом чтении. */
static void ev_header_block(uint32_t d,uint32_t i,uint8_t *o) {
    uint32_t ent=ev.dirs[d].first+i, parent=ev.dirs[d].header;
    if (ev.ents[ent].flags&EV_F_DIR) {
        if (!ev.ents[ent].dir) {
            const ev_ent_t e=ev.ents[ent];
            uint32_t sub;
            int ok;
            if (e.flags&EV_F_ARC) ok=ev_arc_dir_load(e.arc-1u,0,0,e.header,parent,&sub);
            else if (e.flags&EV_F_MEMBER) ok=ev_arc_dir_load(e.arc-1u,e.cluster,e.size,e.header,parent,&sub);
            else ok=ev_dir_load(e.cluster?e.cluster:0xFFFFFFFFu,e.header,parent,&sub);
            if (ok) ev.ents[ent].dir=sub+1u;
        }
        const ev_ent_t *e=&ev.ents[ent];
        ev_head_common(o,e,parent,EV_ST_USERDIR);
        if (e->dir) for (unsigned h=0;h<EV_TABLE;h++) be32(o+(6u+h)*4u,ev.dirs[e->dir-1u].hash[h]);
    } else {
        uint32_t n=ev_blocks(ev.ents[ent].size), here=n<EV_TABLE?n:EV_TABLE;
        if (here && !ev.ents[ent].data) ev.ents[ent].data=ev_alloc(EV_RUN_DATA,here,ent,0);
        if (n>EV_TABLE && !ev.ents[ent].ext) ev.ents[ent].ext=ev_alloc(EV_RUN_EXT,1,ent,1);
        const ev_ent_t *e=&ev.ents[ent];
        ev_head_common(o,e,parent,EV_ST_FILE);
        if (e->data) {
            be32(o+2*4,here); be32(o+4*4,e->data);
            for (uint32_t k=0;k<here;k++) be32(o+(77u-k)*4u,e->data+k);
        }
        be32(o+81*4,e->size); be32(o+126*4,e->ext);
        if ((here && !e->data) || (n>EV_TABLE && !e->ext)) core->log("amiga: EVO: volume blocks used up");
    }
    fs_checksum(o,5,128);
}
static void ev_ext_block(uint32_t k,uint32_t b,uint8_t *o) {
    uint32_t ent=ev.runs[k].owner, from=ev.runs[k].index*EV_TABLE, n=ev_blocks(ev.ents[ent].size);
    uint32_t here=n>from?n-from:0u;
    if (here>EV_TABLE) here=EV_TABLE;
    if (here && !ev.runs[k].data) { uint32_t x=ev_alloc(EV_RUN_DATA,here,ent,from); ev.runs[k].data=x; }
    if (n>from+EV_TABLE && !ev.runs[k].next) { uint32_t x=ev_alloc(EV_RUN_EXT,1,ent,ev.runs[k].index+1u); ev.runs[k].next=x; }
    const ev_run_t *r=&ev.runs[k];
    be32(o,EV_T_LIST); be32(o+4,b);
    if (r->data) {
        be32(o+2*4,here);
        for (uint32_t i=0;i<here;i++) be32(o+(77u-i)*4u,r->data+i);
    }
    be32(o+125*4,ev.ents[ent].header); be32(o+126*4,r->next); be32(o+127*4,EV_ST_FILE);
    fs_checksum(o,5,128);
}
static void ev_data_block(uint32_t k,uint32_t b,uint8_t *o) {
    uint32_t ent=ev.runs[k].owner, block=ev.runs[k].index+(b-ev.runs[k].base), n=ev_blocks(ev.ents[ent].size), run=0;
    if (block>=n) return;
    if (ev.ents[ent].flags&EV_F_MEMBER) { ev_member_block(ent,block,o); return; }
    uint32_t lba=ev_file_lba(ent,block,&run);
    if (!lba) return;
    if (run>n-block) run=n-block;
    const uint8_t *s=ev_sector(lba,run);
    if (s) memcpy(o,s,512);
}
static void ev_block(uint32_t b,uint8_t *o) {
    const fs_layout_t *l=&ev.layout;
    if (b==0) { be32(o,FS_DOS1); return; }   /* контрольная сумма неверна — не загрузочный */
    if (b<FS_RESERVED || b>=l->blocks) return;
    if (b==l->root) {
        fs_root_block(l,o);
        for (unsigned h=0;h<EV_TABLE;h++) be32(o+(6u+h)*4u,ev.dirs[0].hash[h]);
        fs_checksum(o,5,128);
        return;
    }
    if (b>l->root && b<=fs_last(l)) {        /* карта занятости: все блоки заняты — нули */
        if (b>l->root+l->maps) fs_ext_block(l,b-l->root-l->maps-1u,o);
        return;
    }
    int k=ev_find(b);
    if (k<0) return;
    if (ev.runs[k].kind==EV_RUN_HEADS) ev_header_block(ev.runs[k].owner,b-ev.runs[k].base,o);
    else if (ev.runs[k].kind==EV_RUN_EXT) ev_ext_block((uint32_t)k,b,o);
    else ev_data_block((uint32_t)k,b,o);
}
static void ev_rdb_block(uint8_t *b) {
    static const uint8_t id[28]={'V','D','A','C','4',' ',' ',' ',
        'E','V','O',' ','S','D',' ','C','A','R','D',' ',' ',' ',' ',' ','1','.','0',' '};
    memcpy(b,"RDSK",4);
    be32(b+1*4,64); be32(b+3*4,7); be32(b+4*4,FS_BLOCK);
    be32(b+5*4,6);                          /* последний LUN и ID; «последний диск» не ставится */
    be32(b+6*4,0xFFFFFFFFu);
    be32(b+7*4,1);                          /* раздел — в блоке 1 */
    for (unsigned i=8;i<16;i++) be32(b+i*4,0xFFFFFFFFu);
    be32(b+16*4,EV_CYLS); be32(b+17*4,EV_SECS); be32(b+18*4,EV_HEADS);
    be32(b+19*4,1); be32(b+20*4,EV_CYLS); be32(b+24*4,EV_CYLS); be32(b+25*4,EV_CYLS); be32(b+26*4,3);
    be32(b+33*4,EV_CYL-1u); be32(b+34*4,1); be32(b+35*4,EV_CYLS-1u); be32(b+36*4,EV_CYL); be32(b+38*4,1);
    memcpy(b+40*4,id,sizeof id);
    fs_checksum(b,2,64);
}
static void ev_part_block(uint8_t *b) {
    memcpy(b,"PART",4);
    be32(b+1*4,64); be32(b+3*4,7);
    be32(b+4*4,0xFFFFFFFFu);
    be32(b+5*4,0);                          /* не загрузочный */
    b[9*4]=3; memcpy(b+9*4+1,"EVO",3);
    be32(b+32*4,16); be32(b+33*4,128);
    be32(b+35*4,EV_HEADS); be32(b+36*4,1); be32(b+37*4,EV_SECS); be32(b+38*4,FS_RESERVED);
    be32(b+41*4,1); be32(b+42*4,EV_CYLS-1u);
    be32(b+43*4,30);
    be32(b+45*4,0x1FE00u); be32(b+46*4,0x7FFFFFFEu);
    be32(b+47*4,(uint32_t)-128);           /* загрузиться с него и не пытаться */
    be32(b+48*4,FS_DOS1);
    fs_checksum(b,2,64);
}

/* ---- блоки, переписанные Amiga ---- */
static uint8_t *ev_overlay(uint32_t lba,int make) {
    if (!ev.ov_key) {
        if (!make) return NULL;
        ev.ov_key=core->alloc(EV_OVERLAY_SLOTS*sizeof *ev.ov_key,ZM_MEM_PSRAM);
        ev.ov_slot=core->alloc(EV_OVERLAY_SLOTS*sizeof *ev.ov_slot,ZM_MEM_PSRAM);
        ev.ov_data=core->alloc((size_t)EV_OVERLAY*512u,ZM_MEM_PSRAM);
        if (!ev.ov_key || !ev.ov_slot || !ev.ov_data) return NULL;
        memset(ev.ov_key,0,EV_OVERLAY_SLOTS*sizeof *ev.ov_key);
    }
    if (!ev.ov_data) return NULL;
    for (uint32_t i=0,h=(lba*2654435761u)%EV_OVERLAY_SLOTS;i<EV_OVERLAY_SLOTS;i++,h=(h+1u)%EV_OVERLAY_SLOTS) {
        if (ev.ov_key[h]==lba+1u) return ev.ov_data+(size_t)ev.ov_slot[h]*512u;
        if (!ev.ov_key[h]) {
            if (!make || ev.ov_count>=EV_OVERLAY) return NULL;
            ev.ov_key[h]=lba+1u; ev.ov_slot[h]=(uint16_t)ev.ov_count++;
            return ev.ov_data+(size_t)ev.ov_slot[h]*512u;
        }
    }
    return NULL;
}

/* RDB диска 0, как его видит Amiga. С флагом «последний диск» (RDBFF_LAST, у образов из меню F11 он стоит — amiga_fs.h)
 * Kickstart дальше диск не ищет, и тома EVO нет (модель tests/run_evo_boot.py: с флагом — «Please insert volume EVO», без —
 * том на месте). Пока EVO подключён, флаг снимается в отдаваемом секторе; файл образа не меняется. Сумма — заново, только
 * если была верна. */
static void ev_rdb_view(uint8_t *b) {
    if (!ev.ready || memcmp(b,"RDSK",4) || !(b[23]&1u)) return;
    uint32_t n=be32_get(b+4), sum=0;
    if (n<8u || n>128u) return;
    for (uint32_t i=0;i<n;i++) sum+=be32_get(b+i*4u);
    if (sum) return;
    b[23]&=(uint8_t)~1u;
    fs_checksum(b,2,n);
}

/* ---- диск 1 для ядра ---- */
static void ev_read(uint32_t lba,uint8_t *o) {
    const uint8_t *w=ev.ready?ev_overlay(lba,0):NULL;
    if (w) { memcpy(o,w,512); return; }
    memset(o,0,512);
    if (!ev.ready) return;
    if (lba==0) ev_rdb_block(o);
    else if (lba==1) ev_part_block(o);
    else if (lba>=EV_CYL && lba<EV_DISK_BLOCKS) ev_block(lba-EV_CYL,o);
}
static void ev_write(uint32_t lba,const uint8_t *data) {
    uint8_t *w=ev.ready?ev_overlay(lba,1):NULL;
    if (w) memcpy(w,data,512);
    else if (!ev.ov_full++) core->log("amiga: EVO: write cache full, block %u dropped (card untouched)",(unsigned)lba);
}
static void ev_close(void) {
    ev.ready=0;
    lha_close();
    for (unsigned i=0;i<EV_CHAINS;i++) if (ev.chains[i].ext) core->free(ev.chains[i].ext);
    void *mem[]={ev.ents,ev.dirs,ev.runs,ev.cache,ev.ov_key,ev.ov_slot,ev.ov_data};
    for (unsigned i=0;i<sizeof mem/sizeof mem[0];i++) if (mem[i]) core->free(mem[i]);
    memset(&ev,0,sizeof ev);
    core->mutex_lock(lock);
    uint8_t *rx=ev_rx; ev_rx=NULL; ev_sd_want=0;
    core->mutex_unlock(lock);
    if (rx) core->free(rx);
}
static int ev_is_bpb(const uint8_t *s) {
    return s[510]==0x55u && s[511]==0xAAu && (s[0]==0xEBu || s[0]==0xE9u) && little16(s+11)==512u &&
        s[13] && !(s[13]&(s[13]-1u)) && little16(s+14) && (s[16]==1u || s[16]==2u) &&
        (!memcmp(s+0x36,"FAT",3) || !memcmp(s+0x52,"FAT32",5));
}
static const char *ev_open(void) {
    ev.probe=1;
    const uint8_t *s=ev_sector(0,1);
    ev.probe=0;
    if (!s) return "no answer: VDAC4 Cores 0.15+ needed";
    uint32_t part=0;
    if (!ev_is_bpb(s)) {
        if (s[510]!=0x55u || s[511]!=0xAAu) return "no MBR";
        for (unsigned i=0;i<4u && !part;i++) {
            const uint8_t *p=s+446u+i*16u;
            if (p[4]==0x04u || p[4]==0x06u || p[4]==0x0Eu || p[4]==0x0Bu || p[4]==0x0Cu) part=little32(p+8);
        }
        if (!part) return "no FAT16/FAT32 partition";
        s=ev_sector(part,1);
        if (!s || !ev_is_bpb(s)) return "no FAT in the partition";
    }
    uint32_t spc=s[13], rsv=little16(s+14), fats=s[16], root_ent=little16(s+17);
    uint32_t total=little16(s+19)?little16(s+19):little32(s+32);
    uint32_t fat_size=little16(s+22)?little16(s+22):little32(s+36);
    uint32_t root_secs=(root_ent*32u+511u)/512u, system=rsv+fats*fat_size+root_secs;
    if (!fat_size || total<=system) return "bad FAT";
    ev.clusters=(total-system)/spc;
    if (ev.clusters<4085u) return "FAT12 is not supported";
    ev.fat32=ev.clusters>=65525u;
    ev.part=part; ev.spc=spc; ev.fat=part+rsv; ev.root_lba=part+rsv+fats*fat_size; ev.root_secs=root_secs;
    ev.data=part+system; ev.root_cluster=ev.fat32?little32(s+44):0u;
    fs_layout(&ev.layout,EV_PART_BLOCKS,FS_DOS1,"EVO",fs_stamp(evo_time));
    ev.next=FS_RESERVED;
    uint32_t root;
    if (!ev_dir_load(0,ev.layout.root,0,&root)) return "no memory";
    return NULL;
}
/* При старте ядра, до снятия сброса: Kickstart ищет диски сразу. Плагина 0.15+ нет — диска нет, Amiga работает. */
static void ev_mount(void) {
    ev_close();
    ev.cache=core->alloc((size_t)EV_CACHE*512u,ZM_MEM_PSRAM);
    uint8_t *rx=core->alloc((size_t)EV_BURST*512u,ZM_MEM_PSRAM);
    core->mutex_lock(lock); ev_rx=rx; core->mutex_unlock(lock);
    if (!ev.cache || !rx) { core->log("amiga: EVO: no memory"); ev_close(); return; }
    const char *problem=ev_open();
    if (problem) { core->log("amiga: EVO: %s",problem); ev_close(); return; }
    ev.ready=1;
    if (!mount_unit(5,EV_DISK_BLOCKS*512u)) { core->log("amiga: EVO: core does not answer"); ev_close(); return; }
    core->log("amiga: EVO: FAT%u at %u, %u clusters x %u, root %u entries",ev.fat32?32u:16u,(unsigned)ev.part,
              (unsigned)ev.clusters,(unsigned)ev.spc,(unsigned)ev.dirs[0].count);
}
