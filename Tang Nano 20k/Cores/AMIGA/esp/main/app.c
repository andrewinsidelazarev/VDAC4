/* SPDX-License-Identifier: GPL-3.0-or-later
 * AMIGA.ELF: Kickstart from Tang SD, Evo input, stereo via ESP DAC, the WC core START/INPUT/STATUS/EXIT
 * protocol. DF0: /AMIGA/DF0.ADF, меню F11 (disk_menu.h), образ .adf из WC. Образ дискеты целиком в PSRAM;
 * запись идёт и в PSRAM, и сразу в файл на карте Tang. HD0: образ, выбранный в меню (путь в /AMIGA/HD0.CFG),
 * пока не выбран — /AMIGA/HD0.HDF; работает на месте, секторы прямо с карты. Выбранный диск — системный:
 * /AMIGA/DF0.ADF при старте не вставляется, чтобы загрузка шла с диска.
 * Дискета с карты Evo только для чтения: записывать на неё некуда.
 * HD1 (ядро 0.11+): карта Evo целиком — том EVO: в Workbench, только чтение (evo_vol.h, плагин VDAC4 Cores 0.15+);
 * архивы LHA на ней — ещё и каталоги, распакованные на лету (evo_lha.h).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "zm_app.h"
#include "zm_level.h"
#include "ps2_hid.h"

#define ROM_PATH "/AMIGA/KICK31.ROM"
#define ROM_BYTES 524288u
#define ADF_PATH "/AMIGA/DF0.ADF"
#define ADF_BYTES 901120u
#define HDF_PATH "/AMIGA/HD0.HDF"
#define HD0_CFG "/AMIGA/HD0.CFG"
#define HD0_PATH_MAX 512u
#define HDF_MAX_SECTORS 8388607u    /* размер в регистре ядра — 32 бита в байтах */
#define VERSION 0x0501000Cu
#define R_DISK_SIZE 0x180u
#define R_DISK_STATUS 0x184u
#define R_DISK_SECTOR 0x188u
#define R_DISK_TOKEN 0x18Cu
#define R_DISK_ACK 0x190u
#define R_DISK_UNIT 0x194u
#define R_DISK_SIGN 0x198u
#define R_DISK_WP 0x19Cu
#define R_RTC 0xDCu
#define RTC_WAIT_MS 1200u
#define W_DISK 0x5000u
#define DF0_SWAP_MS 2000u           /* без дискеты, чтобы система успела увидеть смену */
static const zm_core_t *core;
static void *lock, *link_lock, *wake;
static volatile int running, state, exit_req, live;
static volatile int restart_req;   /* 8.11.2026, слой 0.25: смена режима совместимости — полный перезапуск сеанса */
static volatile int worker_done=1, audio_done=1;
static uint32_t core_version, input_ms, alive_ms, generation;
static char error_text[80];
static uint8_t ring[256], head, tail, lost, joy, buttons, last_x, last_y, mouse_base;
static int32_t mouse_x, mouse_y;
static uint8_t *df0_data;
static void *df0_file, *hd0_file;
static int df0_ro=1, menu_available;
static char df0_name[64], hd0_name[64], hd0_path[HD0_PATH_MAX];
static uint32_t df0_swap_at, hd0_sectors, zero_sector[128];
static uint32_t sector_words[128];
static uint8_t evo_time[6];
static volatile int rtc_pending;
/* Файл с карты Evo: плагин шлёт его командами 0x77…0x79 (.adf из WC и меню F11) */
static uint8_t *rx_data;
static volatile uint32_t rx_received;
static volatile int rx_ready, rx_receiving;
static int sd_disk_api(void) {
    return core->size>=offsetof(zm_core_t,sd_create)+sizeof core->sd_create && core->sd_open && core->sd_pread;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c=~0u;
    while (n--) { c^=*p++; for (unsigned k=0;k<8;k++) c=(c>>1)^((0u-(c&1u))&0xedb88320u); }
    return ~c;
}
static uint32_t little32(const uint8_t *p) {
    return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static int valid_rom(const uint8_t *p, size_t n) {
    if (n!=ROM_BYTES || memcmp(p,"AMIROMTYPE1",11)==0) return 0;
    uint32_t sum=0;
    for (size_t i=0;i<n;i+=4) {
        uint32_t v=(uint32_t)p[i]<<24|(uint32_t)p[i+1]<<16|(uint32_t)p[i+2]<<8|p[i+3];
        uint32_t old=sum; sum+=v; if (sum<old) ++sum;
    }
    return sum==0xffffffffu;
}
static int rd(uint32_t a,uint32_t *v) { return core->link_read(a,v,1)==ZM_OK; }
static int wr(uint32_t a,uint32_t v) { return core->link_write(a,&v,1)==ZM_OK; }
// The current host can access Tang FAT while TS-Config is running. Preload the
// floppy into PSRAM before core_load, then serve sectors without reopening SD.
static int load_disk(uint8_t **disk) {
    size_t size=0;
    int rc=core->sd_read(ADF_PATH,disk,&size,ADF_BYTES,&running);
    if (rc==ZM_NOT_FOUND) {
        if (*disk) core->free(*disk);
        *disk=NULL;
        return 1;
    }
    if (rc!=ZM_OK || size!=ADF_BYTES) {
        if (*disk) core->free(*disk);
        *disk=NULL;
        return 0;
    }
    return 1;
}
static int disk_step(int *served);
static void ev_read(uint32_t lba,uint8_t *o);
static void ev_rdb_view(uint8_t *b);
static void ev_write(uint32_t lba,const uint8_t *data);
/* Смену носителя ядро принимает только в простое (amiga_disk.sv), запись в занятый ящик молча теряется: прежний диск уже снят,
 * новый не подключён, и система не видит диска. Меню держит этот поток, запрос гостя ждёт ответа: сначала обслужить его. */
static int mount_unit(unsigned unit,uint32_t bytes) {
    for (unsigned tries=0;tries<500u;tries++) {
        uint32_t status=0;
        int served=0;
        if (!rd(R_DISK_STATUS,&status)) return 0;
        if (!(status&2)) break;
        if ((status&1) && !disk_step(&served)) return 0;
        if (!served) core->delay(1);
    }
    return wr(R_DISK_UNIT,unit) && wr(R_DISK_SIZE,bytes) && wr(R_DISK_UNIT,0);
}
static int mount_disk(const uint8_t *disk) {
    uint32_t signature=0;
    if (!rd(R_DISK_SIGN,&signature) || signature!=0x41444632u) return 0;
    return wr(R_DISK_WP,df0_ro) && mount_unit(0,disk?ADF_BYTES:0);
}
static void df0_close_file(void) {
    if (df0_file) { core->sd_close(df0_file); df0_file=NULL; }
}
static int send_sector(const uint8_t *p) {
    for (unsigned part=0;part<8;part++) {
        if (!running || exit_req) return 0;
        uint32_t w[16];
        for (unsigned i=0;i<16;i++) w[i]=little32(p+part*64+i*4);
        if (core->link_write(W_DISK+part*64,w,16)!=ZM_OK) return 0;
    }
    return 1;
}
static int take_sector(void) {
    for (unsigned part=0;part<8;part++)
        if (core->link_read(W_DISK+part*64,sector_words+part*16,16)!=ZM_OK) return 0;
    return 1;
}
/* Карта занята (BUSY) — ядро её вернёт: подождать, отказ — только при настоящей ошибке. */
static int sd_retry_read(void *file,uint32_t sector,uint8_t *data) {
    for (unsigned tries=0;tries<200;tries++) {
        int r=core->sd_pread(file,sector,data,1);
        if (r!=ZM_BUSY) return r==ZM_OK;
        core->delay(5);
    }
    return 0;
}
static int sd_retry_write(void *file,uint32_t sector,const uint8_t *data) {
    for (unsigned tries=0;tries<200;tries++) {
        int r=core->sd_pwrite(file,sector,data,1);
        if (r!=ZM_BUSY) return r==ZM_OK;
        core->delay(5);
    }
    return 0;
}
/* Один запрос сектора, если есть. *served — был запрос. Ноль — обмен с ядром нарушен. */
static int disk_step(int *served) {
    uint32_t status=0, sector=0, token=0;
    *served=0;
    if (!rd(R_DISK_STATUS,&status) || (status&4)) return 0;
    if (!(status&1)) return 1;
    *served=1;
    unsigned drive=(status>>8)&255;
    int writing=(status&8)!=0;
    if (!rd(R_DISK_SECTOR,&sector) || !rd(R_DISK_TOKEN,&token)) return 0;
    if (drive==1) {
        if (sector>=ADF_BYTES/512) return 0;
        if (writing) {
            if (!take_sector()) return 0;
            if (df0_data && !df0_ro) {
                memcpy(df0_data+(size_t)sector*512,sector_words,512);
                if (df0_file && !sd_retry_write(df0_file,sector,(const uint8_t *)sector_words)) {
                    /* Дальше — только чтение: иначе программа считала бы записанным то, чего нет. */
                    core->log("amiga: DF0 write failed, sector %u",(unsigned)sector);
                    df0_ro=1; wr(R_DISK_WP,1); df0_close_file();
                }
            }
        } else if (!send_sector(df0_data?df0_data+(size_t)sector*512:(const uint8_t *)zero_sector)) return 0;
    } else if (drive==0x10) {
        if (!hd0_file || sector>=hd0_sectors) return 0;
        if (writing) {
            if (!take_sector() || !sd_retry_write(hd0_file,sector,(const uint8_t *)sector_words)) return 0;
        } else {
            if (!sd_retry_read(hd0_file,sector,(uint8_t *)sector_words)) return 0;
            if (sector==0) ev_rdb_view((uint8_t *)sector_words);
            if (!send_sector((const uint8_t *)sector_words)) return 0;
        }
    } else if (drive==0x20) {
        /* Том EVO с карты Evo (evo_vol.h): блоки собираются здесь; запись — в память ESP, на карту никогда. */
        if (writing) {
            if (!take_sector()) return 0;
            ev_write(sector,(const uint8_t *)sector_words);
        } else {
            ev_read(sector,(uint8_t *)sector_words);
            if (!send_sector((const uint8_t *)sector_words)) return 0;
        }
    } else return 0;
    return wr(R_DISK_ACK,token);
}
static void rx_discard(void) {
    if (rx_data) core->free(rx_data);
    rx_data=NULL; rx_received=0; rx_ready=0;
}
/* Новая дискета: снять прежнюю (система должна это увидеть), вставить через DF0_SWAP_MS. */
static void df0_change(uint8_t *data,void *file,int read_only,const char *name) {
    mount_unit(0,0);
    if (df0_data) core->free(df0_data);
    df0_close_file();
    df0_data=data; df0_file=file; df0_ro=read_only;
    strncpy(df0_name,name,sizeof df0_name-1); df0_name[sizeof df0_name-1]=0;
    wr(R_DISK_WP,df0_ro);
    uint32_t at=core->millis()+DF0_SWAP_MS;
    df0_swap_at=at?at:1;
}
static void df0_eject(void) {
    df0_swap_at=0;
    mount_unit(0,0);
    if (df0_data) core->free(df0_data);
    df0_data=NULL; df0_close_file(); df0_ro=1; df0_name[0]=0;
}
static const char *df0_insert_received(const char *name) {
    uint8_t *data=rx_data;
    rx_data=NULL; rx_received=0; rx_ready=0;
    df0_change(data,NULL,1,name);
    return NULL;
}
static const char *df0_insert_tang(const char *path,const char *name) {
    uint8_t *data=NULL; size_t size=0; void *file=NULL; uint32_t sectors=0;
    int r=core->sd_read(path,&data,&size,ADF_BYTES,&running);
    if (r!=ZM_OK || size!=ADF_BYTES) {
        if (data) core->free(data);
        return r==ZM_OK?"NOT AN 880K ADF IMAGE":r==ZM_BUSY?"TANG SD IS BUSY, TRY AGAIN":"CANNOT READ THE FILE";
    }
    df0_close_file();       /* та же дискета ещё раз: один файл не держать открытым дважды */
    if (sd_disk_api() && (core->sd_open(path,1,&sectors,&file)!=ZM_OK || sectors!=ADF_BYTES/512)) {
        if (file) core->sd_close(file);
        file=NULL;
    }
    df0_change(data,file,file==NULL,name);
    return NULL;
}
static void hd0_detach(void) {
    mount_unit(4,0);
    if (hd0_file) core->sd_close(hd0_file);
    hd0_file=NULL; hd0_sectors=0; hd0_name[0]=0;
}
/* Образ работает там, где лежит, ничего не копируется. Без RDB ядро диск не подключит (nanomig.v). */
static const char *hd0_attach(const char *path) {
    void *file=NULL; uint32_t sectors=0;
    int r=core->sd_open(path,1,&sectors,&file);
    if (r!=ZM_OK) return r==ZM_BUSY?"TANG SD IS BUSY, TRY AGAIN":"CANNOT OPEN THE IMAGE";
    const char *problem=NULL;
    if (!sectors || sectors>HDF_MAX_SECTORS) problem="THE IMAGE IS TOO BIG";
    else if (!sd_retry_read(file,0,(uint8_t *)sector_words)) problem="CANNOT READ THE IMAGE";
    else if (memcmp(sector_words,"RDSK",4)) problem="NO RDB IN THE IMAGE, CANNOT BE USED";
    if (problem) { core->sd_close(file); return problem; }
    hd0_detach();
    if (!mount_unit(4,sectors*512u)) { core->sd_close(file); return "CORE DOES NOT ANSWER"; }
    const char *slash=strrchr(path,'/');
    strncpy(hd0_name,slash?slash+1:path,sizeof hd0_name-1); hd0_name[sizeof hd0_name-1]=0;
    hd0_file=file; hd0_sectors=sectors;
    if (path!=hd0_path) { strncpy(hd0_path,path,sizeof hd0_path-1); hd0_path[sizeof hd0_path-1]=0; }
    core->log("amiga: HD0 %s, %u sectors",hd0_name,(unsigned)sectors);
    return NULL;
}
/* Что подключить при старте. 1 — диск выбран в меню и файл на месте: он и системный. */
static int hd0_read_config(void) {
    uint8_t *text=NULL; size_t size=0;
    strcpy(hd0_path,HDF_PATH);
    if (!sd_disk_api()) return 0;
    /* Нет файла — диск не выбран. Занятая карта — это ещё не «не выбран»: подождать, иначе Amiga молча стартует без диска. */
    int r=core->sd_read(HD0_CFG,&text,&size,HD0_PATH_MAX-1u,&running);
    for (unsigned tries=0;r==ZM_BUSY && tries<20u && running;tries++) {
        core->delay(100u);
        r=core->sd_read(HD0_CFG,&text,&size,HD0_PATH_MAX-1u,&running);
    }
    if (r!=ZM_OK) {
        if (text) core->free(text);
        if (r!=ZM_NOT_FOUND) core->log("amiga: cannot read %s: %d",HD0_CFG,r);
        return 0;
    }
    size_t n=0;
    while (n<size && text[n]>=0x20) { hd0_path[n]=(char)text[n]; n++; }
    hd0_path[n]=0;
    core->free(text);
    if (!n) return 0;
    void *file=NULL; uint32_t sectors=0;
    if (core->sd_open(hd0_path,0,&sectors,&file)!=ZM_OK) {
        core->log("amiga: chosen HD0 not found: %s",hd0_path);
        return 0;
    }
    core->sd_close(file);
    return 1;
}
/* Путь пустой — диска нет. Запись одним файлом: прерванная не оставит половину пути. */
static int hd0_save_config(const char *path) {
    uint8_t text[HD0_PATH_MAX]; size_t n=strlen(path);
    if (n+1>=sizeof text) return 0;
    memcpy(text,path,n); text[n]='\n';
    return core->sd_write(HD0_CFG,text,n+1,&running)==ZM_OK;
}
/* Другой диск виден системе только после перезапуска. Ядро сначала читает у образа RDB. */
static void amiga_restart(void) {
    uint32_t at=core->millis();
    while (core->millis()-at<100u) {
        int served=0;
        if (!disk_step(&served)) break;
        if (!served) core->delay(1);
    }
    wr(0x44,3);
}
#include "amiga_fs.h"
#include "disk_menu.h"
#include "evo_vol.h"
/* Время Evo: минуты и прочее — двоичные, год — две цифры, как у настоящего MSM6242; месяц 0…11, у числа сверху день недели. */
static void rtc_apply(void) {
    uint8_t t[6];
    core->mutex_lock(lock); memcpy(t,evo_time,6); rtc_pending=0; core->mutex_unlock(lock);
    if (t[4]<1 || t[4]>12 || t[3]<1 || t[3]>31) return;
    static const uint8_t table[12]={0,3,2,5,0,3,5,1,4,6,2,4};
    unsigned year=2000u+t[5], y=year-(t[4]<3);
    unsigned weekday=(y+y/4-y/100+y/400+table[t[4]-1]+t[3])%7;
    const uint32_t field[6]={0u<<8|t[5], 1u<<8|(t[4]-1u), 2u<<8|weekday<<5|t[3],
                             3u<<8|t[2], 4u<<8|t[1], 5u<<8|t[0]};
    for (unsigned i=0;i<6;i++) wr(R_RTC,field[i]);
    core->log("amiga: RTC from Evo 20%02u-%02u-%02u %02u:%02u:%02u",t[5],t[4],t[3],t[2],t[1],t[0]);
}
static void fail(const char *s) {
    strncpy(error_text,s,sizeof error_text-1); error_text[sizeof error_text-1]=0;
    core->log("amiga: %s",error_text);
}
static int wait_packet(uint32_t seq) {
    uint32_t start=core->millis(), s=0, ack=0;
    while (running && !exit_req && core->millis()-start<500) {
        if (!rd(0x44,&s) || !rd(0x48,&ack)) return 0;
        if (s&8) return 0;
        if (!(s&2) && ack==seq) return 1;
        core->delay(1);
    }
    return 0;
}
static int upload_rom(const uint8_t *p) {
    uint32_t wanted=crc32(p,ROM_BYTES), count=0, crc=0;
    if (!wr(0x44,1) || !wr(0x58,wanted)) return 0;
    for (uint32_t off=0;off<ROM_BYTES;off+=256) {
        if (!running || exit_req) return 0;
        uint32_t w[16];
        for (unsigned part=0;part<4;part++) {
            for (unsigned i=0;i<16;i++) w[i]=little32(p+off+part*64+i*4);
            if (core->link_write(0x4000+part*64,w,16)!=ZM_OK) return 0;
        }
        uint32_t seq=off/256+1;
        if (!wr(0x48,(seq<<16)|256) || !wait_packet(seq)) return 0;
    }
    return rd(0x4c,&count) && rd(0x50,&crc) && count==ROM_BYTES && crc==wanted;
}
/* Снять сброс: процессор пошёл. */
static int rom_release(void) {
    uint32_t s=0;
    if (!wr(0x44,2)) return 0;
    core->delay(1);
    return rd(0x44,&s) && (s&15)==4;
}

static int hid_packet(uint8_t cmd,const uint8_t *p,unsigned n) {
    if (!wr(0x10,0x100u|cmd)) return 0;
    for (unsigned i=0;i<n;i++) if (!wr(0x10,p[i])) return 0;
    return 1;
}
static int32_t clamp127(int32_t n) { return n>127?127:n< -127?-127:n; }
static int input_step(ps2_hid_t *parser) {
    static uint16_t pending[128];
    static unsigned at, count;
    static uint8_t down[128], f4_down;
    uint8_t bytes[64], j,b; unsigned n=0; int clear, was_open=menu_open;
    int32_t dx,dy;
    core->mutex_lock(lock);
    clear=lost; lost=0;
    if (clear) at=count=0;
    while (!clear && at==count && tail!=head && n<sizeof bytes) bytes[n++]=ring[tail++];
    j=joy; b=buttons; dx=clamp127(mouse_x); dy=clamp127(mouse_y);
    core->mutex_unlock(lock);
    if (clear) {
        ps2_hid_init(parser);
        if (!wr(0x1c,0)) return 0;
        f4_down=0;
        ja_release_all();
        // The guest OS may have consumed key-down events already. Release them
        // after clearing the transport FIFO, so overflow cannot stick modifiers.
        for (unsigned k=0;k<128;k++) if (down[k]) pending[count++]=(uint16_t)(k|128);
    }
    if (n) {
        at=count=0;
        for (unsigned i=0;i<n;i++) {
            uint16_t ev[2]; int got=ps2_hid_byte(parser,bytes[i],ev);
            for (int k=0;k<got;k++) pending[count++]=ev[k];
        }
    }
    while (at<count) {
        uint16_t ev=pending[at]; uint32_t free_slots=0;
        if (ev!=PS2_HID_CLEAR && menu_key(ev)) {
            ++at;
            // Меню открылось: что держали нажатым, отпустить, отпускания уйдут уже меню.
            if (menu_open && !was_open) {
                was_open=1;
                ja_release_all();
                for (unsigned k=0;k<128;k++) if (down[k]) {
                    uint8_t release=(uint8_t)(k|128);
                    if (!hid_packet(1,&release,1)) return 0;
                    down[k]=0;
                }
            }
            continue;
        }
        if (ev==PS2_HID_CLEAR) { ja_release_all(); if (!wr(0x1c,0)) return 0; }
        else if ((ev&127)==61 && (f4_down ||
                 (!(ev&128) && !down[61] && !ja_held[61] && (down[104] || down[108] || ja_held[104] || ja_held[108]) &&
                  (down[106] || down[110] || ja_held[106] || ja_held[110])))) {
            // Ctrl+Alt+F4 resets once, также когда Ctrl и Alt назначены на джойстик. Plain F4 belongs to the guest/Test Kit.
            if (!(ev&128) && !f4_down && !wr(0x44,3)) return 0;
            f4_down=(ev&128)==0;
        } else if (ja_input(ev)) {
            // Клавиша джойстика: Amiga её не получает.
        } else {
            if (!rd(0x0c,&free_slots)) return 0;
            if (!free_slots) break;
            uint8_t k=(uint8_t)ev;
            if ((k&128) && !down[k&127]) { ++at; continue; }   // нажата в меню, отпущена уже в игре
            if (!hid_packet(1,&k,1)) return 0;
            down[k&127]=(k&128)==0;
        }
        ++at;
    }
    if (!menu_blink_step()) return 0;
    if (menu_open) { b=0; j=0; dx=dy=0; core->mutex_lock(lock); mouse_x=mouse_y=0; core->mutex_unlock(lock); }
    // NanoMig expects USB mouse coordinates; Kempston Y grows upward.
    uint8_t mouse[3]={b,(uint8_t)dx,(uint8_t)-dy};
    if (!hid_packet(2,mouse,3)) return 0;
    core->mutex_lock(lock); mouse_x-=dx; mouse_y-=dy; core->mutex_unlock(lock);
    // USB/NanoMig order: R,L,D,U,fire1,fire2. Кнопки Kempston и клавиатуры в режиме KEYS складываются. В меню клавиши у него, джойстика нет.
    // Устройство 0, не 1: userio при joy_swap=0 перекрещивает входы, joystick0 → _joy1 → порт 2 (игровой, JOY1DAT),
    // joystick1 → порт 1 (мышь). До 0.16 джойстик уходил в порт мыши, игры его не видели (tests/run_joystick.py).
    uint8_t js[2]={0,(uint8_t)((j&31)|((j>>1)&32)|(menu_open?0:ja_mask))};
    return hid_packet(3,js,2);
}

static uint32_t gray_decode(uint32_t n) {
    for (unsigned shift=1;shift<32;shift<<=1) n^=n>>shift;
    return n;
}
static int audio_read(uint32_t a,uint32_t *v,unsigned n) {
    core->mutex_lock(link_lock);
    int ok=live && core->link_read(a,v,n)==ZM_OK;
    core->mutex_unlock(link_lock);
    return ok;
}
/* The same K-weighted integrated loudness processor as TRACKER.ELF.
 * Paula can also be loud at unity: allow attenuation down to -18 dB. */
static void audio_prepare(zm_level_t *level,float *frames,const uint32_t *words,unsigned n) {
    for(unsigned i=0;i<n;i++) {
        frames[2*i]=(float)(int16_t)words[i]/32768.0f;
        frames[2*i+1]=(float)(int16_t)(words[i]>>16)/32768.0f;
    }
    zm_level_process(level,frames,n);
}
static void audio_task(void *arg) {
    (void)arg;
    static zm_level_t level; /* owned only by this task, outside its 4 KiB stack */
    uint32_t level_gen=~0u;
    while (running) {
        if (!live) { core->delay(10); continue; }
        uint32_t gen=generation, wp=0, last=core->millis();
        zm_pcm_format_t fmt={48000,2};
        if (core->pcm_open(&fmt)!=ZM_OK) { core->delay(20); continue; }
        if(level_gen!=gen || !level.ready) {
            zm_level_init(&level,48000,ZM_LEVEL_BASE_DB,-18.0f);level_gen=gen;
        } else zm_level_discontinuity(&level);
        audio_read(0x20,&wp,1);
        uint32_t rp=gray_decode(wp&1023);
        while (running && live && generation==gen) {
            if (!audio_read(0x20,&wp,1)) break;
            wp=gray_decode(wp&1023);
            uint32_t now=core->millis(), have=(wp-rp)&1023;
            if (now-last>=18 || have>768) { rp=(wp-192)&1023; have=192; zm_level_discontinuity(&level); }
            last=now;
            if (have>384) have=384;
            while (have && running && live && generation==gen) {
                uint32_t words[16]; float frames[32];
                unsigned n=have>16?16:have; if (n>1024-rp) n=1024-rp;
                if (!audio_read(0x6000+rp*4,words,n)) break;
                audio_prepare(&level,frames,words,n);
                unsigned done=0;
                while (done<n && running && live && generation==gen) {
                    int wrote=core->pcm_write(frames+2*done,n-done);
                    if (wrote<=0 || (unsigned)wrote>n-done) break;
                    done+=(unsigned)wrote;
                }
                rp=(rp+n)&1023; have-=n;
            }
            core->delay(4);
        }
        core->pcm_flush(); core->pcm_close();
    }
    audio_done=1;
}
/* Плагин молчит дольше 1,5 с — Evo сброшена. Знак жизни — любая его команда (alive_ms, command()), не только ВВОД: файл
 * с карты Evo из меню F11 идёт командами 0x77…0x79 без ВВОДа несколько секунд, и после него слой уходил в WC (02.11.2026;
 * у NES то же учтено в rom_source.h). input_ms — только для мыши: по нему считается сдвиг. Время и отметка — под одним
 * замком, разность — со знаком: без этого ответ плагина между millis() и чтением отметки давал «минус 1 мс», то есть
 * 4 млрд, и тихий выход в WC (02.11.2026, копирование значков с тома EVO). */
static int plugin_silent(uint32_t *now) {
    core->mutex_lock(lock);
    *now=core->millis();
    const int32_t quiet=(int32_t)(*now-alive_ms);
    core->mutex_unlock(lock);
    return quiet>1500;
}
static void worker(void *arg) {
    (void)arg;
    while (running) {
        core->signal_wait(wake,20);
        if (!running || state!=1) continue;
        uint8_t *rom=NULL, *disk=NULL; size_t size=0; int loaded=0, ok=0, start_ok=0;
        df0_ro=1; df0_name[0]=0; df0_swap_at=0; menu_open=0; menu_available=0;
        hd0_file=NULL; hd0_sectors=0; hd0_name[0]=0; df0_file=NULL;
        if (core->sd_read(ROM_PATH,&rom,&size,ROM_BYTES,&running)!=ZM_OK)
            fail("Place 512 KiB Kickstart at /AMIGA/KICK31.ROM on Tang SD");
        else if (!valid_rom(rom,size)) fail("Kickstart size or Amiga checksum is invalid");
        else if (rx_ready && rx_data) {         // .adf, открытый в WC: уже в памяти, только чтение
            hd0_read_config();
            disk=rx_data; rx_data=NULL; rx_ready=0; rx_received=0;
            strcpy(df0_name,"FILE FROM EVO SD");
            start_ok=1;
        }
        else if (hd0_read_config()) start_ok=1; // загрузка с выбранного диска, дискеты нет
        else if (!load_disk(&disk)) fail("DF0.ADF read failed: use a standard 880 KiB ADF");
        else start_ok=1;
        if (start_ok && (!running || exit_req)) { fail("Start cancelled"); start_ok=0; }
        if (start_ok) {
            if (disk && !df0_name[0]) strcpy(df0_name,"DF0.ADF");
            loaded=1; // core_load can fail after FPGA has changed: always attempt recovery.
            if (core->core_load("AMIGA",&core_version)!=ZM_OK) fail("AMIGA.BIN load failed");
            else if (core_version!=VERSION) fail("AMIGA.BIN / AMIGA.ELF version mismatch");
            else if (!mount_disk(disk)) fail("ADF bridge unavailable; update AMIGA.BIN");
            /* 8.11.2026, слой 0.25: режим совместимости — в ядро до загрузки ROM, пока процессор в сбросе */
            else if ((mode_read_config(),!upload_rom(rom))) fail("Kickstart LINK transfer or CRC failed");
            else ok=1;
        }
        if (rom) core->free(rom);
        if (ok && running && !exit_req) {
            ps2_hid_t parser; ps2_hid_init(&parser);
            core->mutex_lock(lock);
            input_ms=alive_ms=core->millis(); mouse_base=1; lost=1;
            core->mutex_unlock(lock);
            core->mutex_lock(link_lock); ++generation; live=1; core->mutex_unlock(link_lock);
            /* Kickstart читает часы через 1,1 с после сброса (модель), а TIME плагин присылает, когда увидел
             * состояние 2. Процессор ждёт время, но недолго: без плагина с TIME Amiga просто стартует позже. */
            state=2;
            for (uint32_t t0=core->millis();!rtc_pending && running && !exit_req && core->millis()-t0<RTC_WAIT_MS;)
                core->signal_wait(wake,20);
            if (rtc_pending) rtc_apply();
            ev_mount();                         // карта Evo — HD1, пока процессор в сбросе: Kickstart ищет диски сразу
            if (!rom_release()) { fail("Kickstart did not start"); ok=0; }
            core->mutex_lock(lock); input_ms=alive_ms=core->millis(); core->mutex_unlock(lock);
            df0_data=disk; disk=NULL;
            /* Карта Tang в этом ядре — при прошивке с посекторным доступом: запись в DF0 и HD0, меню. */
            if (sd_disk_api()) {
                uint32_t sectors=0; void *file=NULL;
                keys_read_config();
                if(!video_read_config()){fail("Video controls unavailable; update AMIGA.BIN");ok=0;}
                menu_available=1;
                if (df0_data && !strcmp(df0_name,"DF0.ADF") &&
                    core->sd_open(ADF_PATH,1,&sectors,&file)==ZM_OK) {
                    if (sectors==ADF_BYTES/512) { df0_file=file; df0_ro=0; wr(R_DISK_WP,0); }
                    else core->sd_close(file);
                }
                if (hd0_path[0]) {
                    const char *problem=hd0_attach(hd0_path);
                    if (problem) core->log("amiga: HD0 %s: %s",hd0_path,problem);
                }
            }
            uint32_t input_at=core->millis()-4;
            while (ok && running && !exit_req) {
                uint32_t now;
                if (restart_req) break;
                if (plugin_silent(&now)) { core->log("amiga: the plugin is silent (Evo reset?), back to slot 0"); break; }
                // Сектора идут подряд, клавиатура и мышь — не чаще 4 мс: ввод стоит дорого по LINK.
                if (now-input_at>=4) {
                    input_at=now;
                    if (!input_step(&parser)) { fail("Amiga input LINK failed"); ok=0; break; }
                }
                if (rtc_pending) rtc_apply();
                if (df0_swap_at && (int32_t)(now-df0_swap_at)>=0) {
                    df0_swap_at=0;
                    if (df0_data) mount_unit(0,ADF_BYTES);
                }
                int served=0;
                if (!disk_step(&served)) { fail("Disk sector transfer failed"); ok=0; break; }
                if (!served) core->signal_wait(wake,2);
            }
        }
        core->mutex_lock(link_lock); live=0; core->mutex_unlock(link_lock);
        if (disk) core->free(disk);
        if (df0_data) core->free(df0_data);
        df0_data=NULL;
        df0_close_file();
        if (hd0_file) { core->sd_close(hd0_file); hd0_file=NULL; }
        ev_close();
        menu_items_free();
        rx_discard();
        /* 8.11.2026, слой 0.25: смена режима совместимости (F11 -> SETUP) — полный перезапуск: ядро и ROM заново, как
         * выход в WC и новый запуск, но без WC. Прежде режим включался тёплым сбросом, и после перехода 68000 -> 68020
         * Amiga на плате зависла (Kickstart доверяет памяти прошлого запуска, а процессор и карта памяти другие). */
        if (restart_req && ok && running && !exit_req) {
            restart_req=0;
            core->log("amiga: restart, compatibility mode %s",mode_compat?"on":"off");
            /* тем же путём, что выход в WC и новый запуск: Tang — в слот 0, затем ядро заново */
            if (loaded && core->core_back(10000)!=ZM_OK) { fail("Could not return to TS-Config"); state=255; continue; }
            state=1; continue;
        }
        restart_req=0;
        if (loaded && core->core_back(10000)!=ZM_OK) { fail("Could not return to TS-Config"); ok=0; }
        state=ok?3:255;
    }
    worker_done=1;
}
static uint16_t crc16(const uint8_t *p,size_t n) {
    uint16_t c=0xFFFF;
    while (n--) { c^=(uint16_t)(*p++)<<8; for (unsigned k=0;k<8;k++) c=(c&0x8000)?(uint16_t)(c<<1^0x1021):(uint16_t)(c<<1); }
    return c;
}
/* Файл от плагина (Cores/NES/esp/main/rom_source.h): до старта ядра или по запросу меню. */
static int rx_command(uint8_t cmd,const uint8_t *p,uint16_t n) {
    uint8_t reply[5]={0};
    int idle=state==0 || state==3 || state==255 || (state==2 && rx_receiving);
    if (idle) {
        if (cmd==0x77 && n==4) {
            if (little32(p)==ADF_BYTES) {
                rx_discard();
                rx_data=core->alloc(ADF_BYTES,ZM_MEM_PSRAM);
                if (rx_data) { error_text[0]=0; if (state!=2) state=0; reply[0]=1; }
                else fail("Not enough PSRAM for the disk image");
            } else fail("Not an 880 KiB ADF image");
        } else if (cmd==0x78 && n>6 && n<=518 && rx_data && !rx_ready) {
            uint32_t at=little32(p), count=n-6u;
            uint16_t expected=(uint16_t)p[n-2]|(uint16_t)p[n-1]<<8;
            if (crc16(p,n-2)==expected && at<=rx_received && count<=ADF_BYTES-at) {
                if (at==rx_received) { memcpy(rx_data+at,p+4,count); rx_received+=count; reply[0]=1; }
                else if (count<=rx_received-at && !memcmp(rx_data+at,p+4,count)) reply[0]=1;
            }
        } else if (cmd==0x79 && n==0 && rx_data && rx_received==ADF_BYTES) { rx_ready=1; reply[0]=1; }
    }
    put32(reply+1,rx_received);
    core->reply(cmd|0x80,reply,sizeof reply);
    return ZM_OK;
}
static int command(uint8_t cmd,const uint8_t *p,uint16_t n) {
    uint8_t ok=0;
    core->mutex_lock(lock); alive_ms=core->millis(); core->mutex_unlock(lock);   // любая команда — плагин жив
    if (cmd==0x6E || cmd==0x6F) return evo_command(cmd,p,n);
    if (cmd==0x76) return ev_sd_done(p,n);
    if (cmd>=0x77 && cmd<=0x79) return rx_command(cmd,p,n);
    if (cmd==0x7A) { const uint8_t reply[2]={1,1}; core->reply(0xFA,reply,sizeof reply); return ZM_OK; }
    if (cmd==0x70) {
        if ((state==0 || state==3 || state==255) &&
            (n==0 || (n==6 && memcmp(p,"AMIGA",6)==0))) {
            core->mutex_lock(lock);
            head=tail=0; lost=1; mouse_base=1; mouse_x=mouse_y=0;
            joy=buttons=0; error_text[0]=0; exit_req=0; state=1;
            core->mutex_unlock(lock); ok=1; core->signal_give(wake);
        }
    } else if (cmd==0x71) {
        if (n>=4 && (state==1 || state==2)) {
            core->mutex_lock(lock);
            uint32_t now=core->millis();
            if (!mouse_base && now-input_ms<=500) {
                mouse_x+=(int8_t)(uint8_t)(p[2]-last_x);
                mouse_y+=(int8_t)(uint8_t)(p[3]-last_y);
            }
            mouse_base=0; last_x=p[2]; last_y=p[3]; input_ms=now;
            joy=p[0]; buttons=(uint8_t)(~p[1]&7);
            for (unsigned i=4;i<n;i++) {
                if ((uint8_t)(head+1)==tail) { head=tail=0; lost=1; break; }
                ring[head++]=p[i];
            }
            core->mutex_unlock(lock); core->signal_give(wake);
            evo_reply_input(1);
            return ZM_OK;
        }
        evo_reply_input(0);
        return ZM_OK;
    } else if (cmd==0x72) {
        uint8_t out[5+sizeof error_text];
        out[0]=(uint8_t)state; put32(out+1,core_version);
        size_t len=strlen(error_text); memcpy(out+5,error_text,len+1);
        core->reply(0xf2,out,(uint16_t)(6+len)); return ZM_OK;
    } else if (cmd==0x73) { exit_req=1; ok=1; core->signal_give(wake); }
    else if (cmd==0x74) {
        if (n==6 && state==2) {
            core->mutex_lock(lock); memcpy(evo_time,p,6); rtc_pending=1; core->mutex_unlock(lock);
            ok=1; core->signal_give(wake);
        }
    } else if (cmd!=0x75) return ZM_ERROR;
    core->reply(cmd|0x80,&ok,1); return ZM_OK;
}
static int start(const zm_core_t *c) {
    if (c->abi_major!=1 || c->abi_minor<7 ||
        c->size<offsetof(zm_core_t,sd_read)+sizeof c->sd_read ||
        !c->sd_read || !c->core_load || !c->core_back || !c->link_read || !c->link_write) return ZM_ERROR;
    core=c; lock=c->mutex_create(); link_lock=c->mutex_create(); wake=c->signal_create();
    if (!lock || !link_lock || !wake) return ZM_NO_MEMORY;
    state=0; live=0; exit_req=0; running=1; worker_done=0;
    // Ядро 0, ниже звука: ответы Evo идут на ядре 1, а TLS и распаковка заняты секундами.
    int r=c->task_start(worker,NULL,16384u|ZM_TASK_STACK_PSRAM,2,0);
    if (r!=ZM_OK) { running=0; worker_done=1; return r; }
    audio_done=0; r=c->task_start(audio_task,NULL,4096,4,0);
    if (r!=ZM_OK) { audio_done=1; running=0; c->signal_give(wake); }
    return r;
}
static void stop(void) {
    running=0; exit_req=1; if (wake) core->signal_give(wake);
    for (unsigned i=0;i<1500 && (!worker_done || !audio_done);i++) core->delay(10);
}
#ifndef AMIGA_HOST_TEST
int main(int argc,char **argv) {
    static const zm_app_t app={sizeof(zm_app_t),ZM_APP_ABI_MAJOR,ZM_APP_ABI_MINOR,
        "amiga","0.26",0x6E,0x7A,start,stop,command,NULL,NULL,NULL};   /* 0.24: разделы NET; 0.25: SETUP, режим совместимости; 0.26: смена режима — перезапуск сеанса */
    if (argc!=3) return 0;
    *(const zm_app_t **)argv[2]=&app;
    return ZM_APP_ABI_MAJOR;
}
#endif
