// GPL-3.0-or-later. Окно меню поверх картинки Amiga (VDAC4): текст пишет ESP (AMIGA.ELF) через LINK.
// Источник для fpga/nes_osd.sv (prepare.py). Основа — окно NES (Cores/NES/fpga/nes_osd.sv), но здесь не
// кадр 1280×720, а кадр AGA 908×626: такт clk_pixel — точка hires, две строки кадра — строка PAL.
// Поэтому точка шрифта — 1 такт × 2 строки, ячейка 8 × 16: те же пропорции, что у текста самой Amiga
// (Topaz на экране hires). Шрифт 5×7 (nes_osd_font.sv), одинарной плотности: штрих в 1 точку hires.
//
// Текст: 512 слов (домен clk), строка r — слова r·16…r·16+15, по 4 знака на слово, младший байт —
// левый знак; байт — {цвет[1:0], знак ASCII − 0x20 [5:0]}; цвет 0 — белый, 1 — жёлтый,
// 2 — голубой, 3 — серый. sel — строка с подсветкой (вне 0…ROWS−1 — нет).
//
// Конвейер — три регистра: счётчики считают пиксель cx+3, тогда box/hl/text/text_rgb к фронту, на
// котором пиксель cx уходит в HDMI, относятся именно к cx — как данные BRAM.
//
// Оформление (всё — с начала кадра, когда значение устоялось):
//   band[r] — строка r на подложке: шапка, строка состояния;
//   rule[r] — в строке r тонкий разделитель (2 строки кадра посередине ячейки) во всю ширину текста.
module nes_osd #(
    parameter COLS = 56, ROWS = 24,
    parameter PAD = 12, SCALE_X = 1,
    parameter WW = 2 * PAD + (COLS * 8 - 2) * SCALE_X,
    parameter WH = 2 * PAD + ROWS * 16 - 2,      // снизу — пустой строки шрифта последней строки
    parameter WX0 = 0, WY0 = 0,
    parameter FRAME_Y = 1
) (
    input clk,                                   // такт LINK
    input wr, input [8:0] wr_addr, input [31:0] wr_data,
    input show, input [5:0] sel,
    input [29:0] band_rows, input [29:0] rule_rows,   // домен clk
    input clk_pixel,
    input [10:0] cx, input [9:0] cy,             // текущий пиксель кадра AGA
    output reg box = 0,                          // пиксель cx внутри окна: затемнить картинку
    output reg hl = 0,                           // пиксель cx на подсвеченной строке
    output reg band = 0,                         // пиксель cx на строке-подложке
    output reg text = 0,                         // пиксель cx — точка знака, рамки, разделителя
    output reg [23:0] text_rgb = 0
);
    localparam TX0 = WX0 + PAD, TY0 = WY0 + PAD;
    localparam [23:0] C_BORDER = 24'h7890D0;
    localparam [23:0] C_RULE = 24'h3E5A9C;       // разделители

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
        if (cx == 0 && cy == FRAME_Y) begin on <= show_sync[1]; sel_row <= sel_b; end
    end

    // оформление: два снятых подряд значения равны — устоялось, берётся с начала кадра
    reg [ROWS-1:0] band_a = 0, band_b = 0, band_r = 0, rule_a = 0, rule_b = 0, rule_r = 0;
    always @(posedge clk_pixel) begin
        band_a <= band_rows[ROWS-1:0]; band_b <= band_a;
        rule_a <= rule_rows[ROWS-1:0]; rule_b <= rule_a;
        if (cx == 0 && cy == FRAME_Y) begin
            if (band_a == band_b) band_r <= band_b;
            if (rule_a == rule_b) rule_r <= rule_b;
        end
    end

    // Счётчики пикселя px = cx+3: x — столбец шрифта 0…7, знак 0…COLS−1;
    // y (на строку кадра) — половина точки 0…1, строка шрифта 0…7, строка текста 0…ROWS−1
    wire [10:0] px = cx + 11'd3;
    reg yp = 0;
    localparam X_BITS=SCALE_X==2 ? 4:3;
    reg [X_BITS-1:0] xphase=0;
    wire [2:0] xf=xphase >> (SCALE_X==2 ? 1:0);
    reg [2:0] yf = 0;
    reg [5:0] xc = 0;
    reg [4:0] yr = 0;
    reg xin = 0, yin = 0;
    always @(posedge clk_pixel) begin
        if (px + 11'd1 == TX0) begin xphase<=0; xc<=0; xin<=1; end
        else if (xin) begin
            xphase<=xphase+1'b1;
            if (&xphase) begin
                if (xc==COLS-1) xin<=0;else xc<=xc+1'b1;
            end
        end
        if (cx == 0) begin
            if (cy == TY0) begin yp <= 0; yf <= 0; yr <= 0; yin <= 1; end
            else if (yin) begin
                yp <= ~yp;
                if (yp) begin
                    yf <= yf + 1'b1;
                    if (yf == 7) begin
                        if (yr == ROWS - 1) yin <= 0; else yr <= yr + 1'b1;
                    end
                end
            end
        end
    end

    // 1: слово текста, окно и рамка пикселя px; рамка — как точка шрифта: такт × 2 строки
    reg [31:0] word = 0;
    always @(posedge clk_pixel) word <= tram[{yr, xc[5:2]}];
    wire in_box = px >= WX0 && px < WX0 + WW && cy >= WY0 && cy < WY0 + WH;
    wire edge_px = px < WX0+SCALE_X || px >= WX0+WW-SCALE_X || cy < WY0+2 || cy >= WY0+WH-2;
    wire in_line = in_box && !edge_px && yin && px >= TX0-4*SCALE_X && px < TX0+(COLS*8-2)*SCALE_X;
    reg a_box = 0, a_border = 0, a_on = 0, a_hl = 0, a_band = 0, a_rule = 0;
    reg [1:0] a_sub = 0;
    reg [2:0] a_xf = 0;
    reg [2:0] a_yf = 0;
    always @(posedge clk_pixel) begin
        a_box <= in_box;
        a_border <= in_box && edge_px;
        a_on <= xin && yin && xf < 5 && yf < 7;
        a_hl <= in_line && {1'b0, yr} == sel_row;
        a_band <= in_line && band_r[yr];
        a_rule <= in_line && rule_r[yr] && yf == 4;
        a_sub <= xc[1:0];
        a_xf <= xf;
        a_yf <= yf;
    end

    // 2: знак из слова, строка его рисунка
    wire [31:0] word_sh = word >> {a_sub, 3'b000};
    wire [7:0] ch_byte = word_sh[7:0];
    wire [4:0] bits;
    nes_osd_font font_rom(.clk(clk_pixel), .ch(ch_byte[5:0]), .line(a_yf), .bits(bits));
    reg b_box = 0, b_border = 0, b_on = 0, b_hl = 0, b_band = 0, b_rule = 0;
    reg [1:0] b_color = 0;
    reg [2:0] b_xf = 0;
    always @(posedge clk_pixel) begin
        b_box <= a_box;
        b_border <= a_border;
        b_on <= a_on;
        b_hl <= a_hl;
        b_band <= a_band;
        b_rule <= a_rule;
        b_color <= ch_byte[7:6];
        b_xf <= a_xf;
    end

    // 3: пиксель
    wire dot = b_on && bits[3'd4 - b_xf];
    always @(posedge clk_pixel) begin
        box <= on && b_box;
        hl <= on && b_hl;
        band <= on && b_band;
        text <= on && (b_border || b_rule || (b_box && dot));
        text_rgb <= b_border ? C_BORDER : b_rule ? C_RULE :
                    b_color == 0 ? 24'hFFFFFF : b_color == 1 ? 24'hFFD040 :
                    b_color == 2 ? 24'h70E0FF : 24'h909090;
    end
endmodule
