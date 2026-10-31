/* SPDX-License-Identifier: GPL-3.0-or-later
 * Вкладка NET меню дискет: коллекции archive.org commodore-amiga-games-adf-a…z.
 * Список раздела — метаданные коллекции (JSON), дискета — ZIP с одним ADF. Архив живёт только в
 * памяти: на карту Tang ложится распакованный образ, /Amiga/Games/Downloads/ИМЯ.adf. Дату файла
 * ставит прошивка по своим часам: их выставляет плагин от часов Evo (SYS_CLOCK), как и RTC Amiga.
 * Образ, который уже скачан, читается с карты и заново не качается. Список раздела тоже лежит на карте,
 * NET_DIR/LIST-А.TXT: первая строка «#изменён проверен», дальше по имени в строке. Изменён — item_last_updated
 * раздела, проверен — день (как в томе AmigaDOS). Раз в NET_CHECK_DAYS спрашивается только item_last_updated: ответ
 * в 21 байт, но и он идёт секунды, поэтому не на каждый заход. Весь список качается, когда раздел изменился или по F5. */
#define NET_META "https://archive.org/metadata/commodore-amiga-games-adf-"
#define NET_FILES "https://archive.org/download/commodore-amiga-games-adf-"
#define NET_DIR "/Amiga/Games/Downloads"
#define NET_WAIT_MS 15000u
#define NET_ZIP_MAX (2u*1024u*1024u)
#define NET_LIST_MAX (4u*1024u*1024u)
#define NET_CHECK_DAYS 30u

/* ---- deflate (RFC 1951): образ целиком в памяти, окно — сам выходной буфер ---- */
typedef struct {
    const uint8_t *in, *in_end;
    uint32_t bits; unsigned have;
    uint8_t *out; size_t at, cap;
    int bad;
} inf_t;
typedef struct { uint16_t count[16], symbol[288]; } inf_huff_t;

static unsigned inf_bits(inf_t *s,unsigned need) {
    while (s->have<need) {
        if (s->in==s->in_end) { s->bad=1; return 0; }
        s->bits|=(uint32_t)*s->in++<<s->have;
        s->have+=8;
    }
    unsigned v=s->bits&((1u<<need)-1u);
    if (need==0) v=0;
    s->bits>>=need; s->have-=need;
    return v;
}
static void inf_build(inf_huff_t *h,const uint8_t *len,unsigned n) {
    uint16_t offs[16];
    memset(h->count,0,sizeof h->count);
    for (unsigned i=0;i<n;i++) h->count[len[i]]++;
    h->count[0]=0;
    offs[1]=0;
    for (unsigned i=1;i<15;i++) offs[i+1]=(uint16_t)(offs[i]+h->count[i]);
    for (unsigned i=0;i<n;i++) if (len[i]) h->symbol[offs[len[i]]++]=(uint16_t)i;
}
static int inf_decode(inf_t *s,const inf_huff_t *h) {
    int code=0,first=0,index=0;
    for (unsigned len=1;len<=15;len++) {
        code|=(int)inf_bits(s,1);
        if (s->bad) return -1;
        int count=h->count[len];
        if (code-count<first) return h->symbol[index+(code-first)];
        index+=count; first+=count; first<<=1; code<<=1;
    }
    s->bad=1;
    return -1;
}
static void inf_codes(inf_t *s,const inf_huff_t *lit,const inf_huff_t *dist) {
    static const uint16_t lbase[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t lext[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t dbase[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t dext[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    size_t next_yield=s->at+65536;
    for (;;) {
        int sym=inf_decode(s,lit);
        if (sym<0) return;
        if (sym<256) {
            if (s->at>=s->cap) { s->bad=1; return; }
            s->out[s->at++]=(uint8_t)sym;
        } else if (sym==256) return;
        else {
            sym-=257;
            if (sym>=29) { s->bad=1; return; }
            unsigned len=lbase[sym]+inf_bits(s,lext[sym]);
            int ds=inf_decode(s,dist);
            if (ds<0 || ds>=30) { s->bad=1; return; }
            size_t d=dbase[ds]+inf_bits(s,dext[ds]);
            if (s->bad || d>s->at || len>s->cap-s->at) { s->bad=1; return; }
            while (len--) { s->out[s->at]=s->out[s->at-d]; s->at++; }
        }
        // Распаковка идёт секунды: задача не должна занимать ядро целиком.
        if (s->at>=next_yield) { core->delay(1); next_yield=s->at+65536; }
    }
}
/* 1 — распаковано ровно cap байтов */
static int inflate_raw(const uint8_t *in,size_t n,uint8_t *out,size_t cap) {
    static inf_huff_t lit, dist;
    static uint8_t lens[320];
    inf_t s={in,in+n,0,0,out,0,cap,0};
    unsigned last;
    do {
        last=inf_bits(&s,1);
        unsigned type=inf_bits(&s,2);
        if (s.bad) return 0;
        if (type==0) {
            s.bits=0; s.have=0;
            if (s.in_end-s.in<4) return 0;
            unsigned len=s.in[0]|(unsigned)s.in[1]<<8, inv=s.in[2]|(unsigned)s.in[3]<<8;
            s.in+=4;
            if ((len^inv)!=0xFFFFu || (size_t)(s.in_end-s.in)<len || len>cap-s.at) return 0;
            memcpy(out+s.at,s.in,len); s.at+=len; s.in+=len;
        } else if (type==1) {
            unsigned i=0;
            for (;i<144;i++) lens[i]=8;
            for (;i<256;i++) lens[i]=9;
            for (;i<280;i++) lens[i]=7;
            for (;i<288;i++) lens[i]=8;
            inf_build(&lit,lens,288);
            for (i=0;i<30;i++) lens[i]=5;
            inf_build(&dist,lens,30);
            inf_codes(&s,&lit,&dist);
        } else if (type==2) {
            static const uint8_t order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            unsigned nlen=inf_bits(&s,5)+257, ndist=inf_bits(&s,5)+1, ncode=inf_bits(&s,4)+4;
            if (s.bad || nlen>286 || ndist>30) return 0;
            memset(lens,0,19);
            for (unsigned i=0;i<ncode;i++) lens[order[i]]=(uint8_t)inf_bits(&s,3);
            inf_build(&lit,lens,19);
            unsigned i=0;
            while (i<nlen+ndist) {
                int sym=inf_decode(&s,&lit);
                if (sym<0) return 0;
                if (sym<16) lens[i++]=(uint8_t)sym;
                else {
                    unsigned prev=0, rep;
                    if (sym==16) {
                        if (!i) return 0;
                        prev=lens[i-1]; rep=3+inf_bits(&s,2);
                    } else if (sym==17) rep=3+inf_bits(&s,3);
                    else rep=11+inf_bits(&s,7);
                    if (s.bad || i+rep>nlen+ndist) return 0;
                    while (rep--) lens[i++]=(uint8_t)prev;
                }
            }
            /* длины кодов нужны второй таблице раньше, чем первая их перезапишет */
            inf_build(&dist,lens+nlen,ndist);
            inf_build(&lit,lens,nlen);
            inf_codes(&s,&lit,&dist);
        } else return 0;
        if (s.bad) return 0;
    } while (!last);
    return s.at==cap;
}
static uint32_t zip16(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8; }
/* Первый образ 880 КиБ в ZIP — в новый буфер PSRAM. NULL и текст ошибки в *why, если нет. */
static uint8_t *zip_extract_adf(const uint8_t *z,size_t n,const char **why) {
    *why="NOT A ZIP ARCHIVE";
    if (n<22) return NULL;
    size_t e=n-22;
    for (;;) {
        if (little32(z+e)==0x06054B50u) break;
        if (!e || n-e>=22+65535) return NULL;
        e--;
    }
    unsigned entries=zip16(z+e+10);
    size_t at=little32(z+e+16);
    *why="NO 880K ADF IN THE ARCHIVE";
    for (unsigned i=0;i<entries;i++) {
        if (at+46>n || little32(z+at)!=0x02014B50u) return NULL;
        unsigned method=zip16(z+at+10), name_n=zip16(z+at+28), extra=zip16(z+at+30), note=zip16(z+at+32);
        uint32_t crc=little32(z+at+16), packed=little32(z+at+20), size=little32(z+at+24), local=little32(z+at+42);
        const uint8_t *name=z+at+46;
        if (at+46+name_n>n) return NULL;
        if (size==ADF_BYTES && name_n>4 && name[name_n-4]=='.' && (name[name_n-3]|32)=='a' &&
            (name[name_n-2]|32)=='d' && (name[name_n-1]|32)=='f') {
            if ((method!=0 && method!=8) || (zip16(z+at+8)&1)) { *why="UNSUPPORTED ZIP COMPRESSION"; return NULL; }
            if ((size_t)local+30>n || little32(z+local)!=0x04034B50u) return NULL;
            size_t data=(size_t)local+30+zip16(z+local+26)+zip16(z+local+28);
            if (data>n || packed>n-data) return NULL;
            uint8_t *out=core->alloc(ADF_BYTES,ZM_MEM_PSRAM);
            *why="NOT ENOUGH MEMORY";
            if (!out) return NULL;
            int ok=method==0?(packed==ADF_BYTES?(memcpy(out,z+data,ADF_BYTES),1):0):inflate_raw(z+data,packed,out,ADF_BYTES);
            if (ok && crc32(out,ADF_BYTES)==crc) return out;
            core->free(out);
            *why="BROKEN ARCHIVE (CRC)";
            return NULL;
        }
        at+=46u+name_n+extra+note;
    }
    return NULL;
}

/* ---- сеть ---- */
static int net_api(void) {
    return core->size>=offsetof(zm_core_t,net_close)+sizeof core->net_close && core->net_open;
}
/* Ответ целиком в PSRAM. *status — код HTTP (0 — нет связи). */
static uint8_t *net_get(const char *url,size_t max,size_t *size,int *status,const char *what) {
    *size=0; *status=0;
    void *stream=core->net_open(url,NULL,NET_WAIT_MS,status);
    if (!stream) return NULL;
    size_t total=0, cap=256u*1024u;
    const char *length=core->net_header(stream,"Content-Length");
    if (length) { while (*length>='0' && *length<='9') total=total*10u+(size_t)(*length++-'0'); }
    if (total>max) { core->net_close(stream); *status=413; return NULL; }
    if (total) cap=total;
    uint8_t *data=core->alloc(cap,ZM_MEM_PSRAM);
    if (!data) { core->net_close(stream); return NULL; }
    size_t have=0; uint32_t shown=core->millis(), idle=shown; int ok=1;
    for (;;) {
        if (!running || exit_req) { ok=0; break; }
        if (have==cap) {
            if (cap>=max) { ok=0; break; }
            size_t grown=cap*2>max?max:cap*2;
            uint8_t *more=core->alloc(grown,ZM_MEM_PSRAM);
            if (!more) { ok=0; break; }
            memcpy(more,data,have); core->free(data);
            data=more; cap=grown;
        }
        int got=core->net_read(stream,data+have,(uint32_t)(cap-have>8192?8192:cap-have),500);
        uint32_t now=core->millis();
        if (got<0) break;
        if (got>0) { have+=(size_t)got; idle=now; }
        else if (now-idle>NET_WAIT_MS) { ok=0; break; }
        if (now-shown>=250) {
            char text[OSD_COLS+1];
            shown=now;
            if (total) snprintf(text,sizeof text,"%s %u%%",what,(unsigned)(have/1024u*100u/(total/1024u+1u)));
            else snprintf(text,sizeof text,"%s %u KB",what,(unsigned)(have/1024u));
            menu_set_status(text,C_WHITE);
            menu_status_flush();
        }
    }
    core->net_close(stream);
    if (!ok || (total && have!=total)) { core->free(data); return NULL; }
    *size=have;
    return data;
}
static int net_add_item(const char *name,unsigned k) {
    if (menu_item_n>=MENU_MAX_ITEMS || !menu_reserve(menu_item_n+1)) { menu_truncated=1; return 0; }
    menu_item_t *it=&menu_items[menu_item_n++];
    memset(it,0,sizeof *it);
    memcpy(it->name,name,k+1);
    menu_disks++;
    return 1;
}
/* Имена .zip из {"result":[{"name":"…","source":…},…]} — в список меню. Вернёт число файлов. */
static unsigned net_parse_list(const uint8_t *j,size_t n) {
    static const char key[]="\"name\":\"";
    const size_t key_n=sizeof key-1;
    unsigned found=0;
    for (size_t i=0;i+key_n<n;i++) {
        if (j[i]!='"' || memcmp(j+i,key,key_n)) continue;
        i+=key_n;
        char name[256]; unsigned k=0; int ok=1;
        while (i<n && j[i]!='"') {
            unsigned c=j[i++];
            if (c=='\\' && i<n) {
                c=j[i++];
                if (c=='u') { c='?'; i+=4; }
                else if (c!='"' && c!='\\' && c!='/') ok=0;
            }
            if (k+1<sizeof name) name[k++]=(char)c; else ok=0;
        }
        name[k]=0;
        if (!ok || k<5 || name[k-4]!='.' || (name[k-3]|32)!='z' || (name[k-2]|32)!='i' || (name[k-1]|32)!='p') continue;
        if (!net_add_item(name,k)) break;
        found++;
    }
    return found;
}
static uint32_t net_updated, net_checked;   /* первая строка файла списка */
/* Первое число после ключа в ответе JSON; 0 — нет. */
static uint32_t net_json_number(const uint8_t *j,size_t n,const char *key) {
    size_t k=strlen(key);
    for (size_t i=0;i+k<n;i++) {
        if (memcmp(j+i,key,k)) continue;
        uint32_t v=0;
        for (i+=k;i<n && j[i]>='0' && j[i]<='9';i++) v=v*10u+(uint32_t)(j[i]-'0');
        return v;
    }
    return 0;
}
static void net_list_path(char *out,size_t cap,char letter) { snprintf(out,cap,"%s/LIST-%c.TXT",NET_DIR,letter&~32); }
/* Список раздела с карты. Вернёт число файлов, 0 — списка нет. */
static unsigned net_list_read(char letter) {
    char path[64]; uint8_t *data=NULL; size_t size=0; unsigned found=0;
    net_list_path(path,sizeof path,letter);
    net_updated=net_checked=0;
    if (core->sd_read(path,&data,&size,NET_LIST_MAX,&running)!=ZM_OK || !data) return 0;
    size_t i=0;
    if (size && data[0]=='#') {
        for (i=1;i<size && data[i]>='0' && data[i]<='9';i++) net_updated=net_updated*10u+(uint32_t)(data[i]-'0');
        for (i++;i<size && data[i]>='0' && data[i]<='9';i++) net_checked=net_checked*10u+(uint32_t)(data[i]-'0');
        while (i<size && data[i]!='\n') i++;
        i++;
    }
    while (i<size) {
        size_t k=0;
        while (i+k<size && data[i+k]!='\n') k++;
        if (k && k<sizeof menu_items[0].name) {
            char name[sizeof menu_items[0].name];
            memcpy(name,data+i,k); name[k]=0;
            if (!net_add_item(name,(unsigned)k)) break;
            found++;
        }
        i+=k+1;
    }
    core->free(data);
    return found;
}
/* Список на карту; не вышло — только в журнал: следующий заход прочитает его из сети. */
static int net_list_save(char letter,unsigned first) {
    char path[64], head[32]; size_t size=0;
    for (unsigned i=first;i<menu_item_n;i++) size+=strlen(menu_items[i].name)+1;
    if (!size) return 0;
    size_t at=(size_t)snprintf(head,sizeof head,"#%u %u\n",(unsigned)net_updated,(unsigned)net_checked);
    size+=at;
    uint8_t *data=core->alloc(size,ZM_MEM_PSRAM);
    if (!data) return 0;
    memcpy(data,head,at);
    for (unsigned i=first;i<menu_item_n;i++) {
        size_t n=strlen(menu_items[i].name);
        memcpy(data+at,menu_items[i].name,n); at+=n; data[at++]='\n';
    }
    net_list_path(path,sizeof path,letter);
    int r=core->sd_write(path,data,size,&running);
    core->free(data);
    if (r!=ZM_OK) core->log("amiga: cannot save %s: %d",path,r);
    return r==ZM_OK;
}
static int net_reload;   /* F5: список раздела — заново из сети */
/* Раздел: последняя буква пути, /A -> A */
static void net_read_letter(void) {
    char url[96]; size_t size=0; int status=0;
    char letter=menu_path[2][1];
    unsigned first=menu_item_n;
    int reload=net_reload;
    uint32_t today=fs_stamp(evo_time).days;
    net_reload=0;
    if (!reload && net_list_read(letter)) {
        if (!today || !net_api() || today-net_checked<NET_CHECK_DAYS) return;
        snprintf(url,sizeof url,"%s%c/item_last_updated",NET_META,letter|32);
        uint8_t *answer=net_get(url,64,&size,&status,"CHECKING ARCHIVE.ORG");
        uint32_t updated=answer && status>=200 && status<300?net_json_number(answer,size,"\"result\":"):0;
        if (answer) core->free(answer);
        menu_status[0]=0;
        if (!updated) return;   /* нет связи — работаем со списком с карты, спросим при следующем заходе */
        if (updated==net_updated) { net_checked=today; net_list_save(letter,first); return; }
        reload=1;               /* раздел изменился */
        menu_item_n=first; menu_disks=0; menu_truncated=0; size=0; status=0;
    }
    snprintf(url,sizeof url,"%s%c",NET_META,letter|32);
    if (!net_api()) { menu_set_status("THE ESP FIRMWARE HAS NO NETWORK ACCESS",C_YELLOW); return; }
    uint8_t *json=net_get(url,NET_LIST_MAX,&size,&status,"READING LIST");
    if (!json || status<200 || status>=300) {
        char text[OSD_COLS+1];
        if (status) snprintf(text,sizeof text,"ARCHIVE.ORG ANSWERED %d",status);
        else snprintf(text,sizeof text,"NO CONNECTION TO ARCHIVE.ORG");
        if (json) core->free(json);
        if (reload) net_list_read(letter);   /* не обновилось — прежний список остаётся */
        menu_set_status(text,C_YELLOW);
        return;
    }
    net_parse_list(json,size);
    net_updated=net_json_number(json,size,"\"item_last_updated\":");
    net_checked=today;
    core->free(json);
    menu_status[0]=0;
    net_list_save(letter,first);
}
/* Имя для карты: то, чего FAT не примет, — '_' ; .zip -> .adf */
static void net_local_name(const char *zip,char *out,size_t cap) {
    size_t n=strlen(zip);
    if (n>4) n-=4;
    if (n+5>cap) n=cap-5;
    for (size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)zip[i];
        out[i]=c<32 || c=='"' || c=='*' || c==':' || c=='<' || c=='>' || c=='?' || c=='\\' || c=='|' || c=='/'?'_':(char)c;
    }
    memcpy(out+n,".adf",5);
}
static size_t net_url_encode(const char *in,char *out,size_t cap) {
    static const char hex[]="0123456789ABCDEF";
    size_t n=0;
    for (;*in;in++) {
        unsigned char c=(unsigned char)*in;
        int plain=(c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c=='~';
        if (n+4>cap) return 0;
        if (plain) out[n++]=(char)c;
        else { out[n++]='%'; out[n++]=hex[c>>4]; out[n++]=hex[c&15]; }
    }
    out[n]=0;
    return n;
}
/* Дискета из NET: уже скачанная — с карты; иначе скачать, распаковать, записать на карту, вставить. */
static const char *net_load(const menu_item_t *it) {
    static char local[256], path[300], url[1024];
    void *file=NULL; uint32_t sectors=0;
    net_local_name(it->name,local,sizeof local);
    snprintf(path,sizeof path,"%s/%s",NET_DIR,local);
    if (sd_disk_api() && core->sd_open(path,0,&sectors,&file)==ZM_OK) {
        core->sd_close(file);
        if (sectors==ADF_BYTES/512) {
            menu_set_status("ALREADY DOWNLOADED, READING",C_WHITE);
            menu_status_flush();
            return df0_insert_tang(path,local);
        }
    }
    if (!net_api()) return "THE ESP FIRMWARE HAS NO NETWORK ACCESS";
    int n=snprintf(url,sizeof url,"%s%c/",NET_FILES,menu_path[2][1]|32);
    if (n<=0 || !net_url_encode(it->name,url+n,sizeof url-(size_t)n)) return "FILE NAME IS TOO LONG";
    size_t size=0; int status=0;
    uint8_t *zip=net_get(url,NET_ZIP_MAX,&size,&status,"DOWNLOADING");
    if (!zip || status<200 || status>=300) {
        if (zip) core->free(zip);
        return status?"ARCHIVE.ORG REFUSED THE FILE":"DOWNLOAD FAILED";
    }
    menu_set_status("UNPACKING...",C_WHITE);
    menu_status_flush();
    const char *why=NULL;
    uint8_t *adf=zip_extract_adf(zip,size,&why);
    core->free(zip);
    if (!adf) return why;
    menu_set_status("SAVING TO TANG SD...",C_WHITE);
    menu_status_flush();
    int saved=core->sd_write(path,adf,ADF_BYTES,&running);
    file=NULL;
    if (saved!=ZM_OK) {
        /* Дискета есть в памяти: играть можно, но на карте её нет — сказать об этом. */
        core->log("amiga: cannot save %s: %d",path,saved);
        df0_change(adf,NULL,1,local);
        return "INSERTED, BUT NOT SAVED ON TANG SD";
    }
    if (sd_disk_api() && (core->sd_open(path,1,&sectors,&file)!=ZM_OK || sectors!=ADF_BYTES/512)) {
        if (file) core->sd_close(file);
        file=NULL;
    }
    df0_change(adf,file,file==NULL,local);
    core->log("amiga: downloaded %s",path);
    return NULL;
}
