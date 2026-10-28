`timescale 1ns/1ps
//
// ws_dark (с 1.38) на модели WS2812: после сброса (низкий дольше RST_NS — 50 мкс у WS2812, 280 у
// WS2812B, прогоняются оба) первые 24 бита — в регистр, остальные — дальше; на сбросе показывается
// регистр (неполный — неизвестно что, это ошибка). Бит — по высокому уровню: дольше 0,5 мкс — «1».
//
// Проверки:
//   — при выбранном FT812 на выводе MISO как есть; наши импульсы при поднятом FT_CS_N — 0,22…0,38 мкс;
//   — после загрузки за ~0,7 мс показан чёрный, дальше поддерживающие биты: при поднятом FT_CS_N
//     вывод низкий не дольше 25 мкс;
//   — шум R-Type: чтения по 2 мкс раз в 15 мкс, MISO случайный (и с длинными единицами — бит «1»
//     у WS2812) — 5 мс показан чёрный;
//   — длинная запись (0,4 мс, MISO 0 — лист R-Type), затем снова шум — показан чёрный;
//   — запись 0,4 мс и сразу чтение с единицами (регистр мусорный): после паузы 1 мс снова чёрный.
module tb_ws_dark #(
    parameter real RST_NS = 280_000.0
);
    localparam real TCK = 16.16;            // 61,875 МГц
    reg clk = 1'b0, rst_n = 1'b0, cs_n = 1'b1, miso = 1'b0;
    wire pin;
    always #(TCK / 2) clk = ~clk;

    ws_dark #(.CLK_KHZ(61_875)) dut (.clk(clk), .rst_n(rst_n), .cs_n(cs_n), .miso(miso), .pin(pin));

    integer errors = 0;
    task automatic bad(input string msg);
        if (errors < 20) $display("[%0t] ОШИБКА: %0s", $time, msg);
        errors++;
    endtask

    // ---- модель WS2812 ----
    reg [23:0] sreg = '0;
    integer    nb = 0;
    reg        shown_black = 1'b0;          // показан чёрный (после сброса с полным регистром нулей)
    reg        shown_known = 1'b0;
    realtime   t_rise = 0, t_fall = 0;
    reg        latched = 1'b1;
    always @(posedge pin) t_rise = $realtime;
    always @(negedge pin) begin
        t_fall = $realtime;
        latched = 1'b0;
        if (nb < 24) begin
            sreg = {sreg[22:0], (t_fall - t_rise) > 500.0};
            nb++;
        end
    end
    always #1000 if (!pin && !latched && $realtime - t_fall >= RST_NS) begin
        // пустой регистр (битов после сброса не было) цвета не меняет; неполный — неизвестно что
        if (nb != 0) begin
            shown_known = (nb == 24);
            shown_black = (nb == 24) && sreg == 24'd0;
        end
        nb = 0;
        latched = 1'b1;
    end

    // ---- наши импульсы и паузы при поднятом FT_CS_N ----
    realtime t_cs = 0, t_hi = 0;
    reg      ka_check = 1'b0;               // проверять паузы (установившийся тёмный режим)
    realtime t_low_from = 0;
    always @(posedge cs_n) t_cs = $realtime;
    always @(posedge pin) if (cs_n) t_hi = $realtime;
    always @(negedge pin) if (cs_n && rst_n && $realtime - t_cs > 1.0) begin
        if ($realtime - t_hi < 220.0 || $realtime - t_hi > 380.0)
            bad($sformatf("наш импульс %0.1f нс", $realtime - t_hi));
    end
    always @(negedge pin or posedge cs_n) t_low_from = $realtime;
    always #500 if (ka_check && cs_n && !pin && $realtime - t_low_from > 25_000.0 && $realtime - t_cs > 25_000.0) begin
        bad($sformatf("пауза без поддерживающего бита %0.1f мкс", ($realtime - t_low_from) / 1000.0));
        t_low_from = $realtime;
    end

    // ---- посылки Evo ----
    // чтение: dur_ns при выбранном FT812, MISO — случайные биты по 35 нс, иногда длинные единицы
    task automatic xread(input real dur_ns, input bit ones);
        realtime t_end;
        cs_n = 1'b0;
        t_end = $realtime + dur_ns;
        while ($realtime < t_end) begin
            if (ones && $urandom_range(0, 15) == 0) begin miso = 1'b1; #(700); end
            else begin miso = $urandom_range(0, 1); #(35); end
            #0 if (pin !== miso) bad("при выбранном FT812 на выводе не MISO");
        end
        miso = 1'b0;
        #(20) cs_n = 1'b1;
    endtask
    task automatic xwrite(input real dur_ns);
        cs_n = 1'b0; miso = 1'b0; #(dur_ns); #(20) cs_n = 1'b1;
    endtask
    // шум R-Type: чтения по 2 мкс раз в 15 мкс; показан должен быть чёрный всё время
    task automatic rtype_noise(input real dur_ns, input string who);
        realtime t_end;
        t_end = $realtime + dur_ns;
        while ($realtime < t_end) begin
            xread(2_000, 1'b1);
            #(13_000);
            if (!shown_black) begin bad($sformatf("%s: показан не чёрный", who)); break; end
        end
    endtask

    initial begin
        #100 rst_n = 1'b1;
        #(1_000_000);
        if (!shown_black) bad("после загрузки за 1 мс не показан чёрный");
        ka_check = 1'b1;
        #(200_000);
        rtype_noise(5_000_000, "шум R-Type");
        // лист R-Type: длинная запись (WS2812 сбрасывается), сразу дальше — шум
        repeat (3) begin
            xwrite(400_000);
            #(40_000);                       // REG_DLSWAP и логика игры — не меньше 30 мкс тишины
            rtype_noise(1_000_000, "после записи листа");
        end
        // плохой случай: запись и сразу чтение с единицами — регистр мусорный
        ka_check = 1'b0;
        xwrite(400_000);
        xread(3_000, 1'b1);
        #(1_200_000);
        if (!shown_black) bad("после мусора и паузы 1,2 мс не показан чёрный");
        ka_check = 1'b1;
        #(200_000);
        rtype_noise(1_000_000, "после восстановления");
        if (errors == 0) $display("ТЕСТ ПРОЙДЕН (порог сброса WS2812 %0.0f мкс)", RST_NS / 1000.0);
        else $display("ТЕСТ НЕ ПРОЙДЕН: ошибок %0d (порог %0.0f мкс)", errors, RST_NS / 1000.0);
        $finish;
    end
endmodule
