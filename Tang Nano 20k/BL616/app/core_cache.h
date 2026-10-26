/* SPDX-License-Identifier: Apache-2.0 */
/* Кэш ядер ПЛИС во flash BL616 и их загрузка по JTAG (протокол — core_mailbox.h). */
#ifndef CORE_CACHE_H
#define CORE_CACHE_H

#include <stdint.h>

#include "ft_copro.h"

/* Шина к ПЛИС; каталог кэша — из flash; заголовок ящика, что лежит там сейчас, — уже виденный. */
void core_cache_init(const ftc_bus_t *bus);
/* BL616 запустился, а в ПЛИС уже ядро из кэша (сброс BL616 при работающем ZX Next; UserCode с
   меткой VDAC4, core_jtag_usercode): вести его, как загруженное самим, — просьба о перезагрузке
   или молчание 3 с — назад в слот 0. Ящик и сопроцессор не трогаются: core_cache_poll держит
   цикл, пока ПЛИС не вернётся в ядро VDAC4; затем — core_cache_init и прочее. */
void core_cache_adopt(const ftc_bus_t *bus, uint32_t now_ms);
/* Шаг главного цикла — до fw_update_poll и сопроцессора. 1 — цикл занят кэшем: идёт запись
   ядра или в ПЛИС работает ядро из кэша (у него нет ни ящика, ни сопроцессора: BL616 только
   смотрит STATUS — просьбу о перезагрузке, — а чтение ПЛИС там зависло бы). */
int core_cache_poll(uint32_t now_ms);
/* 1 — слово CP_INFO сейчас занято ответом кэша */
int core_cache_owns_info(uint32_t now_ms);

#endif
