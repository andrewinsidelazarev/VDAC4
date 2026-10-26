/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Обновление прошивки BL616 по сети: ESP → Tang → BL616. Протокол почтового ящика.
 * Та же раскладка — у ESP (VDAC4/ESP32 S3 Zero N8R8/Firmware/src/bl616_push.cpp); их
 * совместную работу проверяет bl616/app/test/update_test.cpp.
 *
 * Почтовый ящик — 512 слов в ПЛИС (cp_link). ESP пишет его по LINK (MB_PAGE 0x34 и окно
 * 0x200…0x3FC, 128 слов страницы), BL616 читает READ_REQ с адреса 0x3F0000. Слова:
 *   0      заголовок {0xB6, команда, номер}; ESP пишет его последним, когда остальное готово;
 *   1      BEGIN — размер образа, байт; DATA — байтов в посылке;
 *   2      DATA — смещение посылки в образе;
 *   2…9    BEGIN — SHA-256 образа (байты digest[0…31] подряд, младший байт слова первым);
 *   16…511 DATA — данные (до 1984 байт), младший байт слова первым.
 * Команды: BEGIN — начать (BL616 берёт буфер в RAM), DATA — кусок образа, END — проверить
 * SHA-256 и заголовок BFNP, записать во flash с 0x40000 и перезагрузиться, ABORT — бросить.
 * Ответ BL616 — в слове состояния CP_INFO (LINK 0x30): {0xC1, состояние, номер команды};
 * после END — {0xC1, FWU_WRITING, номер}, затем BL616 перезагружается и новая прошивка пишет
 * обычное CP_INFO {0xC0, версия, …}. Новой команды нет 10 с — BL616 бросает обновление.
 */
#ifndef FW_MAILBOX_H
#define FW_MAILBOX_H

#define FWU_MB_BASE      0x3F0000u   /* адрес почтового ящика для BL616 (READ_REQ) */
#define FWU_MB_WORDS     512u
#define FWU_MAGIC        0xB6u
#define FWU_PAYLOAD_WORD 16u
#define FWU_PAYLOAD_MAX  ((FWU_MB_WORDS - FWU_PAYLOAD_WORD) * 4u)   /* 1984 байта */

enum { FWU_BEGIN = 1, FWU_DATA = 2, FWU_END = 3, FWU_ABORT = 4 };

#define FWU_INFO_MAGIC 0xC1u          /* CP_INFO[31:24] во время обновления */
enum {
    FWU_OK = 0x00,
    FWU_BAD_CMD = 0x01,       /* команда без BEGIN или неизвестная */
    FWU_BAD_SIZE = 0x02,      /* размер 0 или больше FWU_MAX_IMAGE, нет памяти */
    FWU_BAD_OFFSET = 0x03,    /* посылка за пределами образа */
    FWU_BAD_SHA = 0x04,       /* SHA-256 собранного образа не тот */
    FWU_BAD_IMAGE = 0x05,     /* нет заголовка BFNP (не образ BL616) */
    FWU_WRITING = 0x10,       /* образ верен: пишется во flash, затем перезагрузка */
};

#define FWU_FLASH_ADDR 0x40000u       /* вторичная прошивка за FPGA Partner */
#define FWU_MAX_IMAGE  (256u * 1024u)

#endif
