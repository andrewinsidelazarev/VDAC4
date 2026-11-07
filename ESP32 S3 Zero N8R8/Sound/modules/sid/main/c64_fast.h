/* Шина и таймеры для регистрового ядра 6502.
 * CPU сохраняет семантику cRSID 1.58 (Hermit), включая нелегальные команды и
 * чтения с побочными действиями. CIA дополнен каскадом A -> B (1.6).
 * Эталон в components/crsid не изменён. */

/* Большинство обращений — RAM ниже $A000. Остальная шина вынесена в одно место:
 * её размножение в каждом операнде раздувало cRSID_emulateCPU за пределы I-cache. */
/* RSID: две страницы CIA во внутренней памяти. Прямые таблицы C64 остаются
 * прежними; процессор перенаправляет только I/O при включённой банковке. */
typedef struct { uint8_t rd[512], wr[512]; } FastCiaIo;
static FastCiaIo* fast_cia_io;
typedef struct {
  cRSID_CIAinstance* owner;
  uint32_t version;
  uint16_t key;
  uint8_t valid, phase;
  uint16_t step[10][16];
} FastCiaPlan;
static FastCiaPlan* fast_cia_plan;
static uint32_t fast_cia_version[2];

static INLINE void fast_cia_changed(uint16_t address) {
  const unsigned reg = address & 255;
  if ((address & 0xFE00) == 0xDC00 && (cRSID_C64.RAMbank[1] & 3) &&
      ((reg >= 4 && reg <= 7) || reg == 14 || reg == 15)) {
    ++fast_cia_version[(address >> 8) & 1];
    if (fast_cia_plan) fast_cia_plan->valid = 0;
  }
}

static __attribute__((noinline)) uint8_t fast_read_io(uint16_t address) {
  /* These ranges cannot have read side effects. Resolve their current bank
   * here without the general PLA pointer table, keeping this code outlined. */
  if (address >= 0xE000)
    return (cRSID_C64.RAMbank[1]&2) ? cRSID_C64.ROMbanks[address] : cRSID_C64.RAMbank[address];
  if (address >= 0xC000 && address < 0xD000) return cRSID_C64.RAMbank[address];
  if (fast_cia_io != NULL && (address & 0xFE00) == 0xDC00 && (cRSID_C64.RAMbank[1] & 3)) {
    uint8_t value = fast_cia_io->rd[address & 0x1FF];
    if ((address & 0xFF) == 0x0D) cRSID_acknowledgeCIAIRQ(&cRSID_C64.CIA[1 + ((address >> 8) & 1)]);
    return value;
  }
  return rd(address);
}
static INLINE uint8_t fast_read(uint16_t address) {
  if (MOSTLY(address < 0xA000)) return cRSID_C64.RAMbank[address];
  return fast_read_io(address);
}
static __attribute__((noinline)) void fast_write_rmw_io(uint16_t address, uint8_t value) {
  fast_cia_changed(address);
  if (fast_cia_io != NULL && (address & 0xFE00) == 0xDC00 && (cRSID_C64.RAMbank[1] & 3))
    fast_cia_io->wr[address & 0x1FF] = value;
  wr(address, value);
}
static __attribute__((noinline)) void fast_write_store_io(uint16_t address, uint8_t value) {
  fast_cia_changed(address);
  if (fast_cia_io != NULL && (address & 0xFE00) == 0xDC00 && (cRSID_C64.RAMbank[1] & 3))
    fast_cia_io->wr[address & 0x1FF] = value;
  wr2(address, value);
  /* cRSID позволяет исполнять RTI из регистра последовательного порта. */
  if (fast_cia_io != NULL && (address == 0xDC0C || address == 0xDD0C) && (cRSID_C64.RAMbank[1] & 3))
    fast_cia_io->rd[address & 0x1FF] = value;
}
static INLINE void fast_write_rmw(uint16_t address, uint8_t value) {
  if (MOSTLY(address < 0xD000 || address >= 0xE000)) cRSID_C64.RAMbank[address] = value;
  else fast_write_rmw_io(address, value);
}
static INLINE void fast_write_store(uint16_t address, uint8_t value) {
  if (MOSTLY(address < 0xD000 || address >= 0xE000)) cRSID_C64.RAMbank[address] = value;
  else fast_write_store_io(address, value);
}

#include "cpu_generated.inc"

/* CIA: каждый управляющий регистр читается один раз. Запись строба нужна лишь
 * при его сбросе; счётчик сохраняет ровно ту же арифметику и момент IRQ, что cRSID.
 * Не объединять инструкции 6502: чтение/запись CIA между ними наблюдаемо. */
static INLINE void fast_timer(uint8_t* wrp, uint8_t* rdp, unsigned timer,
                              unsigned control, unsigned mask, unsigned irq, int cycles) {
  uint8_t ctrl = wrp[control];
  if (ctrl & 0x10) {
    rdp[timer] = wrp[timer];
    rdp[timer + 1] = wrp[timer + 1];
    ctrl &= (uint8_t)~0x10;
    wrp[control] = ctrl;
  } else if ((ctrl & mask) == 1) {
    int count = ((int)rdp[timer + 1] << 8 | rdp[timer]) - cycles;
    if (RARELY(count < 0)) {
      count += ((int)wrp[timer + 1] << 8 | wrp[timer]) + 1;
      if (ctrl & 8) {
        ctrl &= (uint8_t)~1;
        wrp[control] = ctrl;
      }
      rdp[13] |= (uint8_t)(irq | ((wrp[13] & irq) ? 0x80 : 0));
    }
    rdp[timer + 1] = (uint8_t)(count >> 8);
    rdp[timer] = (uint8_t)count;
  }
  rdp[control] = ctrl;
}

/* CIA в каскаде: B считает переполнения A. Период A бывает короче команды 6502
 * (Stereophonik: latch=2), поэтому учитываем все переполнения за команду.
 * Обычный режим оставлен на прежнем быстром пути ниже. */
static INLINE unsigned fast_timer_ticks(uint8_t* w, uint8_t* r, unsigned timer,
                                       unsigned control, unsigned irq, unsigned ticks) {
  /* Cached quotient/remainder for short CIA periods and one CPU instruction.
   * Entry = (quotient << 4) | remainder. No division in the frequent path. */
  static const uint8_t divmod_small[9][9] = {
    {0,16,32,48,64,80,96,112,128},
    {0,1,16,17,32,33,48,49,64},
    {0,1,2,16,17,18,32,33,34},
    {0,1,2,3,16,17,18,19,32},
    {0,1,2,3,4,16,17,18,19},
    {0,1,2,3,4,5,16,17,18},
    {0,1,2,3,4,5,6,16,17},
    {0,1,2,3,4,5,6,7,16},
    {0,1,2,3,4,5,6,7,8},
  };
  uint8_t ctrl = w[control];
  unsigned events = 0;
  if (ctrl & 0x10) {
    r[timer] = w[timer]; r[timer + 1] = w[timer + 1];
    ctrl &= (uint8_t)~0x10;
    w[control] = ctrl;
  } else if ((ctrl & 1) && ticks) {
    unsigned count = (unsigned)r[timer + 1] * 256u + r[timer];
    if (ticks <= count) count -= ticks;
    else {
      const unsigned latch = (unsigned)w[timer + 1] * 256u + w[timer];
      unsigned remaining = ticks - count - 1u;
      events = 1;
      if (ctrl & 8) {
        ctrl &= (uint8_t)~1;
        w[control] = ctrl;
        count = latch;
      }
      else {
        if (remaining > latch) {
          if (latch < 9 && remaining < 9) {
            const unsigned qr = divmod_small[latch][remaining];
            events += qr >> 4; remaining = qr & 15;
          } else {
            events += remaining / (latch + 1u);
            remaining %= latch + 1u;
          }
        }
        count = latch - remaining;
      }
      r[13] |= (uint8_t)(irq | ((w[13] & irq) ? 0x80 : 0));
    }
    r[timer] = (uint8_t)count; r[timer + 1] = (uint8_t)(count >> 8);
  }
  r[control] = ctrl;
  return events;
}
/* Return -1 when the generic model is needed. A table is compiled only for a
 * continuous Phi2 A -> B cascade whose combined period has at most 64 states.
 * CPU writes to timer latches/control invalidate it; IRQ acknowledgement and
 * mask updates use the current read/write bytes on every lookup. */
static __attribute__((noinline)) int fast_cia_compile(FastCiaPlan* p, cRSID_CIAinstance* cia, unsigned index) {
  uint8_t* w=cia->BasePtrWR; uint8_t* r=cia->BasePtrRD;
  if ((w[14]&0x39)!=1 || (w[15]&0x60)!=0x40 || w[5] || r[5]) return 0;
  const unsigned period=(unsigned)w[4]+1;
  if (period>16 || r[4]>=period) return 0;
  if (!p->owner || p->key!=w[4]) {
    for(unsigned count=0;count<period;++count)for(unsigned ticks=0;ticks<=9;++ticks) {
      unsigned next=count,events=0;
      if(ticks<=count)next-=ticks;
      else {unsigned left=ticks-count-1;events=1+left/period;next=period-1-left%period;}
      p->step[ticks][count]=(uint16_t)(next|(events<<8));
    }
  }
  p->owner=cia;p->key=w[4];p->phase=r[4];p->version=fast_cia_version[index];p->valid=1;
  return 1;
}

/* Cache A alone; B may have any 16-bit period, including the 45600 latch used
 * by Stereophonik. B retains force-load, one-shot and IRQ-mask semantics. */
static INLINE int fast_cia_planned(cRSID_CIAinstance* cia,unsigned cycles) {
  FastCiaPlan* p=fast_cia_plan;
  if(!p||cycles>9||(p->owner&&p->owner!=cia))return -1;
  uint8_t* w=cia->BasePtrWR;uint8_t* r=cia->BasePtrRD;
  const unsigned index=cia->BaseAddress==0xDD00;
  if(RARELY(!p->valid||p->version!=fast_cia_version[index])) {
    if(!fast_cia_compile(p,cia,index))return -1;
  }
  const unsigned value=p->step[cycles][p->phase];
  p->phase=(uint8_t)value;r[4]=p->phase;r[5]=0;r[14]=w[14];
  const unsigned events=value>>8;
  if(events)r[13]|=(uint8_t)(1|((w[13]&1)?0x80:0));
  fast_timer_ticks(w,r,6,15,2,events);
  return r[13]&0x80;
}

static INLINE unsigned fast_cia(cRSID_CIAinstance* cia, int cycles) {
  uint8_t* w = cia->BasePtrWR;
  uint8_t* r = cia->BasePtrRD;

  if (RARELY((w[15] & 0x60) == 0x40)) {
    const int cached=fast_cia_planned(cia,(unsigned)cycles);
    if(cached>=0)return (unsigned)cached;
    const unsigned a = fast_timer_ticks(w, r, 4, 14, 1, (w[14] & 0x20) ? 0 : (unsigned)cycles);
    fast_timer_ticks(w, r, 6, 15, 2, a);
    return r[13] & 0x80;
  }
  fast_timer(w, r, 4, 14, 0x21, 1, cycles);
  fast_timer(w, r, 6, 15, 0x41, 2, cycles);
  return r[13] & 0x80;
}

/* VIC: короткий путь одного шага — сложение и сравнение; адреса регистров и
 * параметры растра уже находятся в горячей копии структуры. */
static INLINE unsigned fast_vic(cRSID_VICinstance* vic, unsigned cycles) {
  unsigned row_cycle = (uint8_t)(vic->RowCycleCnt + cycles);
  uint8_t* r = vic->BasePtrRD;
  if (RARELY(row_cycle >= vic->RasterRowCycles)) {
    row_cycle -= vic->RasterRowCycles;
    unsigned row = ((r[0x11] & 0x80) << 1) + r[0x12] + 1;
    if (row >= vic->RasterLines) row = 0;
    r[0x11] = (uint8_t)((r[0x11] & 0x7F) | ((row & 0x100) >> 1));
    r[0x12] = (uint8_t)row;
    const uint8_t* w = vic->BasePtrWR;
    if ((w[0x1A] & 1) && row == ((unsigned)(w[0x11] & 0x80) << 1) + w[0x12]) r[0x19] |= 0x81;
  }
  vic->RowCycleCnt = (uint8_t)row_cycle;
  return r[0x19] & 0x80;
}
