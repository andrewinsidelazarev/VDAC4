/* SPDX-License-Identifier: GPL-3.0-or-later
 * Архивы LHA (.lha, .lzh) на карте Evo — ещё и каталоги тома EVO: (AMIGA.ELF 0.19). Файл архива остаётся файлом, рядом —
 * каталог с тем же именем без расширения: в нём файлы и папки архива, распакованные на лету. Ставить программы можно
 * прямо оттуда, без LhA на Amiga.
 * Заголовки уровней 0, 1 и 2 (уровень 3 — нет); методы -lh0- и -lz4- (без сжатия), -lh1- (динамический Хаффман, LHarc
 * 1.x), -lh4-…-lh7- (статический Хаффман, словарь 4–64 КБ), -lhd- — каталог; члены с другими методами не показываются.
 * Формат и обе распаковки сверены с lhasa (Simon Howard, ISC; копия — libxmp src/depackers/lhasa): словарь сначала
 * заполнен пробелами, коды канонические (по длине, затем по номеру), в имени уровня 0/1 после нуля — комментарий Amiga.
 * Распаковка идёт подряд: блок дальше выданного досчитывается, блок, ушедший из кольца 128 КБ, — член заново с начала.
 * CRC-16 члена сверяется, когда он распакован целиком. Всё состояние — в куче: ни таблиц указателей, ни индексов
 * «статический массив минус N» (загрузчик модулей ESP, tests/check_elf_pointers.py).
 */
#define LHA_RING (1u<<17)                      /* как у lhasa для -lh7-: вдвое больше словаря */
#define LHA_RING_MASK (LHA_RING-1u)
#define LHA_NC 510u                            /* литералы и длины повтора */
#define LHA_NT 19u                             /* коды длин таблицы литералов */
#define LHA_SLOTS 2u                           /* распаковок сразу: файл и его значок */
#define LHA_MAX_MEMBERS 65536u
#define LHA_HEADER_MAX 4096u
#define LH1_CODES 314u
#define LH1_NODES (LH1_CODES*2u-1u)
#define LH1_REORDER 32768u                     /* перестройка дерева -lh1- */
enum { LHA_STORED=0, LHA_LH1=1, LHA_DIR=0xFE, LHA_UNKNOWN=0xFF };     /* -lh4-…-lh7- — 4…7 */

typedef struct { uint32_t ent, first, count, parsed; } lha_arc_t;      /* ent — запись файла архива на FAT */
typedef struct {
    uint32_t data, packed, size, name;         /* сжатое в файле архива, длины, путь в lha.names */
    uint16_t nlen, date, time, crc;            /* дата и время — как у FAT */
    uint8_t method, pad;
} lha_member_t;
typedef struct { uint16_t count[17]; uint16_t sym[LHA_NC]; int16_t fixed; } lha_huff_t;   /* fixed — один код, 0 бит */
typedef struct { uint16_t child, parent, freq, group; uint8_t leaf; } lh1_node_t;
typedef struct {
    uint32_t member;                           /* номер члена + 1; 0 — свободна */
    uint32_t arcent, in_at, in_end, extra;     /* файл архива, следующий байт и конец сжатого, нулей за концом */
    uint8_t in[512]; uint32_t in_len, in_pos;
    uint32_t bits, nbits;
    uint32_t out, copy_len, copy_from, block, pbit;
    uint16_t crc; uint8_t method, failed, checked;
    lha_huff_t c, p, t;
    uint8_t len[LHA_NC];
    lh1_node_t nodes[LH1_NODES];
    uint16_t leaf_nodes[LH1_CODES], groups[LH1_NODES], leader[LH1_NODES];
    uint32_t num_groups;
    uint8_t offset_lookup[256], offset_len[64];
    uint8_t *ring;
} lha_dec_t;
static struct {
    lha_arc_t *arcs; uint32_t n_arcs, cap_arcs;
    lha_member_t *members; uint32_t n_members, cap_members;
    char *names; uint32_t n_names, cap_names;
    lha_dec_t *dec[LHA_SLOTS]; uint32_t turn;
    uint32_t crc_bad, crc_ok, broken, unsupported;
} lha;

static int ev_file_bytes(uint32_t ent,uint32_t at,uint8_t *out,uint32_t n);

/* ---- список архива ---- */
static int lha_is_name(const char *raw,unsigned rn) {
    if (rn<5u || raw[rn-4u]!='.') return 0;
    unsigned a=(uint8_t)raw[rn-3u]|0x20u, b=(uint8_t)raw[rn-2u]|0x20u, c=(uint8_t)raw[rn-1u]|0x20u;
    return a=='l' && ((b=='h' && c=='a') || (b=='z' && c=='h'));
}
/* Файл .lha или .lzh каталога FAT — рядом каталог-архив (разбирается, когда Amiga его откроет). */
static void ev_lha_offer(ev_load_t *ld,uint32_t file,const char *raw,unsigned rn) {
    if (!lha_is_name(raw,rn) || ev.ents[file].size<22u) return;
    if (!ev_room((void **)&lha.arcs,&lha.cap_arcs,lha.n_arcs+1u,sizeof *lha.arcs,EV_ENTS_MAX)) return;
    const uint16_t date=ev.ents[file].date, time=ev.ents[file].time;
    ev_ent_t *x=ev_new(ld,raw,rn-4u);
    if (!x) return;
    lha_arc_t *a=&lha.arcs[lha.n_arcs];
    memset(a,0,sizeof *a);
    a->ent=file;
    x->flags=EV_F_DIR|EV_F_ARC; x->arc=++lha.n_arcs; x->date=date; x->time=time;
}
static uint8_t lha_method(const uint8_t *m) {
    if (m[0]!='-' || m[4]!='-') return LHA_UNKNOWN;
    if (m[1]=='l' && m[2]=='h') {
        if (m[3]=='0') return LHA_STORED;
        if (m[3]=='1') return LHA_LH1;
        if (m[3]>='4' && m[3]<='7') return (uint8_t)(m[3]-'0');
        if (m[3]=='d') return LHA_DIR;
    }
    if (m[1]=='l' && m[2]=='z' && m[3]=='4') return LHA_STORED;
    return LHA_UNKNOWN;
}
/* Время Unix (уровень 2) — в дату и время FAT, по UTC; до 1980 года — 1.1.1980. */
static void lha_unix_fat(uint32_t t,uint16_t *date,uint16_t *time) {
    uint32_t z=t/86400u+719468u, era=z/146097u, doe=z-era*146097u;
    uint32_t yoe=(doe-doe/1460u+doe/36524u-doe/146096u)/365u, doy=doe-(365u*yoe+yoe/4u-yoe/100u);
    uint32_t mp=(5u*doy+2u)/153u, day=doy-(153u*mp+2u)/5u+1u, month=mp<10u?mp+3u:mp-9u, year=yoe+era*400u+(month<=2u);
    uint32_t s=t%86400u;
    if (year<1980u) { *date=(1u<<5)|1u; *time=0; return; }
    if (year>2107u) year=2107u;
    *date=(uint16_t)((year-1980u)<<9|month<<5|day);
    *time=(uint16_t)((s/3600u)<<11|(s/60u%60u)<<5|(s%60u)/2u);
}
/* Путь члена: '\' и 0xFF — разделители; без разделителя в начале и в конце, без пустых частей и «.». Новая длина. */
static unsigned lha_norm(char *p,unsigned n) {
    unsigned o=0, seg=0;
    for (unsigned i=0;i<n;i++) {
        char c=p[i];
        if (c=='\\' || (uint8_t)c==0xFFu) c='/';
        if (c!='/') { p[o++]=c; continue; }
        if (o==seg) continue;
        if (o-seg==1u && p[seg]=='.') { o=seg; continue; }
        p[o++]='/'; seg=o;
    }
    if (o==seg+1u && p[seg]=='.') o=seg;
    while (o && p[o-1u]=='/') o--;
    return o;
}
/* Расширенный заголовок: 1 — имя, 2 — каталог (части через 0xFF). */
static void lha_ext(uint8_t type,const uint8_t *data,unsigned n,char *name,unsigned *name_n,char *dir,unsigned *dir_n) {
    if (n>255u) n=255u;
    if (type==1u) { memcpy(name,data,n); *name_n=n; }
    else if (type==2u) { memcpy(dir,data,n); *dir_n=n; }
}
static void lha_parse(uint32_t a) {
    if (lha.arcs[a].parsed) return;
    lha.arcs[a].parsed=1; lha.arcs[a].first=lha.n_members;
    const uint32_t file=lha.arcs[a].ent, total=ev.ents[file].size;
    static uint8_t h[LHA_HEADER_MAX];
    static char name[256], dir[256], path[520];
    uint32_t at=0;
    while (at+22u<=total && lha.n_members-lha.arcs[a].first<LHA_MAX_MEMBERS) {
        if (!ev_file_bytes(file,at,h,total-at<26u?total-at:26u)) break;
        if (!h[0]) break;                                       /* конец архива */
        const unsigned level=h[20];
        uint32_t hlen, packed=little32(h+7), size=little32(h+11), data;
        unsigned name_n=0, dir_n=0;
        uint16_t date, time, crc;
        uint8_t method=lha_method(h+2);
        if (level<2u) {
            hlen=h[0]+2u;
            if (hlen<24u || hlen>total-at || !ev_file_bytes(file,at,h,hlen)) break;
            const unsigned nl=h[21];
            if (22u+nl+2u>hlen) break;
            time=little16(h+15); date=little16(h+17); crc=little16(h+22+nl);
            while (name_n<nl && h[22+name_n]) name_n++;         /* после нуля — комментарий Amiga */
            memcpy(name,h+22,name_n);
            data=at+hlen;
            if (level==1u) {
                unsigned next=little16(h+hlen-2u), bad=0;
                while (next) {
                    if (next<3u || next>LHA_HEADER_MAX || next>packed || data+next>total || !ev_file_bytes(file,data,h,next)) { bad=1; break; }
                    lha_ext(h[0],h+1,next-3u,name,&name_n,dir,&dir_n);
                    packed-=next; data+=next; next=little16(h+next-2u);
                }
                if (bad) break;
            }
        } else if (level==2u) {
            hlen=little16(h);
            if (hlen<26u || hlen>LHA_HEADER_MAX || hlen>total-at || !ev_file_bytes(file,at,h,hlen)) break;
            lha_unix_fat(little32(h+15),&date,&time); crc=little16(h+21);
            for (unsigned off=24;off+2u<=hlen;) {
                const unsigned n=little16(h+off);
                if (!n || n<3u || off+n>hlen) break;
                lha_ext(h[off+2u],h+off+3u,n-3u,name,&name_n,dir,&dir_n);
                off+=n;
            }
            data=at+hlen;
        } else break;                                           /* уровень 3 или не архив */
        if (data>total || packed>total-data) break;
        at=data+packed;
        if (method==LHA_UNKNOWN) { if (!lha.unsupported++) core->log("amiga: EVO: LHA method not supported, member hidden"); continue; }
        unsigned n=0;
        for (unsigned i=0;i<dir_n;i++) path[n++]=dir[i];
        if (n) path[n++]='/';
        for (unsigned i=0;i<name_n;i++) path[n++]=name[i];
        n=lha_norm(path,n);
        if (!n) continue;
        if (!ev_room((void **)&lha.members,&lha.cap_members,lha.n_members+1u,sizeof *lha.members,LHA_MAX_MEMBERS*16u) ||
            !ev_room((void **)&lha.names,&lha.cap_names,lha.n_names+n,1,64u*1024u*1024u)) break;
        lha_member_t *m=&lha.members[lha.n_members++];
        m->data=data; m->packed=packed; m->size=method==LHA_STORED && size>packed?packed:size;
        m->name=lha.n_names; m->nlen=(uint16_t)n; m->date=date; m->time=time; m->crc=crc; m->method=method; m->pad=0;
        memcpy(lha.names+lha.n_names,path,n); lha.n_names+=n;
    }
    lha.arcs[a].count=lha.n_members-lha.arcs[a].first;
}
/* Каталог архива a: всё, что лежит под путём члена ref длиной plen (plen 0 — корень архива). */
static int ev_arc_dir_load(uint32_t a,uint32_t ref,uint32_t plen,uint32_t header,uint32_t parent,uint32_t *out) {
    lha_parse(a);
    if (!ev_dir_begin(&ev_ld)) return 0;
    const uint32_t first=lha.arcs[a].first, count=lha.arcs[a].count;
    for (uint32_t m=first;m<first+count;m++) {
        const lha_member_t *mb=&lha.members[m];
        const char *p=lha.names+mb->name;
        const unsigned n=mb->nlen;
        unsigned at=0;
        if (plen) {
            if (n<=plen || p[plen]!='/' || memcmp(p,lha.names+lha.members[ref].name,plen)) continue;
            at=plen+1u;
        }
        const char *slash=memchr(p+at,'/',n-at);
        const unsigned end=slash?(unsigned)(slash-p):n;
        const int dir=slash!=NULL || mb->method==LHA_DIR;
        char raw[EV_RAW_NAME]; unsigned rn=0;
        for (unsigned i=at;i<end;i++) rn=ev_put(raw,rn,(uint8_t)p[i]);
        if (!rn) continue;
        if (dir) {                                              /* папка — один раз, на первом её члене */
            int seen=0;
            for (uint32_t i=ev_ld.first;i<ev.n_ents && !seen;i++) {
                const ev_ent_t *x=&ev.ents[i];
                seen=(x->flags&EV_F_DIR) && x->size==end && !memcmp(lha.names+lha.members[x->cluster].name,p,end);
            }
            if (seen) continue;
        }
        ev_ent_t *x=ev_new(&ev_ld,raw,rn);
        if (!x) break;
        x->flags=EV_F_MEMBER|(dir?EV_F_DIR:0u); x->arc=a+1u; x->cluster=m; x->size=dir?end:mb->size;
        x->date=mb->date; x->time=mb->time;
    }
    ev_dir_finish(&ev_ld,header,parent,out);
    return 1;
}

/* ---- распаковка ---- */
static unsigned lha_byte(lha_dec_t *d) {
    if (d->in_pos==d->in_len) {
        uint32_t n=d->in_end-d->in_at;
        if (!n) { if (++d->extra>64u) d->failed=1; return 0; }   /* за концом сжатого — нули, недолго */
        if (n>512u) n=512u;
        if (!ev_file_bytes(d->arcent,d->in_at,d->in,n)) { d->failed=1; return 0; }
        d->in_at+=n; d->in_len=n; d->in_pos=0;
    }
    return d->in[d->in_pos++];
}
static unsigned lha_bits(lha_dec_t *d,unsigned n) {            /* n не больше 16, старшие биты первыми */
    while (d->nbits<n) { d->bits=d->bits<<8|lha_byte(d); d->nbits+=8u; }
    d->nbits-=n;
    return (d->bits>>d->nbits)&((1u<<n)-1u);
}
static unsigned lha_peek8(lha_dec_t *d) {
    while (d->nbits<8u) { d->bits=d->bits<<8|lha_byte(d); d->nbits+=8u; }
    return (d->bits>>(d->nbits-8u))&0xFFu;
}
static void lha_putb(lha_dec_t *d,uint8_t b) {
    d->ring[d->out&LHA_RING_MASK]=b; d->out++;
    d->crc^=b;
    for (unsigned k=0;k<8u;k++) d->crc=(uint16_t)((d->crc>>1)^((d->crc&1u)?0xA001u:0u));
}
/* Канонический код по длинам: короче — раньше, при равной длине — по номеру. */
static int lha_build(lha_huff_t *h,const uint8_t *len,unsigned n) {
    uint16_t offs[17];
    memset(h->count,0,sizeof h->count); h->fixed=-1;
    for (unsigned i=0;i<n;i++) { if (len[i]>16u) return 0; h->count[len[i]]++; }
    h->count[0]=0; offs[1]=0;
    for (unsigned l=1;l<16u;l++) offs[l+1u]=(uint16_t)(offs[l]+h->count[l]);
    for (unsigned i=0;i<n;i++) if (len[i]) h->sym[offs[len[i]]++]=(uint16_t)i;
    return 1;
}
static int lha_decode(lha_dec_t *d,const lha_huff_t *h) {
    if (h->fixed>=0) return h->fixed;
    int code=0, first=0, index=0;
    for (unsigned l=1;l<=16u;l++) {
        code|=(int)lha_bits(d,1);
        const int count=h->count[l];
        if (code-count<first) return h->sym[index+(code-first)];
        index+=count; first+=count; first<<=1; code<<=1;
    }
    return -1;
}
/* Длины кодов: 3 бита, 7 — дальше единицы до нуля; после special-й длины — 2 бита нулей вперёд. */
static int lha_lengths(lha_dec_t *d,lha_huff_t *h,unsigned nn,unsigned nbit,unsigned special) {
    unsigned n=lha_bits(d,nbit);
    if (!n) { const unsigned c=lha_bits(d,nbit); if (c>=nn) return 0; h->fixed=(int16_t)c; return 1; }
    if (n>nn) n=nn;
    unsigned i=0;
    while (i<n) {
        unsigned c=lha_bits(d,3);
        if (c==7u) while (lha_bits(d,1)) if (++c>16u) return 0;
        d->len[i++]=(uint8_t)c;
        if (i==special) for (unsigned z=lha_bits(d,2);z && i<nn;z--) d->len[i++]=0;
    }
    while (i<nn) d->len[i++]=0;
    return lha_build(h,d->len,nn);
}
static int lha_ctable(lha_dec_t *d) {
    unsigned n=lha_bits(d,9);
    if (!n) { const unsigned c=lha_bits(d,9); if (c>=LHA_NC) return 0; d->c.fixed=(int16_t)c; return 1; }
    if (n>LHA_NC) n=LHA_NC;
    unsigned i=0;
    while (i<n) {
        const int c=lha_decode(d,&d->t);
        if (c<0) return 0;
        if (c<=2) {
            unsigned k=c==0?1u:c==1?lha_bits(d,4)+3u:lha_bits(d,9)+20u;
            while (k-- && i<n) d->len[i++]=0;
        } else d->len[i++]=(uint8_t)(c-2);
    }
    while (i<LHA_NC) d->len[i++]=0;
    return lha_build(&d->c,d->len,LHA_NC);
}
static int lha_block(lha_dec_t *d) {
    for (unsigned guard=0;!d->block;guard++) {
        if (guard>4u || d->failed) return 0;
        d->block=lha_bits(d,16);
        if (!lha_lengths(d,&d->t,LHA_NT,5,3) || !lha_ctable(d) || !lha_lengths(d,&d->p,(1u<<d->pbit)-1u,d->pbit,0)) return 0;
    }
    return 1;
}
/* -lh1-: дерево Хаффмана подстраивается под частоты (lhasa lh1_decoder.c), группы — узлы с равной частотой. */
static uint16_t lh1_group_new(lha_dec_t *d) { return d->groups[d->num_groups++]; }
static void lh1_groups_reset(lha_dec_t *d) {
    for (uint32_t i=0;i<LH1_NODES;i++) d->groups[i]=(uint16_t)i;
    d->num_groups=0;
}
static void lh1_init(lha_dec_t *d) {
    static const uint8_t fdist[6]={1,3,8,12,24,16};
    lh1_groups_reset(d);
    int node=(int)LH1_NODES-1;
    const uint16_t leaves=lh1_group_new(d);
    for (uint32_t i=0;i<LH1_CODES;i++,node--) {
        lh1_node_t *x=&d->nodes[node];
        x->leaf=1; x->child=(uint16_t)i; x->freq=1; x->group=leaves;
        d->leader[leaves]=(uint16_t)node; d->leaf_nodes[i]=(uint16_t)node;
    }
    for (uint32_t child=LH1_NODES-1u;node>=0;node--,child-=2u) {
        lh1_node_t *x=&d->nodes[node];
        x->leaf=0; x->child=(uint16_t)child;
        d->nodes[child].parent=(uint16_t)node; d->nodes[child-1u].parent=(uint16_t)node;
        x->freq=(uint16_t)(d->nodes[child].freq+d->nodes[child-1u].freq);
        x->group=x->freq==d->nodes[node+1].freq?d->nodes[node+1].group:lh1_group_new(d);
        d->leader[x->group]=(uint16_t)node;
    }
    uint32_t code=0, offset=0;
    for (unsigned i=0;i<6u;i++) {
        const unsigned len=i+3u, step=1u<<(8u-len);
        for (unsigned j=0;j<fdist[i];j++,offset++,code+=step) {
            for (unsigned k=0;k<step;k++) d->offset_lookup[code+k]=(uint8_t)offset;
            d->offset_len[offset]=(uint8_t)len;
        }
    }
}
static uint16_t lh1_lead(lha_dec_t *d,uint16_t n) {
    const uint16_t l=d->leader[d->nodes[n].group];
    if (l==n) return n;
    lh1_node_t *a=&d->nodes[n], *b=&d->nodes[l];
    const uint8_t leaf=b->leaf; b->leaf=a->leaf; a->leaf=leaf;
    const uint16_t child=b->child; b->child=a->child; a->child=child;
    if (a->leaf) d->leaf_nodes[a->child]=n; else { d->nodes[a->child].parent=n; d->nodes[a->child-1u].parent=n; }
    if (b->leaf) d->leaf_nodes[b->child]=l; else { d->nodes[b->child].parent=l; d->nodes[b->child-1u].parent=l; }
    return l;
}
static void lh1_bump(lha_dec_t *d,uint16_t n) {
    lh1_node_t *x=&d->nodes[n], *o=&d->nodes[n-1u];
    x->freq++;
    if (n<LH1_NODES-1u && x->group==d->nodes[n+1u].group) {
        d->leader[x->group]++;
        if (x->freq==o->freq) x->group=o->group;
        else { x->group=lh1_group_new(d); d->leader[x->group]=n; }
    } else if (x->freq==o->freq) {
        d->groups[--d->num_groups]=x->group;
        x->group=o->group;
    }
}
static void lh1_rebuild(lha_dec_t *d) {
    uint32_t leaves=0;
    for (uint32_t i=0;i<LH1_NODES;i++) if (d->nodes[i].leaf) {
        lh1_node_t *x=&d->nodes[leaves++];
        x->leaf=1; x->child=d->nodes[i].child; x->freq=(uint16_t)((d->nodes[i].freq+1u)/2u);
    }
    int li=(int)LH1_CODES-1, i=(int)LH1_NODES-1;
    for (uint32_t child=LH1_NODES-1u;i>=0;i--,child-=2u) {
        while ((int)child-i<2) { d->nodes[i]=d->nodes[li]; d->leaf_nodes[d->nodes[li].child]=(uint16_t)i; i--; li--; }
        const uint32_t freq=(uint32_t)d->nodes[child].freq+d->nodes[child-1u].freq;
        while (li>=0 && freq>=d->nodes[li].freq) { d->nodes[i]=d->nodes[li]; d->leaf_nodes[d->nodes[li].child]=(uint16_t)i; i--; li--; }
        d->nodes[i].leaf=0; d->nodes[i].freq=(uint16_t)freq; d->nodes[i].child=(uint16_t)child;
        d->nodes[child].parent=(uint16_t)i; d->nodes[child-1u].parent=(uint16_t)i;
    }
    lh1_groups_reset(d);
    uint16_t g=lh1_group_new(d);
    d->nodes[0].group=g; d->leader[g]=0;
    for (uint32_t k=1;k<LH1_NODES;k++) {
        if (d->nodes[k].freq==d->nodes[k-1u].freq) d->nodes[k].group=d->nodes[k-1u].group;
        else { g=lh1_group_new(d); d->nodes[k].group=g; d->leader[g]=(uint16_t)k; }
    }
}
static int lh1_code(lha_dec_t *d) {
    uint32_t n=0;
    for (unsigned guard=0;!d->nodes[n].leaf;guard++) {
        if (guard>LH1_NODES) return -1;
        n=d->nodes[n].child-lha_bits(d,1);
    }
    const uint16_t code=d->nodes[n].child;
    if (d->nodes[0].freq>=LH1_REORDER) lh1_rebuild(d);
    d->nodes[0].freq++;
    for (uint16_t k=d->leaf_nodes[code];k;k=d->nodes[k].parent) { k=lh1_lead(d,k); lh1_bump(d,k); }
    return code;
}
static uint32_t lh1_offset(lha_dec_t *d) {
    const unsigned off=d->offset_lookup[lha_peek8(d)];
    lha_bits(d,d->offset_len[off]);
    return (uint32_t)off<<6|lha_bits(d,6);
}
/* Следующий байт члена. Ноль — сжатое кончилось раньше или испорчено. */
static int lha_step(lha_dec_t *d) {
    if (!d->copy_len) {
        int c;
        uint32_t dist;
        if (d->method==LHA_LH1) {
            c=lh1_code(d);
            if (c<0) return 0;
            if (c<256) { lha_putb(d,(uint8_t)c); return !d->failed; }
            dist=lh1_offset(d);
        } else {
            if (!d->block && !lha_block(d)) return 0;
            d->block--;
            c=lha_decode(d,&d->c);
            if (c<0) return 0;
            if (c<256) { lha_putb(d,(uint8_t)c); return !d->failed; }
            const int p=lha_decode(d,&d->p);
            if (p<0 || p>17) return 0;
            dist=p<=1?(uint32_t)p:(1u<<(p-1))+lha_bits(d,(unsigned)p-1u);
        }
        d->copy_len=(uint32_t)c-256u+3u; d->copy_from=d->out-dist-1u;
    }
    d->copy_len--;
    lha_putb(d,d->ring[d->copy_from++&LHA_RING_MASK]);
    return !d->failed;
}
static void lha_reset(lha_dec_t *d,uint32_t m,uint32_t arcent) {
    const lha_member_t *mb=&lha.members[m];
    d->member=m+1u; d->arcent=arcent; d->in_at=mb->data; d->in_end=mb->data+mb->packed; d->in_len=d->in_pos=0; d->extra=0;
    d->bits=d->nbits=0; d->out=d->copy_len=d->copy_from=d->block=0; d->crc=0; d->failed=0; d->checked=0;
    d->method=mb->method; d->pbit=mb->method>=6u?5u:4u;
    memset(d->ring,' ',LHA_RING);
    if (d->method==LHA_LH1) lh1_init(d);
}
static lha_dec_t *lha_slot(uint32_t m) {
    for (unsigned i=0;i<LHA_SLOTS;i++) if (lha.dec[i] && lha.dec[i]->member==m+1u) return lha.dec[i];
    const unsigned s=lha.turn++%LHA_SLOTS;
    if (!lha.dec[s]) {
        lha_dec_t *d=core->alloc(sizeof *d,ZM_MEM_PSRAM);
        uint8_t *ring=d?core->alloc(LHA_RING,ZM_MEM_PSRAM):NULL;
        if (!ring) { if (d) core->free(d); core->log("amiga: EVO: no memory to unpack LHA"); return NULL; }
        memset(d,0,sizeof *d); d->ring=ring; lha.dec[s]=d;
    }
    lha.dec[s]->member=0;
    return lha.dec[s];
}
/* Блок файла из архива. Не распаковать — нули, и это в журнале. */
static void ev_member_block(uint32_t ent,uint32_t block,uint8_t *o) {
    const uint32_t m=ev.ents[ent].cluster, arcent=lha.arcs[ev.ents[ent].arc-1u].ent;
    const lha_member_t *mb=&lha.members[m];
    const uint32_t from=block*512u;
    if (from>=mb->size) return;
    const uint32_t n=mb->size-from<512u?mb->size-from:512u;
    if (mb->method==LHA_STORED) {
        if (!ev_file_bytes(arcent,mb->data+from,o,n)) memset(o,0,512);
        return;
    }
    lha_dec_t *d=lha_slot(m);
    if (!d) return;
    if (d->member!=m+1u || d->failed || (d->out>LHA_RING && from<d->out-LHA_RING)) lha_reset(d,m,arcent);
    while (d->out<from+n) if (!lha_step(d)) break;
    if (d->out<from+n) {
        if (!lha.broken++) core->log("amiga: EVO: LHA member damaged at %u",(unsigned)d->out);
        d->failed=1;
        return;
    }
    for (uint32_t i=0;i<n;i++) o[i]=d->ring[(from+i)&LHA_RING_MASK];
    if (d->out==mb->size && !d->checked) {
        d->checked=1;
        if (d->crc==mb->crc) lha.crc_ok++;
        else if (!lha.crc_bad++) core->log("amiga: EVO: LHA CRC mismatch %04X/%04X",d->crc,mb->crc);
    }
}
static void lha_close(void) {
    for (unsigned i=0;i<LHA_SLOTS;i++) if (lha.dec[i]) { core->free(lha.dec[i]->ring); core->free(lha.dec[i]); }
    void *mem[]={lha.arcs,lha.members,lha.names};
    for (unsigned i=0;i<sizeof mem/sizeof mem[0];i++) if (mem[i]) core->free(mem[i]);
    memset(&lha,0,sizeof lha);
}
