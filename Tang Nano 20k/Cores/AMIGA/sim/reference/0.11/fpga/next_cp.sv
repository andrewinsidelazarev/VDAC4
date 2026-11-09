// SPDX-License-Identifier: Apache-2.0
//
// Связь ядра ZX Next на VDAC4 с BL616 — только чтобы вернуться в слот 0 (TS-Config). Ядро
// загрузил в SRAM ПЛИС сам BL616 (по JTAG, docs/next-core.md); прошивка BL616 опрашивает
// ПЛИС тем же протоколом, что у ядер VDAC4 (cp_spi, fpga/vdac2/rtl/cp_link.sv): по IRQ_N
// читает STATUS и, если в нём бит 7 флагов (перезагрузка), делает JTAG Reconfig — ПЛИС
// грузит слот 0 из flash Tang (адрес перехода в заголовке этого ядра — 0x000000).
// Посылки WRITE и READ_REQ BL616 здесь не нужны: принимаются и не исполняются; READ_DATA —
// нули. Снимок STATUS — как у cp_link: 0xC5, версия 1, флаги {перезагрузка, 0, 0, 0, 0, 0,
// очередь пуста, 0}, свободно в очереди — 512.
module next_cp (
    input  wire sck,
    input  wire cs_n,
    input  wire mosi,
    output wire miso,
    output wire irq_n,

    input  wire clk,
    input  wire boot                  // ядро просит перезагрузку (домен clk)
);
    reg  [1:0] boot_s;
    always @(posedge clk) boot_s <= {boot_s[0], boot};

    wire        tg;
    wire [1:0]  ev_kind;
    wire [31:0] ev_data;
    wire [8:0]  rb_addr;
    // Снимок — постоянный, кроме бита перезагрузки: cp_spi берёт его байтами по SCK.
    wire [127:0] st_vec = {16'd0, 6'd0, 10'd512, 8'd0, 8'd0, 16'd0, 16'd0, 16'd0, 8'd0,
                           {boot_s[1], 1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b1, 1'b0}, 8'h01, 8'hC5};

    cp_spi spi (
        .sck(sck), .cs_n(cs_n), .mosi(mosi), .miso(miso),
        .tg(tg), .ev_kind(ev_kind), .ev_data(ev_data),
        .st_vec(st_vec), .rb_addr(rb_addr), .rb_q(32'd0)
    );

    assign irq_n = !boot_s[1];
endmodule
