/* Дифференциальная проверка 6502/CIA/VIC против неизменённого cRSID.
 * Сверяются все регистры, память, побочные действия I/O и число тактов. */
#define main sid_module_main
#include "../modules/sid/main/sid_source.c"
#undef main

static cRSID_C64instance saved, expected;
static cRSID_Interface iface;
static uint32_t random_state = 0x24c6502u;
static uint32_t rnd(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}

static int check_cpu(void) {
  unsigned cases = 0;
  for (unsigned code = 0; code < 256; ++code) {
    for (unsigned trial = 0; trial < 128; ++trial) {
      cRSID_initCPU(0x200);
      cRSID_C64.CPU.A = rnd() & 255;
      cRSID_C64.CPU.X = rnd() & 255;
      cRSID_C64.CPU.Y = rnd() & 255;
      cRSID_C64.CPU.SP = rnd() & 255;
      cRSID_C64.CPU.ST = rnd() & 255;
      cRSID_C64.CPU.Addr = rnd() & 65535;
      cRSID_C64.RealSIDmode = trial & 1;
      cRSID_C64.RAMbank[1] = trial & 3;
      cRSID_C64.NMI = (trial & 4) ? 128 : 0;
      cRSID_C64.Returned = trial & 8;
      cRSID_C64.RAMbank[0x200] = code;
      /* Включая переходы через страницу и регистры с подтверждением IRQ. */
      static const uint16_t addresses[] = {0x00FF, 0xDC0D, 0xDD0D, 0xD019,
        0xD400, 0xD420, 0xDE00, 0xFFFF, 0xD012, 0xDC05, 0xDD07, 0xEA31};
      unsigned operand = trial < 96 ? addresses[trial % 12] : rnd();
      cRSID_C64.RAMbank[0x201] = operand;
      cRSID_C64.RAMbank[0x202] = operand >> 8;
      cRSID_C64.IObankRD[0xDC0D] = cRSID_C64.IObankRD[0xDD0D] = 0x83;
      saved = cRSID_C64;
      iface = cRSID;
      unsigned want = cRSID_emulateCPU();
      expected = cRSID_C64;
      cRSID_Interface want_iface = cRSID;
      cRSID_C64 = saved;
      cRSID = iface;
      unsigned got = fast_cpu(&cRSID_C64.CPU);
      if (want != got || memcmp(&expected, &cRSID_C64, sizeof(expected)) ||
          memcmp(&want_iface, &cRSID, sizeof(cRSID))) {
        printf("CPU mismatch opcode=%02X case=%u cycles=%u/%u\n", code, trial, want, got);
        return 1;
      }
      ++cases;
    }
  }
  printf("cpu_cases=%u\n", cases);
  return 0;
}

static int check_timers(void) {
  for (unsigned i = 0; i < 100000; ++i) {
    uint8_t* w = cRSID_C64.CIA[1].BasePtrWR;
    uint8_t* r = cRSID_C64.CIA[1].BasePtrRD;
    for (unsigned n = 0; n < 16; ++n) { w[n] = rnd(); r[n] = rnd(); }
    uint8_t old_w[16], old_r[16], want_w[16], want_r[16];
    memcpy(old_w, w, 16); memcpy(old_r, r, 16);
    unsigned cycles = 2 + i % 8;
    unsigned want = (unsigned char)cRSID_emulateCIA(&cRSID_C64.CIA[1], cycles);
    memcpy(want_w, w, 16); memcpy(want_r, r, 16);
    memcpy(w, old_w, 16); memcpy(r, old_r, 16);
    unsigned got = fast_cia(&cRSID_C64.CIA[1], cycles);
    if (want != got || memcmp(w, want_w, 16) || memcmp(r, want_r, 16)) {
      printf("CIA mismatch case=%u\n", i); return 1;
    }
    w = cRSID_C64.VIC.BasePtrWR; r = cRSID_C64.VIC.BasePtrRD;
    for (unsigned n = 0; n < 0x40; ++n) { w[n] = rnd(); r[n] = rnd(); }
    cRSID_C64.VIC.RowCycleCnt = rnd();
    cRSID_VICinstance old_vic = cRSID_C64.VIC;
    uint8_t old_regs[0x40], want_regs[0x40];
    memcpy(old_regs, r, 0x40);
    want = (unsigned char)cRSID_emulateVIC(cycles);
    cRSID_VICinstance want_vic = cRSID_C64.VIC;
    memcpy(want_regs, r, 0x40);
    cRSID_C64.VIC = old_vic;
    memcpy(r, old_regs, 0x40);
    got = fast_vic(&cRSID_C64.VIC, cycles);
    if (want != got || memcmp(&want_vic, &cRSID_C64.VIC, sizeof(want_vic)) || memcmp(r, want_regs, 0x40)) {
      printf("VIC mismatch case=%u\n", i); return 1;
    }
  }
  puts("cia_cases=100000\nvic_cases=100000");
  return 0;
}

int main(void) {
  cRSID_init(RATE, 0);
  for (unsigned i = 0; i < CRSID_MEMBANK_SIZE; ++i) {
    cRSID_C64.RAMbank[i] = rnd();
    cRSID_C64.ROMbanks[i] = rnd();
  }
  cRSID_C64.VIC.RasterRowCycles = 63;
  cRSID_C64.VIC.RasterLines = 312;
  return check_cpu() || check_timers();
}
