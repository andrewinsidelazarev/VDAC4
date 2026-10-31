/* SPDX-License-Identifier: GPL-3.0-or-later
 * AMIGA.ELF bring-up: Kickstart from Tang SD, Evo input, stereo via ESP DAC.
 * Uses the existing WC core START/INPUT/STATUS/EXIT protocol; optional read-only DF0.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "zm_app.h"
#include "ps2_hid.h"

#define ROM_PATH "/AMIGA/KICK31.ROM"
#define ROM_BYTES 524288u
#define ADF_PATH "/AMIGA/DF0.ADF"
#define ADF_BYTES 901120u
#define VERSION 0x05010001u
static const zm_core_t *core;
static void *lock, *link_lock, *wake;
static volatile int running, state, exit_req, live;
static volatile int worker_done=1, audio_done=1;
static uint32_t core_version, input_ms, generation;
static char error_text[80];
static uint8_t ring[256], head, tail, lost, joy, buttons, last_x, last_y, mouse_base;
static int32_t mouse_x, mouse_y;

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
static int mount_disk(const uint8_t *disk) {
    uint32_t signature=0;
    if (!rd(0x198,&signature) || signature!=0x41444631u) return 0;
    return wr(0x180,disk?ADF_BYTES:0);
}
static int disk_step(const uint8_t *disk) {
    uint32_t status=0, sector=0, token=0;
    if (!rd(0x184,&status) || (status&4)) return 0;
    if (!(status&1)) return 1;
    if (!disk || (status&8) || ((status>>8)&255)!=1 ||
        !rd(0x188,&sector) || !rd(0x18c,&token) || sector>=ADF_BYTES/512) return 0;
    const uint8_t *p=disk+(size_t)sector*512;
    for (unsigned part=0;part<8;part++) {
        if (!running || exit_req) return 0;
        uint32_t w[16];
        for (unsigned i=0;i<16;i++) w[i]=little32(p+part*64+i*4);
        if (core->link_write(0x5000+part*64,w,16)!=ZM_OK) return 0;
    }
    return wr(0x190,token);
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
    uint32_t wanted=crc32(p,ROM_BYTES), count=0, crc=0, s=0;
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
    if (!rd(0x4c,&count) || !rd(0x50,&crc) || count!=ROM_BYTES || crc!=wanted) return 0;
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
    uint8_t bytes[64], j,b; unsigned n=0; int clear;
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
        if (ev==PS2_HID_CLEAR) { if (!wr(0x1c,0)) return 0; }
        else if ((ev&127)==61 && (f4_down ||
                 (!(ev&128) && !down[61] && (down[104] || down[108]) && (down[106] || down[110])))) {
            // Ctrl+Alt+F4 resets once. Plain F4 belongs to the guest/Test Kit.
            if (!(ev&128) && !f4_down && !wr(0x44,3)) return 0;
            f4_down=(ev&128)==0;
        } else {
            if (!rd(0x0c,&free_slots)) return 0;
            if (!free_slots) break;
            uint8_t k=(uint8_t)ev;
            if (!hid_packet(1,&k,1)) return 0;
            down[k&127]=(k&128)==0;
        }
        ++at;
    }
    // NanoMig expects USB mouse coordinates; Kempston Y grows upward.
    uint8_t mouse[3]={b,(uint8_t)dx,(uint8_t)-dy};
    if (!hid_packet(2,mouse,3)) return 0;
    core->mutex_lock(lock); mouse_x-=dx; mouse_y-=dy; core->mutex_unlock(lock);
    // USB/NanoMig order: R,L,D,U,fire1,fire2. Amiga game port is port 1.
    uint8_t js[2]={1,(uint8_t)((j&31)|((j>>1)&32))};
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
static void audio_task(void *arg) {
    (void)arg;
    while (running) {
        if (!live) { core->delay(10); continue; }
        uint32_t gen=generation, wp=0, last=core->millis();
        zm_pcm_format_t fmt={48000,2};
        if (core->pcm_open(&fmt)!=ZM_OK) { core->delay(20); continue; }
        audio_read(0x20,&wp,1);
        uint32_t rp=gray_decode(wp&1023);
        while (running && live && generation==gen) {
            if (!audio_read(0x20,&wp,1)) break;
            wp=gray_decode(wp&1023);
            uint32_t now=core->millis(), have=(wp-rp)&1023;
            if (now-last>=18 || have>768) { rp=(wp-192)&1023; have=192; }
            last=now;
            if (have>384) have=384;
            while (have && running && live && generation==gen) {
                uint32_t words[16]; float frames[32];
                unsigned n=have>16?16:have; if (n>1024-rp) n=1024-rp;
                if (!audio_read(0x6000+rp*4,words,n)) break;
                for (unsigned i=0;i<n;i++) {
                    frames[2*i]=(float)(int16_t)words[i]/32768.0f;
                    frames[2*i+1]=(float)(int16_t)(words[i]>>16)/32768.0f;
                }
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
static void worker(void *arg) {
    (void)arg;
    while (running) {
        core->signal_wait(wake,20);
        if (!running || state!=1) continue;
        uint8_t *rom=NULL, *disk=NULL; size_t size=0; int loaded=0, ok=0;
        if (core->sd_read(ROM_PATH,&rom,&size,ROM_BYTES,&running)!=ZM_OK)
            fail("Place 512 KiB Kickstart at /AMIGA/KICK31.ROM on Tang SD");
        else if (!valid_rom(rom,size)) fail("Kickstart size or Amiga checksum is invalid");
        else if (!load_disk(&disk)) fail("DF0.ADF read failed: use a standard 880 KiB ADF");
        else if (!running || exit_req) fail("Start cancelled");
        else {
            loaded=1; // core_load can fail after FPGA has changed: always attempt recovery.
            if (core->core_load("AMIGA",&core_version)!=ZM_OK) fail("AMIGA.BIN load failed");
            else if (core_version!=VERSION) fail("AMIGA.BIN / AMIGA.ELF version mismatch");
            else if (!mount_disk(disk)) fail("ADF bridge unavailable; update AMIGA.BIN");
            else if (!upload_rom(rom)) fail("Kickstart LINK transfer or CRC failed");
            else ok=1;
        }
        if (rom) core->free(rom);
        if (ok && running && !exit_req) {
            ps2_hid_t parser; ps2_hid_init(&parser);
            core->mutex_lock(lock);
            input_ms=core->millis(); mouse_base=1; lost=1;
            core->mutex_unlock(lock);
            core->mutex_lock(link_lock); ++generation; live=1; core->mutex_unlock(link_lock);
            state=2;
            while (running && !exit_req) {
                if (core->millis()-input_ms>1500) break;
                if (!input_step(&parser)) { fail("Amiga input LINK failed"); ok=0; break; }
                if (!disk_step(disk)) { fail("DF0 sector transfer failed"); ok=0; break; }
                core->signal_wait(wake,4);
            }
        }
        core->mutex_lock(link_lock); live=0; core->mutex_unlock(link_lock);
        if (disk) core->free(disk);
        if (loaded && core->core_back(10000)!=ZM_OK) { fail("Could not return to TS-Config"); ok=0; }
        state=ok?3:255;
    }
    worker_done=1;
}
static int command(uint8_t cmd,const uint8_t *p,uint16_t n) {
    uint8_t ok=0;
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
            core->mutex_unlock(lock); ok=1; core->signal_give(wake);
        }
    } else if (cmd==0x72) {
        uint8_t out[5+sizeof error_text];
        out[0]=(uint8_t)state; put32(out+1,core_version);
        size_t len=strlen(error_text); memcpy(out+5,error_text,len+1);
        core->reply(0xf2,out,(uint16_t)(6+len)); return ZM_OK;
    } else if (cmd==0x73) { exit_req=1; ok=1; core->signal_give(wake); }
    else if (cmd!=0x74 && cmd!=0x75) return ZM_ERROR;
    core->reply(cmd|0x80,&ok,1); return ZM_OK;
}
static int start(const zm_core_t *c) {
    if (c->abi_major!=1 || c->abi_minor<7 ||
        c->size<offsetof(zm_core_t,sd_read)+sizeof c->sd_read ||
        !c->sd_read || !c->core_load || !c->core_back || !c->link_read || !c->link_write) return ZM_ERROR;
    core=c; lock=c->mutex_create(); link_lock=c->mutex_create(); wake=c->signal_create();
    if (!lock || !link_lock || !wake) return ZM_NO_MEMORY;
    state=0; live=0; exit_req=0; running=1; worker_done=0;
    int r=c->task_start(worker,NULL,8192,3,1);
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
        "amiga","0.4",0x70,0x75,start,stop,command,NULL,NULL,NULL};
    if (argc!=3) return 0;
    *(const zm_app_t **)argv[2]=&app;
    return ZM_APP_ABI_MAJOR;
}
#endif
