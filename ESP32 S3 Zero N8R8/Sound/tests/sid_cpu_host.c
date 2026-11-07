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

static int check_cpu(int cached) {
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
      if (cached && cRSID_C64.RealSIDmode) cia_cache_bind();
      unsigned got = fast_cpu(&cRSID_C64.CPU);
      cia_cache_unbind();
      if (want != got || memcmp(&expected, &cRSID_C64, sizeof(expected)) ||
          memcmp(&want_iface, &cRSID, sizeof(cRSID))) {
        printf("CPU mismatch opcode=%02X case=%u cycles=%u/%u\n", code, trial, want, got);
        return 1;
      }
      ++cases;
    }
  }
  printf("%scpu_cases=%u\n", cached ? "cached_" : "", cases);
  return 0;
}

/* Включённая/выключенная банковка, читаемые порты, маски IRQ и разные записи
 * store/RMW. После возврата из кэша вся память должна совпасть с cRSID. */
static int check_operand_fetch(void) {
  static const unsigned pcs[]={0x200,0x9FFD,0x9FFE,0x9FFF,0xA000,0xBFFD,0xBFFE,0xBFFF,
    0xC000,0xCFFD,0xCFFE,0xCFFF,0xD000,0xDC02,0xDC04,0xDC0D,0xDD0D,0xE000,
    0xFFFD,0xFFFE,0xFFFF};
  static const uint8_t ops[]={0x2C,0x4C,0x8D,0xBD};
  unsigned cases=0;
  for(unsigned p=0;p<sizeof(pcs)/sizeof(pcs[0]);++p)
  for(unsigned o=0;o<sizeof(ops);++o)
  for(unsigned trial=0;trial<32;++trial) {
    const unsigned pc=pcs[p],operand=trial<16?0xDC0D:0x9FFF;
    cRSID_initCPU(pc);cRSID_C64.CPU.X=rnd()&255;
    cRSID_C64.CPU.A=rnd()&255;cRSID_C64.CPU.ST=rnd()&255;
    cRSID_C64.RealSIDmode=trial&1;cRSID_C64.NMI=(trial&4)?128:0;
    cRSID_C64.RAMbank[1]=(trial>>1)&3;
    const uint8_t bytes[]={ops[o],operand&255,operand>>8};
    for(unsigned j=0;j<3;++j) {
      const unsigned at=(pc+j)&65535;
      cRSID_C64.RAMbank[at]=cRSID_C64.IObankRD[at]=bytes[j];
      cRSID_C64.ROMbanks[at]=j?(bytes[j]^(0x31>>j)):bytes[j];
    }
    saved=cRSID_C64;iface=cRSID;
    const unsigned want=cRSID_emulateCPU();
    expected=cRSID_C64;const cRSID_Interface want_iface=cRSID;
    cRSID_C64=saved;cRSID=iface;
    if((trial&8)&&cRSID_C64.RealSIDmode)cia_cache_bind();
    const unsigned got=fast_cpu(&cRSID_C64.CPU);
    cia_cache_unbind();
    if(got!=want||memcmp(&expected,&cRSID_C64,sizeof(expected))||
       memcmp(&want_iface,&cRSID,sizeof(cRSID))) {
      printf("Operand fetch mismatch PC=%04X opcode=%02X trial=%u\n",pc,ops[o],trial);return 1;
    }
    ++cases;
  }
  printf("operand_fetch_cases=%u\n",cases);return 0;
}

static int check_read_banks(void) {
  unsigned cases=0;
  for(unsigned bank=0;bank<4;++bank) {
    cRSID_C64.RAMbank[1]=bank;
    for(unsigned addr=0;addr<65536;++addr) {
      if(addr>=0xD000&&addr<0xE000)continue; /* I/O side effects tested separately */
      const unsigned want=rd((uint16_t)addr),got=fast_read((uint16_t)addr);
      if(want!=got){printf("Bank read mismatch %04X bank=%u\n",addr,bank);return 1;}
      ++cases;
    }
  }
  printf("bank_read_cases=%u\n",cases);return 0;
}

static int check_cached_bus(void) {
  static const uint16_t addresses[] = {0xDC00,0xDD00,0xDC0C,0xDD0C,0xDC0D,0xDD0D,
      0xDC05,0xDD05,0xDC07,0xDD07,0xDC0E,0xDD0E,0xDC0F,0xDD0F,0xDC80,0xDDFF};
  for (unsigned i=0;i<4096;++i) {
    cRSID_C64.RealSIDmode=1;
    cRSID_C64.RAMbank[1]=(i>>4)&3;
    for (unsigned n=0;n<512;++n) {
      cRSID_C64.IObankRD[0xDC00+n]=rnd();
      cRSID_C64.IObankWR[0xDC00+n]=rnd();
    }
    uint16_t address=addresses[i%16];
    uint8_t data=rnd(), want=0, got=0;
    unsigned operation=(i>>6)%3;
    saved=cRSID_C64;
    if (operation==0) want=rd(address);
    else if (operation==1) wr(address,data);
    else wr2(address,data);
    expected=cRSID_C64;
    cRSID_C64=saved;
    cia_cache_bind();
    if (operation==0) got=fast_read(address);
    else if (operation==1) fast_write_rmw(address,data);
    else fast_write_store(address,data);
    cia_cache_unbind();
    if (got!=want || memcmp(&expected,&cRSID_C64,sizeof(expected))) {
      printf("CIA cache bus mismatch case=%u address=%04X\n",i,address);return 1;
    }
  }
  puts("cia_cache_bus_cases=4096");
  return 0;
}

static int check_timers(void) {
  for (unsigned i = 0; i < 100000; ++i) {
    uint8_t* w = cRSID_C64.CIA[1].BasePtrWR;
    uint8_t* r = cRSID_C64.CIA[1].BasePtrRD;
    for (unsigned n = 0; n < 16; ++n) { w[n] = rnd(); r[n] = rnd(); }
    /* cRSID не реализует каскад A -> B; он проверяется независимо ниже. */
    if ((w[15] & 0x60) == 0x40) w[15] &= (uint8_t)~0x40;
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


/* Независимая модель каскада: по одному такту, без формул и деления fast_timer_ticks. */
static unsigned tick_oracle(uint8_t* w, uint8_t* r, unsigned timer,
                            unsigned control, unsigned irq, unsigned ticks) {
  unsigned events = 0;
  if (w[control] & 0x10) {
    r[timer] = w[timer]; r[timer + 1] = w[timer + 1];
    w[control] &= (uint8_t)~0x10;
  } else {
    while (ticks-- && (w[control] & 1)) {
      unsigned count = (unsigned)r[timer + 1] * 256 + r[timer];
      if (count) --count;
      else {
        ++events;
        count = (unsigned)w[timer + 1] * 256 + w[timer];
        r[13] |= irq;
        if (w[13] & irq) r[13] |= 0x80;
        if (w[control] & 8) w[control] &= (uint8_t)~1;
      }
      r[timer] = count; r[timer + 1] = count >> 8;
    }
  }
  r[control] = w[control];
  return events;
}

static int check_cascade(void) {
  uint8_t w[16], r[16], want_w[16], want_r[16];
  cRSID_CIAinstance cia = {0};
  cia.BasePtrWR = w; cia.BasePtrRD = r;
  for (unsigned i = 0; i < 100000; ++i) {
    for (unsigned n = 0; n < 16; ++n) w[n] = rnd(), r[n] = rnd();
    w[15] = (w[15] & ~0x60) | 0x40;
    /* Чаще короткие периоды, включая latch=0 и много переполнений за инструкцию. */
    if (i & 1) {
      w[4] &= 7; w[5] = 0; r[4] &= 7; r[5] = 0;
      w[6] &= 7; w[7] = 0; r[6] &= 7; r[7] = 0;
    }
    memcpy(want_w,w,16); memcpy(want_r,r,16);
    const unsigned cycles = 1 + i % 9;
    const unsigned a = tick_oracle(want_w,want_r,4,14,1,(want_w[14]&0x20)?0:cycles);
    tick_oracle(want_w,want_r,6,15,2,a);
    unsigned got = fast_cia(&cia,cycles);
    if (got != (want_r[13]&0x80) || memcmp(w,want_w,16) || memcmp(r,want_r,16)) {
      printf("CIA cascade mismatch case=%u\n",i); return 1;
    }
  }
  /* Stereophonik: A=2, B=4. За 15 тактов A переполняется 5 раз, B — один. */
  memset(w,0,16); memset(r,0,16);
  w[4]=r[4]=2; w[6]=r[6]=4; w[14]=1; w[15]=0x41; w[13]=2;
  fast_cia(&cia,7); fast_cia(&cia,7);
  if (r[6] != 0 || (r[13]&2)) return 1;
  if (fast_cia(&cia,1) != 0x80 || r[4] != 2 || r[6] != 4 || !(r[13]&2)) return 1;
  puts("cia_cascade_cases=100001");
  return 0;
}

static int check_cia_plan(void) {
  uint8_t w[16],r[16],ow[16],orr[16];
  cRSID_CIAinstance cia={0};
  cia.BaseAddress=0xDC00;cia.BasePtrWR=w;cia.BasePtrRD=r;
  FastCiaPlan plan;
  cRSID_C64.RAMbank[1]=3;
  unsigned cases=0;
  for(unsigned trial=0;trial<10000;++trial) {
    memset(&plan,0,sizeof(plan));fast_cia_plan=&plan;
    for(unsigned j=0;j<16;++j)w[j]=rnd(),r[j]=rnd();
    w[4]=rnd()&15;w[5]=0;w[6]=rnd()&7;w[7]=0;
    r[4]=rnd()%(w[4]+1);r[5]=0;r[6]=rnd()%(w[6]+1);r[7]=0;
    if(trial&1){w[6]=rnd();w[7]=rnd();r[6]=w[6];r[7]=w[7];}
    w[14]=(rnd()&0xC6)|1;w[15]=(rnd()&0x86)|0x41;
    memcpy(ow,w,16);memcpy(orr,r,16);
    for(unsigned step=0;step<16;++step) {
      const unsigned cycles=rnd()%10;
      if(step==4){r[13]=orr[13]=0;w[13]=ow[13]=(uint8_t)rnd();}
      if(step==8){w[14]|=0x10;ow[14]|=0x10;fast_cia_changed(0xDC0E);}
      if(step==10){w[15]|=0x10;ow[15]|=0x10;fast_cia_changed(0xDC0F);}
      if(step==12){w[15]|=8;ow[15]|=8;fast_cia_changed(0xDC0F);}
      unsigned ea=tick_oracle(ow,orr,4,14,1,cycles);
      tick_oracle(ow,orr,6,15,2,ea);
      unsigned got=fast_cia(&cia,(int)cycles);
      if(got!=(orr[13]&0x80)||memcmp(w,ow,16)||memcmp(r,orr,16)) {
        printf("CIA plan mismatch trial=%u step=%u\n",trial,step);return 1;
      }
      ++cases;
    }
  }
  fast_cia_plan=NULL;
  printf("cia_plan_cases=%u\n",cases);return 0;
}

int main(void) {
  static Hot test_hot;
  hot=&test_hot;
  cRSID_init(RATE, 0);
  for (unsigned i = 0; i < CRSID_MEMBANK_SIZE; ++i) {
    cRSID_C64.RAMbank[i] = rnd();
    cRSID_C64.ROMbanks[i] = rnd();
  }
  cRSID_C64.VIC.RasterRowCycles = 63;
  cRSID_C64.VIC.RasterLines = 312;
  return check_cpu(0) || check_cpu(1) || check_operand_fetch() || check_read_banks() || check_cached_bus() || check_timers() || check_cascade() || check_cia_plan();
}
