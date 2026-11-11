#define AMIGA_HOST_TEST
#include "../esp/main/app.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t video_register,video_capability=0x41565331u,lace_capability=0x414C4631u;
static int video_write_failed;
static uint32_t mode_register,mode_capability=0x414D4331u;static int mode_write_failed;   // режим совместимости (слой 0.25)
static uint32_t ticks, expected, seq, delivered, mask_lo, mask_hi;
static int mode, started, released, writes, key_room=8, resets;
static uint8_t packet[256], mirror[ROM_BYTES];
static uint8_t hid_cmd, hid_joy, hid_bytes[512], reply_bytes[100];
static unsigned hid_count, reply_len, hid_at;
static uint32_t disk_status, disk_sector, disk_token, disk_commit, mounted;
static uint8_t disk_payload[512];
static uint32_t rtc_fields[8];static unsigned rtc_count;
static int disk_fail_at=-1, sd_result=ZM_NOT_FOUND;
static size_t sd_size=ADF_BYTES;
// Карта Tang: файлы в памяти. Место под новый файл заполнено мусором, как на настоящей карте.
typedef struct { char path[600]; uint8_t *data; size_t size; } mem_file_t;
static mem_file_t mem_files[8];
static int sd_create_result=ZM_OK, unit_now;
static uint32_t hd_mounted;
static mem_file_t *mem_find(const char *path) {
    for(unsigned i=0;i<8;i++) if(mem_files[i].data && !strcmp(mem_files[i].path,path)) return &mem_files[i];
    return NULL;
}
static mem_file_t *mem_add(const char *path,size_t size) {
    for(unsigned i=0;i<8;i++) if(!mem_files[i].data) {
        strcpy(mem_files[i].path,path);mem_files[i].data=malloc(size);CHECK(mem_files[i].data);
        memset(mem_files[i].data,0xEE,size);mem_files[i].size=size;return &mem_files[i];
    }
    CHECK(0);return NULL;
}
static void mem_remove(const char *path) { mem_file_t *f=mem_find(path);CHECK(f);free(f->data);f->data=NULL; }
static int sd_write_mock(const char *path,const uint8_t *data,size_t size,const volatile int *active) {
    (void)active;mem_file_t *f=mem_find(path);if(f)mem_remove(path);
    f=mem_add(path,size);memcpy(f->data,data,size);return ZM_OK;
}
static int sd_open_mock(const char *path,int writable,uint32_t *sectors,void **file) {
    (void)writable;mem_file_t *f=mem_find(path);*file=NULL;*sectors=0;
    if(!f)return ZM_NOT_FOUND;
    *sectors=(uint32_t)(f->size/512);*file=f;return ZM_OK;
}
static int sd_pread_mock(void *file,uint32_t sector,uint8_t *data,uint32_t count) {
    mem_file_t *f=file;CHECK(((size_t)sector+count)*512<=f->size);memcpy(data,f->data+(size_t)sector*512,count*512);return ZM_OK;
}
static int sd_pwrite_mock(void *file,uint32_t sector,const uint8_t *data,uint32_t count) {
    mem_file_t *f=file;CHECK(((size_t)sector+count)*512<=f->size);memcpy(f->data+(size_t)sector*512,data,count*512);return ZM_OK;
}
static void sd_close_mock(void *file) { CHECK(file); }
static int sd_create_mock(const char *path,uint32_t sectors) {
    if(sd_create_result!=ZM_OK)return sd_create_result;
    CHECK(!mem_find(path));mem_add(path,(size_t)sectors*512);return ZM_OK;
}
static void dump_file(const char *path,const char *out) {
    mem_file_t *m=mem_find(path);CHECK(m);FILE *o=fopen(out,"wb");CHECK(o);CHECK(fwrite(m->data,1,m->size,o)==m->size);fclose(o);
}
static int sd_read_mock(const char *path,uint8_t **data,size_t *size,size_t limit,const volatile int *active) {
    if(!strcmp(path,HD0_CFG)||!strcmp(path,KEYS_CFG)||!strcmp(path,VIDEO_CFG)||!strcmp(path,MODE_CFG)){
        mem_file_t *m=mem_find(path);*data=NULL;*size=0;if(!m)return ZM_NOT_FOUND;
        CHECK(m->size<=limit);*data=malloc(m->size);memcpy(*data,m->data,m->size);*size=m->size;return ZM_OK;
    }
    if(mem_find(path)){mem_file_t *m=mem_find(path);*data=malloc(m->size);memcpy(*data,m->data,m->size);*size=m->size;return ZM_OK;}
    if(strstr(path,"/LIST-")){*data=NULL;*size=0;return ZM_NOT_FOUND;}
    CHECK(strcmp(path,ADF_PATH)==0);CHECK(limit==ADF_BYTES);CHECK(*active);
    *data=NULL;*size=0;
    if(sd_result==ZM_OK){*data=calloc(1,sd_size);CHECK(*data);*size=sd_size;}
    return sd_result;
}
static void no_lock(void *p) { (void)p; }
// Карта Evo для тома EVO: плагин VDAC4 Cores 0.15 — запрос сектора в ответе на ВВОД, сектора — SD_DONE #76 подряд.
static uint8_t *evo_card;static size_t evo_card_size;static int evo_plugin_on;static unsigned evo_requests,evo_sectors;
static uint32_t evo_mounted;
static void evo_plugin(void) {
    uint8_t q[4]={0,0xFF,0,0};reply_len=0;command(0x71,q,4);
    if(reply_len!=8||reply_bytes[0]!=1||reply_bytes[1]!=7)return;
    uint8_t tag=reply_bytes[2];uint32_t lba=little32(reply_bytes+3);unsigned count=reply_bytes[7];
    CHECK(count>=1&&count<=16);evo_requests++;
    for(unsigned k=0;k<count;k++){
        static uint8_t f[518];f[0]=tag;f[1]=0;put32(f+2,lba+k);
        if(((size_t)lba+k+1)*512>evo_card_size){f[1]=2;command(0x76,f,6);CHECK(reply_bytes[0]==1);return;}
        memcpy(f+6,evo_card+((size_t)lba+k)*512,512);command(0x76,f,518);evo_sectors++;
        if(!reply_bytes[0])return;
    }
}
static void delay_mock(uint32_t ms) { ticks+=ms; if(evo_card&&evo_plugin_on&&evo_req_pending)evo_plugin(); }
static uint32_t millis_mock(void) { return ticks; }
static int reply_mock(uint8_t cmd,const void *p,uint16_t n) {
    (void)cmd; CHECK(n<=sizeof reply_bytes); memcpy(reply_bytes,p,n);reply_len=n;return ZM_OK;
}
static int read_mock(uint32_t a,uint32_t *v,uint32_t n) {
    if(a>=0x5000 && a<0x5200){for(unsigned i=0;i<n;i++)v[i]=0xa5000000u+(a-0x5000)/4+i;return ZM_OK;}
    CHECK(n==1); *v=0;
    if(a==R_VIDEO_CAP)*v=video_capability;
    if(a==R_LACE_CAP)*v=lace_capability;
    if(a==R_VIDEO)*v=video_register;
    if(a==R_MODE_CAP)*v=mode_capability;
    if(a==R_MODE)*v=mode_register;
    if(a==0x44)*v=mode==4?3:mode==5?9:released?4:1;
    if(a==0x48)*v=seq;
    if(a==0x4c)*v=delivered-(mode==3?4:0);
    if(a==0x50)*v=crc32(mirror,delivered)^(mode==2?1:0);
    if(a==0xc)*v=key_room;
    if(a==0x198)*v=0x41444632;
    if(a==0x184)*v=disk_status;
    if(a==0x188)*v=disk_sector;
    if(a==0x18c)*v=disk_token;
    return ZM_OK;
}
static int write_mock(uint32_t a,const uint32_t *v,uint32_t n) {
    if(a==R_VIDEO){if(video_write_failed)return ZM_ERROR;video_register=*v;}
    if(a==R_MODE){if(mode_write_failed)return ZM_ERROR;mode_register=*v;}
    if(a==0xdc && rtc_count<8)rtc_fields[rtc_count++]=v[0];
    if(a>=0x5000 && a<0x5200){
        CHECK(a+n*4<=0x5200);if((int)(a-0x5000)==disk_fail_at)return ZM_ERROR;
        for(unsigned i=0;i<n;i++)put32(disk_payload+a-0x5000+4*i,v[i]);return ZM_OK;
    }
    if(a>=0x4000 && a<0x4100) {
        if(mode==1 && seq==2)return ZM_ERROR;
        CHECK(started);CHECK(a+n*4<=0x4100);
        for(unsigned i=0;i<n;i++) {
            unsigned index=(a-0x4000)/4+i;
            if(index<32)mask_lo|=1u<<index;else mask_hi|=1u<<(index-32);
            put32(packet+index*4,v[i]);
        }
        return ZM_OK;
    }
    if(a>=0x3000 && a<0x3800)return ZM_OK; // окно меню
    CHECK(n==1);
    if(a==0x194)unit_now=(int)*v;
    if(a==0x180){if(unit_now==4)hd_mounted=*v;else if(unit_now==5)evo_mounted=*v;else mounted=*v;}
    if(a==0x190)disk_commit=*v;
    if(a==0x44 && *v==1){seq=delivered=mask_lo=mask_hi=0;started=1;released=0;}
    if(a==0x44 && *v==2){CHECK(delivered==ROM_BYTES);CHECK(expected==crc32(mirror,ROM_BYTES));released=1;}
    if(a==0x44 && *v==3)resets++;
    if(a==0x58)expected=*v;
    if(a==0x48){CHECK(mask_lo==~0u&&mask_hi==~0u);CHECK((*v&65535)==256);CHECK((*v>>16)==seq+1);
        memcpy(mirror+delivered,packet,256);delivered+=256;seq++;mask_lo=mask_hi=0;writes++;}
    if(a==0x10){if(*v&256){hid_cmd=(uint8_t)*v;hid_at=0;}
        else {if(hid_cmd==1){CHECK(hid_count<sizeof hid_bytes);hid_bytes[hid_count++]=(uint8_t)*v;}
            // Джойстик — устройство 0: в ядре это порт 2, игровой (userio перекрещивает входы, tests/run_joystick.py).
            if(hid_cmd==3&&hid_at==0)CHECK(*v==0);
            if(hid_cmd==3&&hid_at==1)hid_joy=(uint8_t)*v;hid_at++;}}
    return ZM_OK;
}
static void log_mock(const char *f,...) {(void)f;}
static void *alloc_mock(size_t n,uint32_t caps) {(void)caps;return malloc(n);}
static int sd_list_mock(const char *path,const char *suffix,uint32_t skip,zm_sd_entry_t *e,uint32_t cap,uint32_t *count,int *more,const volatile int *active) {
    (void)cap;(void)active;CHECK(!strcmp(path,"/AMIGA"));
    int hdd=!strcmp(suffix,".hdf");CHECK(hdd||!strcmp(suffix,".adf"));
    *count=0;*more=0;if(skip)return ZM_OK;
    if(!hdd){memset(&e[0],0,sizeof e[0]);strcpy(e[0].name,"DF0.ADF");e[0].size=ADF_BYTES;*count=1;}
    for(unsigned i=0;i<8;i++){
        mem_file_t *m=&mem_files[i];size_t n=m->data?strlen(m->path):0;
        if(n<4||strcmp(m->path+n-4,hdd?".HDF":".ADF"))continue;
        memset(&e[*count],0,sizeof e[0]);strcpy(e[*count].name,strrchr(m->path,'/')+1);e[*count].size=(uint32_t)m->size;++*count;
    }
    return ZM_OK;
}
static void send_keys(ps2_hid_t *parser,const uint8_t *k,unsigned n) {
    uint8_t q[64]={0,7,11,19};CHECK(n+4<=sizeof q);memcpy(q+4,k,n);
    command(0x71,q,(uint16_t)(n+4));CHECK(input_step(parser));
}
static void tap(ps2_hid_t *parser,uint8_t code) { const uint8_t k[]={code,0xf0,code};send_keys(parser,k,3); }
static void tap_ext(ps2_hid_t *parser,uint8_t code) { const uint8_t k[]={0xe0,code,0xe0,0xf0,code};send_keys(parser,k,5); }
// Том EVO как его читает FFS: RDB, корень, карта занятости, каталоги по ячейкам хеша, файлы с блоками расширения. Каждый
// прочитанный блок — в дамп для check_evo.py (своё чтение FFS на Python).
static FILE *evo_dumpf;static unsigned evo_files,evo_dirs,evo_blocks;static uint32_t evo_fnv;
// Блоки данных первого обхода (номер, файл, байты): Workbench читает файлы вперемешку — значок, файл, соседний каталог.
static uint32_t *mix_lba,*mix_file;static uint8_t *mix_data;static unsigned mix_n,mix_cap,mix_on,mix_reads,mix_worst;static uint32_t mix_seed;
static void mix_keep(uint32_t lba,const uint8_t *b) {
    if(!mix_on)return;
    if(mix_n==mix_cap){
        mix_cap=mix_cap?mix_cap*2:4096;mix_lba=realloc(mix_lba,mix_cap*4u);mix_file=realloc(mix_file,mix_cap*4u);
        mix_data=realloc(mix_data,(size_t)mix_cap*512u);CHECK(mix_lba&&mix_file&&mix_data);
    }
    mix_lba[mix_n]=lba;mix_file[mix_n]=evo_files;memcpy(mix_data+(size_t)mix_n*512u,b,512);mix_n++;
}
static void evo_get(uint32_t lba,uint8_t *b) {
    ev_read(lba,b);evo_blocks++;
    for(unsigned i=0;i<512;i++)evo_fnv=(evo_fnv^b[i])*16777619u;
    if(evo_dumpf){uint8_t h[4];put32(h,lba);CHECK(fwrite(h,1,4,evo_dumpf)==4);CHECK(fwrite(b,1,512,evo_dumpf)==512);}
}
static uint32_t evo_sum(const uint8_t *b,unsigned words){uint32_t s=0;for(unsigned i=0;i<words;i++)s+=be32_get(b+i*4);return s;}
static void evo_walk(uint32_t key,int depth) {
    uint8_t d[512];evo_get(EV_CYL+key,d);CHECK(evo_sum(d,128)==0);CHECK(depth<16);
    for(unsigned h=0;h<EV_TABLE;h++)for(uint32_t x=be32_get(d+(6+h)*4);x;){
        uint8_t e[512];evo_get(EV_CYL+x,e);
        CHECK(evo_sum(e,128)==0);CHECK(be32_get(e)==2);CHECK(be32_get(e+4)==x);CHECK(be32_get(e+125*4)==key);
        CHECK(ev_hash((const char *)e+0x1B1,e[0x1B0])==h);
        if(be32_get(e+127*4)==EV_ST_USERDIR){evo_dirs++;evo_walk(x,depth+1);}
        else{
            CHECK(be32_get(e+127*4)==EV_ST_FILE);evo_files++;
            uint32_t n=ev_blocks(be32_get(e+81*4)),got=0;uint8_t list[512],data[512];memcpy(list,e,512);
            for(;;){
                for(uint32_t i=0;i<be32_get(list+8);i++){evo_get(EV_CYL+be32_get(list+(77-i)*4),data);mix_keep(EV_CYL+be32_get(list+(77-i)*4),data);got++;}
                uint32_t next=be32_get(list+126*4);if(!next)break;
                evo_get(EV_CYL+next,list);CHECK(evo_sum(list,128)==0);CHECK(be32_get(list)==EV_T_LIST);CHECK(be32_get(list+125*4)==x);
            }
            CHECK(got==n);
        }
        x=be32_get(e+124*4);
    }
}
static unsigned mix_rand(unsigned n){mix_seed=mix_seed*1103515245u+12345u;return (mix_seed>>8)%n;}
static unsigned mix_first(unsigned i){while(i&&mix_file[i-1]==mix_file[i])i--;return i;}
static void mix_read(unsigned i) {
    uint8_t b[512];const unsigned before=evo_requests;ev_read(mix_lba[i],b);mix_reads++;
    if(evo_requests-before>mix_worst)mix_worst=evo_requests-before;
    if(memcmp(b,mix_data+(size_t)i*512u,512)){printf("FAIL mixed read: block %u, file %u, its block %u\n",mix_lba[i],mix_file[i],i-mix_first(i));exit(1);}
}
// Три читателя вперемешку: каждый — файл за файлом подряд, иногда шаг назад внутри файла или другой файл; потом чтение
// вразброс. Всё сверяется с первым обходом; худшее число запросов к карте Evo на один сектор — задержка, которую видит Amiga.
static void evo_mixed(void) {
    CHECK(mix_n);mix_seed=1;mix_reads=mix_worst=0;unsigned at[3];
    for(unsigned s=0;s<3;s++)at[s]=mix_first(mix_rand(mix_n));
    for(unsigned step=0;step<30000;step++){
        const unsigned s=mix_rand(3),i=at[s],r=mix_rand(100);
        mix_read(i);
        if(r<3)at[s]=mix_first(i)+mix_rand(i-mix_first(i)+1);
        else if(r<5||i+1==mix_n||mix_file[i+1]!=mix_file[i])at[s]=mix_first(mix_rand(mix_n));
        else at[s]=i+1;
    }
    for(unsigned step=0;step<3000;step++)mix_read(mix_rand(mix_n));
    CHECK(lha.crc_bad==0);CHECK(lha.broken==0);
    printf("EVO mixed: %u reads of %u data blocks OK, worst %u card requests for one sector\n",mix_reads,mix_n,mix_worst);
    mix_n=0;
}
static void evo_test(const char *path,const char *dump) {
    FILE *f=fopen(path,"rb");CHECK(f);fseek(f,0,SEEK_END);evo_card_size=(size_t)ftell(f);fseek(f,0,SEEK_SET);
    evo_card=malloc(evo_card_size);CHECK(evo_card);CHECK(fread(evo_card,1,evo_card_size,f)==evo_card_size);fclose(f);
    uint8_t *orig=malloc(evo_card_size);CHECK(orig);memcpy(orig,evo_card,evo_card_size);
    int saved=state;state=2;running=1;exit_req=0;disk_status=0;
    // Плагин старше 0.15 молчит: диска нет, ждали не дольше EV_PROBE_MS, Amiga стартует без него.
    evo_plugin_on=0;evo_mounted=0;uint32_t t0=ticks;ev_mount();CHECK(!ev.ready);CHECK(!evo_mounted);CHECK(ticks-t0<=EV_PROBE_MS+20);
    evo_plugin_on=1;evo_requests=evo_sectors=0;ev_mount();CHECK(ev.ready);CHECK(evo_mounted==EV_DISK_BLOCKS*512u);
    evo_dumpf=dump?fopen(dump,"wb"):NULL;CHECK(!dump||evo_dumpf);
    uint8_t b[512];evo_files=evo_dirs=evo_blocks=0;
    evo_get(0,b);CHECK(!memcmp(b,"RDSK",4));CHECK(evo_sum(b,64)==0);
    evo_get(1,b);CHECK(!memcmp(b,"PART",4));CHECK(evo_sum(b,64)==0);
    evo_get(EV_CYL,b);CHECK(be32_get(b)==FS_DOS1);
    for(uint32_t x=ev.layout.root;x<=fs_last(&ev.layout);x++)evo_get(EV_CYL+x,b);
    evo_fnv=2166136261u;mix_on=1;evo_walk(ev.layout.root,0);mix_on=0;const uint32_t first_fnv=evo_fnv;
    // Архивы: каждый член распакован целиком, CRC-16 сошлась, порчи нет.
    CHECK(lha.crc_bad==0);CHECK(lha.broken==0);
    if(getenv("EVO_LHA_SAMPLES"))CHECK(lha.crc_ok==6);   // настоящие архивы libxmp: -lh1-, -lh5- (уровни 0, 1), -lh6-, -lh7- (1, 2)
    printf("EVO LHA: %u archives, %u members, %u compressed members CRC-16 OK\n",lha.n_arcs,lha.n_members,lha.crc_ok);
    unsigned walked=evo_blocks,requests=evo_requests,sectors=evo_sectors;
    if(evo_dumpf)fclose(evo_dumpf);evo_dumpf=NULL;
    // Номера, раз выданные, те же: второй обход читает только кэш и те же блоки.
    // Второй обход — те же байты: блоки из кэша номеров, члены архивов распакованы заново (две ячейки распаковки на все).
    evo_files=evo_dirs=0;evo_fnv=2166136261u;evo_walk(ev.layout.root,0);CHECK(evo_fnv==first_fnv);CHECK(lha.crc_bad==0);
    evo_mixed();
    // Запись Amiga — в память ESP; карта не меняется, чтение возвращает записанное.
    uint8_t w[512];memset(w,0x5A,512);ev_write(EV_CYL+ev.layout.root,w);ev_read(EV_CYL+ev.layout.root,b);CHECK(!memcmp(b,w,512));
    CHECK(!memcmp(evo_card,orig,evo_card_size));
    // Через ящик ядра: запрос диска 0x20 — сектор в окно, подтверждение номером.
    disk_status=1|(0x20u<<8);disk_sector=1;disk_token=77;disk_commit=0;int served=0;
    CHECK(disk_step(&served));CHECK(served);CHECK(disk_commit==77);CHECK(!memcmp(disk_payload,"PART",4));
    disk_status=1|8|(0x20u<<8);disk_sector=EV_CYL+5;disk_token=78;CHECK(disk_step(&served));CHECK(disk_commit==78);
    CHECK(!memcmp(evo_card,orig,evo_card_size));disk_status=0;
    // RDB диска 0 с флагом «последний диск», как у образов из меню F11: пока EVO подключён, Amiga видит его без флага
    // (иначе Kickstart не ищет диск 1, tests/run_evo_boot.py), сумма верна; файл образа тот же; без EVO — как есть.
    {
        mem_file_t *m=mem_add("/AMIGA/EVOTEST.HDF",HDF_START*512u);hdf_rdb_block(64,m->data);CHECK(m->data[23]==7);
        void *saved_file=hd0_file;uint32_t saved_sectors=hd0_sectors;hd0_file=m;hd0_sectors=HDF_START;
        disk_status=1|(0x10u<<8);disk_sector=0;disk_token=90;CHECK(disk_step(&served));CHECK(disk_commit==90);
        CHECK(!memcmp(disk_payload,"RDSK",4));CHECK(disk_payload[23]==6);CHECK(evo_sum(disk_payload,64)==0);CHECK(m->data[23]==7);
        ev.ready=0;disk_token=91;CHECK(disk_step(&served));CHECK(disk_commit==91);CHECK(disk_payload[23]==7);ev.ready=1;
        hd0_file=saved_file;hd0_sectors=saved_sectors;disk_status=0;mem_remove("/AMIGA/EVOTEST.HDF");
    }
    printf("EVO %s: %u files, %u dirs, %u blocks read; card %u sectors in %u requests\n",path,evo_files,evo_dirs,walked,sectors,requests);
    ev_close();CHECK(!ev.ready);state=saved;free(orig);free(evo_card);evo_card=NULL;
}
static void test_paula_level(void) {
    zm_level_t lv;uint32_t words[16]={0};float frames[32];
    zm_level_init(&lv,48000,ZM_LEVEL_BASE_DB,-18.0f);
    for(unsigned b=0;b<18000;b++) {
        audio_prepare(&lv,frames,words,16);
        for(unsigned i=0;i<32;i++)CHECK(frames[i]==0.0f);
    }
    CHECK(lv.blocks==0 && lv.gain_db==ZM_LEVEL_BASE_DB);
    for(unsigned i=0;i<16;i++)words[i]=(uint16_t)-1024|((uint32_t)(uint16_t)2048<<16);
    audio_prepare(&lv,frames,words,16);
    const float g=powf(10.0f,ZM_LEVEL_BASE_DB/20.0f);
    for(unsigned i=0;i<16;i++) {CHECK(fabsf(frames[2*i]+g/32.0f)<1e-6f);CHECK(fabsf(frames[2*i+1]-g/16.0f)<1e-6f);}
    for(unsigned i=0;i<16;i++)words[i]=0x80007fff;
    audio_prepare(&lv,frames,words,16);
    for(unsigned i=0;i<16;i++){CHECK(frames[2*i]>0.75f && frames[2*i]<1.0f);CHECK(frames[2*i+1]<-0.75f && frames[2*i+1]>-1.0f);}
    zm_level_init(&lv,48000,ZM_LEVEL_BASE_DB,-18.0f);
    for(unsigned n=0;n<48000u*45;n+=16) {
        for(unsigned i=0;i<16;i++) {
            const int16_t v=(int16_t)(13107.0f*sinf(6.283185307179586f*1000.0f*(float)((n+i)%48)/48000.0f));
            words[i]=(uint16_t)v|((uint32_t)(uint16_t)v<<16);
        }
        const float before=lv.gain_db;audio_prepare(&lv,frames,words,16);
        CHECK(fabsf(lv.gain_db-before)<0.0501f);
        for(unsigned i=0;i<16;i++){CHECK(isfinite(frames[2*i]) && fabsf(frames[2*i])<1.0f);CHECK(frames[2*i]==frames[2*i+1]);}
    }
    CHECK(lv.gain_db<0.0f);CHECK(fabsf(lv.song_lufs+lv.gain_db-ZM_LEVEL_TARGET)<0.15f);
    const float gain=lv.gain_db;const unsigned blocks=lv.blocks;
    zm_level_discontinuity(&lv);memset(words,0,sizeof words);
    for(unsigned b=0;b<18000;b++)audio_prepare(&lv,frames,words,16);
    CHECK(lv.gain_db==gain && lv.blocks==blocks && lv.step_count==0);
    zm_level_init(&lv,48000,ZM_LEVEL_BASE_DB,-18.0f);CHECK(lv.blocks==0 && lv.gain_db==ZM_LEVEL_BASE_DB);
    puts("PASS Paula PCM: signed stereo, tracker loudness target, gradual gain, soft limiter, silence and discontinuity");
}
int main(int argc,char **argv) {
    test_paula_level();
    CHECK(argc>=2);FILE *f=fopen(argv[1],"rb");CHECK(f!=NULL);
    uint8_t *rom=malloc(ROM_BYTES);CHECK(rom!=NULL);CHECK(fread(rom,1,ROM_BYTES,f)==ROM_BYTES);fclose(f);
    CHECK(valid_rom(rom,ROM_BYTES));CHECK(!valid_rom(rom,ROM_BYTES-1));
    rom[1024]^=1;CHECK(!valid_rom(rom,ROM_BYTES));rom[1024]^=1;
    static zm_core_t api;api.link_read=read_mock;api.link_write=write_mock;
    api.delay=delay_mock;api.millis=millis_mock;api.mutex_lock=no_lock;api.mutex_unlock=no_lock;
    api.signal_give=no_lock;api.reply=reply_mock;core=&api;running=1;exit_req=0;
    api.sd_read=sd_read_mock;api.free=free;
    api.sd_list=sd_list_mock;api.alloc=alloc_mock;api.log=log_mock;
    api.size=sizeof api;api.sd_write=sd_write_mock;api.sd_open=sd_open_mock;api.sd_pread=sd_pread_mock;
    api.sd_pwrite=sd_pwrite_mock;api.sd_close=sd_close_mock;api.sd_create=sd_create_mock;
    // Молчание плагина: ответ новее now (пришёл с другого ядра между чтениями) — не молчание; 1500 мс — ещё нет, 1501 — да.
    // Файл с карты Evo из меню F11 (0x77…0x79) идёт без ВВОДа: его команды — тоже знак жизни, а отметка мыши та же.
    {const uint32_t saved=ticks;uint32_t now;ticks=100000;alive_ms=ticks+1;CHECK(!plugin_silent(&now));CHECK(now==ticks);
     alive_ms=ticks-1500;CHECK(!plugin_silent(&now));alive_ms=ticks-1501;CHECK(plugin_silent(&now));
     input_ms=ticks-9000;const uint8_t chunk[6]={0};CHECK(command(0x78,chunk,sizeof chunk)==ZM_OK);
     CHECK(!plugin_silent(&now));CHECK(input_ms==ticks-9000);ticks+=1501;CHECK(plugin_silent(&now));
     input_ms=alive_ms=0;ticks=saved;}
    uint8_t *disk=NULL;CHECK(load_disk(&disk));CHECK(!disk);CHECK(mount_disk(disk));CHECK(!mounted);
    sd_result=ZM_ERROR;CHECK(!load_disk(&disk));CHECK(!disk);
    sd_result=ZM_OK;sd_size=ADF_BYTES-1;CHECK(!load_disk(&disk));CHECK(!disk);
    sd_size=ADF_BYTES;CHECK(load_disk(&disk));CHECK(disk);CHECK(mount_disk(disk));CHECK(mounted==ADF_BYTES);
    for(unsigned i=0;i<ADF_BYTES;i++)disk[i]=(uint8_t)(i*37+11);
    df0_data=disk;df0_ro=0;int served=0;
    CHECK(disk_step(&served));CHECK(!served);disk_status=0x103;disk_token=17;disk_sector=1759;
    CHECK(disk_step(&served));CHECK(served);CHECK(disk_commit==17);CHECK(memcmp(disk_payload,disk+1759*512,512)==0);
    disk_commit=0;disk_sector=1760;CHECK(!disk_step(&served));CHECK(!disk_commit);
    disk_sector=0;disk_status=0x203;CHECK(!disk_step(&served));
    // Запись дискеты: в память, с защитой — нет; ответ гостю нужен в обоих случаях.
    disk_status=0x10b;disk_token=18;disk_sector=3;CHECK(disk_step(&served));CHECK(disk_commit==18);
    CHECK(little32(disk+3*512)==0xa5000000u);CHECK(little32(disk+3*512+508)==0xa5000000u+127);
    df0_ro=1;disk[4*512]=0x55;disk_sector=4;disk_token=19;CHECK(disk_step(&served));CHECK(disk_commit==19);CHECK(disk[4*512]==0x55);df0_ro=0;
    // Дискету сняли, пока шёл запрос: нули вместо данных, обмен не ломается.
    df0_data=NULL;disk_status=0x103;disk_token=20;CHECK(disk_step(&served));CHECK(disk_commit==20);
    uint8_t empty[512]={0};CHECK(memcmp(disk_payload,empty,512)==0);
    // HD0: нет файла или сектор вне образа — отказ.
    disk_status=0x1003;CHECK(!disk_step(&served));disk_status=0x103;df0_data=disk;
    disk_commit=0;disk_fail_at=256;CHECK(!disk_step(&served));CHECK(!disk_commit);
    disk_fail_at=-1;exit_req=1;CHECK(!disk_step(&served));CHECK(!disk_commit);exit_req=0;
    disk_status=0;df0_data=NULL;free(disk);
    // Время Evo: поля MSM6242 ядра: 31.10.2026 — суббота.
    {const uint8_t now[6]={5,4,3,31,10,26};memcpy(evo_time,now,6);rtc_pending=1;rtc_apply();CHECK(!rtc_pending);
     CHECK(rtc_count==6);CHECK(rtc_fields[0]==(0u<<8|26));CHECK(rtc_fields[1]==(1u<<8|9));
     CHECK(rtc_fields[2]==(2u<<8|6u<<5|31));CHECK(rtc_fields[3]==(3u<<8|3));CHECK(rtc_fields[5]==(5u<<8|5));
     const uint8_t bad[6]={0,0,0,0,13,26};memcpy(evo_time,bad,6);rtc_count=0;rtc_apply();CHECK(rtc_count==0);}
    // Файл от плагина: длина только 880 КиБ, CRC и смещение блока.
    {state=0;uint8_t q[4];put32(q,ADF_BYTES-1);command(0x77,q,4);CHECK(reply_bytes[0]==0);error_text[0]=0;
     put32(q,ADF_BYTES);command(0x77,q,4);CHECK(reply_bytes[0]==1);CHECK(rx_data);
     static uint8_t pk[518];put32(pk,0);for(unsigned i=0;i<512;i++)pk[4+i]=(uint8_t)i;
     uint16_t c=crc16(pk,516);pk[516]=(uint8_t)c;pk[517]=(uint8_t)(c>>8);
     command(0x78,pk,518);CHECK(reply_bytes[0]==1);CHECK(rx_received==512);
     pk[5]^=1;command(0x78,pk,518);CHECK(reply_bytes[0]==0);pk[5]^=1;
     command(0x79,NULL,0);CHECK(reply_bytes[0]==0);CHECK(!rx_ready);
     rx_received=ADF_BYTES;command(0x79,NULL,0);CHECK(reply_bytes[0]==1);CHECK(rx_ready);
     command(0x7A,NULL,0);CHECK(reply_bytes[0]==1&&reply_bytes[1]==1);rx_discard();CHECK(!rx_data);state=0;}
    CHECK(upload_rom(rom));CHECK(!released);CHECK(rom_release());CHECK(released);CHECK(writes==2048);CHECK(memcmp(rom,mirror,ROM_BYTES)==0);
    for(mode=1;mode<=5;mode++){CHECK(!upload_rom(rom));CHECK(!released);}
    mode=0;CHECK(upload_rom(rom));mode=4;CHECK(!rom_release());
    mode=0;exit_req=1;CHECK(!upload_rom(rom));CHECK(!released);exit_req=0;
    state=0;CHECK(command(0x70,(const uint8_t*)"WRONG",6)==ZM_OK);CHECK(reply_bytes[0]==0);
    CHECK(command(0x70,(const uint8_t*)"AMIGA",6)==ZM_OK);CHECK(reply_bytes[0]==1);CHECK(state==1);
    ps2_hid_t parser;ps2_hid_init(&parser);CHECK(input_step(&parser)); // initial clear
    state=2;
    uint8_t keys[]={0,7,10,20,0x1c,0xf0,0x1c}; // A down/up
    CHECK(command(0x71,keys,sizeof keys)==ZM_OK);
    key_room=0;CHECK(input_step(&parser));CHECK(hid_count==0);
    key_room=8;CHECK(input_step(&parser));CHECK(hid_count==2);
    CHECK(hid_bytes[0]==4 && hid_bytes[1]==132);
    unsigned f4_at=hid_count;
    uint8_t plain_f4[]={0,7,11,19,0x0c,0xf0,0x0c};
    command(0x71,plain_f4,sizeof plain_f4);CHECK(input_step(&parser));CHECK(resets==0);
    CHECK(hid_count==f4_at+2 && hid_bytes[f4_at]==61 && hid_bytes[f4_at+1]==(61|128));
    uint8_t held[]={0,7,11,19,0x14,0x11,0x0c,0x0c}; // Ctrl+Alt+F4, typematic -> one reset
    command(0x71,held,sizeof held);CHECK(input_step(&parser));CHECK(resets==1);
    uint8_t again[]={0,7,11,19,0xf0,0x0c,0x0c};
    command(0x71,again,sizeof again);CHECK(input_step(&parser));CHECK(resets==2);
    uint8_t release_reset[]={0,7,11,19,0xf0,0x0c,0xf0,0x14,0xf0,0x11};
    command(0x71,release_reset,sizeof release_reset);CHECK(input_step(&parser));CHECK(resets==2);
    uint8_t ctrl[]={0,7,11,19,0x14};command(0x71,ctrl,sizeof ctrl);CHECK(input_step(&parser));
    unsigned before=hid_count;lost=1;CHECK(input_step(&parser));CHECK(hid_count==before+1);
    CHECK(hid_bytes[hid_count-1]==(104|128)); // release Ctrl after input overflow
    // NET (слой 0.24): разделы -> буквы -> дискеты; список коллекции, буква из него, распаковка deflate, имя на карте, адрес.
    {
        static const char json[]="{\"files\":[{\"name\":\"A (1990)(B)[cr C].zip\",\"size\":\"5\"},"
            "{\"name\":\"x_meta.xml\"},{\"name\":\"Don\\\"t (Disk 1 of 2).zip\"},{\"name\":\"$623 (19xx)(Clique)[WB].zip\"},"
            "{\"name\":\"1 Week (2001)(Contraz)(AGA)[h BTTR].zip\"},{\"name\":\"bad\\nline.zip\"}],\"item_last_updated\":1738875211}";
        menu_items_free();menu_card=2;
        size_t len=0;unsigned found=0;
        char *names=net_parse_names((const uint8_t*)json,sizeof json-1,&len,&found);
        CHECK(names);CHECK(found==4);
        CHECK(len==strlen("A (1990)(B)[cr C].zip\nDon\"t (Disk 1 of 2).zip\n$623 (19xx)(Clique)[WB].zip\n1 Week (2001)(Contraz)(AGA)[h BTTR].zip\n"));
        {static const char head[]="A (1990)(B)[cr C].zip\nDon\"t (Disk 1 of 2).zip\n$623";CHECK(!memcmp(names,head,sizeof head-1));}
        char local[64];net_local_name("Don\"t (Disk 1 of 2).zip",local,sizeof local);CHECK(!strcmp(local,"Don_t (Disk 1 of 2).adf"));
        net_place_t p;char id[96],path[96];
        // Место по пути: корень, раздел, буква; прежний путь /A и буква у раздела без букв — не места.
        strcpy(menu_path[2],"/");CHECK(!net_place(&p));
        strcpy(menu_path[2],"/A");CHECK(!net_place(&p));
        strcpy(menu_path[2],"/PACKMAGS/A");CHECK(!net_place(&p));
        strcpy(menu_path[2],"/GAMES");CHECK(net_place(&p));CHECK(!net_list_level(&p));
        strcpy(menu_path[2],"/PACKMAGS");CHECK(net_place(&p));CHECK(net_list_level(&p));
        strcpy(menu_path[2],"/GAMES/T");CHECK(net_place(&p));CHECK(net_list_level(&p));
        net_item_id(&p,id,sizeof id);CHECK(!strcmp(id,"commodore-amiga-games-adf-r_202301"));
        strcpy(menu_path[2],"/GAMES/Y");CHECK(net_place(&p));net_item_id(&p,id,sizeof id);CHECK(!strcmp(id,"commodore-amiga-games-adf-v_202301"));
        strcpy(menu_path[2],"/GAMES/F");CHECK(net_place(&p));net_item_id(&p,id,sizeof id);CHECK(!strcmp(id,"commodore-amiga-games-adf-f_20220210"));
        strcpy(menu_path[2],"/GAMES/0-9");CHECK(net_place(&p));CHECK(p.letter==0);
        net_item_id(&p,id,sizeof id);CHECK(!strcmp(id,"commodore-amiga-games-adf-0-9_202202"));
        net_list_path(&p,path,sizeof path);CHECK(!strcmp(path,NET_DIR "/LIST-0-9.TXT"));
        strcpy(menu_path[2],"/DEMO PACKS/Q");CHECK(net_place(&p));net_item_id(&p,id,sizeof id);CHECK(!strcmp(id,"commodore-amiga-demos-packs"));
        net_list_path(&p,path,sizeof path);CHECK(!strcmp(path,"/Amiga/DemoPacks/Downloads/LIST.TXT"));
        // Корень — 16 разделов в своём порядке, раздел с буквами — 0-9, A…Z.
        strcpy(menu_path[2],"/");menu_read_dir();CHECK(menu_item_n==16);CHECK(menu_dirs==16);
        CHECK(!strcmp(menu_items[0].name,"GAMES"));CHECK(!strcmp(menu_items[2].name,"DEMOS"));CHECK(!strcmp(menu_items[15].name,"WORKBENCH"));
        strcpy(menu_path[2],"/DEMOS");menu_read_dir();CHECK(menu_item_n==28);   // ".." + 27
        CHECK(!strcmp(menu_items[1].name,"0-9"));CHECK(!strcmp(menu_items[2].name,"A"));CHECK(!strcmp(menu_items[27].name,"Z"));
        strcpy(menu_path[2],"/A");menu_read_dir();CHECK(!strcmp(menu_path[2],"/"));CHECK(menu_item_n==16);   // путь слоя 0.23
        // Игры: список буквы на карте, тот же порядок и те же имена; проверен сегодня — в сеть не ходит.
        net_updated=1738875211;net_checked=fs_stamp(evo_time).days;
        strcpy(menu_path[2],"/GAMES/A");CHECK(net_place(&p));
        CHECK(net_list_save(&p,(const uint8_t*)names,len));CHECK(mem_find(NET_DIR "/LIST-A.TXT"));
        menu_item_n=0;menu_disks=0;net_updated=net_checked=0;
        net_read_list(&p);CHECK(menu_item_n==4);CHECK(menu_disks==4);
        CHECK(net_updated==1738875211);
        CHECK(!strcmp(menu_items[0].name,"A (1990)(B)[cr C].zip"));CHECK(!strcmp(menu_items[1].name,"Don\"t (Disk 1 of 2).zip"));
        // Раздел с буквами: один список коллекции, буква — из него.
        net_updated=1758116311;net_checked=fs_stamp(evo_time).days;
        strcpy(menu_path[2],"/DEMOS/D");CHECK(net_place(&p));
        CHECK(net_list_save(&p,(const uint8_t*)names,len));CHECK(mem_find("/Amiga/Demos/Downloads/LIST.TXT"));
        menu_item_n=0;menu_disks=0;net_read_list(&p);CHECK(menu_item_n==1);CHECK(!strcmp(menu_items[0].name,"Don\"t (Disk 1 of 2).zip"));
        strcpy(menu_path[2],"/DEMOS/0-9");CHECK(net_place(&p));
        menu_item_n=0;menu_disks=0;net_read_list(&p);CHECK(menu_item_n==2);
        CHECK(!strcmp(menu_items[0].name,"$623 (19xx)(Clique)[WB].zip"));CHECK(!strcmp(menu_items[1].name,"1 Week (2001)(Contraz)(AGA)[h BTTR].zip"));
        strcpy(menu_path[2],"/DEMOS/Q");menu_read_dir();CHECK(menu_item_n==1);CHECK(menu_items[0].flags&MI_PARENT);   // пустая буква
        CHECK(!strcmp(menu_status,"NO DISKS HERE"));
        // Другая буква игр — свой файл, его нет.
        strcpy(menu_path[2],"/GAMES/B");CHECK(net_place(&p));net_list_path(&p,path,sizeof path);CHECK(!mem_find(path));
        core->free(names);
        {static const char a[]="{\"files_count\":2029,\"item_last_updated\":1738875211,\"x\":1}";
         CHECK(net_json_number((const uint8_t*)a,sizeof a-1,"\"item_last_updated\":")==1738875211);
         CHECK(net_json_number((const uint8_t*)a,sizeof a-1,"\"result\":")==0);}
        char url[128];CHECK(net_url_encode("A (1)[x].zip",url,sizeof url));CHECK(!strcmp(url,"A%20%281%29%5Bx%5D.zip"));
        CHECK(!net_url_encode("A (1)[x].zip",url,5));
        mem_remove(NET_DIR "/LIST-A.TXT");mem_remove("/Amiga/Demos/Downloads/LIST.TXT");
        strcpy(menu_path[2],"/");menu_items_free();menu_card=0;
        for(int z=2;z<argc;z++){
            FILE *zf=fopen(argv[z],"rb");CHECK(zf);
            fseek(zf,0,SEEK_END);long zn=ftell(zf);fseek(zf,0,SEEK_SET);
            uint8_t *zd=malloc(zn);CHECK(fread(zd,1,zn,zf)==(size_t)zn);fclose(zf);
            const char *why=NULL;uint8_t *adf=zip_extract_adf(zd,zn,&why);
            if(!adf){printf("zip %s: %s\n",argv[z],why);exit(1);}
            CHECK(crc32(adf,ADF_BYTES)==crc32(adf,ADF_BYTES));
            FILE *rf;char rn[300];snprintf(rn,sizeof rn,"%s.raw",argv[z]);rf=fopen(rn,"rb");
            if(rf){static uint8_t ref[ADF_BYTES];CHECK(fread(ref,1,ADF_BYTES,rf)==ADF_BYTES);fclose(rf);CHECK(!memcmp(ref,adf,ADF_BYTES));}
            zd[zn/2]^=0x55;uint8_t *bad=zip_extract_adf(zd,zn,&why);CHECK(!bad);
            free(adf);free(zd);
        }
    }
    // Меню F11: F1 остаётся Amiga, F11 — меню; пока оно открыто, клавиши Amiga не идут.
    {
        unsigned count=hid_count;
        menu_available=0;uint8_t f11[]={0,7,11,19,0x78,0xf0,0x78};
        command(0x71,f11,sizeof f11);CHECK(input_step(&parser));CHECK(hid_count==count);CHECK(!menu_open);
        menu_available=1;
        uint8_t f1[]={0,7,11,19,0x05,0xf0,0x05};
        command(0x71,f1,sizeof f1);CHECK(input_step(&parser));CHECK(hid_count==count+2);CHECK(hid_bytes[count]==58);
        count=hid_count;
        command(0x71,f11,sizeof f11);CHECK(input_step(&parser));CHECK(menu_open);CHECK(menu_item_n==2);
        CHECK(!strcmp(menu_items[1].name,"DF0.ADF"));CHECK(menu_cursor==1);
        unsigned moused=hid_count;
        uint8_t a_key[]={0,7,11,19,0x1c,0xf0,0x1c};command(0x71,a_key,sizeof a_key);CHECK(input_step(&parser));
        CHECK(hid_count==moused);
        sd_result=ZM_OK;sd_size=ADF_BYTES;
        uint8_t enter[]={0,7,11,19,0x5a,0xf0,0x5a};command(0x71,enter,sizeof enter);CHECK(input_step(&parser));
        CHECK(!menu_open);CHECK(df0_data);CHECK(!strcmp(df0_name,"DF0.ADF"));CHECK(df0_ro);CHECK(df0_swap_at);CHECK(mounted==0);
        df0_eject();CHECK(!df0_data);CHECK(!df0_swap_at);CHECK(!df0_name[0]);
        menu_items_free();
    }
    // Новые образы (INS) и выбор системного диска (вкладка HDD).
    {
        enum { P_ENTER=0x5a,P_ESC=0x76,P_BKSP=0x66,P_TAB=0x0d,P_F11=0x78,P_INS=0x70,P_DEL=0x71,P_LEFT=0x6b };
        const uint8_t now[6]={5,4,3,31,10,26};memcpy(evo_time,now,6);
        menu_card=CARD_TANG;menu_available=1;
        tap(&parser,P_F11);CHECK(menu_open);
        tap_ext(&parser,P_INS);CHECK(menu_mode==MODE_NEW);CHECK(!strcmp(menu_new_name,"DISK01"));
        tap(&parser,P_ENTER);CHECK(menu_mode==MODE_LIST);
        {mem_file_t *m=mem_find("/AMIGA/DISK01.ADF");CHECK(m);CHECK(m->size==ADF_BYTES);}
        CHECK(!strcmp(menu_status,"CREATED DISK01.ADF"));CHECK(!strcmp(menu_items[menu_cursor].name,"DISK01.ADF"));
        dump_file("/AMIGA/DISK01.ADF","new.adf");
        // Следующее свободное имя; имя набирается; занятое не затирается; ESC отменяет.
        tap_ext(&parser,P_INS);CHECK(!strcmp(menu_new_name,"DISK02"));
        tap(&parser,P_BKSP);tap(&parser,0x16);CHECK(!strcmp(menu_new_name,"DISK01"));
        mem_find("/AMIGA/DISK01.ADF")->data[0]=0x77;
        tap(&parser,P_ENTER);CHECK(!strcmp(menu_status,"FILE ALREADY EXISTS"));CHECK(mem_find("/AMIGA/DISK01.ADF")->data[0]==0x77);
        mem_find("/AMIGA/DISK01.ADF")->data[0]='D';
        tap_ext(&parser,P_INS);tap(&parser,P_BKSP);tap(&parser,0x1c);tap(&parser,0x4e);CHECK(!strcmp(menu_new_name,"DISK0A-"));
        tap(&parser,P_ESC);CHECK(menu_mode==MODE_LIST);CHECK(menu_open);CHECK(!mem_find("/AMIGA/DISK0A-.ADF"));
        // Вставить дискету: выбор диска должен её вынуть, иначе загрузится она.
        CHECK(!strcmp(menu_items[menu_cursor].name,"DISK01.ADF"));tap(&parser,P_ENTER);CHECK(!menu_open);CHECK(df0_data);CHECK(!df0_ro);
        tap(&parser,P_F11);
        tap(&parser,P_TAB);tap(&parser,P_TAB);tap(&parser,P_TAB);CHECK(menu_card==CARD_HDD);CHECK(menu_item_n==1);
        // Нет места: файла нет, меню живо.
        tap_ext(&parser,P_INS);CHECK(!strcmp(menu_new_name,"HD01"));
        sd_create_result=ZM_NO_MEMORY;tap(&parser,P_ENTER);CHECK(!strcmp(menu_status,"NOT ENOUGH SPACE ON TANG SD"));sd_create_result=ZM_OK;
        tap_ext(&parser,P_INS);
        CHECK(new_sizes_mb[menu_new_size]==256);tap_ext(&parser,P_LEFT);tap_ext(&parser,P_LEFT);tap_ext(&parser,P_LEFT);tap_ext(&parser,P_LEFT);
        CHECK(new_sizes_mb[menu_new_size]==32);
        tap(&parser,P_ENTER);
        {mem_file_t *m=mem_find("/AMIGA/HD01.HDF");CHECK(m);CHECK(m->size==32u*1024u*1024u);CHECK(!memcmp(m->data,"RDSK",4));}
        dump_file("/AMIGA/HD01.HDF","new.hdf");
        CHECK(!strcmp(menu_items[menu_cursor].name,"HD01.HDF"));
        // Подтверждение: ESC оставляет всё как есть.
        int resets_before=resets;
        tap(&parser,P_ENTER);CHECK(menu_mode==MODE_USE);tap(&parser,P_ESC);CHECK(menu_mode==MODE_LIST);CHECK(!hd0_file);CHECK(resets==resets_before);
        // Сброс из меню: F4 спрашивает, Esc отменяет, Enter сбрасывает и закрывает меню.
        menu_key(HID_F4);CHECK(menu_mode==MODE_RESET);menu_key(HID_ESC);CHECK(menu_mode==MODE_LIST);CHECK(menu_open);CHECK(resets==resets_before);
        menu_key(HID_F4);menu_key(HID_ENTER);CHECK(menu_mode==MODE_LIST);CHECK(!menu_open);CHECK(resets==resets_before+1);
        resets_before=resets;menu_key(HID_F11);CHECK(menu_open);
        tap(&parser,P_ENTER);tap(&parser,P_ENTER);
        CHECK(!menu_open);CHECK(hd0_file==mem_find("/AMIGA/HD01.HDF"));CHECK(hd0_sectors==65536);CHECK(hd_mounted==32u*1024u*1024u);
        CHECK(!strcmp(hd0_name,"HD01.HDF"));CHECK(resets==resets_before+1);CHECK(!df0_data);CHECK(!df0_name[0]);
        {mem_file_t *m=mem_find(HD0_CFG);CHECK(m);CHECK(m->size==16);CHECK(!memcmp(m->data,"/AMIGA/HD01.HDF\n",16));}
        CHECK(hd0_read_config());CHECK(!strcmp(hd0_path,"/AMIGA/HD01.HDF"));
        // Ядро читает RDB из выбранного файла.
        disk_status=0x1003;disk_sector=0;disk_token=33;{int sv=0;CHECK(disk_step(&sv));CHECK(sv);}CHECK(disk_commit==33);CHECK(!memcmp(disk_payload,"RDSK",4));disk_status=0;
        // Тот же диск ещё раз — не перезапуск. Образ без RDB не подключается, прежний остаётся.
        tap(&parser,P_F11);CHECK(menu_card==CARD_HDD);
        tap(&parser,P_ENTER);tap(&parser,P_ENTER);CHECK(menu_open);CHECK(!strcmp(menu_status,"THIS DISK IS ALREADY IN USE"));CHECK(resets==resets_before+1);
        mem_add("/AMIGA/BAD.HDF",1024*1024);
        CHECK(!strcmp(hd0_attach("/AMIGA/BAD.HDF"),"NO RDB IN THE IMAGE, CANNOT BE USED"));CHECK(hd0_sectors==65536);CHECK(hd_mounted==32u*1024u*1024u);
        mem_remove("/AMIGA/BAD.HDF");
        // DEL: диска нет и после следующего запуска; записи в HD0 больше не идут.
        tap_ext(&parser,P_DEL);CHECK(menu_mode==MODE_REMOVE);tap(&parser,P_ENTER);
        CHECK(!menu_open);CHECK(!hd0_file);CHECK(hd_mounted==0);CHECK(resets==resets_before+2);
        CHECK(mem_find(HD0_CFG)->size==1);CHECK(!hd0_read_config());CHECK(!hd0_path[0]);
        disk_status=0x1003;{int sv=0;CHECK(!disk_step(&sv));}disk_status=0;
        mem_remove(HD0_CFG);CHECK(!hd0_read_config());CHECK(!strcmp(hd0_path,HDF_PATH));
        // Диск выбран, а файл исчез: снова дискета.
        {const char c[]="/AMIGA/NONE.HDF\n";CHECK(sd_write_mock(HD0_CFG,(const uint8_t *)c,sizeof c-1,&running)==ZM_OK);}
        CHECK(!hd0_read_config());mem_remove(HD0_CFG);
        menu_items_free();menu_card=CARD_TANG;
        // Вкладка KEYS: клавиатура как джойстик порта 2.
        {
            enum { P_ENTER=0x5a,P_ESC=0x76,P_F11=0x78,P_F12=0x07,P_DEL=0x71,P_TAB=0x0d,P_UP=0x75,P_DOWN=0x72,P_LCTRL=0x14,P_LALT=0x11,P_F4=0x0c,P_A=0x1c,P_SPACE=0x29 };
            keys_read_config();CHECK(!ja_on);CHECK(ja_key[JA_FIRE1]==104);menu_available=1;
            // Режим выключен: стрелка идёт Amiga, джойстик молчит.
            unsigned count=hid_count;tap_ext(&parser,P_UP);CHECK(hid_count==count+2);CHECK(hid_bytes[count]==HID_UP);CHECK(hid_joy==0);
            tap(&parser,P_F11);CHECK(menu_open);for(unsigned i=0;i<4;i++)tap(&parser,P_TAB);
            CHECK(menu_card==CARD_KEYS);CHECK(menu_item_n==JA_COUNT);CHECK(menu_cursor==0);CHECK(!menu_status[0]);
            tap(&parser,P_ENTER);CHECK(ja_on);
            {mem_file_t *m=mem_find(KEYS_CFG);CHECK(m);CHECK(m->size==22);CHECK(!memcmp(m->data,"1 82 81 80 79 104 106\n",22));}
            // Меню: стрелки двигают строку и не доходят до джойстика.
            const uint8_t held_in_menu[]={0xe0,P_DOWN};send_keys(&parser,held_in_menu,2);CHECK(menu_cursor==1);CHECK(hid_joy==0);
            tap(&parser,P_ESC);CHECK(!menu_open);
            const uint8_t released_in_game[]={0xe0,0xf0,P_DOWN};count=hid_count;send_keys(&parser,released_in_game,3);CHECK(hid_count==count);CHECK(hid_joy==0);
            // Игра: направления и огонь складываются в байт порта, Amiga их как клавиши не видит.
            count=hid_count;
            const uint8_t up_fire[]={0xe0,P_UP,P_LCTRL};send_keys(&parser,up_fire,3);CHECK(hid_joy==(8|16));CHECK(hid_count==count);
            const uint8_t fire2[]={P_LALT};send_keys(&parser,fire2,1);CHECK(hid_joy==(8|16|32));
            // Ctrl+Alt+F4 работает и когда Ctrl и Alt на джойстике.
            unsigned resets_keys=resets;const uint8_t f4[]={P_F4,0xf0,P_F4};send_keys(&parser,f4,3);CHECK(resets==resets_keys+1);CHECK(hid_count==count);
            const uint8_t off_up[]={0xe0,0xf0,P_UP,0xf0,P_LCTRL,0xf0,P_LALT};send_keys(&parser,off_up,7);CHECK(hid_joy==0);CHECK(hid_count==count);
            // Прочие клавиши идут Amiga как обычно.
            tap(&parser,P_A);CHECK(hid_count==count+2);CHECK(hid_bytes[count]==4);
            // Меню открылось при нажатом огне: отпущен сразу.
            const uint8_t held_fire[]={P_LCTRL};send_keys(&parser,held_fire,1);CHECK(hid_joy==16);
            tap(&parser,P_F11);CHECK(menu_open);CHECK(ja_mask==0);CHECK(hid_joy==0);CHECK(!ja_held[104]);
            const uint8_t up_fire_menu[]={0xf0,P_LCTRL};send_keys(&parser,up_fire_menu,2);CHECK(menu_open);
            // Новая клавиша огня, отмена, запрет F12, занятая меняется местами.
            CHECK(menu_card==CARD_KEYS);menu_cursor=5;tap(&parser,P_ENTER);CHECK(menu_mode==MODE_KEY);CHECK(ja_pick==JA_FIRE1);CHECK(!strcmp(menu_status,"PRESS THE KEY FOR FIRE 1"));
            // Пока ждём клавишу, её значение («L CTRL» справа) мигает жёлтым, действие — жёлтое; соседняя строка не мигает.
            {
                const unsigned row=MENU_TOP+5,col=OSD_COLS-1-6;const uint8_t lit=(uint8_t)(C_YELLOW<<6|('L'-0x20));
                const uint8_t fire2_key=osd_shadow[row+1][OSD_COLS-1-5];   // «L ALT» строки FIRE 2
                CHECK(osd_shadow[row][col]==lit);CHECK(osd_shadow[row][3]==(uint8_t)(C_YELLOW<<6|('F'-0x20)));
                ticks+=KEYS_BLINK_MS-1;CHECK(input_step(&parser));CHECK(osd_shadow[row][col]==lit);
                ticks+=1;CHECK(input_step(&parser));CHECK(osd_shadow[row][col]==0);CHECK(osd_shadow[row][3]==(uint8_t)(C_YELLOW<<6|('F'-0x20)));
                ticks+=KEYS_BLINK_MS;CHECK(input_step(&parser));CHECK(osd_shadow[row][col]==lit);
                CHECK(osd_shadow[row+1][OSD_COLS-1-5]==fire2_key);CHECK(fire2_key==(uint8_t)(C_CYAN<<6|('L'-0x20)));
                ticks+=KEYS_BLINK_MS;CHECK(input_step(&parser));CHECK(osd_shadow[row][col]==0);
            }
            // Отмена: строка снова обычная, мигание стоит.
            tap(&parser,P_ESC);CHECK(menu_mode==MODE_LIST);CHECK(ja_key[JA_FIRE1]==104);
            CHECK(osd_shadow[MENU_TOP+5][OSD_COLS-1-6]==(uint8_t)(C_CYAN<<6|('L'-0x20)));CHECK(osd_shadow[MENU_TOP+5][3]==(uint8_t)(C_WHITE<<6|('F'-0x20)));
            ticks+=KEYS_BLINK_MS;CHECK(input_step(&parser));CHECK(osd_shadow[MENU_TOP+5][OSD_COLS-1-6]==(uint8_t)(C_CYAN<<6|('L'-0x20)));
            tap(&parser,P_ENTER);tap(&parser,P_F12);CHECK(menu_mode==MODE_KEY);CHECK(ja_key[JA_FIRE1]==104);
            tap(&parser,P_SPACE);CHECK(menu_mode==MODE_LIST);CHECK(ja_key[JA_FIRE1]==44);
            menu_cursor=6;tap(&parser,P_ENTER);tap(&parser,P_SPACE);CHECK(ja_key[JA_FIRE2]==44);CHECK(ja_key[JA_FIRE1]==106);CHECK(!strcmp(menu_status,"SWAPPED WITH FIRE 1"));
            // После перезапуска настройка та же; порченый файл не годится.
            keys_read_config();CHECK(ja_on);CHECK(ja_key[JA_FIRE2]==44);CHECK(ja_key[JA_FIRE1]==106);
            {const char c[]="1 82 82 80 79 104 106\n";CHECK(sd_write_mock(KEYS_CFG,(const uint8_t *)c,sizeof c-1,&running)==ZM_OK);}
            keys_read_config();CHECK(!ja_on);CHECK(ja_key[JA_DOWN]==HID_DOWN);
            {const char c[]="1 82 81 80 79 68 106\n";CHECK(sd_write_mock(KEYS_CFG,(const uint8_t *)c,sizeof c-1,&running)==ZM_OK);}
            keys_read_config();CHECK(!ja_on);CHECK(ja_key[JA_FIRE1]==104);
            // DEL: значения по умолчанию, режим не меняется.
            ja_on=1;ja_key[JA_FIRE1]=44;menu_cursor=0;tap_ext(&parser,P_DEL);CHECK(ja_on);CHECK(ja_key[JA_FIRE1]==104);CHECK(!strcmp(menu_status,"DEFAULT KEYS"));
            tap(&parser,P_ESC);CHECK(!menu_open);ja_on=0;mem_remove(KEYS_CFG);menu_status[0]=0;
            menu_items_free();menu_card=CARD_TANG;
            char name[16];key_name(44,name,sizeof name);CHECK(!strcmp(name,"SPACE"));key_name(104,name,sizeof name);CHECK(!strcmp(name,"L CTRL"));
            key_name(30,name,sizeof name);CHECK(!strcmp(name,"1"));key_name(58,name,sizeof name);CHECK(!strcmp(name,"F1"));key_name(82,name,sizeof name);CHECK(!strcmp(name,"UP"));
        }
        // SETUP (слой 0.25, прежде VIDEO): строка 0 — размер, строка 1 — режим совместимости; клавиши гостя проходят.
        {
            CHECK(video_read_config());CHECK(video_config==0);CHECK(video_register==0);
            mode_read_config();   // как при запуске (worker: до upload_rom); MODE.CFG нет — режим 0
            CHECK(mode_supported);CHECK(mode_compat==0);CHECK(mode_register==0);   // запуск: mode_read_config до ROM
            const unsigned before=hid_count;tap(&parser,0x0b);tap(&parser,0x83); // F6/F7, ordinary Amiga keys
            CHECK(video_config==0);CHECK(video_register==0);CHECK(hid_count==before+4);
            tap(&parser,0x78);CHECK(menu_open);for(unsigned i=0;i<5;i++)tap(&parser,0x0d);
            CHECK(menu_card==CARD_VIDEO);CHECK(menu_item_n==3);CHECK(menu_cursor==0);
            CHECK(!strcmp(menu_items[0].name,"PICTURE SIZE"));CHECK(!strcmp(menu_items[1].name,"COMPATIBILITY"));
            CHECK(!strcmp(menu_items[2].name,"INTERLACE"));
            CHECK(!strcmp(video_value(1),"OFF"));
            tap(&parser,0x5a);CHECK(video_config==1);CHECK(video_register==1);CHECK(!strcmp(video_value(0),"95%"));
            tap(&parser,0x5a);CHECK(video_config==2);tap(&parser,0x5a);CHECK(video_config==0);tap(&parser,0x5a);CHECK(video_config==1);
            CHECK(mem_find(VIDEO_CFG));video_config=0;CHECK(video_read_config());CHECK(video_config==1);
            {const char old[]="AMIGA-VIDEO 1 1 2\n";CHECK(video_parse((const uint8_t*)old,sizeof old-1));CHECK(video_config==2);}
            video_config=1;CHECK(video_apply());
            // Режим: Enter — в ядро и на карту, вопрос о перезапуске; Enter — перезапуск сеанса (ядро и ROM заново).
            tap_ext(&parser,0x72);CHECK(menu_cursor==1);
            unsigned resets_before=resets;
            tap(&parser,0x5a);CHECK(mode_compat==1);CHECK(mode_register==1);CHECK(menu_mode==MODE_RESTART);CHECK(mem_find(MODE_CFG));
            CHECK(!strcmp(video_value(1),"68000, NO FAST RAM"));CHECK(strstr(menu_status,"68000"));
            tap(&parser,0x5a);CHECK(!menu_open);CHECK(restart_req==1);CHECK(resets==resets_before);restart_req=0;   // перезапуск сеанса, не тёплый сброс
            mode_compat=0;mode_register=0;mode_read_config();CHECK(mode_compat==1);CHECK(mode_register==1);   // с карты
            // Esc на вопросе: режим уже в ядре и на карте, сброса нет — вступит при следующем.
            tap(&parser,0x78);CHECK(menu_open);CHECK(menu_card==CARD_VIDEO);
            menu_cursor=1;resets_before=resets;tap(&parser,0x5a);CHECK(mode_compat==0);CHECK(menu_mode==MODE_RESTART);
            tap(&parser,0x76);CHECK(menu_mode==MODE_LIST);CHECK(menu_open);CHECK(resets==resets_before);CHECK(mode_register==0);
            // Del: размер 100 %, режим выключен (уже выключен — без вопроса о сбросе).
            unsigned disk_before=mounted;tap_ext(&parser,0x71);CHECK(video_config==0);CHECK(mode_compat==0);CHECK(menu_mode==MODE_LIST);CHECK(mounted==disk_before);
            video_write_failed=1;menu_cursor=0;tap(&parser,0x5a);CHECK(video_config==0);CHECK(strstr(menu_status,"FAILED"));video_write_failed=0;
            mode_write_failed=1;menu_cursor=1;tap(&parser,0x5a);CHECK(mode_compat==0);CHECK(strstr(menu_status,"FAILED"));mode_write_failed=0;
            // Ядро без режима: пункт N/A, Enter не меняет; MODE.CFG с 1 при запуске — режим 0.
            mode_capability=0;CHECK(!mode_apply());CHECK(!mode_supported);CHECK(!strcmp(video_value(1),"N/A"));
            tap(&parser,0x5a);CHECK(mode_compat==0);CHECK(strstr(menu_status,"UPDATE AMIGA.BIN"));
            {const char one[]="AMIGA-MODE 1 1\n";CHECK(sd_write_mock(MODE_CFG,(const uint8_t*)one,sizeof one-1,&running)==ZM_OK);}
            mode_read_config();CHECK(mode_compat==0);mode_capability=0x414D4331u;
            {const char bad[]="AMIGA-MODE 1 2\n";CHECK(!mode_parse((const uint8_t*)bad,sizeof bad-1));}
            {const char bad[]="AMIGA-MODE 1 1 x";CHECK(!mode_parse((const uint8_t*)bad,sizeof bad-1));}
            {const char good[]="AMIGA-MODE 1 1\r\n";CHECK(mode_parse((const uint8_t*)good,sizeof good-1));CHECK(mode_compat==1);}
            video_capability=0;CHECK(!video_apply());video_capability=0x41565331u;
            {const char bad[]="AMIGA-VIDEO 1 2 0\n";CHECK(!video_parse((const uint8_t*)bad,sizeof bad-1));}
            {const char bad[]="AMIGA-VIDEO 1 1 2 trailing";CHECK(!video_parse((const uint8_t*)bad,sizeof bad-1));}
            {const char bad[]="AMIGA-VIDEO 1 1 3\n";CHECK(!video_parse((const uint8_t*)bad,sizeof bad-1));}
            mode_compat=0;CHECK(mode_apply());
            // INTERLACE (слой 0.28, ядро 0.13.9): строка 2 — бит 2 R_VIDEO, сразу и на карту (AMIGA-VIDEO 2), без сброса;
            // размер бит не сбрасывает; прежний формат VIDEO.CFG (версия 1) — поле не включает.
            video_config=1;CHECK(video_apply());CHECK(lace_supported);CHECK(!strcmp(video_value(2),"BOTH FIELDS"));
            menu_cursor=2;resets_before=resets;tap(&parser,0x5a);CHECK(video_config==5);CHECK(video_register==5);
            CHECK(!strcmp(video_value(2),"LAST FIELD"));CHECK(menu_mode==MODE_LIST);CHECK(resets==resets_before);
            menu_cursor=0;tap(&parser,0x5a);CHECK(video_config==6);CHECK(video_register==6);
            video_config=0;CHECK(video_read_config());CHECK(video_config==6);CHECK(video_register==6);
            {const char v2[]="AMIGA-VIDEO 2 1 0\n";CHECK(video_parse((const uint8_t*)v2,sizeof v2-1));CHECK(video_config==4);}
            {const char v1[]="AMIGA-VIDEO 1 1 0\n";CHECK(video_parse((const uint8_t*)v1,sizeof v1-1));CHECK(video_config==0);}
            {const char bad[]="AMIGA-VIDEO 2 2 0\n";CHECK(!video_parse((const uint8_t*)bad,sizeof bad-1));}
            // Ядро без бита 2 (до 0.13.9): пункт N/A, в регистр — только размер, Enter не меняет.
            lace_capability=0;video_config=6;CHECK(video_apply());CHECK(!lace_supported);CHECK(video_register==2);
            CHECK(!strcmp(video_value(2),"N/A"));
            menu_cursor=2;tap(&parser,0x5a);CHECK(video_config==6);CHECK(strstr(menu_status,"UPDATE AMIGA.BIN"));
            lace_capability=0x414C4631u;tap_ext(&parser,0x71);CHECK(video_config==0);CHECK(video_register==0);   // Del
            tap(&parser,0x76);CHECK(!menu_open);mem_remove(VIDEO_CFG);mem_remove(MODE_CFG);menu_items_free();menu_card=CARD_TANG;
        }
        // Самый большой диск: карта занятости не влезает в корень, цепочка дополнительных блоков.
        fs_layout_t l;const uint32_t cyl=2048u*2048u/HDF_CYL_SECTORS,blocks=(cyl-HDF_RDB_CYLS)*HDF_CYL_SECTORS;
        fs_stamp_t none={0,0,0};fs_layout(&l,blocks,FS_DOS1,"BIG",none);CHECK(l.maps==(blocks-2+4063)/4064);CHECK(l.exts==(l.maps-25+126)/127);CHECK(l.exts>1);
        static uint8_t bb[512];uint64_t free_bits=0;uint32_t seen=0;
        CHECK(fs_make_block(&l,l.root,bb));
        for(unsigned i=0;i<25;i++,seen++)CHECK(be32_get(bb+(79+i)*4)==l.root+1+seen);
        uint32_t ext=be32_get(bb+104*4);CHECK(ext==l.root+l.maps+1);
        while(ext){CHECK(fs_make_block(&l,ext,bb));for(unsigned i=0;i<127&&seen<l.maps;i++,seen++)CHECK(be32_get(bb+i*4)==l.root+1+seen);ext=be32_get(bb+127*4);}
        CHECK(seen==l.maps);
        for(uint32_t m=0;m<l.maps;m++){CHECK(fs_make_block(&l,l.root+1+m,bb));uint32_t sum=0;for(unsigned i=0;i<128;i++){uint32_t w=be32_get(bb+i*4);sum+=w;if(i)for(unsigned b=0;b<32;b++)free_bits+=w>>b&1;}CHECK(sum==0);}
        CHECK(free_bits==blocks-2u-1u-l.maps-l.exts);
        CHECK(!fs_make_block(&l,l.root-1,bb));CHECK(!fs_make_block(&l,fs_last(&l)+1,bb));
    }
    if(getenv("EVO_FAT"))evo_test(getenv("EVO_FAT"),getenv("EVO_DUMP"));
    if(getenv("EVO_FAT2"))evo_test(getenv("EVO_FAT2"),getenv("EVO_DUMP2"));
    command(0x72,NULL,0);CHECK(reply_len>=6&&reply_bytes[0]==2);
    command(0x73,NULL,0);CHECK(exit_req);
    puts("PASS actual AMIGA.ELF C sources: ROM checksum/corruption, complete upload, DF0 write and hot swap, F11 menu, new ADF/HDF, system disk choice, NET sections, letters, collection list and real ZIP/deflate, Evo time and file, I/O/CRC/length/timeout/error/cancel failures, DF0 absent/size/bounds/readonly/transfer/cancel, WC commands, keyboard backpressure, guest F4 and Ctrl+Alt+F4 reset, modifier recovery, keyboard as joystick (KEYS tab, port 2, blinking key while assigning), Evo card as read-only FFS volume EVO (mixed-order reads), plugin silence check");
    free(rom);return 0;
}
