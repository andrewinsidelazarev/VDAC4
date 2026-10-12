// TurboFM Dump (TFD): записи в регистры двух YM2203 по кадрам 50 Гц. По
// разбору TFD из ZXTune (vitamin.caig@gmail.com); SPDX-License-Identifier: LGPL-3.0-only
//
// Файл: «TFMD», три строки с нулём (название, автор, комментарий), затем поток.
// Байты потока: FF — граница кадра; FE n — 3+n границ подряд (пустые кадры);
// FD/FC — дальше пишется второй/первый чип; FB — конец; FA — метка повтора
// (кадр, в котором она стоит); любой другой байт — номер регистра, за ним
// значение. Кадр N — записи между границами N и N+1; записи до первой границы и
// после последней не звучат (как у ZXTune). После каждой границы снова чип 0.
#pragma once
#include "tfc.h"
namespace tsfm {
struct Tfd {
  const uint8_t* data;
  size_t size;
  uint32_t start;          // первый байт потока после строк
  uint32_t cursor;         // следующий байт потока
  uint32_t frame;          // сыгранных кадров
  uint32_t frames;         // кадров в песне
  uint32_t empty;          // пустых кадров осталось от группы границ
  uint32_t loop_cursor;    // начало кадра с меткой повтора (нет метки — начало песни)
  uint32_t loop_frame;
  uint32_t loop_empty;     // пустых кадров в начале повтора (без метки — как у начала)
  bool error;
  char title[64],author[64];
  bool open(const uint8_t*,size_t,const zm_host_t*);
  void reset();
  // Записи одного кадра. За концом песня продолжается с кадра метки повтора
  // (нет метки — с начала): так более короткая дорожка MTC звучит до конца
  // самой длинной, как половины TS и каналы TFC.
  bool tick(FmWrite,void*);
private:
  unsigned byte(uint32_t&);
  bool group(unsigned code,uint32_t&,uint32_t& count);
};
}
