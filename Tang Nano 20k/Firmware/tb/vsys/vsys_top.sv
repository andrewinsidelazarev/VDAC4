// Стенд Verilator (tb/vsys): ядро FT812 слота 1 целиком — ts_fb_core как в ts_fb_hdmi_top с
// HAS_EVO = 0 (720p60, SCALE, HDMI), SDRAM — потактовая модель, хост Evo по SPI и такты — из C++
// (vsys_main.cpp). Калибровки выборки SDRAM нет (CALIBRATE = 0, точка 1, как найдена на плате),
// flash Tang чистая (настройки — DEF_*), ESP и BL616 молчат.
//
// Наружу, кроме выводов, — отводы для замеров (домен clk_mem): смены листа, кадры
// растеризатора, буфер вывода. VSYS_SD (scripts/run_vsys.sh, если в дереве есть sd_bridge.sv) —
// ядро с SD_FAST: контроллер SDRAM и модель — на clk_sd = 2 × clk_mem (CL 3).
module vsys_top #(
    parameter int H_FP = 110,         // 110 — 720p60 (ядро FT812 с 1.20), 440 — 720p50 (до 1.20)
`ifdef VSYS_SD
    parameter bit SD_FAST = 1'b1
`else
    parameter bit SD_FAST = 1'b0
`endif
) (
    input  wire        clk_mem,
    input  wire        clk_sd,         // 2 × clk_mem, фронты совпадают (только SD_FAST)
    input  wire        pixel_clk,
    input  wire        rst_n,
    input  wire        ft_sck,
    input  wire        ft_cs_n,
    input  wire        ft_mosi,
    output wire        ft_miso,
    output wire        ft_int_n,
    output wire        vid_vs,
    output wire        vid_de,
    // замеры
    output reg  [31:0] n_swaps,        // смен листа (apply_swap ядра FT812)
    output reg  [31:0] n_takes,        // кадров FT, взятых выводом (смена буфера вывода)
    output wire [15:0] r_frames,       // кадров растеризатора
    output wire [15:0] r_time,         // время последнего кадра, такты / 1024
    output wire [31:0] ft_frames,      // REG_FRAMES
    output wire [1:0]  ft_dlswap,      // REG_DLSWAP
    output wire [1:0]  fb_front,       // буфер кадра FT, который выводится
    output wire [1:0]  r_pub_buf,      // буфер последнего объявленного кадра растеризатора
    output wire        mem_ready,
    output wire        ramg_drop,      // FT_STAT 25: очередь RAM_G хоста переполнялась (липкий)
    output wire        ramg_late,      // FT_STAT 26: ответ на чтение RAM_G опоздал (липкий)
    // такты контроллера SDRAM (clk_mem или clk_sd), когда он занят: [0] вывод/захват (метка 00x),
    // [1] RAM_G (010), [2] тексели (100), [3] выгрузка плиток (101), [4] оверлеи (110),
    // [5] регенерация, [6] всего
    output reg  [31:0] busy [0:6],
    output wire        sd_fast,
    // такты clk_mem растеризатора по причинам (ядра с 1.24, VSYS_SD; у прочих — нули), см. ниже
    output reg  [31:0] rc [0:47]
);
    assign sd_fast = SD_FAST;
    // VSYS_768 (run_vsys.sh, если fb_scanout дерева знает OUT768) — вывод ядра с 1.41: 1366×768 RB,
    // 72 МГц (строка 1500, кадр 800); иначе — 720p60 (H_FP 110) или 720p50 (H_FP 440)
`ifdef VSYS_768
    localparam int OUT_W = 1366, OUT_H = 768;
    localparam int HFP = 14, HSY = 56, HBP = 64, VFP = 1, VSY = 3, VBP = 28, PKHZ = 72_000;
`else
    localparam int OUT_W = 1280, OUT_H = 720;
    localparam int HFP = H_FP, HSY = 40, HBP = 220, VFP = 5, VSY = 5, VBP = 20, PKHZ = 74_250;
`endif

    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;

    ts_fb_core #(
        .OUT_W(OUT_W), .OUT_H(OUT_H), .IMG_X((OUT_W - 720) / 2), .IMG_Y((OUT_H - 576) / 2),
        .H_FP(HFP), .H_SYNC(HSY), .H_BP(HBP), .V_FP(VFP), .V_SYNC(VSY), .V_BP(VBP),
        .HS_NEG(1'b0), .VS_NEG(1'b0), .PIX_KHZ(PKHZ), .MEM_KHZ(61_875),
        .SDRAM_INIT_US(20), .DEF_SRC(2'd0), .DEF_RD(2'd1), .CALIBRATE(1'b0),
        .SCALE(1'b1), .HDMI(1'b1), .FLASH_BOOT_US(20),
        .BOOT_SLOT(8'd1), .BOOT_NEXT(8'd0), .HAS_EVO(1'b0), .HAS_FT(1'b1)
`ifdef VSYS_SD
        , .SD_FAST(SD_FAST)
`endif
`ifdef VSYS_768
        , .OUT768(1'b1)
`endif
    ) core (
        .clk_cap(1'b0), .rst_cap_n(1'b0),
        .clk_mem(clk_mem), .rst_mem_n(rst_n),
        .pixel_clk(pixel_clk), .rst_pix_n(rst_n),
`ifdef VSYS_SD
        .clk_sd(clk_sd), .rst_sd_n(rst_n),
`endif
        .blank(1'b1), .btn(2'b00), .boot_req(), .cap_clk_phase(),
        .link_sck(1'b0), .link_cs_n(1'b1), .link_io_in(4'hF), .link_io_out(), .link_io_oe(),
        .ft_sck(ft_sck), .ft_cs_n(ft_cs_n), .ft_mosi(ft_mosi), .ft_miso(ft_miso), .ft_int_n(ft_int_n),
        .cp_sck(1'b0), .cp_cs_n(1'b1), .cp_mosi(1'b0), .cp_miso(), .cp_irq_n(),
        .flash_sck(), .flash_cs_n(), .flash_mosi(), .flash_miso(1'b1),
        .sd_cs_n(), .sd_sck(), .sd_mosi(), .sd_miso(1'b1),
        .mux_d(8'd0), .hsync_in(1'b0), .vsync_in(1'b0), .mux_sel(),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n), .sdram_we_n(sd_we_n),
        .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in), .sdram_dly(), .sdram_dly_df(1'b0),
        .vid_r(), .vid_g(), .vid_b(), .vid_de(vid_de), .vid_hs(), .vid_vs(vid_vs),
        .tmds_blue(), .tmds_green(), .tmds_red(),
        .cap_overflow(), .frames_written(), .frames_rejected(), .fetch_late(),
        .have_frame(), .mem_ready(mem_ready), .raster_locked(), .raster_mode(),
        .raster_line_cycles(), .raster_lines(), .raster_hs_cycles(),
        .aligned_h(), .aligned_v(), .bb_valid(), .bb_x0(), .bb_x1(), .bb_y0(), .bb_y1(),
        .pattern_on(), .chk_err(), .chk_tick()
    );

    // клок микросхемы — инверсия клока контроллера, как в ts_fb_hdmi_top
    wire sd_clk = SD_FAST ? ~clk_sd : ~clk_mem;
    sdram_cyc #(.CL(SD_FAST ? 3 : 2)) sdram (
        .clk(sd_clk), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm),
        .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe), .dq_to_ctrl(sd_dq_in)
    );

    // --- замеры (иерархические ссылки; ширина буферов у старых ядер — 1 бит) ---------------------
    assign r_frames  = core.g_render.render.frames;
    assign r_time    = core.g_render.render.frame_time;
    assign r_pub_buf = 2'(core.g_render.render.pub_buf);
    assign ft_frames = core.g_ft.ft.frames;
    assign ft_dlswap = core.g_ft.ft.dlswap_reg;
    assign ramg_drop = core.g_ft.ft.ramg_drop;
    assign ramg_late = core.g_ft.ft.ramg_late;
    wire [3:0] sd_state;
    wire [2:0] sd_tag;
    wire       sd_ref;
    wire       b_clk = SD_FAST ? clk_sd : clk_mem;
`ifdef VSYS_SD
    generate if (SD_FAST) begin : g_tap_fast
        // метка операции контроллера — тексель (своя очередь) или голова общей очереди в такт c_ack
        reg [2:0] tag_q;
        always @(posedge clk_sd)
            if (core.g_sd_fast.bridge.c_ack)
                tag_q <= core.g_sd_fast.bridge.c_tx ? 3'b100 : core.g_sd_fast.bridge.head[2:0];
        assign sd_state = 4'(core.g_sd_fast.sdram.ctl.state);
        assign sd_tag   = tag_q;
        assign sd_ref   = !core.g_sd_fast.sdram.ctl.sdram_ras_n && !core.g_sd_fast.sdram.ctl.sdram_cas_n &&
                          core.g_sd_fast.sdram.ctl.sdram_we_n;
    end else begin : g_tap
        assign sd_state = 4'(core.g_sd.sdram.state);
        assign sd_tag   = core.g_sd.sdram.op_tag;
        assign sd_ref   = !core.g_sd.sdram.sdram_ras_n && !core.g_sd.sdram.sdram_cas_n &&
                          core.g_sd.sdram.sdram_we_n;
    end endgenerate
`else
    assign sd_state = 4'(core.sdram.state);
    assign sd_tag   = core.sdram.op_tag;
    assign sd_ref   = core.sdram.sdram_ras_n == 1'b0 && core.sdram.sdram_cas_n == 1'b0 &&
                      core.sdram.sdram_we_n == 1'b1;      // команда REF в этот такт
`endif
    // сигналы моста SDRAM для счётчиков (ядро без моста — SD_FAST = 0: нули)
    wire br_ack, br_c_req, br_cq_room, br_h_ok, br_h_we, br_req, br_w_coll, br_w_pend, br_w_resv, br_we,
         br_wf_room, br_wr_take, br_cq_ne;
`ifdef VSYS_SD
    generate if (SD_FAST) begin : g_br
        assign br_ack = core.g_sd_fast.bridge.ack;
        assign br_c_req = core.g_sd_fast.bridge.c_req;
        assign br_cq_room = core.g_sd_fast.bridge.cq_room;
        assign br_h_ok = core.g_sd_fast.bridge.h_ok;
        assign br_h_we = core.g_sd_fast.bridge.h_we;
        assign br_req = core.g_sd_fast.bridge.req;
        assign br_w_coll = core.g_sd_fast.bridge.w_coll;
        assign br_w_pend = core.g_sd_fast.bridge.w_pend;
        assign br_w_resv = core.g_sd_fast.bridge.w_resv;
        assign br_we = core.g_sd_fast.bridge.we;
        assign br_wf_room = core.g_sd_fast.bridge.wf_room;
        assign br_wr_take = core.g_sd_fast.bridge.wr_take;
        assign br_cq_ne = core.g_sd_fast.bridge.cq_w_s != core.g_sd_fast.bridge.cq_r;
    end else begin : g_no_br
        assign {br_ack, br_c_req, br_cq_room, br_h_ok, br_h_we, br_req, br_w_coll, br_w_pend, br_w_resv,
                br_we, br_wf_room, br_wr_take, br_cq_ne} = 13'b0011000000100;
    end endgenerate
`else
    assign {br_ack, br_c_req, br_cq_room, br_h_ok, br_h_we, br_req, br_w_coll, br_w_pend, br_w_resv,
            br_we, br_wf_room, br_wr_take, br_cq_ne} = 13'b0011000000100;
`endif
    reg        in_ref;
    // S_IDLE = 4: состояния 5…8 (ACT, WRITE, READ, WAIT) — операция клиента или регенерация
    always @(posedge b_clk) begin
        if (!rst_n) begin
            for (int i = 0; i < 7; i++) busy[i] <= 32'd0;
            in_ref <= 1'b0;
        end else begin
            if (sd_ref) in_ref <= 1'b1;
            else if (sd_state == 4'd4) in_ref <= 1'b0;
            if (sd_state > 4'd4) begin
                busy[6] <= busy[6] + 32'd1;
                if (in_ref || sd_ref)        busy[5] <= busy[5] + 32'd1;
                else if (sd_tag[2:1] == 2'b00) busy[0] <= busy[0] + 32'd1;
                else if (sd_tag == 3'b010)   busy[1] <= busy[1] + 32'd1;
                else if (sd_tag == 3'b100)   busy[2] <= busy[2] + 32'd1;
                else if (sd_tag == 3'b101)   busy[3] <= busy[3] + 32'd1;
                else                         busy[4] <= busy[4] + 32'd1;
            end
        end
    end
    // --- растеризатор: на что уходят такты кадра ------------------------------------------------
    // Исполнитель (пока он занят кадром): каждый такт — ровно одна причина, по старшинству:
    //   1 точка выдана; 2 промах кэша текселей (P4 стоит); 3 корень сглаживания; 4 буфер точек
    //   (ft_rd_pix) не берёт; 5 промах палитры; 6 задание идёт, точки нет (смена строки, слив);
    //   без задания — по состоянию автомата: 7 ждёт обходчик (очередь пуста), 8 слово состояния,
    //   9 подготовка задания (SET0…SET5), 10 перенос — ждёт пустого конвейера, 11 начало плитки
    //   (ждёт свободный буфер плитки, очистка меток), 12 конец плитки, 13 SAVE/RESTORE.
    // 0 — такты кадра (R_RUN), 14 — кадр идёт, исполнитель свободен; обходчик: 15 слов листа
    // разобрано, 16 ждёт места в очереди заданий, 17 занят, 18 заданий, 21…24 — заданий BITMAPS,
    // RECTS, POINTS, состояния/CLEAR, 25 выборка пуста (переход в листе); 19 плиток, 20 заполнений
    // кэша текселей (пакет SDRAM), 26 исполнитель занят, 27 кадров начато, 28 брошено,
    // 30/31 — такты заполнения кэша: заявка ждёт приёма / данные идут.
    initial for (int i = 0; i < 48; i++) rc[i] = 32'd0;
`ifdef VSYS_SD
    wire        r_run  = 2'(core.g_render.render.rst_q) == 2'd1;
    wire        x_busy = core.g_render.render.exec.busy;
    wire        x_iss  = core.g_render.render.exec.issue;
    wire        x_miss = core.g_render.render.exec.p4_miss;
    wire        x_band = core.g_render.render.exec.p4_band && !core.g_render.render.exec.sq_done;
    wire        x_bp   = core.g_render.render.exec.p5_v && !core.g_render.render.exec.f_ready;
    wire        x_pal  = core.g_render.render.exec.q6_miss;
    wire        x_jrun = core.g_render.render.exec.j_run;
    wire [3:0]  x_st   = 4'(core.g_render.render.exec.xs_q);
    wire        x_qv   = core.g_render.render.exec.q_v;
    wire        k_go   = core.g_render.render.walk.d_go;
    wire        k_full = core.g_render.render.walk.x_v && !core.g_render.render.walk.x_none &&
                         core.g_render.render.walk.it_full;
    wire        k_busy = core.g_render.render.walk.busy;
    wire        k_itv  = core.g_render.render.walk.it_v;
    wire [2:0]  k_kind = core.g_render.render.walk.x_kind;
    wire        k_fqe  = core.g_render.render.walk.fq_n == 2'd0 && !core.g_render.render.walk.eol_wait;
    always @(posedge clk_mem) begin
        if (r_run) rc[0] <= rc[0] + 32'd1;
        if (core.g_render.render.start) rc[27] <= rc[27] + 32'd1;
        // брошен повтор (условие R_RUN в ft_render: повтор, хост просит смену листа)
        if (r_run && core.g_render.render.f_redo && core.g_render.render.swap_wait &&
            !core.g_render.render.start) rc[28] <= rc[28] + 32'd1;
        if (x_busy) begin
            rc[26] <= rc[26] + 32'd1;
            if (x_iss)        rc[1] <= rc[1] + 32'd1;
            else if (x_miss)  rc[2] <= rc[2] + 32'd1;
            else if (x_band)  rc[3] <= rc[3] + 32'd1;
            else if (x_bp)    rc[4] <= rc[4] + 32'd1;
            else if (x_pal)   rc[5] <= rc[5] + 32'd1;
            else if (x_jrun)  rc[6] <= rc[6] + 32'd1;
            else case (x_st)
                4'd2:  if (x_qv) rc[8] <= rc[8] + 32'd1; else rc[7] <= rc[7] + 32'd1;
                4'd5, 4'd6, 4'd7, 4'd8, 4'd9, 4'd10, 4'd14, 4'd15: rc[9] <= rc[9] + 32'd1;
                4'd11: rc[10] <= rc[10] + 32'd1;
                4'd1:  rc[11] <= rc[11] + 32'd1;
                4'd13: rc[12] <= rc[12] + 32'd1;
                4'd3, 4'd4: rc[13] <= rc[13] + 32'd1;
                default: rc[29] <= rc[29] + 32'd1;
            endcase
        end else if (r_run) rc[14] <= rc[14] + 32'd1;
        if (k_go)   rc[15] <= rc[15] + 32'd1;
        if (k_full) rc[16] <= rc[16] + 32'd1;
        if (k_busy) rc[17] <= rc[17] + 32'd1;
        if (k_busy && k_fqe) rc[25] <= rc[25] + 32'd1;
        if (k_itv) begin
            rc[18] <= rc[18] + 32'd1;
            case (k_kind)
                3'd1: rc[21] <= rc[21] + 32'd1;
                3'd2: rc[22] <= rc[22] + 32'd1;
                3'd3: rc[23] <= rc[23] + 32'd1;
                3'd0: rc[24] <= rc[24] + 32'd1;
                default: ;
            endcase
        end
        if (core.g_render.render.tile_go) rc[19] <= rc[19] + 32'd1;
        if (core.g_render.render.tx_ack)  rc[20] <= rc[20] + 32'd1;
        // заполнение кэша: 30 — заявка ждёт приёма, 31 — хоть одно место заполнения занято
        if (core.g_render.render.exec.tx_req && !core.g_render.render.exec.tx_ack) rc[30] <= rc[30] + 32'd1;
        if (core.g_render.render.exec.fs_any) rc[31] <= rc[31] + 32'd1;
        // выгрузка плиток: 32 — идёт (ft_rd_pix не в D_IDLE), 33 — запрос ждёт приёма, 34 — слов
        // отдано; мост: 35 — очередь команд полна, 36 — запись ждёт места или конца сбора прошлой
        if (3'(core.g_render.render.pix.dst_q) != 3'd0) rc[32] <= rc[32] + 32'd1;
        if (core.g_render.render.w_req && !core.g_render.render.w_ack) rc[33] <= rc[33] + 32'd1;
        if (core.g_render.render.pix.w_take) rc[34] <= rc[34] + (SD_FAST ? 32'd2 : 32'd1);
        if (!br_cq_room) rc[35] <= rc[35] + 32'd1;
        if (br_req && br_we && !br_ack &&
            !(br_wf_room && !br_w_pend && !br_wr_take))
            rc[36] <= rc[36] + 32'd1;
        // запись растеризатора (метка 101) ждёт приёма: 40 — идёт сбор прошлой записи, 41 — нет
        // места в FIFO записи, 42 — очередь команд полна, 43 — арбитр выбрал другого клиента
        if (core.g_render.render.w_req && !core.g_render.render.w_ack && !br_ack) begin
            if (core.sd_tag != 3'b101) rc[43] <= rc[43] + 32'd1;
            else if (br_w_coll || br_w_resv) rc[40] <= rc[40] + 32'd1;
            else if (!br_wf_room) rc[41] <= rc[41] + 32'd1;
            else if (!br_cq_room) rc[42] <= rc[42] + 32'd1;
        end
    end
    // мост, сторона clk_sd: 37 — голова очереди — чтение, ждёт места в FIFO чтения; 38 — запись,
    // ждёт слов; 39 — контроллер свободен, очередь пуста
    always @(posedge clk_sd) begin
        if (!br_c_req && br_cq_ne) begin
            if (!br_h_ok && !br_h_we) rc[37] <= rc[37] + 32'd1;
            if (!br_h_ok && br_h_we)  rc[38] <= rc[38] + 32'd1;
        end
        if (!br_c_req && !br_cq_ne)
            rc[39] <= rc[39] + 32'd1;
    end
`endif
    wire       act_dl = core.g_ft.ft.active_dl;
    wire [1:0] front  = 2'(core.fb.ft_front);
    assign fb_front = front;
    reg        act_dl_q;
    reg  [1:0] front_q;
    always @(posedge clk_mem) begin
        act_dl_q <= act_dl;
        front_q  <= front;
        if (!rst_n) begin
            n_swaps <= 32'd0;
            n_takes <= 32'd0;
        end else begin
            if (act_dl != act_dl_q) n_swaps <= n_swaps + 32'd1;
            if (front != front_q)   n_takes <= n_takes + 32'd1;
        end
    end
endmodule
