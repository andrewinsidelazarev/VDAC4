/* SPDX-License-Identifier: Apache-2.0 */
/* Перезагрузка ПЛИС Tang Nano 20K командой JTAG с выводов BL616 (fpga_jtag.c). */
#ifndef FPGA_JTAG_H
#define FPGA_JTAG_H

/* JTAG Reconfig (IR 0x3C) и NOOP (0x02): ПЛИС грузится из flash по адресу перехода multiboot
   своего битстрима. Выводы JTAG после команды — снова за FPGA Partner (USB-JTAG). */
void fpga_jtag_reconfig(void);

#endif
