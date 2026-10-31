/* SPDX-License-Identifier: GPL-3.0-or-later
 * Новые образы для меню F11: пустая отформатированная дискета ADF и диск HDF с разделом DH0.
 * Здесь только содержимое блоков, запись на карту — в disk_menu.h. Числа в образах — big-endian.
 * Том AmigaDOS: загрузочные блоки 0 и 1, корень посередине, за ним блоки карты занятости: бит 1 —
 * блок свободен, нулевой бит первого слова — блок 2. Диск: RDB (ядро берёт из него геометрию,
 * nanomig.v), раздел DH0 на всё после RDB; файловая система FFS есть в Kickstart. */
#define FS_BLOCK 512u
#define FS_RESERVED 2u
#define FS_MAP_BITS (127u*32u)
#define FS_ROOT_MAPS 25u
#define FS_EXT_MAPS 127u
#define FS_ROOT_MAPS_AT 79u
#define FS_DOS0 0x444F5300u
#define FS_DOS1 0x444F5301u
#define ADF_BLOCKS 1760u
#define HDF_HEADS 16u
#define HDF_SECS 32u
#define HDF_CYL_SECTORS (HDF_HEADS*HDF_SECS)
#define HDF_RDB_CYLS 2u
#define HDF_START (HDF_RDB_CYLS*HDF_CYL_SECTORS)
#define HDF_MIN_CYLS (HDF_RDB_CYLS+1u)

typedef struct { uint32_t days, mins, ticks; } fs_stamp_t;
typedef struct {
    uint32_t blocks, root, maps, exts, dostype;
    const char *name;
    fs_stamp_t when;
} fs_layout_t;

static void be32(uint8_t *p,uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static uint32_t be32_get(const uint8_t *p) {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
/* Сумма всех слов блока с контрольным становится нулём. */
static void fs_checksum(uint8_t *block,unsigned at,unsigned words) {
    uint32_t sum=0;
    be32(block+at*4,0);
    for (unsigned i=0;i<words;i++) sum+=be32_get(block+i*4);
    be32(block+at*4,0u-sum);
}
/* Время Evo (сек, мин, час, число, месяц, год от 2000) в дни от 1.1.1978, минуты, тики 1/50 с. */
static fs_stamp_t fs_stamp(const uint8_t *t) {
    static const uint16_t before[12]={0,31,59,90,120,151,181,212,243,273,304,334};
    fs_stamp_t s={0,0,0};
    if (t[4]<1 || t[4]>12 || t[3]<1 || t[3]>31) return s;
    unsigned year=2000u+t[5];
    s.days=(year-1978u)*365u+before[t[4]-1]+t[3]-1u;
    for (unsigned y=1980;y<year;y+=4) s.days++;
    if (year%4==0 && t[4]>2) s.days++;
    s.mins=t[2]*60u+t[1];
    s.ticks=t[0]*50u;
    return s;
}
static void fs_layout(fs_layout_t *l,uint32_t blocks,uint32_t dostype,const char *name,fs_stamp_t when) {
    uint32_t bits=blocks-FS_RESERVED;
    l->blocks=blocks; l->dostype=dostype; l->name=name; l->when=when;
    l->root=(blocks-1u+FS_RESERVED)/2u;
    l->maps=(bits+FS_MAP_BITS-1u)/FS_MAP_BITS;
    l->exts=l->maps>FS_ROOT_MAPS?(l->maps-FS_ROOT_MAPS+FS_EXT_MAPS-1u)/FS_EXT_MAPS:0;
}
static uint32_t fs_last(const fs_layout_t *l) { return l->root+l->maps+l->exts; }
static void fs_root_block(const fs_layout_t *l,uint8_t *b) {
    be32(b,2);                          /* T_HEADER */
    be32(b+12,72);                      /* таблица хешей каталога */
    be32(b+78*4,0xFFFFFFFFu);           /* карта занятости верна */
    for (uint32_t i=0;i<FS_ROOT_MAPS && i<l->maps;i++) be32(b+(FS_ROOT_MAPS_AT+i)*4,l->root+1u+i);
    if (l->exts) be32(b+104*4,l->root+l->maps+1u);
    const fs_stamp_t *w=&l->when;
    static const unsigned at[3]={105,118,121};  /* корень изменён, том изменён, том создан */
    for (unsigned i=0;i<3;i++) { be32(b+at[i]*4,w->days); be32(b+(at[i]+1)*4,w->mins); be32(b+(at[i]+2)*4,w->ticks); }
    size_t n=strlen(l->name);
    if (n>30) n=30;
    b[0x1B0]=(uint8_t)n;
    memcpy(b+0x1B1,l->name,n);
    be32(b+127*4,1);                    /* ST_ROOT */
    fs_checksum(b,5,128);
}
static void fs_map_block(const fs_layout_t *l,uint32_t index,uint8_t *b) {
    uint32_t used=fs_last(l);
    uint32_t first=FS_RESERVED+index*FS_MAP_BITS;
    for (unsigned k=0;k<127;k++) {
        uint32_t word=0;
        for (unsigned bit=0;bit<32;bit++) {
            uint32_t n=first+k*32u+bit;
            if (n<l->blocks && (n<l->root || n>used)) word|=1u<<bit;
        }
        be32(b+4+k*4,word);
    }
    fs_checksum(b,0,128);
}
static void fs_ext_block(const fs_layout_t *l,uint32_t index,uint8_t *b) {
    uint32_t map=FS_ROOT_MAPS+index*FS_EXT_MAPS;
    for (unsigned k=0;k<FS_EXT_MAPS && map+k<l->maps;k++) be32(b+k*4,l->root+1u+map+k);
    if (index+1u<l->exts) be32(b+127*4,l->root+l->maps+1u+index+1u);
}
/* Блок n тома; 0 — блок пустой, писать его не нужно. */
static int fs_make_block(const fs_layout_t *l,uint32_t n,uint8_t *b) {
    memset(b,0,FS_BLOCK);
    if (n==0) { be32(b,l->dostype); return 1; }     /* контрольная сумма неверна — не загрузочная */
    if (n<l->root || n>fs_last(l)) return 0;
    if (n==l->root) fs_root_block(l,b);
    else if (n<=l->root+l->maps) fs_map_block(l,n-l->root-1u,b);
    else fs_ext_block(l,n-l->root-l->maps-1u,b);
    return 1;
}
static void hdf_rdb_block(uint32_t cylinders,uint8_t *b) {
    static const uint8_t id[28]={'V','D','A','C','4',' ',' ',' ',
        'A','M','I','G','A',' ','H','D','F',' ',' ',' ',' ',' ',' ',' ','1','.','0',' '};
    memset(b,0,FS_BLOCK);
    memcpy(b,"RDSK",4);
    be32(b+1*4,64); be32(b+3*4,7); be32(b+4*4,FS_BLOCK);
    be32(b+5*4,7);                      /* последний диск, LUN и ID */
    be32(b+6*4,0xFFFFFFFFu);            /* нет сбойных блоков */
    be32(b+7*4,1);                      /* раздел — в блоке 1 */
    for (unsigned i=8;i<16;i++) be32(b+i*4,0xFFFFFFFFu);
    be32(b+16*4,cylinders); be32(b+17*4,HDF_SECS); be32(b+18*4,HDF_HEADS);
    be32(b+19*4,1);
    be32(b+20*4,cylinders); be32(b+24*4,cylinders); be32(b+25*4,cylinders);
    be32(b+26*4,3);
    be32(b+33*4,HDF_START-1u);
    be32(b+34*4,HDF_RDB_CYLS); be32(b+35*4,cylinders-1u);
    be32(b+36*4,HDF_CYL_SECTORS);
    be32(b+38*4,1);
    memcpy(b+40*4,id,sizeof id);
    fs_checksum(b,2,64);
}
static void hdf_part_block(uint32_t cylinders,uint8_t *b) {
    memset(b,0,FS_BLOCK);
    memcpy(b,"PART",4);
    be32(b+1*4,64); be32(b+3*4,7);
    be32(b+4*4,0xFFFFFFFFu);            /* раздел один */
    be32(b+5*4,1);                      /* загрузочный */
    b[9*4]=3; memcpy(b+9*4+1,"DH0",3);
    be32(b+32*4,16);                    /* длина таблицы окружения */
    be32(b+33*4,128);
    be32(b+35*4,HDF_HEADS);
    be32(b+36*4,1);
    be32(b+37*4,HDF_SECS);
    be32(b+38*4,FS_RESERVED);
    be32(b+41*4,HDF_RDB_CYLS); be32(b+42*4,cylinders-1u);
    be32(b+43*4,30);                    /* буферов */
    be32(b+45*4,0x1FE00u);              /* за раз не больше 255 секторов: предел команды IDE */
    be32(b+46*4,0x7FFFFFFEu);
    be32(b+48*4,FS_DOS1);
    fs_checksum(b,2,64);
}
