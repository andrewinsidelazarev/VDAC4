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
