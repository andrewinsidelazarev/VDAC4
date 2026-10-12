#pragma once
// Host model of the ROM boundary only. Actual JPEG pixels are decoded by jpgd.
typedef unsigned char BYTE;
typedef unsigned int UINT;
struct JDEC { void* device; unsigned width, height; };
struct JRECT { unsigned short left, right, top, bottom; };
enum JRESULT { JDR_OK, JDR_INTR, JDR_INP, JDR_MEM1, JDR_MEM2,
               JDR_PAR, JDR_FMT1, JDR_FMT2, JDR_FMT3 };
JRESULT jd_prepare(JDEC*, UINT(*)(JDEC*, BYTE*, UINT), void*, UINT, void*);
JRESULT jd_decomp(JDEC*, UINT(*)(JDEC*, void*, JRECT*), BYTE);
