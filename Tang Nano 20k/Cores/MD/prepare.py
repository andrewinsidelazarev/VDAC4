# SPDX-License-Identifier: GPL-3.0-or-later
# Mega Drive для VDAC4: fpga/core/system.sv из upstream/src/system.sv (mdtang 903d30c) с правками
# байтами — концы строк CRLF исходника сохраняются. Остальные файлы mdtang build.tcl берёт как есть.
#
# Правки (03.11.2026):
#  1. Звук синтезирует ESP: jt12, jt89 и фильтры убраны (NO_SOUND), но Z80 остаётся — в mdtang
#     NO_SOUND убирал и его. Вместо YM2612 — md_ym_stub (таймеры, флаги, занятость для чтения
#     обратно), записи в порты YM и PSG — md_snd_events (слова с меткой времени для ESP).
#  2. Видеопамять VDP — в SDRAM: 64 КБ — 32 блока BSRAM из 46 у GW2AR-18, не помещаются. Четыре
#     dpram заменены портами с запросом и подтверждением; VDP и так ждал подтверждения (наследие
#     fpgagen, где видеопамять тоже была в SDRAM).
#  3. Пауза меню F1. 4. Диагностика Z80 (Z80_DBG -> LINK 54/55). 5. Снимок: обработчик 68000.
#  6. Снимок: Z80, ОЗУ Z80, банки, таймеры YM, VDP; копии vdp.v и t80s.v с отводами — в fpga/core.
#  7. Стоянка машины для синхронизации кадра со стандартной развёрткой HDMI (SYNC_STALL).
# Запуск: python prepare.py  (из папки ядра)
from pathlib import Path

ROOT = Path(__file__).resolve().parent
src = (ROOT / 'upstream/src/system.sv').read_bytes()


def sub(old, new, count=1):
    global src
    old = old.encode().replace(b'\n', b'\r\n')
    new = new.encode().replace(b'\n', b'\r\n')
    n = src.count(old)
    if n != count:
        raise SystemExit(f'prepare.py: ожидалось {count}, найдено {n}: {old[:60]!r}')
    src = src.replace(old, new)


# 1. звук — на ESP
sub('// `define NO_SOUND\n', '`define NO_SOUND   // VDAC4: звук синтезирует ESP (md_snd_events)\n')
sub('`ifdef NO_SOUND\n// nand2mario: dummy values', '`ifdef NO_Z80\n// nand2mario: dummy values')
sub('\toutput [15:0] DAC_RDATA,\n',
    '\toutput [15:0] DAC_RDATA,\n'
    '\toutput        VRAM_REQ,\t\t// VDAC4: видеопамять VDP в SDRAM (16 бит: процессор и DMA)\n'
    '\tinput         VRAM_ACK,\n\toutput        VRAM_WE,\n\toutput  [1:0] VRAM_BE,\n'
    '\toutput [15:1] VRAM_A,\n\toutput [15:0] VRAM_D,\n\tinput  [15:0] VRAM_Q,\n'
    '\toutput        VRAM32_REQ,\t\t// 32 бита: выборка фона и спрайтов\n'
    '\tinput         VRAM32_ACK,\n\toutput [15:1] VRAM32_A,\n\tinput  [31:0] VRAM32_Q,\n'
    '\toutput        SND_PUSH,\t\t// VDAC4: события звука для ESP (md_snd_events)\n'
    '\toutput [31:0] SND_WORD,\n'
    '\toutput [19:0] SND_TIME,\t\t// время YM (такты синтеза): по нему ESP ведёт звук и без событий\n')
sub('wire signed [15:0] fm_adjust_l',
    '`ifdef NO_SOUND\n'
    '// VDAC4: вместо YM2612 — таймеры и флаги для чтения обратно; записи в порты YM и PSG уходят на ESP.\n'
    'wire fm_synth;\n'
    'md_ym_stub fm\n(\n'
    '\t.clk(MCLK),\n\t.rst(~Z80_RESET_N),\n\t.cen(FM_CLKEN),\n'
    '\t.addr(ZBUS_A[1:0]),\n\t.wr(FM_SEL & ZBUS_WE),\n\t.din(ZBUS_DO),\n\t.dout(FM_DO),\n'
    '\t.synth(fm_synth)\n);\n\n'
    'md_snd_events snd_events\n(\n'
    '\t.clk(MCLK),\n\t.synth(fm_synth),\n\t.ym_rst(~Z80_RESET_N),\n'
    '\t.ym_wr(FM_SEL & ZBUS_WE),\n\t.ym_port(ZBUS_A[1:0]),\n\t.ym_data(ZBUS_DO),\n'
    '\t.psg_wr_n(MBUS_RNW | ~VDP_SEL | ~MBUS_A[4] | MBUS_A[3]),\n\t.psg_data(MBUS_DO[15:8]),\n'
    '\t.push(SND_PUSH),\n\t.word(SND_WORD),\n\t.stamp(SND_TIME)\n);\n\n'
    'assign DAC_LDATA = 0;\nassign DAC_RDATA = 0;\n'
    '`endif\n\n'
    'wire signed [15:0] fm_adjust_l')

# 2. видеопамять — в SDRAM
i = src.index(b'dpram #(14) vram_l1')
j = src.index(b'always @(posedge MCLK) vram32_ack <= vram32_req;')
j = src.index(b'\r\n', j) + 2
src = src[:i] + (
    '// VDAC4: видеопамять — в SDRAM (md_sdram, банк 3), порты наружу\n'
    'assign VRAM_REQ = vram_req;\nassign VRAM_WE = vram_we;\nassign VRAM_BE = ~{vram_u_n, vram_l_n};\n'
    'assign VRAM_A = vram_a;\nassign VRAM_D = vram_d;\nwire vram_ack = VRAM_ACK;\n'
    'assign VRAM32_REQ = vram32_req;\nassign VRAM32_A = vram32_a;\nassign vram32_q = VRAM32_Q;\n'
    'wire vram32_ack = VRAM32_ACK;\n').encode().replace(b'\n', b'\r\n') + src[j:]
sub('\t.vram_q(vram_a[1] ? vram_q2 : vram_q1),\n', '\t.vram_q(VRAM_Q),\n')

# 3. пауза (меню F1): стоят 68000, Z80 и таймеры YM, а VDP рисует дальше — кадрового буфера нет,
#    картинку под меню даёт сам VDP. Прерывание кадра 68000 после паузы получит одно (как после
#    долгого DMA). У mdtang наоборот: VDP стоял (CE ~pause_reg), Z80 и FM шли.
sub('\t\t\t\tZ80_CLKENn <= 1; // ~PAUSE_EN;\n', '\t\t\t\tZ80_CLKENn <= ~PAUSE_EN;\t// VDAC4: на паузе Z80 стоит\n')
sub('\t\t\tZ80_CLKENp <= 1; // ~PAUSE_EN;\n', '\t\t\tZ80_CLKENp <= ~PAUSE_EN;\n')
sub('\t\t\tFM_CLKEN <= 1; // ~PAUSE_EN;\t\t// nand2mario: do not pause FM for frame sync\n',
    '\t\t\tFM_CLKEN <= ~PAUSE_EN;\t\t// VDAC4: таймеры YM на паузе стоят (звук — на ESP)\n')
sub('\t.CE(~pause_reg),\n', '\t.CE(1\'b1),\t\t\t// VDAC4: VDP на паузе рисует (картинка под меню)\n')

# 4. диагностика Z80 (03.11.2026, плата: в Jungle Book звук пропал, игра идёт; кольцо событий звука показало,
#    что Z80 перестал писать в YM — последние 1024 события: «заглушить PSG» раз в 312 мкс, потом тишина).
#    Наружу (LINK 54/55): PC последней выборки кода, сброс, захват шины 68000, ожидание, HALT, запрос и
#    захват шины 68000 самим Z80, банк окна 8000, счётчики выборок, записей в YM и записей Z80 в PSG.
sub('\toutput [19:0] SND_TIME,\t\t// время YM (такты синтеза): по нему ESP ведёт звук и без событий\n',
    '\toutput [19:0] SND_TIME,\t\t// время YM (такты синтеза): по нему ESP ведёт звук и без событий\n'
    '\toutput [63:0] Z80_DBG,\t\t// VDAC4: состояние Z80 для LINK 54/55 (диагностика на плате)\n')
sub('wire  [7:0] Z80_DO;\n', 'wire  [7:0] Z80_DO;\nwire        Z80_M1_N, Z80_HALT_N;\t// VDAC4: для диагностики (Z80_DBG)\n')
sub('\t.BUSRQ_n(Z80_BUSRQ_N),\n\t.BUSAK_n(Z80_BUSAK_N),\n',
    '\t.BUSRQ_n(Z80_BUSRQ_N),\n\t.BUSAK_n(Z80_BUSAK_N),\n\t.M1_n(Z80_M1_N),\n\t.HALT_n(Z80_HALT_N),\n')
sub('`endif\n\nendmodule',
    '`endif\n\n'
    '// VDAC4 (03.11.2026): состояние Z80 для LINK 54/55 — где стоит звуковой драйвер, если звук пропал.\n'
    '// [15:0] PC последней выборки кода (M1), [24:16] банк окна 8000, [25] захват шины 68000 у Z80 (BGACK_N),\n'
    '// [26] запрос шины 68000 (BR_N), [27] HALT_N, [28] WAIT_N, [29] BUSAK_N, [30] BUSRQ_N, [31] RESET_N;\n'
    '// [39:32] записи Z80 в PSG, [47:40] записи в YM (Z80 и 68000), [63:48] выборки кода.\n'
    'reg [15:0] dbg_pc = 0, dbg_m1 = 0;\nreg  [7:0] dbg_ym = 0, dbg_psg = 0;\nreg        dbg_m1_d = 0, dbg_ym_d = 0, dbg_psg_d = 0;\n'
    'wire       dbg_fetch = ~Z80_M1_N & ~Z80_MREQ_N;\n'
    'wire       dbg_psg_wr = ~(MBUS_RNW | ~VDP_SEL | ~MBUS_A[4] | MBUS_A[3]) & (msrc == MSRC_Z80);\n'
    'always @(posedge MCLK) begin\n'
    '\tdbg_m1_d <= dbg_fetch; dbg_ym_d <= FM_SEL & ZBUS_WE; dbg_psg_d <= dbg_psg_wr;\n'
    '\tif (dbg_fetch & ~dbg_m1_d) begin dbg_pc <= Z80_A; dbg_m1 <= dbg_m1 + 1\'d1; end\n'
    '\tif (FM_SEL & ZBUS_WE & ~dbg_ym_d) dbg_ym <= dbg_ym + 1\'d1;\n'
    '\tif (dbg_psg_wr & ~dbg_psg_d) dbg_psg <= dbg_psg + 1\'d1;\n'
    'end\n'
    'assign Z80_DBG = {dbg_m1, dbg_ym, dbg_psg,\n'
    '\tZ80_RESET_N, Z80_BUSRQ_N, Z80_BUSAK_N, ~Z80_MBUS_DTACK_N | ~Z80_ZBUS_DTACK_N | ~Z80_IO, Z80_HALT_N,\n'
    '\tZ80_BR_N, Z80_BGACK_N, BAR[23:15], dbg_pc};\n\n'
    'endmodule')

# 5. снимок F5/F6, часть 68000 (03.11.2026). Внутреннее состояние fx68k наружу не выведено, поэтому регистры
#    68000 сохраняет и загружает он сам: по запросу (SNAP_IRQ) ядро даёт прерывание уровня 7 (его не
#    замаскировать, принимается на границе команды), вектор $7C подменяется адресом $A12000 — памятью-вставкой
#    1К слов, видимой 68000 только во время снимка (SNAP_ACT; иначе там пусто, как без Sega CD). Обработчик (его
#    кладёт ESP) пишет D0–A6, USP и SSP в буфер вставки и ждёт команды в $A127FE (SNAP_CMD): 1 — вернуться, 2 —
#    взять регистры из буфера (их положил ESP), SSP и USP — оттуда же, и RTE: SR и PC — из кадра исключения на
#    стеке в ОЗУ, которое загружено вместе со снимком. Порт B вставки — ESP (LINK, md_link 5D/5E).
sub('\toutput [63:0] Z80_DBG,\t\t// VDAC4: состояние Z80 для LINK 54/55 (диагностика на плате)\n',
    '\toutput [63:0] Z80_DBG,\t\t// VDAC4: состояние Z80 для LINK 54/55 (диагностика на плате)\n'
    '\tinput         SNAP_ACT,\t\t// VDAC4 снимок: вставка $A12000-$A127FF видна 68000\n'
    '\tinput         SNAP_IRQ,\t\t// запрос прерывания уровня 7 (снят — следующий запрос снова даст уровень 7)\n'
    '\toutput        SNAP_TAKEN,\t\t// 68000 принял уровень 7 (до снятия SNAP_IRQ)\n'
    '\tinput  [15:0] SNAP_CMD,\t\t// команда обработчику: слово $A127FE\n'
    '\tinput   [9:0] SNAP_OVL_A,\t\t// порт B вставки (домен MCLK)\n'
    '\tinput         SNAP_OVL_WE,\n'
    '\tinput  [15:0] SNAP_OVL_D,\n'
    '\toutput [15:0] SNAP_OVL_Q,\n')
sub('\t\tif((~old_as & M68K_AS_N) || &scnt) begin\n\t\t\tif (M68K_VINT) M68K_IPL_N <= 3\'b001;',
    '\t\tif((~old_as & M68K_AS_N) || &scnt) begin\n\t\t\tif (snap_req) M68K_IPL_N <= 3\'b000;\t// VDAC4: снимок — уровень 7\n'
    '\t\t\telse if (M68K_VINT) M68K_IPL_N <= 3\'b001;')
sub('wire M68K_INTACK = &M68K_FC;\n',
    'wire M68K_INTACK = &M68K_FC;\n\n'
    '// VDAC4 снимок: уровень 7 — до подтверждения (цикл INTACK уровня 7), затем вектор $7C/$7E — адрес $A12000\n'
    'reg  snap_taken = 0, snap_vec = 0, snap_vec_lo = 0;\n'
    'wire snap_req = SNAP_IRQ & ~snap_taken;\n'
    'assign SNAP_TAKEN = snap_taken;\n'
    'always @(posedge MCLK) begin\n'
    '\tif (reset | ~SNAP_IRQ) begin snap_taken <= 0; snap_vec <= 0; snap_vec_lo <= 0; end\n'
    '\telse if (M68K_INTACK & ~M68K_AS_N & (M68K_A[3:1] == 3\'d7) & ~snap_taken) begin snap_taken <= 1; snap_vec <= 1; end\n'
    '\telse if (snap_vec) begin\n'
    '\t\tif (~M68K_AS_N & M68K_RNW & (M68K_A[23:1] == 23\'h3F)) snap_vec_lo <= 1;\t// читается $7E\n'
    '\t\telse if (snap_vec_lo & M68K_AS_N) begin snap_vec <= 0; snap_vec_lo <= 0; end\n'
    '\tend\n'
    'end\n\n'
    '// вставка: 1К слов; порт A — шина MBUS (состояние MBUS_OVL), порт B — ESP. Чтение — только без записи\n'
    '// (обычный режим DPB: «чтение до записи» Gowin в двухпортовой памяти не умеет, PA2122)\n'
    'reg  [15:0] snap_ovl [0:1023];\n'
    'reg  [15:0] snap_ovl_qa = 0, snap_ovl_qb = 0;\n'
    'reg         snap_ovl_wa = 0;\n'
    'always @(posedge MCLK) begin\n'
    '\tif (snap_ovl_wa) snap_ovl[MBUS_A[10:1]] <= MBUS_DO;\n'
    '\telse snap_ovl_qa <= snap_ovl[MBUS_A[10:1]];\n'
    'end\n'
    'always @(posedge MCLK) begin\n'
    '\tif (SNAP_OVL_WE) snap_ovl[SNAP_OVL_A] <= SNAP_OVL_D;\n'
    '\telse snap_ovl_qb <= snap_ovl[SNAP_OVL_A];\n'
    'end\n'
    'assign SNAP_OVL_Q = snap_ovl_qb;\n')
sub('\t\t\tMBUS_WAIT_FOR_S4  = 16;\n', '\t\t\tMBUS_WAIT_FOR_S4  = 16,\n\t\t\tMBUS_OVL          = 17;\t// VDAC4: вставка снимка\n')
sub('\t\tif (M68K_AS_N) M68K_MBUS_DTACK_N <= 1;\n', '\t\tif (M68K_AS_N) M68K_MBUS_DTACK_N <= 1;\n\t\tsnap_ovl_wa <= 0;\n')
sub('\t\t\t\t//SVP: A15000-A5000F \n',
    '\t\t\t\t//VDAC4 снимок: вставка A12000-A127FF — только во время снимка\n'
    '\t\t\t\telse if(SNAP_ACT && MBUS_A[23:11] == 13\'h1424) mstate <= MBUS_OVL;\n\n'
    '\t\t\t\t//SVP: A15000-A5000F \n')
sub('\t\tMBUS_SVP_READ:\n',
    '\t\tMBUS_OVL:\t\t\t// VDAC4: слово вставки (данные — тактом позже адреса) или команда $A127FE\n'
    '\t\t\tbegin\n'
    '\t\t\t\tif (~MBUS_RNW) snap_ovl_wa <= 1;\n'
    '\t\t\t\tdata <= &MBUS_A[10:1] ? SNAP_CMD : snap_ovl_qa;\n'
    '\t\t\t\tmstate <= MBUS_FINISH;\n'
    '\t\t\tend\n\n'
    '\t\tMBUS_SVP_READ:\n')
sub('\t\t\t\t\t\tM68K_MBUS_D <= data;\n',
    '\t\t\t\t\t\tM68K_MBUS_D <= (snap_vec && MBUS_A[23:2] == 22\'h1F) ? (MBUS_A[1] ? 16\'h2000 : 16\'h00A1) : data;\t// VDAC4: вектор снимка\n')

# 6. снимок F5/F6: Z80, ОЗУ Z80, банки, таймеры YM, VDP (03.11.2026).
#    Z80 останавливается сам, своим NMI (на Mega Drive NMI не подключён, игры его не ждут): доходит до
#    границы команды, кладёт PC в стек и идёт на $0066 — там вместо ОЗУ вставка: JR $ (18 FE), пока ESP
#    не отпустит, затем RETN (ED 45). Решение «стоять или отпустить» берётся только в начале выборки $0066,
#    поэтому JR и RETN не перемешаются. Пока Z80 крутится во вставке, его регистры (T80 REG) неподвижны,
#    кроме PC и R; ESP пишет PC = $0066. Загрузка: T80 держится в сбросе (SNAP_Z80_CTL[3]) всё время, пока
#    ESP пишет ОЗУ Z80 и линии; в такт снятия сброса — DIRSet: регистры из DIR, T80 начинает с M1 по
#    адресу PC ($0066 — снова вставка, затем RETN по сохранённому в ОЗУ PC). Сброс Z80 самой игрой
#    снимает вставку: иначе код игры по $0066 подменился бы.
#    Порт B ОЗУ Z80 — ESP (мост LINK). Банки (BAR, SSF2, SRAM) и таймеры YM — векторами SNAP_SYS/YM.
#    Таймеры YM (FM_CLKEN) стоят, пока идёт снимок (SNAP_ACT), как на паузе. Подтверждение уровня 7 не
#    доходит до VDP (иначе он снимет ожидающее VINT/HINT). VDP: регистры, ADDR, CODE, PENDING — отвод dbg.
sub('\tinput         SNAP_ACT,\t\t// VDAC4 снимок: вставка $A12000-$A127FF видна 68000\n',
    '\tinput         SNAP_ACT,\t\t// VDAC4 снимок: вставка $A12000-$A127FF видна 68000\n'
    '\tinput   [3:0] SNAP_Z80_CTL,\t// снимок Z80: [0] взвести вставку $0066, [1] отпустить (RETN), [2] NMI,\n'
    '\t\t\t\t\t\t\t\t// [3] держать T80 в сбросе, на снятии — DIRSet (загрузка регистров)\n'
    '\toutput  [1:0] SNAP_Z80_ST,\t// [0] Z80 стоит во вставке, [1] вставка ещё действует\n'
    '\toutput [211:0] SNAP_Z80_REG,\t// регистры T80 (REG)\n'
    '\tinput  [211:0] SNAP_Z80_DIR,\t// регистры для загрузки (DIR)\n'
    '\tinput  [12:0] SNAP_ZRAM_A,\t\t// порт B ОЗУ Z80 (MCLK)\n'
    '\tinput         SNAP_ZRAM_WE,\n'
    '\tinput   [7:0] SNAP_ZRAM_D,\n'
    '\toutput  [7:0] SNAP_ZRAM_Q,\n'
    '\toutput [63:0] SNAP_SYS_Q,\t\t// {18\'b0, BANK_REG[7..1], BANK_ROM, BANK_SRAM, BAR}\n'
    '\tinput  [63:0] SNAP_SYS_D,\n'
    '\toutput [63:0] SNAP_YM_Q,\t\t// таймеры YM (md_ym_stub)\n'
    '\tinput  [63:0] SNAP_YM_D,\n'
    '\tinput         SNAP_SYS_WE,\t\t// записать SNAP_SYS_D и SNAP_YM_D\n'
    '\tinput   [7:0] SNAP_VDP_A,\t\t// отвод VDP: 00-1f регистры, 20/21 ADDR, 22 {PENDING, CODE, ADDR[16]}\n'
    '\toutput  [7:0] SNAP_VDP_Q,\n')
sub('\t\t\tFM_CLKEN <= ~PAUSE_EN;\t\t// VDAC4: таймеры YM на паузе стоят (звук — на ESP)\n',
    '\t\t\tFM_CLKEN <= ~(PAUSE_EN | SNAP_ACT);\t// VDAC4: таймеры YM на паузе и в снимке стоят (звук — на ESP)\n')
sub('\t.INTACK(M68K_INTACK),\n', '\t.INTACK(M68K_INTACK & ~SNAP_IRQ),\t// VDAC4: подтверждение уровня 7 снимка — не VDP\n')
sub('\t.SPR_EN(SPR_EN)\n', '\t.SPR_EN(SPR_EN),\n\t.dbg_in(SNAP_VDP_A),\t\t// VDAC4: снимок\n\t.dbg_out(SNAP_VDP_Q)\n')
sub('\t\tBANK_REG <= \'{0,1,2,3,4,5,6,7};\n\tend\n\telse begin\n',
    '\t\tBANK_REG <= \'{0,1,2,3,4,5,6,7};\n\tend\n\telse begin\n'
    '\t\tif (SNAP_SYS_WE) begin\t\t\t\t// VDAC4: загрузка снимка\n'
    '\t\t\tBANK_SRAM <= SNAP_SYS_D[9]; BANK_ROM <= SNAP_SYS_D[10];\n'
    '\t\t\tBANK_REG[1] <= SNAP_SYS_D[15:11]; BANK_REG[2] <= SNAP_SYS_D[20:16]; BANK_REG[3] <= SNAP_SYS_D[25:21];\n'
    '\t\t\tBANK_REG[4] <= SNAP_SYS_D[30:26]; BANK_REG[5] <= SNAP_SYS_D[35:31]; BANK_REG[6] <= SNAP_SYS_D[40:36];\n'
    '\t\t\tBANK_REG[7] <= SNAP_SYS_D[45:41];\n'
    '\t\tend\n')
sub('\t.RESET_n(Z80_RESET_N),\n', '\t.RESET_n(Z80_RESET_N & ~snz_ld_rst),\t// VDAC4: снимок — сброс перед DIRSet\n')
sub('\t.NMI_n(\'1),\n', '\t.NMI_n(~SNAP_Z80_CTL[2]),\t\t// VDAC4: снимок — стоянка Z80\n'
    '\t.REG(SNAP_Z80_REG),\n\t.DIRSet(snz_dirset),\n\t.DIR(SNAP_Z80_DIR),\n')
sub('\t.DI((~Z80_MBUS_DTACK_N) ? Z80_MBUS_D : Z80_ZBUS_D),\n\t.DO(Z80_DO)\n);\n\n`else\ntv80s',
    '\t.DI(snz_hit ? snz_byte : (~Z80_MBUS_DTACK_N) ? Z80_MBUS_D : Z80_ZBUS_D),\t// VDAC4: вставка $0066\n'
    '\t.DO(Z80_DO)\n);\n\n`else\ntv80s')
sub('wire        Z80_M1_N, Z80_HALT_N;\t// VDAC4: для диагностики (Z80_DBG)\n',
    'wire        Z80_M1_N, Z80_HALT_N;\t// VDAC4: для диагностики (Z80_DBG)\n\n'
    '// VDAC4 снимок: стоянка Z80 во вставке $0066 (описание — prepare.py, правка 6)\n'
    'reg        snz_on = 0, snz_rel = 0, snz_park = 0, snz_arm_d = 0, snz_fetch_d = 0;\n'
    'wire       snz_fetch = ~Z80_M1_N & ~Z80_MREQ_N & ~Z80_RD_N;\t\t// выборка кода (M1)\n'
    'wire       snz_hit = snz_on & (Z80_A[15:1] == 15\'h0033);\t\t\t// $0066/$0067\n'
    'wire [7:0] snz_byte = Z80_A[0] ? (snz_rel ? 8\'h45 : 8\'hFE) : (snz_rel ? 8\'hED : 8\'h18);\n'
    'always @(posedge MCLK) begin\n'
    '\tsnz_arm_d <= SNAP_Z80_CTL[0]; snz_fetch_d <= snz_fetch;\n'
    '\tif (~Z80_RESET_N) snz_on <= 0;\t\t\t\t\t\t\t\t// сброс игрой — вставки нет\n'
    '\telse if (SNAP_Z80_CTL[0] & ~snz_arm_d) begin snz_on <= 1; snz_rel <= 0; snz_park <= 0; end\n'
    '\telse if (snz_on & snz_fetch & ~snz_fetch_d) begin\t\t\t\t// начало выборки\n'
    '\t\tif (Z80_A == 16\'h0066) begin snz_rel <= SNAP_Z80_CTL[1]; snz_park <= 1; end\n'
    '\t\telse if (snz_rel & (Z80_A != 16\'h0067)) snz_on <= 0;\t\t\t// RETN выполнен — вставка снята\n'
    '\tend\n'
    'end\n'
    'assign SNAP_Z80_ST = {snz_on, snz_park};\n\n'
    '// загрузка регистров: SNAP_Z80_CTL[3] держит T80 в сбросе; в такт снятия — DIRSet (на следующем фронте\n'
    '// регистры берутся из DIR, TState ещё 0 — T80 начнёт с выборки по PC)\n'
    'reg  [2:0] snz_ld_s = 0;\n'
    'reg        snz_dirset = 0;\n'
    'wire       snz_ld_rst = snz_ld_s[2];\n'
    'always @(posedge CLK_Z80) begin\n'
    '\tsnz_ld_s <= {snz_ld_s[1:0], SNAP_Z80_CTL[3]};\n'
    '\tsnz_dirset <= snz_ld_s[2] & ~snz_ld_s[1];\n'
    'end\n')
sub('\t.q_a(ZRAM_DO)\n', '\t.q_a(ZRAM_DO),\n'
    '\t.address_b(SNAP_ZRAM_A),\t\t// VDAC4: снимок — ESP\n\t.data_b(SNAP_ZRAM_D),\n\t.wren_b(SNAP_ZRAM_WE),\n\t.q_b(SNAP_ZRAM_Q)\n')
sub('\tif (reset) BAR <= 0;\n', '\tif (reset) BAR <= 0;\n\telse if (SNAP_SYS_WE) BAR <= SNAP_SYS_D[8:0];\t// VDAC4: загрузка снимка\n')
sub('\t.dout(FM_DO),\n\t.synth(fm_synth)\n',
    '\t.dout(FM_DO),\n\t.synth(fm_synth),\n\t.st_we(SNAP_SYS_WE),\t\t\t// VDAC4: снимок\n\t.st_d(SNAP_YM_D),\n\t.st_q(SNAP_YM_Q)\n')
sub('\tZ80_BR_N, Z80_BGACK_N, BAR[23:15], dbg_pc};\n',
    '\tZ80_BR_N, Z80_BGACK_N, BAR[23:15], dbg_pc};\n'
    'assign SNAP_SYS_Q = {18\'b0, BANK_REG[7], BANK_REG[6], BANK_REG[5], BANK_REG[4], BANK_REG[3], BANK_REG[2], BANK_REG[1],\n'
    '\tBANK_ROM, BANK_SRAM, BAR};\n')

# 7. синхронизация кадра (ядро 0.8, 04.11.2026; пользователь: «нужно приведение сигнала к одному из стандартных»).
#    Развёртка HDMI — стандарт 720p60/720p50 (md_video.sv), Mega Drive на 54 МГц быстрее неё; md_video, дописав
#    строку 0 кадра VDP, держит SYNC_STALL, пока развёртка не дойдёт до нужной строки. На стоянке стоят 68000, Z80,
#    таймеры YM (как на паузе меню) и VDP — CE, снятый по границе такта 68000 (sync_reg, как pause_reg; mdtang
#    так и делал: .CE(~pause_reg), PAUSE_EN — от framebuffer_sync). Пауза меню VDP не трогает (картинка под меню).
sub('\tinput         PAUSE_EN,\n',
    '\tinput         PAUSE_EN,\n'
    '\tinput         SYNC_STALL,\t\t// VDAC4: стоянка для синхронизации кадра с развёрткой HDMI (md_video)\n')
sub('reg pause_reg;\t\t\t\t// PAUSE_EN registered on whole m68k cycle\n',
    'reg pause_reg;\t\t\t\t// PAUSE_EN registered on whole m68k cycle\n'
    'reg sync_reg = 0;\t\t\t// VDAC4: SYNC_STALL на такт 68000 — VDP стоит вместе с процессорами\n'
    'wire hold_en = PAUSE_EN | SYNC_STALL;\n')
sub('\t\t\t\tZ80_CLKENn <= ~PAUSE_EN;\t// VDAC4: на паузе Z80 стоит\n',
    '\t\t\t\tZ80_CLKENn <= ~hold_en;\t// VDAC4: на паузе и стоянке Z80 стоит\n')
sub('\t\t\tZ80_CLKENp <= ~PAUSE_EN;\n', '\t\t\tZ80_CLKENp <= ~hold_en;\n')
sub('\t\t\tM68K_CLKENp <= ~PAUSE_EN;\n\t\t\tpause_reg <= PAUSE_EN;\n',
    '\t\t\tM68K_CLKENp <= ~hold_en;\n\t\t\tpause_reg <= hold_en;\n\t\t\tsync_reg <= SYNC_STALL;\n')
sub('\t\t\tFM_CLKEN <= ~(PAUSE_EN | SNAP_ACT);\t// VDAC4: таймеры YM на паузе и в снимке стоят (звук — на ESP)\n',
    '\t\t\tFM_CLKEN <= ~(hold_en | SNAP_ACT);\t// VDAC4: таймеры YM на паузе, стоянке и в снимке стоят (звук — на ESP)\n')
sub('\t.CE(1\'b1),\t\t\t// VDAC4: VDP на паузе рисует (картинка под меню)\n',
    '\t.CE(~sync_reg),\t\t// VDAC4: на паузе меню VDP рисует (картинка под меню), на стоянке синхронизации — стоит\n')

# копии upstream с отводами для снимка — в fpga/core (build.tcl и модель берут их вместо upstream)
def copy_patched(rel, pairs):
    b = (ROOT / 'upstream/src' / rel).read_bytes()
    for old, new in pairs:
        o = old.encode().replace(b'\n', b'\r\n'); n = new.encode().replace(b'\n', b'\r\n')
        if b.count(o) != 1:
            raise SystemExit(f'prepare.py: {rel}: найдено {b.count(o)}: {o[:60]!r}')
        b = b.replace(o, n)
    dst = ROOT / 'fpga/core' / Path(rel).name
    dst.write_bytes(b)
    print('prepare.py:', dst.relative_to(ROOT), len(b), 'байт')

copy_patched('vdp/vdp.v', [
    ('   output reg [7:0] dbg_out\n', '   output     [7:0] dbg_out          // VDAC4 снимок: 00-1f регистры, 20/21 ADDR, 22 {PENDING, CODE, ADDR[16]}\n'),
    ('   assign dbg_out = REG[dbg_in[4:0]];\n', ''),
    ('                                    \nendmodule',
     '                                    \n'
     '   // VDAC4 (03.11.2026): отвод для снимка F5/F6 — регистры и состояние порта управления\n'
     '   assign dbg_out = ~dbg_in[5] ? REG[dbg_in[4:0]] : dbg_in[1:0] == 2\'d0 ? ADDR[7:0] :\n'
     '                    dbg_in[1:0] == 2\'d1 ? ADDR[15:8] : {PENDING, CODE, ADDR[16]};\n\n'
     'endmodule'),
])
copy_patched('t80/t80s.v', [
    ('   output [7:0]  DO\n);', '   output [7:0]  DO,\n'
     '   output [211:0] REG,      // VDAC4 снимок: регистры T80 (порядок — t80.v)\n'
     '   input         DIRSet,\n   input [211:0] DIR\n);'),
    ('      .IntCycle_n(IntCycle_n)\n', '      .IntCycle_n(IntCycle_n), .REG(REG), .DIRSet(DIRSet), .DIR(DIR)\n'),
])

out = ROOT / 'fpga/core/system.sv'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(src)
print('prepare.py:', out.relative_to(ROOT), len(src), 'байт')
