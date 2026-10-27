// GPL-3.0-or-later. Окно меню поверх картинки NES (VDAC4): текст пишет ESP (NES.ELF) через LINK.
// Окно — в пикселях кадра 1280×720 (счётчики cx/cy hdmi.sv): одинаково при 4:3 и 16:9 и лежит
// внутри картинки 4:3 (160…1119). Знак — ячейка 8×10 точек шрифта 5×7 (nes_osd_font.sv), точка —
// 2×2 пикселя; COLS×ROWS знаков.
//
// Текст: 512 слов (домен clk), строка r — слова r·16…r·16+15, по 4 знака на слово, младший байт —
// левый знак; байт — {цвет[1:0], знак ASCII − 0x20 [5:0]}; цвет 0 — белый, 1 — жёлтый,
// 2 — голубой, 3 — серый. sel — строка с подсветкой (вне 0…ROWS−1 — нет).
//
// Конвейер — три регистра: счётчики считают пиксель cx+3, тогда box/hl/text/text_rgb к фронту, на
// котором nes2hdmi_vdac4 защёлкивает rgb пикселя cx, относятся именно к cx — как данные BRAM.
//
// VDAC4 0.10 — оформление (всё — с начала кадра, когда значение устоялось):
//   band[r] — строка r на подложке (выход band, цвет — у nes2hdmi_vdac4): шапка, строка состояния;
//   rule[r] — в строке r тонкий разделитель (2 пикселя посередине ячейки) во всю ширину текста;
//   bar — полоса хода {вкл[31], строка[30:26], столбец[25:20], столбцов[19:14], 4'b0, заполнено[9:0]}:
//         рамка 2 пикселя (по высоте — пиксели 0…15 ячейки, справа — без пустых трёх точек последнего
//         знака), внутри с отступом 2 — заполненная часть «заполнено» пикселей высотой 8.
module nes_osd #(
    parameter COLS = 56, ROWS = 30,
    parameter PAD = 16,                          // поле от рамки до текста
    parameter WW = 2 * PAD + COLS * 16 - 6,      // справа нет пустых трёх точек последнего знака
    parameter WH = 2 * PAD + ROWS * 20 - 6,      // снизу — пустых трёх строк последней строки
    parameter WX0 = (1280 - WW) / 2, WY0 = (720 - WH) / 2
) (
    input clk,                                   // такт LINK (NES)
    input wr, input [8:0] wr_addr, input [31:0] wr_data,
    input show, input [5:0] sel,
    input [29:0] band_rows, input [29:0] rule_rows, input [31:0] bar_cfg,   // 0.10, домен clk
    input clk_pixel,
    input [10:0] cx, input [9:0] cy,             // текущий пиксель hdmi.sv
    output reg box = 0,                          // пиксель cx внутри окна: затемнить картинку
    output reg hl = 0,                           // пиксель cx на подсвеченной строке
    output reg band = 0,                         // пиксель cx на строке-подложке (0.10)
    output reg text = 0,                         // пиксель cx — точка знака, рамки, разделителя, полосы
    output reg [23:0] text_rgb = 0
);
    localparam TX0 = WX0 + PAD, TY0 = WY0 + PAD;
    localparam [23:0] C_BORDER = 24'h7890D0;
    localparam [23:0] C_RULE = 24'h3E5A9C;       // разделители и рамка полосы хода
    localparam [23:0] C_BAR = 24'h70E0FF;        // заполненная часть полосы хода

    reg [31:0] tram [0:511] /* synthesis syn_ramstyle = "block_ram" */;
    always @(posedge clk) if (wr) tram[wr_addr] <= wr_data;

    // смена показа и подсветки — с начала кадра (без половины окна на одном кадре)
    reg [1:0] show_sync = 0;
    reg [5:0] sel_a = 63, sel_b = 63;
    reg on = 0;
    reg [5:0] sel_row = 63;
    always @(posedge clk_pixel) begin
        show_sync <= {show_sync[0], show};
        sel_a <= sel; sel_b <= sel_a;
        if (cx == 0 && cy == 720) begin on <= show_sync[1]; sel_row <= sel_b; end
    end

    // оформление 0.10: два снятых подряд значения равны — устоялось, берётся с начала кадра
    reg [29:0] band_a = 0, band_b = 0, band_r = 0, rule_a = 0, rule_b = 0, rule_r = 0;
    reg [31:0] bar_a = 0, bar_b = 0;
    reg bar_on = 0;
    reg [4:0] bar_row = 0;
    reg [10:0] bar_x0 = 0, bar_x1 = 0, bar_end = 0;   // рамка [x0, x1), заполнено — до bar_end
    wire [10:0] cfg_x0 = TX0 + {bar_b[25:20], 4'b0};
    wire [10:0] cfg_x1 = TX0 + {bar_b[25:20], 4'b0} + {bar_b[19:14], 4'b0} - 11'd6;
    wire [10:0] cfg_end = cfg_x0 + 11'd4 + {1'b0, bar_b[9:0]};
    always @(posedge clk_pixel) begin
        band_a <= band_rows; band_b <= band_a;
        rule_a <= rule_rows; rule_b <= rule_a;
        bar_a <= bar_cfg; bar_b <= bar_a;
        if (cx == 0 && cy == 720) begin
            if (band_a == band_b) band_r <= band_b;
            if (rule_a == rule_b) rule_r <= rule_b;
            if (bar_a == bar_b) begin
                bar_on <= bar_b[31] && bar_b[19:14] >= 6'd2;
                bar_row <= bar_b[30:26];
                bar_x0 <= cfg_x0;
                bar_x1 <= cfg_x1;
                bar_end <= cfg_end > cfg_x1 - 11'd4 ? cfg_x1 - 11'd4 : cfg_end;
            end
        end
    end

    // Счётчики пикселя px = cx+3: x — точка 0…1, столбец шрифта 0…7, знак 0…COLS−1;
    // y (на строку кадра) — точка 0…1, строка шрифта 0…9, строка текста 0…ROWS−1
    wire [10:0] px = cx + 11'd3;
    reg xp = 0, yp = 0;
    reg [2:0] xf = 0;
    reg [3:0] yf = 0;
    reg [5:0] xc = 0;
    reg [4:0] yr = 0;
    reg xin = 0, yin = 0;
    always @(posedge clk_pixel) begin
        if (px + 11'd1 == TX0) begin xp <= 0; xf <= 0; xc <= 0; xin <= 1; end
        else if (xin) begin
            xp <= ~xp;
            if (xp) begin
                xf <= xf + 1'b1;
                if (xf == 7) begin
                    if (xc == COLS - 1) xin <= 0; else xc <= xc + 1'b1;
                end
            end
        end
        if (cx == 0) begin
            if (cy == TY0) begin yp <= 0; yf <= 0; yr <= 0; yin <= 1; end
            else if (yin) begin
                yp <= ~yp;
                if (yp) begin
                    if (yf != 9) yf <= yf + 1'b1;
                    else begin
                        yf <= 0;
                        if (yr == ROWS - 1) yin <= 0; else yr <= yr + 1'b1;
                    end
                end
            end
        end
    end

    // 1: слово текста, окно и рамка пикселя px
    reg [31:0] word = 0;
    always @(posedge clk_pixel) word <= tram[{yr, xc[5:2]}];
    wire in_box = px >= WX0 && px < WX0 + WW && cy >= WY0 && cy < WY0 + WH;
    wire edge_px = px < WX0 + 2 || px >= WX0 + WW - 2 || cy < WY0 + 2 || cy >= WY0 + WH - 2;
    wire in_line = in_box && !edge_px && yin && px >= TX0 - 4 && px < TX0 + COLS * 16 - 2;   // ширина строки
    wire bar_line = bar_on && yin && yr == bar_row && px >= bar_x0 && px < bar_x1 && yf <= 7;
    reg a_box = 0, a_border = 0, a_on = 0, a_hl = 0, a_band = 0, a_rule = 0, a_bar_edge = 0, a_bar_fill = 0;
    reg [1:0] a_sub = 0;
    reg [2:0] a_xf = 0;
    reg [2:0] a_yf = 0;
    always @(posedge clk_pixel) begin
        a_box <= in_box;
        a_border <= in_box && edge_px;
        a_on <= xin && yin && xf < 5 && yf < 7;
        a_hl <= in_line && {1'b0, yr} == sel_row;
        a_band <= in_line && band_r[yr];
        a_rule <= in_line && rule_r[yr] && yf == 4;                     // пиксели 8 и 9 ячейки 0…19
        // полоса хода: ячейка 0…15 по высоте, рамка 2 пикселя, отступ 2, заполнено — 4…11
        a_bar_edge <= bar_line && (yf == 0 || yf == 7 || px < bar_x0 + 11'd2 || px >= bar_x1 - 11'd2);
        a_bar_fill <= bar_line && yf >= 2 && yf <= 5 && px >= bar_x0 + 11'd4 && px < bar_end;
        a_sub <= xc[1:0];
        a_xf <= xf;
        a_yf <= yf[2:0];
    end

    // 2: знак из слова, строка его рисунка
    wire [31:0] word_sh = word >> {a_sub, 3'b000};
    wire [7:0] ch_byte = word_sh[7:0];
    wire [4:0] bits;
    nes_osd_font font_rom(.clk(clk_pixel), .ch(ch_byte[5:0]), .line(a_yf), .bits(bits));
    reg b_box = 0, b_border = 0, b_on = 0, b_hl = 0, b_band = 0, b_rule = 0, b_bar_edge = 0, b_bar_fill = 0;
    reg [1:0] b_color = 0;
    reg [2:0] b_xf = 0;
    always @(posedge clk_pixel) begin
        b_box <= a_box;
        b_border <= a_border;
        b_on <= a_on;
        b_hl <= a_hl;
        b_band <= a_band;
        b_rule <= a_rule;
        b_bar_edge <= a_bar_edge;
        b_bar_fill <= a_bar_fill;
        b_color <= ch_byte[7:6];
        b_xf <= a_xf;
    end

    // 3: пиксель
    wire dot = b_on && bits[3'd4 - b_xf];
    always @(posedge clk_pixel) begin
        box <= on && b_box;
        hl <= on && b_hl;
        band <= on && b_band;
        text <= on && (b_border || b_rule || b_bar_edge || b_bar_fill || (b_box && dot));
        text_rgb <= b_border ? C_BORDER : b_bar_fill ? C_BAR : (b_bar_edge || b_rule) ? C_RULE :
                    b_color == 0 ? 24'hFFFFFF : b_color == 1 ? 24'hFFD040 :
                    b_color == 2 ? 24'h70E0FF : 24'h909090;
    end
endmodule
