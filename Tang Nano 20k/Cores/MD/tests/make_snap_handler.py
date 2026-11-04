# SPDX-License-Identifier: GPL-3.0-or-later
# Обработчик снимка 68000 (03.11.2026): собирает слова программы для вставки $A12000 и пишет заголовок
# esp/main/md_snap_68k.h — его берут MD.ELF (ESP кладёт программу во вставку) и модель tests/md_model.cpp.
# Ассемблера 68000 под рукой нет: коды команд — по таблицам M68000 PRM, переходы считает этот сценарий.
#
# Вставка — 1К слов $A12000-$A127FF (номера слов):
#   000-07f программа;  080-09d D0-D7, A0-A6 (movem.l);  09e-09f USP;  0a0-0a1 SSP;
#   0a2 состояние: 1 — регистры сохранены, ждёт команд; иначе — номер последней выполненной команды;
#       0 — регистры взяты из буфера, RTE;
#   100-3fe сценарий: записи по 4 слова {оп, адрес [31:16], адрес [15:0], данные}; оп 0 — конец;
#   3ff команда ESP (регистр LINK 5f, слово $A127FE): {номер [15:8], код [7:0]} — новая команда
#       отличается от прошлой номером. Код 1 — вернуться (регистры из буфера), 2 — то же, но USP и SSP
#       тоже из буфера (загрузка снимка), 3 — выполнить сценарий.
# Сценарий: оп 1 — запись слова, 2 — запись байта (младший байт данных), 3 — чтение слова в данные,
#   4 — чтение байта в данные, 5 — ждать, пока (слово & данные) == 0, 6 — пока (слово & данные) != 0
#   (ожидание — не больше 65536 опросов).
import sys
from pathlib import Path

BASE = 0xA12000
code = []          # (слово | метка-ссылка)
labels = {}


def w(*ws):
    code.extend(ws)


def L(name):
    labels[name] = len(code)


def bcc(op, name):              # короткий переход: смещение от адреса за словом команды
    code.append(('b', op, name))


def dbcc(op, name):             # DBcc d4: смещение 16 бит от адреса слова смещения
    code.append(op); code.append(('d', name))


def a32(x):
    return [(x >> 16) & 0xFFFF, x & 0xFFFF]


ST = BASE + 0xA2 * 2            # $A12144 — состояние
CMD = BASE + 0x3FF * 2          # $A127FE — команда
REGS = BASE + 0x80 * 2          # $A12100
USP = BASE + 0x9E * 2           # $A1213C
SSP = BASE + 0xA0 * 2           # $A12140
SCR = BASE + 0x100 * 2          # $A12200

w(0x48F9, 0x7FFF, *a32(REGS))   # movem.l d0-d7/a0-a6,REGS
w(0x4E68)                       # move.l  usp,a0
w(0x23C8, *a32(USP))            # move.l  a0,USP
w(0x23CF, *a32(SSP))            # move.l  a7,SSP
w(0x7200)                       # moveq   #0,d1            — последняя команда
w(0x33FC, 0x0001, *a32(ST))     # move.w  #1,ST            — регистры сохранены
L('wait')
w(0x3039, *a32(CMD))            # move.w  CMD,d0
w(0xB041)                       # cmp.w   d1,d0
bcc(0x6700, 'wait')             # beq.s   wait
w(0x3200)                       # move.w  d0,d1
w(0x3400)                       # move.w  d0,d2
w(0x0242, 0x00FF)               # andi.w  #$ff,d2
w(0x0C42, 0x0001)               # cmpi.w  #1,d2
bcc(0x6700, 'ret')              # beq.s   ret
w(0x0C42, 0x0002)               # cmpi.w  #2,d2
bcc(0x6700, 'load')             # beq.s   load
w(0x0C42, 0x0003)               # cmpi.w  #3,d2
bcc(0x6600, 'ack')              # bne.s   ack
w(0x41F9, *a32(SCR))            # lea     SCR,a0
L('next')
w(0x3418)                       # move.w  (a0)+,d2         — оп
bcc(0x6700, 'ack')              # beq.s   ack
w(0x2258)                       # movea.l (a0)+,a1         — адрес
w(0x5342)                       # subq.w  #1,d2
bcc(0x6600, 'n2')               # bne.s   n2
w(0x3298)                       # move.w  (a0)+,(a1)       — 1: запись слова
bcc(0x6000, 'next')
L('n2')
w(0x5342)
bcc(0x6600, 'n3')
w(0x3618)                       # move.w  (a0)+,d3         — 2: запись байта
w(0x1283)                       # move.b  d3,(a1)
bcc(0x6000, 'next')
L('n3')
w(0x5342)
bcc(0x6600, 'n4')
w(0x30D1)                       # move.w  (a1),(a0)+       — 3: чтение слова
bcc(0x6000, 'next')
L('n4')
w(0x5342)
bcc(0x6600, 'n5')
w(0x7600)                       # moveq   #0,d3            — 4: чтение байта
w(0x1611)                       # move.b  (a1),d3
w(0x30C3)                       # move.w  d3,(a0)+
bcc(0x6000, 'next')
L('n5')
w(0x78FF)                       # moveq   #-1,d4           — счёт опросов
w(0x5342)                       # subq.w  #1,d2
bcc(0x6600, 'n6')
L('w5')
w(0x3611)                       # move.w  (a1),d3          — 5: ждать нуля
w(0xC650)                       # and.w   (a0),d3
dbcc(0x57CC, 'w5')              # dbeq    d4,w5
w(0x5448)                       # addq.w  #2,a0
bcc(0x6000, 'next')
L('n6')
L('w6')
w(0x3611)                       # move.w  (a1),d3          — 6: ждать не нуля
w(0xC650)                       # and.w   (a0),d3
dbcc(0x56CC, 'w6')              # dbne    d4,w6
w(0x5448)                       # addq.w  #2,a0
bcc(0x6000, 'next')
L('ack')
w(0x33C1, *a32(ST))             # move.w  d1,ST            — команда выполнена
bcc(0x6000, 'wait')
L('load')
w(0x2079, *a32(USP))            # movea.l USP,a0
w(0x4E60)                       # move.l  a0,usp
w(0x2E79, *a32(SSP))            # movea.l SSP,a7
L('ret')
w(0x4CF9, 0x7FFF, *a32(REGS))   # movem.l REGS,d0-d7/a0-a6
w(0x33FC, 0x0000, *a32(ST))     # move.w  #0,ST
w(0x4E73)                       # rte

out = []
for i, x in enumerate(code):
    if isinstance(x, tuple) and x[0] == 'b':
        d = (labels[x[2]] - (i + 1)) * 2
        assert -128 <= d <= 127 and d != 0, (x, d)
        out.append(x[1] | (d & 0xFF))
    elif isinstance(x, tuple) and x[0] == 'd':
        d = (labels[x[1]] - i) * 2
        out.append(d & 0xFFFF)
    else:
        out.append(x)
assert len(out) <= 0x80, len(out)

root = Path(__file__).resolve().parent.parent
dst = root / 'esp/main/md_snap_68k.h'
lines = ['// SPDX-License-Identifier: GPL-3.0-or-later',
         '// Обработчик снимка 68000 — собран tests/make_snap_handler.py (описание вставки и команд — там).',
         '// Не править руками.', '#pragma once', '#include <stdint.h>', '',
         '#define MD_SNAP_REGS 0x80      // слово вставки: D0-D7, A0-A6, USP, SSP (по 2 слова, старшее первым)',
         '#define MD_SNAP_STATE 0xA2     // состояние обработчика',
         '#define MD_SNAP_SCRIPT 0x100   // сценарий: записи по 4 слова',
         '#define MD_SNAP_SCRIPT_MAX 191 // записей (последняя — оп 0)',
         '#define MD_SNAP_CMD_RET 1', '#define MD_SNAP_CMD_LOAD 2', '#define MD_SNAP_CMD_SCRIPT 3',
         '#define MD_SNAP_OP_END 0', '#define MD_SNAP_OP_WW 1', '#define MD_SNAP_OP_WB 2', '#define MD_SNAP_OP_RW 3',
         '#define MD_SNAP_OP_RB 4', '#define MD_SNAP_OP_WAIT0 5', '#define MD_SNAP_OP_WAIT1 6', '',
         'static const uint16_t md_snap_68k[%d] = {' % len(out)]
for k in range(0, len(out), 10):
    lines.append('    ' + ', '.join('0x%04X' % v for v in out[k:k + 10]) + ',')
lines.append('};')
text = '\n'.join(lines) + '\n'
old = dst.read_bytes() if dst.exists() else b''
new = text.replace('\n', '\r\n').encode('utf-8') if b'\r\n' in old else text.encode('utf-8')
if new != old:
    dst.write_bytes(new)
print('%d слов, %s' % (len(out), dst))
