/* Шина и таймеры для регистрового ядра 6502.
 * Семантика cRSID 1.58 (Hermit) сохранена, включая его особенности нелегальных
 * команд и чтений с побочными действиями. Эталон в components/crsid не изменён. */

/* Большинство обращений — RAM ниже $A000. Остальная шина вынесена в одно место:
 * её размножение в каждом операнде раздувало cRSID_emulateCPU за пределы I-cache. */
static __attribute__((noinline)) uint8_t fast_read_io(uint16_t address) {
  return rd(address);
}
static INLINE uint8_t fast_read(uint16_t address) {
  if (MOSTLY(address < 0xA000)) return cRSID_C64.RAMbank[address];
  return fast_read_io(address);
}
static __attribute__((noinline)) void fast_write_rmw_io(uint16_t address, uint8_t value) {
  wr(address, value);
}
static __attribute__((noinline)) void fast_write_store_io(uint16_t address, uint8_t value) {
  wr2(address, value);
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
static INLINE unsigned fast_cia(cRSID_CIAinstance* cia, int cycles) {
  uint8_t* w = cia->BasePtrWR;
  uint8_t* r = cia->BasePtrRD;
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
