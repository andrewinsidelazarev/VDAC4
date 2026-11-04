// SPDX-License-Identifier: GPL-3.0-or-later
// Такты Mega Drive VDAC4 (03.11.2026). У GW2AR-18 в этом корпусе две PLL: эта и HDMI.
//   CLKOUT  54 МГц — MCLK и контроллер SDRAM (27 x 2; настоящий NTSC — 53,693, игра на 0,57 % быстрее)
//   CLKOUTD 27 МГц — такт Z80 (mdtang: половина MCLK, фронты — на фронтах MCLK)
//   CLKOUTP 54 МГц со сдвигом 135° — такт микросхемы SDRAM (md_sdram: время шины)
module md_pll_sys(input clkin, output clkout, output clkoutd, output clkoutp, output lock);
rPLL rpll_inst(.CLKOUT(clkout), .LOCK(lock), .CLKOUTP(clkoutp), .CLKOUTD(clkoutd), .CLKOUTD3(),
    .RESET(1'b0), .RESET_P(1'b0), .CLKIN(clkin), .CLKFB(1'b0),
    .FBDSEL(6'b0), .IDSEL(6'b0), .ODSEL(6'b0), .PSDA(4'b0), .DUTYDA(4'b0), .FDLY(4'b0));
defparam rpll_inst.FCLKIN = "27";
defparam rpll_inst.DYN_IDIV_SEL = "false";
defparam rpll_inst.IDIV_SEL = 0;
defparam rpll_inst.DYN_FBDIV_SEL = "false";
defparam rpll_inst.FBDIV_SEL = 1;
defparam rpll_inst.DYN_ODIV_SEL = "false";
defparam rpll_inst.ODIV_SEL = 16;
defparam rpll_inst.PSDA_SEL = "0110";      // 6 x 22,5° = 135° (md_sdram: окно данных чтения)
defparam rpll_inst.DYN_DA_EN = "false";
defparam rpll_inst.DUTYDA_SEL = "1000";
defparam rpll_inst.CLKOUT_FT_DIR = 1'b1;
defparam rpll_inst.CLKOUTP_FT_DIR = 1'b1;
defparam rpll_inst.CLKOUT_DLY_STEP = 0;
defparam rpll_inst.CLKOUTP_DLY_STEP = 0;
defparam rpll_inst.CLKFB_SEL = "internal";
defparam rpll_inst.CLKOUT_BYPASS = "false";
defparam rpll_inst.CLKOUTP_BYPASS = "false";
defparam rpll_inst.CLKOUTD_BYPASS = "false";
defparam rpll_inst.DYN_SDIV_SEL = 2;
defparam rpll_inst.CLKOUTD_SRC = "CLKOUT";
defparam rpll_inst.CLKOUTD3_SRC = "CLKOUT";
defparam rpll_inst.DEVICE = "GW2AR-18C";
endmodule
