/* SPDX-License-Identifier: Apache-2.0 */
/* Обновление прошивки BL616 по сети (протокол — fw_mailbox.h). */
#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdint.h>

#include "ft_copro.h"

/* Шина к ПЛИС; заголовок почтового ящика, что лежит там после перезагрузки, — уже виденный. */
void fw_update_init(const ftc_bus_t *bus);
/* Шаг: без обновления — раз в 50 мс смотрит заголовок ящика; в обновлении — каждый вызов.
   1 — идёт обновление (сопроцессор стоит). После END верного образа не возвращается. */
int fw_update_poll(uint32_t now_ms);
/* Буфер образа вместо кучи (прошивка 11): память, которая во время обновления не нужна (рабочая
   память 3D и картинок — сопроцессор стоит). У прошивки 9 образ шёл в malloc, а куча после декодера картинок — ~76 КБ:
   образ больше её не принимался. 0 — буфер был занят обновлением, которое не кончилось
   перезагрузкой: владелец памяти должен её заново подготовить (fw_update_clobbered). */
void fw_update_set_buffer(void *buf, uint32_t size);
int fw_update_clobbered(void);
/* 1 — слово CP_INFO сейчас занято ответом обновления (обычное слово состояния не писать) */
int fw_update_owns_info(uint32_t now_ms);

/* Заменяемое на ПК (update_test): запись образа во flash и перезагрузка. */
void fw_update_commit(const uint8_t *img, uint32_t size);

#endif
