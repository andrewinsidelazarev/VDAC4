#define AMIGA_HOST_TEST
#include "../esp/main/app.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t ticks, expected, seq, delivered, mask_lo, mask_hi;
static int mode, started, released, writes, key_room=8, resets;
static uint8_t packet[256], mirror[ROM_BYTES];
static uint8_t hid_cmd, hid_bytes[512], reply_bytes[100];
static unsigned hid_count, reply_len, hid_at;
static uint32_t disk_status, disk_sector, disk_token, disk_commit, mounted;
static uint8_t disk_payload[512];
static int disk_fail_at=-1, sd_result=ZM_NOT_FOUND;
static size_t sd_size=ADF_BYTES;
static int sd_read_mock(const char *path,uint8_t **data,size_t *size,size_t limit,const volatile int *active) {
    CHECK(strcmp(path,ADF_PATH)==0);CHECK(limit==ADF_BYTES);CHECK(*active);
    *data=NULL;*size=0;
    if(sd_result==ZM_OK){*data=calloc(1,sd_size);CHECK(*data);*size=sd_size;}
    return sd_result;
}
static void no_lock(void *p) { (void)p; }
static void delay_mock(uint32_t ms) { ticks+=ms; }
static uint32_t millis_mock(void) { return ticks; }
static int reply_mock(uint8_t cmd,const void *p,uint16_t n) {
    (void)cmd; CHECK(n<=sizeof reply_bytes); memcpy(reply_bytes,p,n);reply_len=n;return ZM_OK;
}
static int read_mock(uint32_t a,uint32_t *v,uint32_t n) {
    CHECK(n==1); *v=0;
    if(a==0x44)*v=mode==4?3:mode==5?9:released?4:1;
    if(a==0x48)*v=seq;
    if(a==0x4c)*v=delivered-(mode==3?4:0);
    if(a==0x50)*v=crc32(mirror,delivered)^(mode==2?1:0);
    if(a==0xc)*v=key_room;
    if(a==0x198)*v=0x41444631;
    if(a==0x184)*v=disk_status;
    if(a==0x188)*v=disk_sector;
    if(a==0x18c)*v=disk_token;
    return ZM_OK;
}
static int write_mock(uint32_t a,const uint32_t *v,uint32_t n) {
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
    CHECK(n==1);
    if(a==0x180)mounted=*v;
    if(a==0x190)disk_commit=*v;
    if(a==0x44 && *v==1){seq=delivered=mask_lo=mask_hi=0;started=1;released=0;}
    if(a==0x44 && *v==2){CHECK(delivered==ROM_BYTES);CHECK(expected==crc32(mirror,ROM_BYTES));released=1;}
    if(a==0x44 && *v==3)resets++;
    if(a==0x58)expected=*v;
    if(a==0x48){CHECK(mask_lo==~0u&&mask_hi==~0u);CHECK((*v&65535)==256);CHECK((*v>>16)==seq+1);
        memcpy(mirror+delivered,packet,256);delivered+=256;seq++;mask_lo=mask_hi=0;writes++;}
    if(a==0x10){if(*v&256){hid_cmd=(uint8_t)*v;hid_at=0;}
        else {if(hid_cmd==1){CHECK(hid_count<sizeof hid_bytes);hid_bytes[hid_count++]=(uint8_t)*v;}hid_at++;}}
    return ZM_OK;
}
int main(int argc,char **argv) {
    CHECK(argc==2);FILE *f=fopen(argv[1],"rb");CHECK(f!=NULL);
    uint8_t *rom=malloc(ROM_BYTES);CHECK(rom!=NULL);CHECK(fread(rom,1,ROM_BYTES,f)==ROM_BYTES);fclose(f);
    CHECK(valid_rom(rom,ROM_BYTES));CHECK(!valid_rom(rom,ROM_BYTES-1));
    rom[1024]^=1;CHECK(!valid_rom(rom,ROM_BYTES));rom[1024]^=1;
    static zm_core_t api;api.link_read=read_mock;api.link_write=write_mock;
    api.delay=delay_mock;api.millis=millis_mock;api.mutex_lock=no_lock;api.mutex_unlock=no_lock;
    api.signal_give=no_lock;api.reply=reply_mock;core=&api;running=1;exit_req=0;
    api.sd_read=sd_read_mock;api.free=free;
    uint8_t *disk=NULL;CHECK(load_disk(&disk));CHECK(!disk);CHECK(mount_disk(disk));CHECK(!mounted);
    sd_result=ZM_ERROR;CHECK(!load_disk(&disk));CHECK(!disk);
    sd_result=ZM_OK;sd_size=ADF_BYTES-1;CHECK(!load_disk(&disk));CHECK(!disk);
    sd_size=ADF_BYTES;CHECK(load_disk(&disk));CHECK(disk);CHECK(mount_disk(disk));CHECK(mounted==ADF_BYTES);
    for(unsigned i=0;i<ADF_BYTES;i++)disk[i]=(uint8_t)(i*37+11);
    CHECK(disk_step(disk));disk_status=0x103;disk_token=17;disk_sector=1759;
    CHECK(disk_step(disk));CHECK(disk_commit==17);CHECK(memcmp(disk_payload,disk+1759*512,512)==0);
    disk_commit=0;disk_sector=1760;CHECK(!disk_step(disk));CHECK(!disk_commit);
    disk_sector=0;disk_status=0x10b;CHECK(!disk_step(disk));disk_status=0x203;CHECK(!disk_step(disk));
    disk_status=0x103;CHECK(!disk_step(NULL));disk_fail_at=256;CHECK(!disk_step(disk));CHECK(!disk_commit);
    disk_fail_at=-1;exit_req=1;CHECK(!disk_step(disk));CHECK(!disk_commit);exit_req=0;
    disk_status=0;free(disk);
    CHECK(upload_rom(rom));CHECK(released);CHECK(writes==2048);CHECK(memcmp(rom,mirror,ROM_BYTES)==0);
    for(mode=1;mode<=5;mode++){CHECK(!upload_rom(rom));CHECK(!released);}
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
    command(0x72,NULL,0);CHECK(reply_len>=6&&reply_bytes[0]==2);
    command(0x73,NULL,0);CHECK(exit_req);
    puts("PASS actual AMIGA.ELF C sources: ROM checksum/corruption, complete upload, I/O/CRC/length/timeout/error/cancel failures, DF0 absent/size/bounds/readonly/transfer/cancel, WC commands, keyboard backpressure, guest F4 and Ctrl+Alt+F4 reset, modifier recovery");
    free(rom);return 0;
}
