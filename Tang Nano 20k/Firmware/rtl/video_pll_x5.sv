// SPDX-License-Identifier: Apache-2.0
// The selected 27/28 MHz pixel clock is multiplied by five for OSER10.
module video_pll_x5 (
    input  wire clkin,
    input  wire reset,
    output wire clkout,
    output wire lock
);
    wire unused_p, unused_d, unused_d3, gnd = 1'b0;
    rPLL pll (
        .CLKOUT(clkout), .LOCK(lock), .CLKOUTP(unused_p),
        .CLKOUTD(unused_d), .CLKOUTD3(unused_d3),
        .RESET(reset), .RESET_P(gnd), .CLKIN(clkin), .CLKFB(gnd),
        .FBDSEL(6'b0), .IDSEL(6'b0), .ODSEL(6'b0), .PSDA(4'b0),
        .DUTYDA(4'b0), .FDLY(4'b0)
    );
    defparam pll.FCLKIN = "27";
    defparam pll.IDIV_SEL = 3;
    defparam pll.FBDIV_SEL = 19;
    defparam pll.ODIV_SEL = 8;
    defparam pll.DYN_IDIV_SEL = "false";
    defparam pll.DYN_FBDIV_SEL = "false";
    defparam pll.DYN_ODIV_SEL = "false";
    defparam pll.PSDA_SEL = "0000";
    defparam pll.DYN_DA_EN = "false";
    defparam pll.DUTYDA_SEL = "1000";
    defparam pll.CLKFB_SEL = "internal";
    defparam pll.DEVICE = "GW2AR-18C";
endmodule
