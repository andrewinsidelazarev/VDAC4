// Стенд Verilator ядра видео целиком (tb/vsys/vvid_main.cpp): ts_fb_core как в ts_fb_hdmi_top с
// HAS_EVO = 0, HAS_FT = 0, HAS_VIDEO = 1 (720p50, SCALE, HDMI, SDRAM на clk_mem), SDRAM —
// потактовая модель sdram_cyc, хост Evo по SPI и такты — из C++. Калибровки выборки SDRAM нет
// (CALIBRATE = 0, точка 1, как найдена на плате), flash Tang — модель чтения в vvid_main (без
// --settings — чистая: настройки DEF_*, 4:3, 100 %), ESP и BL616 молчат. M_SEL — вход blank
// (1 — режим FT812); тишина до возврата в слот 0 — 2 мс вместо 500 (vvid_main --boot).
//
// Наружу, кроме выводов, — отводы домена clk_mem: буфер вывода кадра FT, объявленный буфер,
// слоты темпа и отменённые кадры ядра видео.
module vvid_top (
    input  wire        clk_mem,
    input  wire        pixel_clk,
    input  wire        rst_n,
    input  wire        ft_sck,
    input  wire        ft_cs_n,
    input  wire        ft_mosi,
    output wire        ft_miso,
    input  wire        blank,          // M_SEL = 1
    output wire        boot_req,       // ядро просит назад в слот 0
    output wire [7:0]  vid_r,
    output wire [7:0]  vid_g,
    output wire [7:0]  vid_b,
    output wire        vid_de,
    output wire        vid_vs,
    output wire        flash_sck,      // flash Tang (настройки слота 0) — модель в vvid_main
    output wire        flash_cs_n,
    output wire        flash_mosi,
    input  wire        flash_miso,
    // отводы
    output wire        mem_ready,
    output wire [1:0]  fb_front,       // буфер кадра FT, который выводится
    output wire        fb_showing,
    output wire        pub_v,
    output wire [1:0]  pub_buf,        // объявленный кадр
    output wire [10:0] pub_w,
    output wire [9:0]  pub_h,
    output wire [31:0] st_slots,       // слотов темпа ядра видео
    output wire [15:0] st_aborts       // отменённых кадров
);
    localparam int OUT_W = 1280, OUT_H = 720;

    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;

    ts_fb_core #(
        .OUT_W(OUT_W), .OUT_H(OUT_H), .IMG_X((OUT_W - 720) / 2), .IMG_Y((OUT_H - 576) / 2),
        .H_FP(440), .H_SYNC(40), .H_BP(220), .V_FP(5), .V_SYNC(5), .V_BP(20),
        .HS_NEG(1'b0), .VS_NEG(1'b0), .PIX_KHZ(74_250), .MEM_KHZ(61_875),
        .SDRAM_INIT_US(20), .DEF_SRC(2'd0), .DEF_RD(2'd1), .CALIBRATE(1'b0),
        .SCALE(1'b1), .HDMI(1'b1), .FLASH_BOOT_US(20),
        .BOOT_SLOT(8'd3), .BOOT_NEXT(8'd0), .HAS_EVO(1'b0), .HAS_FT(1'b0), .HAS_VIDEO(1'b1),
        .SWITCH_IDLE_MS(2), .SD_FAST(1'b0)
    ) core (
        .clk_cap(1'b0), .rst_cap_n(1'b0),
        .clk_mem(clk_mem), .rst_mem_n(rst_n),
        .pixel_clk(pixel_clk), .rst_pix_n(rst_n),
        .clk_sd(1'b0), .rst_sd_n(1'b0),
        .blank(blank), .btn(2'b00), .boot_req(boot_req), .cap_clk_phase(),
        .link_sck(1'b0), .link_cs_n(1'b1), .link_io_in(4'hF), .link_io_out(), .link_io_oe(),
        .ft_sck(ft_sck), .ft_cs_n(ft_cs_n), .ft_mosi(ft_mosi), .ft_miso(ft_miso), .ft_int_n(),
        .cp_sck(1'b0), .cp_cs_n(1'b1), .cp_mosi(1'b0), .cp_miso(), .cp_irq_n(),
        .flash_sck(flash_sck), .flash_cs_n(flash_cs_n), .flash_mosi(flash_mosi), .flash_miso(flash_miso),
        .sd_cs_n(), .sd_sck(), .sd_mosi(), .sd_miso(1'b1),
        .mux_d(8'd0), .hsync_in(1'b0), .vsync_in(1'b0), .mux_sel(),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n), .sdram_we_n(sd_we_n),
        .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in), .sdram_dly(),
        .sdram_dly_df(1'b0), .sdram_phase(),
        .vid_r(vid_r), .vid_g(vid_g), .vid_b(vid_b), .vid_de(vid_de), .vid_hs(), .vid_vs(vid_vs),
        .tmds_blue(), .tmds_green(), .tmds_red(),
        .cap_overflow(), .frames_written(), .frames_rejected(), .fetch_late(),
        .have_frame(), .mem_ready(mem_ready), .raster_locked(), .raster_mode(),
        .raster_line_cycles(), .raster_lines(), .raster_hs_cycles(),
        .aligned_h(), .aligned_v(), .bb_valid(), .bb_x0(), .bb_x1(), .bb_y0(), .bb_y1(),
        .pattern_on(), .chk_err(), .chk_tick()
    );

    sdram_cyc #(.CL(2)) sdram (
        .clk(~clk_mem), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm),
        .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe), .dq_to_ctrl(sd_dq_in)
    );

    assign fb_front   = core.ft_front;
    assign fb_showing = core.ft_showing;
    assign pub_v      = core.ft_pub_v;
    assign pub_buf    = core.ft_pub_buf;
    assign pub_w      = core.ft_pub_w;
    assign pub_h      = core.ft_pub_h;
    assign st_slots   = core.rd_status;
    assign st_aborts  = core.rd_time;
endmodule
