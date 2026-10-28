
#include <defs.h>
#include <wc_h.h>

static const WC_HDR hdr =
{
  /* +00 -reserved-          */   "",
  /* +16 Magic string        */   {'W', 'i', 'l', 'd', 'C', 'o', 'm', 'm', 'a', 'n', 'd', 'e', 'r', 'M', 'D', 'L'},
  /* +32 Format version      */   7,
  /* +33 -reserved-          */   "",
  /* +34 Number of pages     */   16, // четыре банка кода, восемь страниц буфера EVO и четыре — таблице перемотки банка ядра видео (12…15)
  /* +35 Page at 0x8000      */   0,
  /* +36 Page definitions    */   {
  /*  +0 Page, +1 NBlocks    */     {0, N_BLK0}, {9, N_BLK9}, {10, N_BLK10}, {11, N_BLK11}, {1, 0}
  /*                         */   },
  /* +48 -reserved-          */   "",
  /* +64 Extensions          */   {
  /*                         */     {'J', 'P', 'G'},
  /*                         */     {'P', 'N', 'G'},
  /*                         */     {'D', 'L', 'S'},
  /*                         */     {'D', 'X', 'P'},
  /*                         */     {'A', 'V', 'I'},
  /*                         */     {'X', 'M', ' '},
  /*                         */     {'X', 'M', 'Z'}
  /*                         */   },
  /* +160 0x00               */   "",
  /* +161 Max file size      */   0xFFFFFFFF,
  /* +165 Plugin name        */   "FT812 Viewer VDAC4",
  /* +197 Starting condition */   SC_EXT    // только по расширению: это просмотрщик, в F10 ему делать нечего
};
