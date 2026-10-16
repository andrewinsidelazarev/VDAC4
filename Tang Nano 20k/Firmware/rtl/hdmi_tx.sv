// SPDX-License-Identifier: Apache-2.0
//
// Слова TMDS трёх каналов: DVI (HDMI = 0) или HDMI с пакетом AVI InfoFrame (HDMI = 1).
//
// ЗАЧЕМ HDMI. В DVI приёмник не знает, что за сигнал, и телевизор решает сам: считает RGB
// видео-диапазоном 16–235 («переконтраст»), обрезает края кадра (overscan) и включает свою
// обработку (динамическая яркость). AVI InfoFrame говорит прямо: RGB полного диапазона
// (Q = 2), компьютерный источник (ITC = 1) типа «игра» (CN = 3), картинка без обрезки
// (S = 2, underscan), формат кадра (VIC). Что из этого выполнит конкретный телевизор —
// решает он; диапазон RGB телевизоры обычно учитывают.
//
// СТРОЕНИЕ HDMI (HDMI 1.4, раздел 5.2). Видео, синхроимпульсы и цвета — те же, что в DVI;
// добавляются:
//   - перед каждой активной строкой — преамбула видео (8 тактов, CTL0..3 = 1,0,0,0) и
//     защитная полоса (2 такта); чтобы успеть их выдать, видео задерживается на 10 тактов;
//   - раз в кадр, по фронту кадрового импульса (в гашении), — островок данных: преамбула
//     (8 тактов, CTL = 1,0,1,0), защитная полоса (2), пакет AVI InfoFrame (32 такта, TERC4),
//     защитная полоса (2). Канал 0 в островке несёт HSYNC и VSYNC, бит заголовка пакета
//     и признак первого такта; каналы 1 и 2 — по два бита четырёх подпакетов за такт.
// Заголовок (24 бита) и подпакеты (по 56 бит) защищены кодом BCH с образующим
// 1 + x⁶ + x⁷ + x⁸; пакет постоянный и считается при сборке.
//
// Задержка слов: HDMI — 11 тактов от входа (10 на преамбулу и 1 в кодере), DVI — 1.
//
module hdmi_tx #(
    parameter bit         HDMI     = 1'b1,
    parameter bit         VS_HIGH  = 1'b1,     // активный уровень VSYNC на входе
    parameter logic [7:0] VIC      = 8'd19,    // 19 — 1280×720p50; 17 — 720×576p50 4:3
    parameter logic [1:0] PIC_AR   = 2'b10     // формат кадра: 1 — 4:3, 2 — 16:9
) (
    input  wire       pixel_clk,
    input  wire       reset_n,
    input  wire [7:0] r,
    input  wire [7:0] g,
    input  wire [7:0] b,
    input  wire       de,
    input  wire       hs,                    // уровни как на выходе (полярность уже учтена)
    input  wire       vs,
    output wire [9:0] tmds_blue,
    output wire [9:0] tmds_green,
    output wire [9:0] tmds_red
);
    // ------------------------------------------------------------------
    // Пакет AVI InfoFrame (CEA-861, версия 2, 13 байтов)
    // ------------------------------------------------------------------
    localparam logic [7:0] HB0 = 8'h82, HB1 = 8'h02, HB2 = 8'h0D;
    localparam logic [7:0] PB1 = {1'b0, 2'b00, 1'b0, 2'b00, 2'b10};   // RGB; S = 2: underscan
    localparam logic [7:0] PB2 = {2'b00, PIC_AR, 4'b1000};            // кадр PIC_AR, активное = кадр
    localparam logic [7:0] PB3 = {1'b1, 3'b000, 2'b10, 2'b00};        // ITC = 1; Q = 2: полный 0–255
    localparam logic [7:0] PB4 = {1'b0, VIC[6:0]};
    localparam logic [7:0] PB5 = {2'b00, 2'b11, 4'b0000};             // CN = 3: игра; повтора точек нет
    localparam logic [7:0] SUM = HB0 + HB1 + HB2 + PB1 + PB2 + PB3 + PB4 + PB5;
    localparam logic [7:0] PB0 = 8'd0 - SUM;                          // контрольная сумма

    // BCH: ECC по битам, младший бит первого байта — первым
    function automatic [7:0] bch(input [55:0] data, input integer nbits);
        reg [7:0] e;
        integer i;
        begin
            e = 8'h00;
            for (i = 0; i < nbits; i = i + 1)
                e = (e >> 1) ^ ((e[0] ^ data[i]) ? 8'b1000_0011 : 8'h00);
            bch = e;
        end
    endfunction

    localparam logic [23:0] HDR24 = {HB2, HB1, HB0};
    localparam logic [31:0] HDR   = {bch({32'd0, HDR24}, 24), HDR24};
    localparam logic [55:0] SP0D  = {8'h00, PB5, PB4, PB3, PB2, PB1, PB0};   // PB0…PB6
    localparam logic [63:0] SP0   = {bch(SP0D, 56), SP0D};
    // Подпакеты 1…3 — нули (PB7…PB27); их ECC тоже 0

    function automatic [9:0] terc4(input [3:0] d);
        case (d)
            4'h0: terc4 = 10'b1010011100;
            4'h1: terc4 = 10'b1001100011;
            4'h2: terc4 = 10'b1011100100;
            4'h3: terc4 = 10'b1011100010;
            4'h4: terc4 = 10'b0101110001;
            4'h5: terc4 = 10'b0100011110;
            4'h6: terc4 = 10'b0110001110;
            4'h7: terc4 = 10'b0100111100;
            4'h8: terc4 = 10'b1011001100;
            4'h9: terc4 = 10'b0100111001;
            4'hA: terc4 = 10'b0110011100;
            4'hB: terc4 = 10'b1011000110;
            4'hC: terc4 = 10'b1010001110;
            4'hD: terc4 = 10'b1001110001;
            4'hE: terc4 = 10'b0101100011;
            default: terc4 = 10'b1011000011;
        endcase
    endfunction

    localparam logic [9:0] VGB_02 = 10'b1011001100;   // защитная полоса видео, каналы 0 и 2
    localparam logic [9:0] GB_1   = 10'b0100110011;   // канал 1 видео; каналы 1 и 2 островка

    generate if (!HDMI) begin : g_dvi
        tmds_encoder enc_blue (
            .pixel_clk(pixel_clk), .reset_n(reset_n), .video_data(b),
            .control_data({vs, hs}), .data_enable(de), .tmds(tmds_blue)
        );
        tmds_encoder enc_green (
            .pixel_clk(pixel_clk), .reset_n(reset_n), .video_data(g),
            .control_data(2'b00), .data_enable(de), .tmds(tmds_green)
        );
        tmds_encoder enc_red (
            .pixel_clk(pixel_clk), .reset_n(reset_n), .video_data(r),
            .control_data(2'b00), .data_enable(de), .tmds(tmds_red)
        );
    end else begin : g_hdmi
        // ------------------------------------------------------------------
        // Видео на 10 тактов позже входа
        // ------------------------------------------------------------------
        localparam int D = 10;
        reg [26:0] dl [0:D-1] /* synthesis syn_srlstyle = "registers" */;
        initial begin : dl_init                  // как у триггеров ПЛИС после загрузки
            integer i;
            for (i = 0; i < D; i = i + 1) dl[i] = '0;
        end
        always_ff @(posedge pixel_clk) begin : delay
            integer i;
            dl[0] <= {r, g, b, de, hs, vs};
            for (i = 1; i < D; i = i + 1) dl[i] <= dl[i-1];
        end
        wire [7:0] dr = dl[D-1][26:19], dg = dl[D-1][18:11], db = dl[D-1][10:3];
        wire       dde = dl[D-1][2], dhs = dl[D-1][1], dvs = dl[D-1][0];

        // ------------------------------------------------------------------
        // Преамбула и защитная полоса видео: с фронта DE на входе
        // ------------------------------------------------------------------
        reg       de_q, vs_q;
        reg [3:0] vp;                            // 9…1 — такты после фронта DE на входе
        wire      de_rise = de && !de_q;
        wire      v_pre   = de_rise || vp >= 4'd3;
        wire      v_guard = vp == 4'd2 || vp == 4'd1;

        // Островок данных: с фронта кадрового импульса на входе
        wire      vs_rise = (vs == VS_HIGH) && (vs_q != VS_HIGH);
        reg [5:0] di;                            // такт островка 0…43
        reg       di_on;
        wire      d_pre   = di_on && di < 6'd8;
        wire      d_guard = di_on && (di == 6'd8 || di == 6'd9 || di == 6'd42 || di == 6'd43);
        wire      d_data  = di_on && di >= 6'd10 && di < 6'd42;
        wire [4:0] k      = di - 6'd10;          // такт пакета 0…31

        always_ff @(posedge pixel_clk or negedge reset_n) begin
            if (!reset_n) begin
                de_q <= 1'b0; vs_q <= !VS_HIGH; vp <= '0; di <= '0; di_on <= 1'b0;
            end else begin
                de_q <= de;
                vs_q <= vs;
                if (de_rise)         vp <= 4'd9;
                else if (vp != 4'd0) vp <= vp - 4'd1;
                if (vs_rise) begin
                    di_on <= 1'b1; di <= '0;
                end else if (di_on) begin
                    if (di == 6'd43) di_on <= 1'b0;
                    di <= di + 6'd1;
                end
            end
        end

        // Кодеры TMDS: видео и управляющие символы; CTL каналов 1 и 2 — в преамбулах
        wire [9:0] e_blue, e_green, e_red;
        tmds_encoder enc_blue (
            .pixel_clk(pixel_clk), .reset_n(reset_n), .video_data(db),
            .control_data({dvs, dhs}), .data_enable(dde), .tmds(e_blue)
        );
        tmds_encoder enc_green (
            .pixel_clk(pixel_clk), .reset_n(reset_n), .video_data(dg),
            .control_data((v_pre || d_pre) ? 2'b01 : 2'b00), .data_enable(dde), .tmds(e_green)
        );
        tmds_encoder enc_red (
            .pixel_clk(pixel_clk), .reset_n(reset_n), .video_data(dr),
            .control_data(d_pre ? 2'b01 : 2'b00), .data_enable(dde), .tmds(e_red)
        );

        // Символы вместо кодеров: защитные полосы и TERC4 островка — в тот же такт кодера
        reg       ov;
        reg [9:0] ov_blue, ov_green, ov_red;
        always_ff @(posedge pixel_clk or negedge reset_n) begin
            if (!reset_n) begin
                ov <= 1'b0; ov_blue <= '0; ov_green <= '0; ov_red <= '0;
            end else begin
                ov <= v_guard || d_guard || d_data;
                if (v_guard) begin
                    ov_blue <= VGB_02; ov_green <= GB_1; ov_red <= VGB_02;
                end else if (d_guard) begin
                    ov_blue <= terc4({2'b11, dvs, dhs}); ov_green <= GB_1; ov_red <= GB_1;
                end else begin
                    ov_blue  <= terc4({k != 5'd0, HDR[k], dvs, dhs});
                    ov_green <= terc4({3'b000, SP0[{k, 1'b0}]});
                    ov_red   <= terc4({3'b000, SP0[{k, 1'b1}]});
                end
            end
        end
        assign tmds_blue  = ov ? ov_blue  : e_blue;
        assign tmds_green = ov ? ov_green : e_green;
        assign tmds_red   = ov ? ov_red   : e_red;
    end endgenerate
endmodule
