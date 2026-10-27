// Стенд nes_video_scale (VDAC4 0.9: размер картинки 100 / 95 / 90 %). Счётчики кадра 720p — как у hdmi.sv
// (1650 × 750, видимые 1280 × 720). Проверяется:
//  - 100 % — потактово то же, что прежний модуль (nes_video_scale_old: копия из коммита до 0.9): адрес BRAM
//    и active, в 4:3 и 16:9;
//  - 95 и 90 % — окно по центру: первая и последняя строка, левый край, ширина каждой строки окна; вне окна —
//    ничего; видны все 256 столбцов и 224 строки источника, по порядку, каждая — W/256 (H/224) раз ±1.
// Ожидается: «ВСЁ ВЕРНО».
`timescale 1ns/1ps
module tb;
    reg clk_pixel = 0, clk_nes = 0, resetn = 0;
    always #6.7 clk_pixel = ~clk_pixel;        // ~74,25 МГц
    always #23.1 clk_nes = ~clk_nes;           // ~21,6 МГц
    reg [10:0] cx = 0;
    reg [9:0] cy = 0;
    always @(posedge clk_pixel) begin
        if (cx == 1649) begin cx <= 0; cy <= cy == 749 ? 10'd0 : cy + 1'b1; end
        else cx <= cx + 1'b1;
    end
    reg restore = 0, restore_wide = 0;
    reg [1:0] size = 0;
    wire [15:0] addr_new, addr_old;
    wire act_new, act_old;
    wire [7:0] xx_n, yy_n, xx_o, yy_o;
    wire wide_n, wide_o, req_n, req_o;
    nes_video_scale dut(.clk_nes(clk_nes), .clk_pixel(clk_pixel), .resetn(resetn), .f6(1'b0),
        .restore(restore), .restore_wide(restore_wide), .size(size), .cx(cx), .cy(cy),
        .address(addr_new), .active(act_new), .xx(xx_n), .yy(yy_n), .wide(wide_n), .requested(req_n));
    nes_video_scale_old ref0(.clk_nes(clk_nes), .clk_pixel(clk_pixel), .resetn(resetn), .f6(1'b0),
        .restore(restore), .restore_wide(restore_wide), .cx(cx), .cy(cy),
        .address(addr_old), .active(act_old), .xx(xx_o), .yy(yy_o), .wide(wide_o), .requested(req_o));

    integer errors = 0;
    task automatic set_mode(input w, input [1:0] s);
        @(posedge clk_nes); restore_wide = w; restore = 1; size = s;
        @(posedge clk_nes); restore = 0;
    endtask
    task automatic wait_frame_start;
        do @(posedge clk_pixel); while (!(cx == 0 && cy == 0));
    endtask

    // один кадр: сравнение с прежним (compare) и геометрия
    integer line_count [0:749];
    integer first_x [0:749];
    reg [7:0] prev_addr_lo;
    reg prev_next;
    integer xcount [0:255];
    integer ycount [0:223];
    task automatic check_frame(input integer W, input integer H, input integer L, input integer T,
                               input compare, input [8*8-1:0] name);
        integer i, mism, first_line, last_line, bad_line, x_seq_bad, y_seq_bad, lo, hi, y_prev, x_prev;
        reg [7:0] pending_x, pending_y;
        reg pending;
        mism = 0; first_line = -1; last_line = -1; bad_line = 0; x_seq_bad = 0; y_seq_bad = 0;
        for (i = 0; i < 750; i = i + 1) begin line_count[i] = 0; first_x[i] = -1; end
        for (i = 0; i < 256; i = i + 1) xcount[i] = 0;
        for (i = 0; i < 224; i = i + 1) ycount[i] = 0;
        y_prev = -1; x_prev = -1;
        // с последнего такта прошлого кадра: адрес первого пикселя готовится именно там
        do @(posedge clk_pixel); while (!(cx == 1649 && cy == 749));
        pending = dut.next_active;
        pending_x = addr_new[7:0];
        pending_y = addr_new[15:8];
        @(posedge clk_pixel);
        repeat (1650 * 750) begin
            // адрес этого такта — для следующего пикселя: он «сбывается», если следующий такт active
            if (act_new) begin
                line_count[cy] = line_count[cy] + 1;
                if (first_x[cy] < 0) first_x[cy] = cx;
                if (!pending) x_seq_bad = x_seq_bad + 1;
                else begin
                    if (cy == T && pending_y != 8'd8 && first_x[cy] == cx) y_seq_bad = y_seq_bad + 1;
                    if (cy == 360) begin
                        xcount[pending_x] = xcount[pending_x] + 1;
                        if (x_prev >= 0 && pending_x != x_prev && pending_x != x_prev + 1) x_seq_bad = x_seq_bad + 1;
                        x_prev = pending_x;
                    end
                    if (cx == L + 100) begin
                        ycount[pending_y - 8] = ycount[pending_y - 8] + 1;
                        if (y_prev >= 0 && pending_y - 8 != y_prev && pending_y - 8 != y_prev + 1) y_seq_bad = y_seq_bad + 1;
                        y_prev = pending_y - 8;
                    end
                end
            end
            if (compare && (addr_new !== addr_old || act_new !== act_old)) mism = mism + 1;
            pending = dut.next_active;
            pending_x = addr_new[7:0];
            pending_y = addr_new[15:8];
            @(posedge clk_pixel);
        end
        for (i = 0; i < 750; i = i + 1) begin
            if (line_count[i]) begin
                if (first_line < 0) first_line = i;
                last_line = i;
                if (line_count[i] != W || first_x[i] != L) bad_line = bad_line + 1;
            end
        end
        lo = W / 256; hi = (W + 255) / 256;
        for (i = 0; i < 256; i = i + 1) if (xcount[i] < lo || xcount[i] > hi) x_seq_bad = x_seq_bad + 1;
        lo = H / 224; hi = (H + 223) / 224;
        for (i = 0; i < 224; i = i + 1) if (ycount[i] < lo || ycount[i] > hi) y_seq_bad = y_seq_bad + 1;
        if (mism || first_line != T || last_line != T + H - 1 || bad_line || x_seq_bad || y_seq_bad) begin
            $display("ОШИБКА %0s: расхождений с прежним %0d, строки %0d…%0d (ждали %0d…%0d), плохих строк %0d, x %0d, y %0d",
                     name, mism, first_line, last_line, T, T + H - 1, bad_line, x_seq_bad, y_seq_bad);
            errors = errors + 1;
        end else if (compare)
            $display("  ок %0s: окно %0d×%0d с (%0d, %0d), потактово как прежний, все столбцы и строки источника по порядку",
                     name, W, H, L, T);
        else
            $display("  ок %0s: окно %0d×%0d с (%0d, %0d), все столбцы и строки источника по порядку", name, W, H, L, T);
    endtask

    initial begin
        #100 resetn = 1;
        set_mode(0, 0); wait_frame_start; wait_frame_start;
        check_frame(960, 720, 160, 0, 1, "4:3 100");
        set_mode(1, 0); wait_frame_start; wait_frame_start;
        check_frame(1280, 720, 0, 0, 1, "16:9 100");
        set_mode(0, 1); wait_frame_start; wait_frame_start;
        check_frame(912, 684, 184, 18, 0, "4:3 95");
        set_mode(0, 2); wait_frame_start; wait_frame_start;
        check_frame(864, 648, 208, 36, 0, "4:3 90");
        set_mode(1, 1); wait_frame_start; wait_frame_start;
        check_frame(1216, 684, 32, 18, 0, "16:9 95");
        set_mode(1, 2); wait_frame_start; wait_frame_start;
        check_frame(1152, 648, 64, 36, 0, "16:9 90");
        set_mode(0, 3); wait_frame_start; wait_frame_start;
        check_frame(960, 720, 160, 0, 1, "4:3 s3");
        if (errors == 0) $display("ВСЁ ВЕРНО"); else $display("ОШИБОК: %0d", errors);
        $finish;
    end
endmodule
