/* Производный код cRSID 1.58, Hermit (Mihaly Horvath), лицензия WTF.
 * Создан tools/make_sid_cpu.py; править генератор, не этот файл.
 * Порядок чтений, записей, флаги и особенности cRSID сохранены. */
static INLINE void fast_addrModeImmediate(cRSID_CPUinstance* r)
{ ++r->PC; r->Addr = r->PC; r->Cycles=2; }

static INLINE void fast_addrModeZeropage(cRSID_CPUinstance* r)
{ ++r->PC; r->Addr = fast_read(r->PC); r->Cycles=3; }

static INLINE void fast_addrModeAbsolute(cRSID_CPUinstance* r) {
 ++r->PC; r->Addr = fast_read(r->PC);
 ++r->PC; r->Addr += fast_read(r->PC)<<8; r->Cycles=4;
}

static INLINE void fast_addrModeZeropageXindexed(cRSID_CPUinstance* r)
{ ++r->PC; r->Addr = (fast_read(r->PC) + r->X) & 0xFF; r->Cycles=4; }

static INLINE void fast_addrModeZeropageYindexed(cRSID_CPUinstance* r)
{ ++r->PC; r->Addr = (fast_read(r->PC) + r->Y) & 0xFF; r->Cycles=4; }

static INLINE void fast_addrModeXindexed(cRSID_CPUinstance* r) {
 ++r->PC; r->Addr = fast_read(r->PC) + r->X;
 ++r->PC; r->SamePage = (r->Addr <= 0xFF); r->Addr += fast_read(r->PC)<<8;
 r->Cycles = 5;
}

static INLINE void fast_addrModeYindexed(cRSID_CPUinstance* r) {
 ++r->PC; r->Addr = fast_read(r->PC) + r->Y; ++r->PC;
 r->SamePage = (r->Addr <= 0xFF); r->Addr += fast_read(r->PC)<<8; r->Cycles=5;
}

static INLINE void fast_addrModeIndirectYindexed(cRSID_CPUinstance* r) {
 ++r->PC; r->Addr = fast_read(fast_read(r->PC)) + r->Y;
 r->SamePage = (r->Addr <= 0xFF); r->Addr += fast_read( (fast_read(r->PC)+1)&0xFF ) << 8;
 r->Cycles = 6;
}

static INLINE void fast_addrModeXindexedIndirect(cRSID_CPUinstance* r) {
 ++r->PC;
 r->Addr = ( fast_read( fast_read(r->PC) + r->X ) & 0xFF )
                      + ( ( fast_read( fast_read(r->PC) + r->X + 1 ) & 0xFF ) << 8 );
 r->Cycles = 6;
}

static INLINE void fast_clrC(cRSID_CPUinstance* r) { r->ST &= ~C; }
static INLINE void fast_setC(cRSID_CPUinstance* r, FASTVAR unsigned char expr)
{ r->ST &= ~C; r->ST |= (expr!=0); }

static INLINE void fast_clrNZC(cRSID_CPUinstance* r)
{ r->ST &= ~(N|Z|C); }

static INLINE void fast_clrNVZC(cRSID_CPUinstance* r)
{ r->ST &= ~(N|V|Z|C); }

static INLINE void fast_setNZbyA(cRSID_CPUinstance* r)
{ r->ST &= ~(N|Z); r->ST |= ((!r->A)<<1) | (r->A&N); }

static INLINE void fast_setNZbyT(cRSID_CPUinstance* r, FASTVAR short int t)
{ t&=0xFF; r->ST &= ~(N|Z); r->ST |= ((!t)<<1) | (t&N); }

static INLINE void fast_setNZbyX(cRSID_CPUinstance* r)
{ r->ST &= ~(N|Z); r->ST |= ((!r->X)<<1) | (r->X&N); }

static INLINE void fast_setNZbyY(cRSID_CPUinstance* r)
{ r->ST &= ~(N|Z); r->ST |= ((!r->Y)<<1) | (r->Y&N); }

static INLINE void fast_setNZbyM(cRSID_CPUinstance* r)
{ r->ST &= ~(N|Z); r->ST |= ((!fast_read(r->Addr))<<1) | (fast_read(r->Addr)&N); }

static INLINE void fast_setNZCbyAdd(cRSID_CPUinstance* r)
{ r->ST &= ~(N|Z|C); r->ST |= (r->A&N)|(r->A>255); r->A&=0xFF; r->ST|=(!r->A)<<1; }

static INLINE void fast_setVbyAdd(cRSID_CPUinstance* r, FASTVAR unsigned char m, FASTVAR short int t) {
 r->ST &= ~V; r->ST |= ( (~(t^m)) & (t^r->A) & N ) >> 1;
}

static INLINE void fast_setNZCbySub(cRSID_CPUinstance* r, FASTVAR signed short t)
{ r->ST &= ~(N|Z|C); r->ST |= (t&N) | (t>=0);  r->ST |= ((!(t&0xFF))<<1); }

static INLINE void fast_push(cRSID_CPUinstance* r, FASTVAR unsigned char value)
{ cRSID_C64.RAMbank[0x100+r->SP] = value; --r->SP; r->SP&=0xFF; }

static INLINE unsigned char fast_pop(cRSID_CPUinstance* r)
{ ++r->SP; r->SP&=0xFF; return cRSID_C64.RAMbank[0x100+r->SP]; }

static unsigned char fast_cpu_general(cRSID_CPUinstance* state, unsigned char opcode) {
 cRSID_CPUinstance regs = *state;
 cRSID_CPUinstance* r = &regs;

 static const unsigned char FlagSwitches[] = { 0x01, 0x21, 0x04, 0x24, 0x00, 0x40, 0x08, 0x28 },
                            BranchFlags[] = { 0x80, 0x40, 0x01, 0x02 };

 FASTVAR unsigned int  PrevPC;
 FASTVAR unsigned char IR;
 FASTVAR short int T;

 PrevPC = r->PC;
 IR = opcode; r->Cycles=2; r->SamePage=0;

 if (IR&1) {

  switch ( (IR & 0x1F) >> 1 ) {
   case    0:  case    1: fast_addrModeXindexedIndirect(r); break;
   case    2:  case    3: fast_addrModeZeropage(r); break;
   case    4:  case    5: fast_addrModeImmediate(r); break;
   case    6:  case    7: fast_addrModeAbsolute(r); break;
   case    8:  case    9: fast_addrModeIndirectYindexed(r); break;
   case  0xA:             fast_addrModeZeropageXindexed(r); break;
   case  0xB:             if ((IR&0xC0)!=0x80) fast_addrModeZeropageXindexed(r);
                          else fast_addrModeZeropageYindexed(r);
                          break;
   case  0xC:  case  0xD: fast_addrModeYindexed(r); break;
   case  0xE:             fast_addrModeXindexed(r); break;
   case  0xF:             if ((IR&0xC0)!=0x80) fast_addrModeXindexed(r);
                          else fast_addrModeYindexed(r);
                          break;
  }
  r->Addr &= 0xFFFF;

  switch ( (IR & 0xE0) >> 5 ) {

   case 0: if ( MOSTLY ((IR&0x1F) != 0xB) ) {
            if ( RARELY ((IR&3) == 3) ) { fast_clrNZC(r); fast_setC(r, fast_read(r->Addr)>=N); fast_write_rmw( r->Addr, fast_read(r->Addr)<<1 ); r->Cycles+=2; }
            else r->Cycles -= r->SamePage;
            r->A |= fast_read(r->Addr); fast_setNZbyA(r);
           }
           else { r->A &= fast_read(r->Addr); fast_setNZbyA(r); fast_setC(r,  r->A >= N ); }
           break;

   case 1: if ( MOSTLY ((IR&0x1F) != 0xB) ) {
            if ( RARELY ((IR&3) == 3) ) {
             T = (fast_read(r->Addr)<<1) + (r->ST&C); fast_clrNZC(r); fast_setC(r, T>255); T&=0xFF; fast_write_rmw(r->Addr,T); r->Cycles+=2;
            }
            else r->Cycles -= r->SamePage;
            r->A &= fast_read(r->Addr); fast_setNZbyA(r);
           }
           else { r->A &= fast_read(r->Addr); fast_setNZbyA(r); fast_setC(r,  r->A >= N ); }
           break;

   case 2: if ( MOSTLY ((IR&0x1F) != 0xB) ) {
            if ( RARELY ((IR&3) == 3) ) { fast_clrNZC(r); fast_setC(r, fast_read(r->Addr)&1); fast_write_rmw(r->Addr,fast_read(r->Addr)>>1); r->Cycles+=2; }
            else r->Cycles -= r->SamePage;
            r->A ^= fast_read(r->Addr); fast_setNZbyA(r);
           }
           else { r->A &= fast_read(r->Addr); fast_setC(r,  r->A & 1 ); r->A >>= 1; r->A &= 0xFF; fast_setNZbyA(r); }
           break;

   case 3: if ( MOSTLY ((IR&0x1F) != 0xB) ) {
            if( RARELY ((IR&3) == 3) ) {
             T = (fast_read(r->Addr)>>1) + ((r->ST&C)<<7); fast_clrNZC(r); fast_setC(r, T&1); fast_write_rmw(r->Addr,T); r->Cycles+=2;
            }
            else r->Cycles -= r->SamePage;
            T = r->A; r->A += fast_read(r->Addr) + (r->ST & C);
            if ( RARELY ((r->ST & D) && (r->A&0xF)>9) ) { r->A+=0x10; r->A&=0xF0; }
            fast_setNZCbyAdd(r); fast_setVbyAdd(r, fast_read(r->Addr),T);
           }
           else {
            r->A &= fast_read(r->Addr);

            T = r->A; r->A = (r->A>>1) + ((r->ST&C)<<7); fast_setC(r, T>=N); fast_setNZbyA(r);
            r->ST &= ~V; r->ST |= (T & V) ^ (r->A & V);
           }
           break;

   case 4: if ( RARELY ((IR&0x1F) == 0xB) ) { r->A = r->X & fast_read(r->Addr); fast_setNZbyA(r); }
           else if ( RARELY ((IR&0x1F) == 0x1B) ) { r->SP = r->A & r->X; fast_write_rmw( r->Addr, r->SP & ((r->Addr>>8)+1) ); }
           else { fast_write_store( r->Addr, r->A & (RARELY((IR&3)==3)? r->X:0xFF) ); }
           break;

   case 5: if ( MOSTLY ((IR&0x1F) != 0x1B) ) { r->A=fast_read(r->Addr); if(RARELY((IR&3)==3)) r->X=r->A; }
           else { r->A=r->X=r->SP = fast_read(r->Addr) & r->SP; }
           fast_setNZbyA(r); r->Cycles -= r->SamePage;
           break;

   case 6: if( MOSTLY ((IR&0x1F) != 0xB) ) {
            if ( RARELY ((IR&3) == 3) ) { fast_write_rmw(r->Addr,fast_read(r->Addr)-1); r->Cycles+=2;}
            else r->Cycles -= r->SamePage;
            T = r->A - fast_read(r->Addr);
           }
           else { r->X = T = (r->A & r->X) - fast_read(r->Addr); r->X &= 0xFF; }
           fast_setNZCbySub(r,  T );
           break;

   case 7: if( RARELY ((IR&3)==3 && (IR&0x1F)!=0xB) ) { fast_write_rmw( r->Addr, fast_read(r->Addr)+1 ); r->Cycles+=2; }
           else r->Cycles -= r->SamePage;
           T = r->A; r->A -= fast_read(r->Addr) + !(r->ST & C);
           fast_setNZCbySub(r,  r->A ); r->A &= 0xFF; fast_setVbyAdd(r,  ~fast_read(r->Addr), T );
           break;
  }
 }

 else if (IR&2) {

  switch (IR & 0x1F) {
   case    2: fast_addrModeImmediate(r); break;
   case    6: fast_addrModeZeropage(r); break;
   case  0xE: fast_addrModeAbsolute(r); break;
   case 0x16: if ( (IR&0xC0) != 0x80 ) fast_addrModeZeropageXindexed(r);
              else fast_addrModeZeropageYindexed(r);
              break;
   case 0x1E: if ( (IR&0xC0) != 0x80 ) fast_addrModeXindexed(r);
              else fast_addrModeYindexed(r);
              break;
  }
  r->Addr&=0xFFFF;

  switch ( (IR & 0xE0) >> 5 ) {

   case 0: fast_clrC(r);
           if ( CALMLY ((IR&0xF)==0xA) ) { r->A = (r->A << 1) + (r->ST & C); fast_setNZCbyAdd(r); }
           else { T = (fast_read(r->Addr)<<1) + (r->ST & C); fast_setC(r, T>255); fast_setNZbyT(r, T); fast_write_rmw(r->Addr,T); r->Cycles+=2; }
           break;
   case 1: if ( CALMLY ((IR&0xF)==0xA) ) { r->A = (r->A << 1) + (r->ST & C); fast_setNZCbyAdd(r); }
           else { T = (fast_read(r->Addr)<<1) + (r->ST & C); fast_setC(r, T>255); fast_setNZbyT(r, T); fast_write_rmw(r->Addr,T); r->Cycles+=2; }
           break;

   case 2: fast_clrC(r);
           if ( CALMLY ((IR&0xF)==0xA) ) { T = r->A; r->A= (r->A >> 1) + ((r->ST&C) << 7); fast_setC(r, T&1); r->A &= 0xFF; fast_setNZbyA(r); }
           else { T = (fast_read(r->Addr)>>1) + ((r->ST&C) << 7); fast_setC(r,  fast_read(r->Addr) & 1 ); fast_setNZbyT(r, T); fast_write_rmw(r->Addr,T); r->Cycles+=2; }
           break;
   case 3: if ( CALMLY ((IR&0xF)==0xA) ) { T = r->A; r->A= (r->A >> 1) + ((r->ST&C) << 7); fast_setC(r, T&1); r->A &= 0xFF; fast_setNZbyA(r); }
           else { T = (fast_read(r->Addr)>>1) + ((r->ST&C) << 7); fast_setC(r,  fast_read(r->Addr) & 1 ); fast_setNZbyT(r, T); fast_write_rmw(r->Addr,T); r->Cycles+=2; }
           break;

   case 4: if ( MOSTLY (IR&4) ) { fast_write_store( r->Addr, r->X ); }
           else if ( RARELY (IR&0x10) ) r->SP = r->X;
           else { r->A = r->X; fast_setNZbyA(r); }
           break;

   case 5: if ( MOSTLY ((IR&0xF) != 0xA) ) { r->X = fast_read(r->Addr); r->Cycles -= r->SamePage; }
           else if ( RARELY (IR & 0x10) ) r->X = r->SP;
           else r->X = r->A;
           fast_setNZbyX(r);
           break;

   case 6: if ( TIGHTLY (IR&4) ) { fast_write_rmw(r->Addr,fast_read(r->Addr)-1); fast_setNZbyM(r); r->Cycles+=2; }
           else { --r->X; fast_setNZbyX(r); }
           break;

   case 7: if ( TIGHTLY (IR&4) ) { fast_write_rmw(r->Addr,fast_read(r->Addr)+1); fast_setNZbyM(r); r->Cycles+=2; }
           break;
  }
 }

 else if ( (IR & 0xC) == 8 ) {
  if ( IR&0x10 ) {
   if ( IR == 0x98 ) { r->A = r->Y; fast_setNZbyA(r); }
   else {
    if (FlagSwitches[IR>>5] & 0x20) r->ST |= (FlagSwitches[IR>>5] & 0xDF);
    else r->ST &= ~( FlagSwitches[IR>>5] & 0xDF );
   }
  }
  else {
   switch ( (IR & 0xF0) >> 5 ) {
    case 0: fast_push(r,  r->ST ); r->Cycles=3; break;
    case 1: r->ST = fast_pop(r); r->Cycles=4; break;
    case 2: fast_push(r,  r->A ); r->Cycles=3; break;
    case 3: r->A = fast_pop(r); fast_setNZbyA(r); r->Cycles=4; break;
    case 4: --r->Y; fast_setNZbyY(r); break;
    case 5: r->Y = r->A; fast_setNZbyY(r); break;
    case 6: ++r->Y; fast_setNZbyY(r); break;
    case 7: ++r->X; fast_setNZbyX(r); break;
   }
  }
 }

 else {

  if ( (IR&0x1F) == 0x10 ) {
   ++r->PC;
   T = fast_read( r->PC ); if (T & 0x80) T -= 0x100;
   if (IR & 0x20) {
    if (r->ST & BranchFlags[IR>>6]) { r->PC += T; r->Cycles=3; }
   }
   else {
    if ( !(r->ST & BranchFlags[IR>>6]) ) { r->PC += T; r->Cycles=3; }
   }
  }

  else {
   switch (IR&0x1F) {
    case    0: fast_addrModeImmediate(r); break;
    case    4: fast_addrModeZeropage(r); break;
    case  0xC: fast_addrModeAbsolute(r); break;
    case 0x14: fast_addrModeZeropageXindexed(r); break;
    case 0x1C: fast_addrModeXindexed(r); break;
   }
   r->Addr &= 0xFFFF;

   switch ( (IR & 0xE0) >> 5 ) {

    case 0: if( TIGHTLY (!(IR&4)) ) {
             fast_push(r, (r->PC+2-1) >> 8); fast_push(r, (r->PC+2-1) & 0xFF); fast_push(r, r->ST|B); r->ST |= I;
             r->PC = fast_read(0xFFFE) + (fast_read(0xFFFF)<<8) - 1; r->Cycles=7;
            }
            else if ( RARELY (IR == 0x1C) ) r->Cycles -= r->SamePage;
            break;

    case 1: if ( CALMLY (IR & 0xF) ) {
             if ( MOSTLY (!(IR&0x10)) ) { r->ST &= 0x3D; r->ST |= (fast_read(r->Addr) & 0xC0) | ( (!(r->A & fast_read(r->Addr))) << 1 ); }
             else if ( RARELY (IR == 0x3C) ) r->Cycles -= r->SamePage;
            }
            else {
             fast_push(r,  (r->PC+2-1) >> 8 ); fast_push(r,  (r->PC+2-1) & 0xFF );
             r->PC = fast_read(r->Addr) + fast_read(r->Addr+1) * 256 - 1; r->Cycles=6;
            }
            break;

    case 2: if ( MOSTLY (IR & 0xF) ) {
             if ( MOSTLY (IR == 0x4C) ) {
              r->PC = ( RARELY ( cRSID_C64.RealSIDmode && cRSID_C64.NMI && (PrevPC == 0xDC02 || PrevPC == 0xDC04) ) ) ?
                                 (unsigned int) ( ( (0x800) + (r->Addr&0xFF) ) - 1 )
                                 : (r->Addr - 1);
              r->Cycles=3; fast_read(r->Addr+1);

             }
             else if ( RARELY (IR==0x5C) ) r->Cycles -= r->SamePage;
            }
            else {
             r->ST = fast_pop(r); T = fast_pop(r); r->PC = (fast_pop(r) << 8) + T - 1; r->Cycles=6;
             if ( LIKELY (cRSID_C64.Returned && r->SP >= 0xFF) ) { ++r->PC;  { *state = regs; return 0xFE; } }
            }
            break;

    case 3: if ( CALMLY (IR & 0xF) ) {
             if ( MOSTLY (IR == 0x6C) ) {
              r->PC = fast_read( (r->Addr&0xFF00) + ((r->Addr+1)&0xFF) );
              r->PC = (r->PC << 8) + fast_read(r->Addr) - 1; r->Cycles=5;
             }
             else if ( RARELY (IR == 0x7C) ) r->Cycles -= r->SamePage;
            }
            else {
             if ( RARELY (r->SP >= 0xFF) ) { cRSID_C64.Returned=1; { *state = regs; return 0xFF; }}
             T=fast_pop(r); r->PC = (fast_pop(r) << 8) + T; r->Cycles=6;
            }
            break;

    case 4: if ( MOSTLY (IR & 4) ) { fast_write_store( r->Addr, r->Y ); }
            break;

    case 5: r->Y = fast_read(r->Addr); fast_setNZbyY(r); r->Cycles -= r->SamePage;
            break;

    case 6: if ( MOSTLY (!(IR&0x10)) ) {
             T = r->Y - fast_read(r->Addr); fast_setNZCbySub(r,  T );
            }
            else if ( RARELY (IR==0xDC) ) r->Cycles -= r->SamePage;
            break;

    case 7: if ( MOSTLY (!(IR&0x10)) ) {
             T = r->X - fast_read(r->Addr); fast_setNZCbySub(r,  T );
            }
            else if ( RARELY (IR==0xFC) ) r->Cycles -= r->SamePage;
            break;
   }
  }
 }

 ++r->PC;

 if ( CALMLY (!cRSID_C64.RealSIDmode) ) {
  if ( RARELY ( (cRSID_C64.RAMbank[1]&3)>1 && PrevPC<0xE000 && (r->PC==0xEA31 || r->PC==0xEA81 || r->PC==0xEA7E) ) ) { *state = regs; return 0xFE; }
 }

 { *state = regs; return r->Cycles; }
}

/* Fetch adjacent operand bytes from one currently selected plain RAM/ROM
 * bank. I/O, bank boundaries and address wrap keep ordered bus reads.
 * Self-modifying code and the bank register are read afresh each time. */
static INLINE void fast_quick_absolute(cRSID_CPUinstance* r,unsigned pc,int indexed) {
 const uint8_t* bank;
 if(MOSTLY(pc<0x9FFE))bank=cRSID_C64.RAMbank;
 else if(pc>=0xE000&&pc<0xFFFE)
  bank=(cRSID_C64.RAMbank[1]&2)?cRSID_C64.ROMbanks:cRSID_C64.RAMbank;
 else if(pc>=0xC000&&pc<0xCFFE)bank=cRSID_C64.RAMbank;
 else if(pc>=0xA000&&pc<0xBFFE)
  bank=(cRSID_C64.RAMbank[1]&3)==3?cRSID_C64.ROMbanks:cRSID_C64.RAMbank;
 else bank=NULL;
 if(bank) {
  const uint8_t* code=bank+pc;
  unsigned low=code[1];
  if(indexed)low+=r->X;
  const unsigned address=low+((unsigned)code[2]<<8);
  r->PC=pc+2;
  r->SamePage=indexed&&(low<=255);
  r->Addr=address&0xFFFF;
  r->Cycles=indexed?5:4;
 } else {
  if(indexed)fast_addrModeXindexed(r);else fast_addrModeAbsolute(r);
  r->Addr&=0xFFFF;
 }
}

/* Predecoded frequent legal instructions. Direct hot registers avoid copying
 * all CPU fields and entering the large general decoder for every instruction.
 * A fetch with I/O side effects is done once, including the fallback path. */
static __attribute__((always_inline)) inline unsigned char fast_cpu_mode(cRSID_CPUinstance* restrict r,int real,unsigned nmi) {
 static const uint8_t decoded[256] = {
  [0x2C]=1,[0x4C]=2,[0x8D]=3,[0xBD]=4,[0xD0]=5,[0xE8]=6
 };
 const unsigned PrevPC=r->PC;
 const uint8_t IR=fast_read(r->PC);
 switch(decoded[IR]) {
  case 1:
   r->Cycles=2;r->SamePage=0;
   fast_quick_absolute(r,PrevPC,0);
   r->ST&=0x3D;
   r->ST|=(fast_read(r->Addr)&0xC0)|((!(r->A&fast_read(r->Addr)))<<1);
   break;
  case 2:
   r->Cycles=2;r->SamePage=0;
   fast_quick_absolute(r,PrevPC,0);
   r->PC=(RARELY((PrevPC==0xDC02||PrevPC==0xDC04)&&real&&nmi)) ?
          (unsigned)(0x800+(r->Addr&0xFF)-1) : r->Addr-1;
   r->Cycles=3;fast_read(r->Addr+1);
   break;
  case 3:
   r->Cycles=2;r->SamePage=0;
   fast_quick_absolute(r,PrevPC,0);
   fast_write_store(r->Addr,r->A);
   break;
  case 4:
   r->Cycles=2;r->SamePage=0;
   fast_quick_absolute(r,PrevPC,1);
   r->A=fast_read(r->Addr);r->Cycles-=r->SamePage;fast_setNZbyA(r);
   break;
  case 5: {
   r->Cycles=2;r->SamePage=0;
   ++r->PC;int delta=fast_read(r->PC);if(delta&0x80)delta-=0x100;
   if(!(r->ST&Z)){r->PC+=delta;r->Cycles=3;}
  } break;
  case 6:
   r->Cycles=2;r->SamePage=0;
   ++r->X;fast_setNZbyX(r);
   break;
  default:return fast_cpu_general(r,IR);
 }
 ++r->PC;
 if(CALMLY(!real)) {
  if(RARELY((cRSID_C64.RAMbank[1]&3)>1&&PrevPC<0xE000&&
             (r->PC==0xEA31||r->PC==0xEA81||r->PC==0xEA7E)))return 0xFE;
 }
 return r->Cycles;
}

static INLINE unsigned char fast_cpu(cRSID_CPUinstance* r) {
 return fast_cpu_mode(r,cRSID_C64.RealSIDmode,cRSID_C64.NMI);
}

static INLINE char fast_interrupts_state(cRSID_CPUinstance* r,unsigned irq,unsigned nmi) {
 enum StatusFlagBitValues { B=0x10, I=0x04 };

 if ( RARELY (nmi > r->PrevNMI) ) {

  fast_push(r, r->PC>>8); fast_push(r, r->PC&0xFF); fast_push(r, r->ST); r->ST |= I;
  r->PC = *cRSID_getMemReadPtr(0xFFFA) + (*cRSID_getMemReadPtr(0xFFFB)<<8);
  r->PrevNMI = nmi;
  return 1;
 }
 else if ( RARELY (irq && !(r->ST&I) ) ) {

  fast_push(r, r->PC>>8); fast_push(r, r->PC&0xFF); fast_push(r, r->ST); r->ST |= I;
  r->PC = *cRSID_getMemReadPtr(0xFFFE) + (*cRSID_getMemReadPtr(0xFFFF)<<8);
  r->PrevNMI = nmi;
  return 1;
 }
 r->PrevNMI = nmi;

 return 0;
}

static INLINE char fast_interrupts(cRSID_CPUinstance* r) {
 return fast_interrupts_state(r,cRSID_C64.IRQ,cRSID_C64.NMI);
}

static void fast_init_begin (cRSID_SIDheader* SIDheader, char subtune) {
 static cRSID_C64instance* C64 = &cRSID_C64;

 cRSID.PlaytimeExpired=0; C64->FadeLevel=0xF; cRSID.Paused = 1;

 if (subtune==0) subtune = 1;
 else if (subtune > SIDheader->SubtuneAmount) subtune = SIDheader->SubtuneAmount;
 cRSID.SubTune = subtune; C64->SecondCnt = cRSID.PlayTime = 0; cRSID.PlaybackSpeed=1;

 cRSID_setC64(C64); cRSID_initC64(C64);

 cRSID.InitAddress = ((SIDheader->InitAddressH)<<8) + (SIDheader->InitAddressL);
 if (C64->RAMbank[1] == 0x37) {
  if ( (0xA000 <= cRSID.InitAddress && cRSID.InitAddress < 0xC000)
       || (cRSID.LoadAddress < 0xC000 && cRSID.EndAddress >= 0xA000) ) C64->RAMbank[1] = 0x36;
  else if (cRSID.InitAddress >= 0xE000 || cRSID.EndAddress >=0xE000) C64->RAMbank[1] = 0x35;
 }
 cRSID_initCPU( cRSID.InitAddress );
 C64->CPU.A = subtune - 1;

}

static void fast_init_end(cRSID_SIDheader* SIDheader, char subtune) {
 cRSID_C64instance* C64 = &cRSID_C64;
 static const unsigned char PowersOf2[] = {1,2,4,8,16,32,64,128};

 if (subtune>32) cRSID.TimerSource = cRSID.SIDheader->SubtuneTimeSources[0] & 0x80;
 else cRSID.TimerSource = cRSID.SIDheader->SubtuneTimeSources[(32-subtune)>>3] & PowersOf2[(subtune-1)&7];
 if (cRSID.TimerSource || C64->IObankWR[0xDC05]!=0x40 || C64->IObankWR[0xDC04]!=0x24) {
  cRSID.FrameCycles = ( ( C64->IObankWR[0xDC04] + (C64->IObankWR[0xDC05]<<8) ) );
  cRSID.TimerSource = 1;
 }

 cRSID.PlayAddress = (SIDheader->PlayAddressH<<8) + SIDheader->PlayAddressL;
 if (cRSID.PlayAddress) {
  if (C64->RAMbank[1] == 0x37) {
   if (0xA000 <= cRSID.PlayAddress && cRSID.PlayAddress < 0xC000) C64->RAMbank[1] = 0x36;
  }
  else if (cRSID.PlayAddress >= 0xE000) C64->RAMbank[1] = 0x35;
 }
 else {
  cRSID.PlayAddress = (C64->RAMbank[1] & 3) < 2 ? cRSID_readMem(0xFFFE) + (cRSID_readMem(0xFFFF)<<8)
                                                 : cRSID_readMem(0x314) + (cRSID_readMem(0x315)<<8);
  if (cRSID.PlayAddress==0) {
   cRSID_initCPU( cRSID.PlayAddress );
   C64->Finished=1; C64->Returned=1; cRSID.Paused = 0; return;
  }
 }

 if (!cRSID.RealSIDmode) {
  cRSID_initCPU( cRSID.PlayAddress );
  C64->FrameCycleCnt=0; C64->Finished=1; C64->SampleCycleCnt=0;
 }
 else { C64->Finished=0; C64->Returned=0; }

 cRSID.Paused = 0;
}
