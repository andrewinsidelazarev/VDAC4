`timescale 1ns/1ps
//
// ws_dark: после загрузки и после каждого выбора FT812 — тишина ≥ 280 мкс, затем ровно 24
// нулевых бита WS2812 (высокий 0,22…0,38 мкс, период 1,0…1,4 мкс) и снова низкий; при
// опущенном FT_CS_N на выводе — MISO как есть, импульсов нет; без нового выбора — без повтора.
module tb_ws_dark;
    localparam real TCK = 16.16;            // 61,875 МГц
    reg clk = 1'b0, rst_n = 1'b0, cs_n = 1'b1, miso = 1'b0;
    wire pin;
    always #(TCK / 2) clk = ~clk;

    ws_dark #(.CLK_KHZ(61_875)) dut (.clk(clk), .rst_n(rst_n), .cs_n(cs_n), .miso(miso), .pin(pin));

    integer errors = 0, pulses = 0;
    realtime t_rise = 0, t_fall = 0, t_first = 0, t_cs = 0;
    always @(posedge cs_n) t_cs = $realtime;     // спад вывода с MISO на 0 при подъёме — не импульс
    // импульсы при поднятом FT_CS_N
    always @(posedge pin) if (rst_n && cs_n) begin
        if (pulses == 0) t_first = $realtime;
        else if ($realtime - t_rise < 1000.0 || $realtime - t_rise > 1400.0) begin
            errors++; $display("период бита %0.1f нс", $realtime - t_rise);
        end
        t_rise = $realtime;
    end
    always @(negedge pin) if (rst_n && cs_n && $realtime - t_cs > 1.0) begin
        t_fall = $realtime;
        if (t_fall - t_rise < 220.0 || t_fall - t_rise > 380.0) begin
            errors++; $display("[%0t] высокий %0.1f нс, cs %b", $time, t_fall - t_rise, cs_n);
        end
        pulses++;
    end

    task automatic frame(input string who, input realtime quiet_from);
        pulses = 0;
        #(400_000);                          // 400 мкс: сброс 320 мкс и кадр ~30 мкс
        if (pulses != 24) begin errors++; $display("%s: импульсов %0d, ждали 24", who, pulses); end
        else if (t_first - quiet_from < 280_000.0) begin
            errors++; $display("%s: кадр через %0.1f мкс тишины", who, (t_first - quiet_from) / 1000.0);
        end
        pulses = 0;
        #(1_000_000);                        // повтора нет
        if (pulses != 0) begin errors++; $display("%s: повтор, импульсов %0d", who, pulses); end
    endtask

    initial begin
        #100 rst_n = 1'b1;
        frame("после загрузки", 100.0);
        // обмен: FT_CS_N опущен — на выводе MISO
        cs_n = 1'b0;
        repeat (50) begin
            #(37) miso = $urandom_range(0, 1);
            #1 if (pin !== miso) begin errors++; $display("при выбранном FT812 вывод %b, MISO %b", pin, miso); end
        end
        miso = 1'b1;
        #100 cs_n = 1'b1;
        #1 if (pin !== 1'b0) begin errors++; $display("после выбора вывод %b, а не 0", pin); end
        miso = 1'b0;
        frame("после обмена", $realtime);
        // короткие паузы (< сброса) между обменами — кадра нет, пока не тихо 320 мкс
        repeat (5) begin
            cs_n = 1'b0; #(2_000); cs_n = 1'b1; #(200_000);
            if (pulses != 0) begin errors++; $display("кадр в паузе 200 мкс"); end
        end
        frame("после серии обменов", $realtime - 200_000);
        if (errors == 0) $display("ТЕСТ ПРОЙДЕН");
        else $display("ТЕСТ НЕ ПРОЙДЕН: ошибок %0d", errors);
        $finish;
    end
endmodule
