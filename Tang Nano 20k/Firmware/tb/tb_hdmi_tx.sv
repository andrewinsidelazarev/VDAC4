// Проверка hdmi_tx: поток TMDS разбирается так, как его разбирает приёмник HDMI, — по
// преамбулам (CTL), а не по внутренним сигналам модуля.
//   iverilog -g2012 -DSIMULATION -I rtl -s tb_hdmi_tx -o t.vvp rtl/tmds_encoder.sv \
//       rtl/hdmi_tx.sv tb/tb_hdmi_tx.sv && vvp -n t.vvp
`timescale 1ns/1ps
module tb_hdmi_tx;
    // Уменьшенный растр: модулю размер кадра неважен (он работает по фронтам DE и VSYNC),
    // а пакет — тот же (VIC 19). Синхроимпульсы положительные, как у 720p.
    localparam int OUT_W = 128, OUT_H = 16, H_FP = 40, H_SYNC = 16, H_BP = 40;
    localparam int V_FP = 2, V_SYNC = 2, V_BP = 4;
    localparam int H_TOTAL = OUT_W + H_FP + H_SYNC + H_BP, V_TOTAL = OUT_H + V_FP + V_SYNC + V_BP;
    localparam int FRAMES = 4;

    reg clk = 1'b0, rst_n = 1'b0;
    always #6.734 clk = ~clk;

    integer h = 0, v = OUT_H;          // старт в кадровом гашении
    reg  [7:0] r = 0, g = 0, b = 0;
    reg        de = 0, hs = 0, vs = 0;
    wire [9:0] t0, t1, t2;

    hdmi_tx #(.HDMI(1'b1), .VS_HIGH(1'b1), .VIC(8'd19), .PIC_AR(2'b10)) dut (
        .pixel_clk(clk), .reset_n(rst_n), .r(r), .g(g), .b(b), .de(de), .hs(hs), .vs(vs),
        .tmds_blue(t0), .tmds_green(t1), .tmds_red(t2)
    );

    function automatic [7:0] px(input integer x, input integer y, input integer c);
        px = (x * (3 + c) + y * (5 + 2 * c) + c * 77) & 8'hFF;
    endfunction

    // Развёртка; входные сигналы меняются по фронту, как из регистров
    always @(posedge clk) if (rst_n) begin
        h = (h == H_TOTAL - 1) ? 0 : h + 1;
        if (h == 0) v = (v == V_TOTAL - 1) ? 0 : v + 1;
        de <= (h < OUT_W) && (v < OUT_H);
        hs <= (h >= OUT_W + H_FP) && (h < OUT_W + H_FP + H_SYNC);
        if (h == OUT_W + H_FP) vs <= (v >= OUT_H + V_FP) && (v < OUT_H + V_FP + V_SYNC);
        r <= px(h, v, 0); g <= px(h, v, 1); b <= px(h, v, 2);
    end

    // Эталон входа — та же развёртка с задержкой модуля (11 тактов)
    localparam int LAT = 11;
    reg [26:0] ref_q [0:LAT-1];
    always @(posedge clk) begin : refpipe
        integer i;
        ref_q[0] <= {r, g, b, de, hs, vs};
        for (i = 1; i < LAT; i = i + 1) ref_q[i] <= ref_q[i-1];
    end
    wire [26:0] ref_now = ref_q[LAT-1];

    integer errors = 0, checks = 0;
    task automatic err(input string s);
        begin errors = errors + 1; if (errors < 30) $display("[%0t] ОШИБКА: %0s", $time, s); end
    endtask

    // ---- декодирование символов ----
    function automatic integer ctl(input [9:0] w);   // управляющий символ → {c1,c0}, иначе −1
        case (w)
            10'b1101010100: ctl = 0;
            10'b0010101011: ctl = 1;
            10'b0101010100: ctl = 2;
            10'b1010101011: ctl = 3;
            default:        ctl = -1;
        endcase
    endfunction
    function automatic integer terc4_dec(input [9:0] w);
        case (w)
            10'b1010011100: terc4_dec = 0;  10'b1001100011: terc4_dec = 1;
            10'b1011100100: terc4_dec = 2;  10'b1011100010: terc4_dec = 3;
            10'b0101110001: terc4_dec = 4;  10'b0100011110: terc4_dec = 5;
            10'b0110001110: terc4_dec = 6;  10'b0100111100: terc4_dec = 7;
            10'b1011001100: terc4_dec = 8;  10'b0100111001: terc4_dec = 9;
            10'b0110011100: terc4_dec = 10; 10'b1011000110: terc4_dec = 11;
            10'b1010001110: terc4_dec = 12; 10'b1001110001: terc4_dec = 13;
            10'b0101100011: terc4_dec = 14; 10'b1011000011: terc4_dec = 15;
            default:        terc4_dec = -1;
        endcase
    endfunction
    function automatic [7:0] tmds_video(input [9:0] w);
        reg [7:0] d;
        integer i;
        begin
            d = w[9] ? ~w[7:0] : w[7:0];
            tmds_video[0] = d[0];
            for (i = 1; i < 8; i = i + 1) tmds_video[i] = w[8] ? (d[i] ^ d[i-1]) : ~(d[i] ^ d[i-1]);
        end
    endfunction

    // BCH (32,24)/(64,56), образующий 1 + x⁶ + x⁷ + x⁸, — тем же сдвиговым регистром, что
    // у известных реализаций HDMI (младший бит первым); это сверка упаковки битов, а не
    // независимая проверка самого кода
    function automatic [7:0] bch_ref(input [55:0] d, input integer n);
        reg [7:0] s;
        integer i;
        reg fb;
        begin
            s = 8'h00;
            for (i = 0; i < n; i = i + 1) begin
                fb = d[i] ^ s[0];
                s = {1'b0, s[7:1]};
                if (fb) s = s ^ 8'b1000_0011;
            end
            bch_ref = s;
        end
    endfunction

    // ---- разбор потока ----
    localparam int S_CTRL = 0, S_VPRE = 1, S_VGB = 2, S_VID = 3, S_DPRE = 4, S_DGB0 = 5,
                   S_DATA = 6, S_DGB1 = 7;
    integer st = S_CTRL, cnt = 0, kk = 0;
    integer lines_ok = 0, islands = 0, frames = 0, vid_px = 0;
    reg [31:0] hdr;
    reg [63:0] sp [0:3];
    reg        bit3_first;
    integer    prev_c1 = -1, prev_c2 = -1, run = 0;

    task automatic check_packet;
        reg [7:0] pb [0:13];
        integer i, sum;
        begin
            islands = islands + 1;
            checks = checks + 1;
            if (hdr[7:0] != 8'h82 || hdr[15:8] != 8'h02 || hdr[23:16] != 8'h0D)
                err($sformatf("заголовок AVI %06h", hdr[23:0]));
            if (hdr[31:24] != bch_ref({32'd0, hdr[23:0]}, 24)) err("ECC заголовка");
            for (i = 0; i < 4; i = i + 1)
                if (sp[i][63:56] != bch_ref(sp[i][55:0], 56)) err($sformatf("ECC подпакета %0d", i));
            for (i = 0; i < 7; i = i + 1) begin pb[i] = sp[0][i*8 +: 8]; pb[7 + i] = sp[1][i*8 +: 8]; end
            sum = hdr[7:0] + hdr[15:8] + hdr[23:16];
            for (i = 0; i < 14; i = i + 1) sum = sum + pb[i];
            if ((sum & 8'hFF) != 0) err($sformatf("контрольная сумма AVI: %02h", sum & 8'hFF));
            if (pb[1][6:5] != 2'b00) err("AVI: не RGB");
            if (pb[1][1:0] != 2'b10) err("AVI: не underscan");
            if (pb[2][5:4] != 2'b10) err("AVI: формат кадра не 16:9");
            if (pb[3][7] != 1'b1) err("AVI: ITC = 0");
            if (pb[3][3:2] != 2'b10) err("AVI: диапазон RGB не полный");
            if (pb[4] != 8'd19) err($sformatf("AVI: VIC %0d", pb[4]));
            if (pb[5][5:4] != 2'b11) err("AVI: тип содержимого не «игра»");
            if (!bit3_first) err("канал 0, бит 3: в первом такте пакета не 0");
        end
    endtask

    always @(posedge clk) if (rst_n && frames < FRAMES) begin : parse
        integer c0, c1, c2, d0, d1, d2;
        c0 = ctl(t0); c1 = ctl(t1); c2 = ctl(t2);
        d0 = terc4_dec(t0); d1 = terc4_dec(t1); d2 = terc4_dec(t2);
        case (st)
            S_CTRL, S_VPRE, S_DPRE: begin
                if (c0 >= 0 && c1 >= 0 && c2 >= 0) begin
                    // синхроимпульсы в управляющих символах канала 0
                    if (c0 != {ref_now[0], ref_now[1]}) err("HSYNC/VSYNC в управляющем символе");
                    if (c1 == prev_c1 && c2 == prev_c2) run = run + 1; else run = 1;
                    prev_c1 = c1; prev_c2 = c2;
                    st = (c1 == 1 && c2 == 0) ? S_VPRE : (c1 == 1 && c2 == 1) ? S_DPRE : S_CTRL;
                    if (c1 == 0 && c2 == 0) ;
                    else if (!((c1 == 1 && c2 == 0) || (c1 == 1 && c2 == 1))) err("неизвестная преамбула");
                end else if (st == S_VPRE && run == 8 && t0 == 10'b1011001100 && t1 == 10'b0100110011 &&
                             t2 == 10'b1011001100) begin
                    st = S_VGB; cnt = 1;
                end else if (st == S_DPRE && run == 8 && d0 >= 12 && t1 == 10'b0100110011 &&
                             t2 == 10'b0100110011) begin
                    if ((d0 & 3) != {ref_now[0], ref_now[1]}) err("HSYNC/VSYNC в защитной полосе");
                    st = S_DGB0; cnt = 1;
                end else begin
                    err($sformatf("неожиданный символ вне периода: %b %b %b (преамбула %0d)", t0, t1, t2, run));
                    st = S_CTRL;
                end
            end
            S_VGB: begin
                if (t0 != 10'b1011001100 || t1 != 10'b0100110011 || t2 != 10'b1011001100)
                    err("вторая защитная полоса видео");
                st = S_VID; cnt = 0;
            end
            S_VID: begin
                if (c0 >= 0) begin
                    // конец строки
                    if (cnt != OUT_W) err($sformatf("строка видео %0d точек", cnt));
                    else lines_ok = lines_ok + 1;
                    prev_c1 = c1; prev_c2 = c2; run = 1;
                    st = S_CTRL;
                end else begin
                    if (!ref_now[2]) err("видео там, где на входе нет DE");
                    if (tmds_video(t2) != ref_now[26:19] || tmds_video(t1) != ref_now[18:11] ||
                        tmds_video(t0) != ref_now[10:3]) err("точка видео не совпала со входом");
                    cnt = cnt + 1; vid_px = vid_px + 1;
                end
            end
            S_DGB0: begin
                if (d0 < 12 || t1 != 10'b0100110011 || t2 != 10'b0100110011) err("вторая защитная полоса островка");
                st = S_DATA; kk = 0; hdr = 0; sp[0] = 0; sp[1] = 0; sp[2] = 0; sp[3] = 0;
            end
            S_DATA: begin
                if (d0 < 0 || d1 < 0 || d2 < 0) err("не TERC4 в пакете");
                if (kk == 0) bit3_first = !d0[3]; else if (!d0[3]) err("бит 3 канала 0 не 1");
                if ((d0 & 3) != {ref_now[0], ref_now[1]}) err("HSYNC/VSYNC в пакете");
                hdr[kk] = d0[2];
                begin : bits
                    integer i;
                    for (i = 0; i < 4; i = i + 1) begin
                        sp[i][2*kk]     = d1[i];
                        sp[i][2*kk + 1] = d2[i];
                    end
                end
                kk = kk + 1;
                if (kk == 32) begin st = S_DGB1; cnt = 0; end
            end
            S_DGB1: begin
                if (d0 < 12 || t1 != 10'b0100110011 || t2 != 10'b0100110011) err("защитная полоса после пакета");
                cnt = cnt + 1;
                if (cnt == 2) begin check_packet(); st = S_CTRL; run = 0; prev_c1 = -1; end
            end
        endcase
        if (v == V_TOTAL - 1 && h == H_TOTAL - 1) frames = frames + 1;
    end

    initial begin
        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        wait (frames == FRAMES);
        checks = checks + 1;
        if (lines_ok < (FRAMES - 1) * OUT_H) err($sformatf("целых строк видео %0d", lines_ok));
        if (islands < FRAMES - 1) err($sformatf("пакетов AVI %0d за %0d кадров", islands, FRAMES));
        $display("строк видео: %0d, пакетов AVI: %0d, точек: %0d, ошибок: %0d", lines_ok, islands, vid_px, errors);
        if (errors == 0) $display("ТЕСТ ПРОЙДЕН"); else $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end
endmodule
