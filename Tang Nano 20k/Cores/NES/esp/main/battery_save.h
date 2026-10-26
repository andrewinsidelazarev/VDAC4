/* SPDX-License-Identifier: Apache-2.0
 * Память картриджа сохраняется до BOOT, файловая запись — после BOOT.
 * Формат .sav: заголовок 512 байтов, затем RAM с дополнением до 512 байтов.
 * CRC32 ROM связывает сохранение с игрой; CRC32 заголовка и RAM проверяются.
 */
#define SAVE_HEADER 512u
static uint8_t *save_data;
static size_t save_size, save_ram_size, save_received;
static uint32_t save_rom_crc, save_offset;
static uint16_t save_seq;
static uint32_t save_mode_bit = 1;   /* бит 68 «мост ваш»: 1 — удержание NES, 8 — снимок на паузе */
static int save_prepared, save_restored, save_captured, save_returned, save_is_sd;
static char save_path[512];

static void save_discard(void)
{
    if (save_data) core->free(save_data);
    save_data = NULL; save_size = save_ram_size = save_received = 0;
    save_prepared = save_restored = save_captured = save_returned = 0;
}

static int save_sd_available(void)
{
    return core->size >= offsetof(zm_core_t, sd_write) + sizeof core->sd_write &&
           core->sd_read && core->sd_write;
}

static void save_header(void)
{
    memset(save_data, 0, SAVE_HEADER);
    memcpy(save_data, "NVD4SAV1", 8);
    wr32(save_data+8, (uint32_t)save_ram_size);
    wr32(save_data+12, save_rom_crc);
    wr32(save_data+16, save_offset);
    wr32(save_data+20, rom_crc(save_data+SAVE_HEADER, save_size-SAVE_HEADER));
    wr32(save_data+24, rom_crc(save_data,24));
}

static int save_valid(const uint8_t *p, size_t n)
{
    return p && n==save_size && n>=SAVE_HEADER && !memcmp(p,"NVD4SAV1",8) &&
        rom_u32(p+8)==save_ram_size && rom_u32(p+12)==save_rom_crc &&
        rom_u32(p+16)==save_offset && rom_u32(p+24)==rom_crc(p,24) &&
        rom_u32(p+20)==rom_crc(p+SAVE_HEADER,n-SAVE_HEADER);
}

static int save_prepare(const uint8_t *rom, size_t size, int from_sd)
{
    save_discard(); save_is_sd=from_sd; save_seq=0; save_offset=0;
    if (!(rom[6]&2)) { save_prepared=save_restored=1; return 1; }
    const int nes20=(rom[7]&12)==8;
    unsigned mapper=(rom[6]>>4)|(rom[7]&0xf0);
    if (nes20) mapper|=(rom[8]&15u)<<8;
    else {
        unsigned dirty=rom[9]&0xfe;
        for(unsigned i=10;i<16;i++) dirty|=rom[i];
        if(dirty) mapper&=15;
    }
    // Эти мапперы хранят часть данных внутри собственного RTL/flash. Не выдавать
    // снимок одной SDRAM за полное сохранение такого картриджа.
    if (mapper==19 || mapper==30 || mapper==111) {
        fail("This mapper's battery storage is unsupported"); return 0;
    }
    if (nes20 && ((rom[11]>>4) || ((rom[10]&15) && (rom[10]>>4)))) {
        fail("Mixed / CHR nonvolatile RAM unsupported"); return 0;
    }
    if (mapper==16 || mapper==159) {
        save_ram_size=mapper==159 ? 128 : 256; save_offset=0x40000;
    } else if (nes20) {
        const unsigned shift=rom[10]>>4;
        if (!shift || shift>12) { fail("Unsupported NES 2.0 save RAM size"); return 0; }
        save_ram_size=64u<<shift;
    } else {
        save_ram_size=rom[8] ? (size_t)rom[8]*8192u : mapper==5 ? 65536u : 8192u;
    }
    if (save_ram_size>262144u) { fail("Save RAM exceeds 256 KiB"); return 0; }
    if (from_sd && !save_sd_available()) { fail("Battery saves need ESP 0.1.94+"); return 0; }
    save_size=SAVE_HEADER+((save_ram_size+511u)&~511u);
    save_data=core->alloc(save_size,ZM_MEM_PSRAM);
    if(!save_data) { save_size=0; fail("No PSRAM for battery save"); return 0; }
    memset(save_data,0,save_size);
    if(save_offset) memset(save_data+SAVE_HEADER,0xff,save_ram_size);
    save_rom_crc=rom_crc(rom,size); save_header(); save_prepared=1;
    if(from_sd) {
        const size_t n=strlen(rom_path);
        if(n<4 || n>=sizeof save_path) { fail("Invalid save path"); return 0; }
        memcpy(save_path,rom_path,n+1); memcpy(save_path+n-3,"sav",3);
        uint8_t *data=NULL; size_t length=0;
        int r=core->sd_read(save_path,&data,&length,save_size,&running);
        if(r==ZM_NOT_FOUND && n+4<sizeof save_path) {
            char backup[512]; memcpy(backup,save_path,n); memcpy(backup+n,".bak",5);
            r=core->sd_read(backup,&data,&length,save_size,&running);
        }
        if(r==ZM_NOT_FOUND) {
            char backup[512]; memcpy(backup,save_path,n+1); memcpy(backup+n-3,"svb",3);
            r=core->sd_read(backup,&data,&length,save_size,&running);
        }
        if(r!=ZM_NOT_FOUND && (r!=ZM_OK || !save_valid(data,length))) {
            if(data) core->free(data);
            fail("Save unreadable or wrong game/CRC; kept intact"); return 0;
        }
        if(r==ZM_OK) { memcpy(save_data,data,length); core->free(data); }
        save_restored=1;
    }
    return 1;
}

static int save_block(uint32_t offset, uint8_t *data, int write)
{
    uint32_t words[64], s=0, accepted=0, crc=0;
    if(write) {
        for(unsigned i=0;i<64;i++) words[i]=rom_u32(data+4*i);
        for(unsigned i=0;i<64;i+=16)
            if(core->link_write(0x5000+4*i,words+i,16)!=ZM_OK) return 0;
    }
    if(link_wr(0x5c,offset)!=ZM_OK) return 0;
    const uint16_t seq=save_seq+1;
    if(link_wr(0x60,((uint32_t)seq<<16)|(write?0x8000:0)|256)!=ZM_OK) return 0;
    const uint32_t started=core->millis();
    do {
        core->delay(1);
        if(link_rd(0x68,&s)!=ZM_OK || link_rd(0x60,&accepted)!=ZM_OK) continue;
        if(!(s&2) && accepted==seq) break;
    } while(running && core->millis()-started<1000);
    if((s&6) || !(s&save_mode_bit) || accepted!=seq) return 0;
    save_seq=seq;
    if(!write) {
        for(unsigned i=0;i<64;i+=16)
            if(core->link_read(0x5000+4*i,words+i,16)!=ZM_OK) return 0;
        for(unsigned i=0;i<64;i++) wr32(data+4*i,words[i]);
    }
    return link_rd(0x64,&crc)==ZM_OK && crc==rom_crc(data,256);
}

static int save_restore(void)
{
    if(!save_size) return 1;
    if(!save_restored || core_version<0x04010004 || link_wr(0x58,1)!=ZM_OK) return 0;
    core->delay(1);
    uint8_t verify[256];
    const size_t length=(save_ram_size+255u)&~255u;
    for(size_t at=0; at<length && running; at+=256) {
        uint8_t *data=save_data+SAVE_HEADER+at;
        if(!save_block(save_offset+(uint32_t)at,data,1) ||
           !save_block(save_offset+(uint32_t)at,verify,0) || memcmp(data,verify,256)) return 0;
    }
    return running && link_wr(0x58,0)==ZM_OK;
}

static int save_capture(void)
{
    if(!save_size || save_captured) return 1;
    if(link_wr(0x58,1)!=ZM_OK) return 0;
    core->delay(1);
    const size_t length=(save_ram_size+255u)&~255u;
    for(size_t at=0; at<length && running; at+=256)
        if(!save_block(save_offset+(uint32_t)at,save_data+SAVE_HEADER+at,0)) return 0;
    if(!running) return 0;
    save_header(); save_captured=1;
    return 1;
}

static void save_write_sd(void)
{
    if(core->sd_write(save_path,save_data,save_size,&running)==ZM_OK) {
        core->log("nes: battery saved to Tang SD, %u bytes",(unsigned)save_ram_size);
        save_discard(); nes_cfg_finish();
    } else {
        strncpy(error_text,"Save write failed. Enter retries; Esc discards",sizeof error_text-1);
        state=7; // Снимок остаётся в ESP до повторной записи или явного отказа.
    }
}

static int save_command(uint8_t cmd,const uint8_t *p,uint16_t n)
{
    uint8_t reply[520]={0}; size_t length=1;
    if(cmd==0x7b) {
        reply[0]=1; reply[1]=(uint8_t)save_is_sd;
        wr32(reply+2,(uint32_t)save_size); wr32(reply+6,save_rom_crc); length=10;
    } else if(cmd==0x7c && state==0 && save_prepared && !save_is_sd && n) {
        if(p[0]==0 && n==5 && rom_u32(p+1)==save_size) { save_received=0; reply[0]=1; }
        else if(p[0]==1 && n>7 && n<=519) {
            const size_t at=rom_u32(p+1), count=n-7;
            if(at<=save_received && at<=save_size && count<=save_size-at &&
               rom_crc16(p+1,n-3)==((uint16_t)p[n-2]|(uint16_t)p[n-1]<<8)) {
                if(at==save_received) { memcpy(save_data+at,p+5,count); save_received+=count; reply[0]=1; }
                else if(count<=save_received-at && !memcmp(save_data+at,p+5,count)) reply[0]=1;
            }
        } else if(p[0]==2 && n==1 && save_received==save_size && save_valid(save_data,save_size)) {
            save_restored=1; reply[0]=1;
        } else if(p[0]==3 && n==1 && !save_received) { save_restored=1; reply[0]=1; }
        wr32(reply+1,(uint32_t)save_received); length=5;
    } else if(cmd==0x7d && state==6 && save_captured && n==4) {
        const uint32_t at=rom_u32(p);
        if(at<save_size && !(at&511)) {
            reply[0]=1; wr32(reply+1,at); memcpy(reply+5,save_data+at,512);
            const uint16_t crc=rom_crc16(reply+1,516);
            reply[517]=(uint8_t)crc; reply[518]=(uint8_t)(crc>>8); length=519;
        }
    } else if(cmd==0x7e && n==1 && (state==6 || state==7 || (state==255 && save_size))) {
        if(p[0]==0 || (p[0]==1 && state==6)) { save_discard(); nes_cfg_schedule(); reply[0]=1; }
        else if(p[0]==2 && state==7 && save_captured && save_is_sd && save_returned) {
            error_text[0]=0; state=5; reply[0]=1; core->signal_give(wake);
        }
    }
    return core->reply(cmd|0x80,reply,(uint16_t)length);
}
