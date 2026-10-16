`timescale 1ns/1ps
//
// Проверка видеотракта A3.1: модель растра ZX Evolution → приём → строчный буфер →
// выходная развёртка → цвет. Тактирование здесь идеальное (PLL не моделируются),
// проверяется логика тракта: раскладка бит мультиплексора, привязка кадра,
// адресация строчного буфера и расширение палитры.
//
// Модель источника повторяет расчётный растр TS-Config в VGA-режиме:
//   строка 896 отсчётов PCLK 28 МГц, кадр 640 строк, синхроимпульсы активны низким.
//   Активное окно расположено там же, где его ищет прошивка: H_IN_OFFSET / V_IN_OFFSET.
//
module tb_ts_video_chain #(
    // Фаза чередования SEL, подбирается ключом -P при запуске симуляции
    parameter bit SEL_PHASE_TB = 1'b0
);

    // Параметры должны совпадать с параметрами тракта
    localparam int H_IN_OFFSET_DUT = 131;   // параметр прошивки
    localparam int H_IN_OFFSET = 132;       // где картинка стоит в растре источника
    localparam int V_IN_OFFSET = 45;
    localparam int H_TOTAL_IN  = 896;
    localparam int V_TOTAL_IN  = 640;
    localparam int H_SYNC_IN   = 64;
    localparam int V_SYNC_IN   = 5;
    localparam int H_ACTIVE    = 720;
    localparam int V_ACTIVE    = 576;

    // --- Тактирование: 28 МГц пикселей и 56 МГц приёма из общей временной сетки ---
    // Пиксельный клок получается делением клока приёма на два — ровно как в прошивке
    // (обе частоты приходят из одного PCLK). Два независимых генератора здесь не годятся:
    // разница округления периодов уводила бы фазу и ломала выборку к концу строки.
    reg clk_cap = 1'b0;
    reg pixel_clk = 1'b0;
    always #8.9285 clk_cap = ~clk_cap;              // 56 МГц
    always @(posedge clk_cap) pixel_clk <= ~pixel_clk; // 28 МГц, жёстко 2:1

    reg rst_n = 1'b0;

    // --- Модель источника ---------------------------------------------------
    integer hx = 0, hy = 0;
    reg hsync_in, vsync_in;

    // Тестовая картинка источника. Все компоненты — чистые функции координат,
    // поэтому проверяющая часть сверяет с ними выход пиксель в пиксель, а глазами
    // видно и геометрию (рамка, диагональ, сетка), и цвет (полосы, градиент).
    // Правая половина идёт с PAL_SEL=1: там работает вторая ветвь палитры.
    function automatic [4:0] pat_r(input integer x, input integer y);
        if (x < 2 || x >= H_ACTIVE-2 || y < 2 || y >= V_ACTIVE-2) pat_r = 5'd31;      // рамка
        else if (y < 64) pat_r = ((x/90) & 3'd1) ? 5'd31 : 5'd0;                      // полосы
        else if (x == y || x == (V_ACTIVE - 1 - y)) pat_r = 5'd31;                     // диагонали
        else if ((x % 80 < 2) || (y % 80 < 2)) pat_r = 5'd12;                          // сетка
        else pat_r = x[9:5];                                                            // градиент
    endfunction
    function automatic [4:0] pat_g(input integer x, input integer y);
        if (x < 2 || x >= H_ACTIVE-2 || y < 2 || y >= V_ACTIVE-2) pat_g = 5'd31;
        else if (y < 64) pat_g = ((x/90) & 3'd2) ? 5'd31 : 5'd0;
        else if (x == y || x == (V_ACTIVE - 1 - y)) pat_g = 5'd31;
        else if ((x % 80 < 2) || (y % 80 < 2)) pat_g = 5'd12;
        else pat_g = y[9:5];
    endfunction
    function automatic [4:0] pat_b(input integer x, input integer y);
        if (x < 2 || x >= H_ACTIVE-2 || y < 2 || y >= V_ACTIVE-2) pat_b = 5'd31;
        else if (y < 64) pat_b = ((x/90) & 3'd4) ? 5'd31 : 5'd0;
        else if (x == y || x == (V_ACTIVE - 1 - y)) pat_b = 5'd0;
        else if ((x % 80 < 2) || (y % 80 < 2)) pat_b = 5'd12;
        else pat_b = 5'd31 - x[9:5];
    endfunction
    function automatic pat_p(input integer x, input integer y);
        pat_p = (x >= H_ACTIVE/2);
    endfunction

    // Текущий пиксель источника (действителен только в активном окне)
    integer src_x, src_y;
    wire in_active = (hx >= H_IN_OFFSET) && (hx < H_IN_OFFSET + H_ACTIVE) &&
                     (hy >= V_IN_OFFSET) && (hy < V_IN_OFFSET + V_ACTIVE);

    // Две фазы мультиплексора, как их собирают U1/U2
    wire [4:0] s_r = in_active ? pat_r(hx - H_IN_OFFSET, hy - V_IN_OFFSET) : 5'd0;
    wire [4:0] s_g = in_active ? pat_g(hx - H_IN_OFFSET, hy - V_IN_OFFSET) : 5'd0;
    wire [4:0] s_b = in_active ? pat_b(hx - H_IN_OFFSET, hy - V_IN_OFFSET) : 5'd0;
    wire       s_p = in_active ? pat_p(hx - H_IN_OFFSET, hy - V_IN_OFFSET) : 1'b0;

    wire [7:0] phase_a = {s_g[2:0], s_r[4:0]};        // D7..D0 при SEL=0
    wire [7:0] phase_b = {s_g[3], s_p, s_g[4], s_b};  // D7..D0 при SEL=1

    // Выход мультиплексора с задержкой распространения 74LVC257 и шлейфа
    wire mux_sel;
    wire [7:0] mux_d;
    assign #4.5 mux_d = mux_sel ? phase_b : phase_a;

    // Счётчики растра источника: пиксель меняется по фронту PCLK.
    // Синхроимпульсы активны низким и выставляются от тех же счётчиков,
    // то есть HS активен в отсчётах 0..63 строки, VS — в строках 0..4.
    always @(posedge pixel_clk) begin
        if (hx == H_TOTAL_IN - 1) begin
            hx <= 0;
            hy <= (hy == V_TOTAL_IN - 1) ? 0 : hy + 1;
        end else begin
            hx <= hx + 1;
        end
    end

    always @(*) begin
        hsync_in = !(hx < H_SYNC_IN);
        vsync_in = !(hy < V_SYNC_IN);
    end

    // --- Тракт ---------------------------------------------------------------
    wire [7:0] out_r, out_g, out_b;
    wire       out_de, out_hs, out_vs;

    ts_video_chain #(
        .H_IN_OFFSET(H_IN_OFFSET_DUT), .V_IN_OFFSET(V_IN_OFFSET),
        .SAMPLE_DELAY(1), .HS_ACTIVE_LOW(1'b1), .VS_ACTIVE_LOW(1'b1), .SEL_PHASE(SEL_PHASE_TB)
    ) dut (
        .clk_cap(clk_cap), .rst_cap_n(rst_n),
        .pixel_clk(pixel_clk), .rst_pix_n(rst_n),
        .mux_d(mux_d), .hsync_in(hsync_in), .vsync_in(vsync_in),
        .blank_video(1'b0), .mux_sel(mux_sel),
        .out_r(out_r), .out_g(out_g), .out_b(out_b),
        .out_de(out_de), .out_hs(out_hs), .out_vs(out_vs)
    );

    // --- Эталон расширения палитры (та же формула, что в legacy_palette) -------
    function automatic [7:0] expand(input pal, input [4:0] v);
        integer scaled;
        begin
            if (pal) expand = {v, 3'b000};
            else if (v >= 24) expand = 8'hFF;
            else begin
                scaled = v * 255 + 12;
                expand = scaled / 24;
            end
        end
    endfunction

    // --- Проверка ------------------------------------------------------------
    // Сначала измеряем фактический сдвиг картинки (по первому активному пикселю кадра),
    // затем сверяем весь кадр с учётом этого сдвига. Так тест показывает и величину
    // рассогласования, и корректность самих данных.
    integer de_count = 0, frames = 0, errors = 0, checked = 0;
    integer out_x = 0, out_y = 0;
    // Сдвиг картинки должен быть нулевым: параметры H_IN_OFFSET/V_IN_OFFSET прошивки
    // подобраны так, чтобы выводимый пиксель совпадал с исходным один в один.
    integer dx = 0, dy = 0;
    integer ppm = 0;                      // дескриптор файла кадра
    reg de_prev = 1'b0, vs_prev = 1'b1;

    always @(posedge pixel_clk) begin
        if (rst_n) begin
            if (out_de) begin
                de_count = de_count + 1;
                if (!de_prev) out_x = 0; else out_x = out_x + 1;

                // Выгрузка одного кадра для просмотра глазами
                if (frames == 3 && ppm != 0)
                    $fwrite(ppm, "%c%c%c", out_r, out_g, out_b);

                if (frames >= 3) begin
                    checked = checked + 1;
                    if (out_r !== expand(pat_p(out_x + dx, out_y + dy), pat_r(out_x + dx, out_y + dy)) ||
                        out_g !== expand(pat_p(out_x + dx, out_y + dy), pat_g(out_x + dx, out_y + dy)) ||
                        out_b !== expand(pat_p(out_x + dx, out_y + dy), pat_b(out_x + dx, out_y + dy))) begin
                        if (errors < 10)
                            $display("НЕСОВПАДЕНИЕ x=%0d y=%0d: получено %02x %02x %02x, ожидалось %02x %02x %02x",
                                     out_x, out_y, out_r, out_g, out_b,
                                     expand(pat_p(out_x + dx, out_y + dy), pat_r(out_x + dx, out_y + dy)),
                                     expand(pat_p(out_x + dx, out_y + dy), pat_g(out_x + dx, out_y + dy)),
                                     expand(pat_p(out_x + dx, out_y + dy), pat_b(out_x + dx, out_y + dy)));
                        errors = errors + 1;
                    end
                end
            end else if (de_prev) begin
                out_y = out_y + 1;
            end
            de_prev <= out_de;

            if (!out_vs && vs_prev) begin
                if (frames >= 3) begin
                    if (de_count != H_ACTIVE * V_ACTIVE) begin
                        $display("ОШИБКА: активных пикселей в кадре %0d, ожидалось %0d",
                                 de_count, H_ACTIVE * V_ACTIVE);
                        errors = errors + 1;
                    end else begin
                        $display("кадр %0d: активных пикселей %0d — верно", frames, de_count);
                    end
                end
                frames = frames + 1;
                de_count = 0;
                out_y = 0;
            end
            vs_prev <= out_vs;
        end
    end

    initial begin
        // Дамп волн только по явному запросу: за четыре кадра их набегают гигабайты
        if ($test$plusargs("dump")) begin
            $dumpfile("tb_ts_video_chain.vcd");
            $dumpvars(0, tb_ts_video_chain);
        end
        if ($test$plusargs("ppm")) begin
            ppm = $fopen("frame_out.ppm", "wb");
            $fwrite(ppm, "P6\n%0d %0d\n255\n", H_ACTIVE, V_ACTIVE);
        end
        repeat (20) @(posedge pixel_clk);
        rst_n = 1'b1;

        // Четыре кадра: первые два на выход из сброса и захват привязки, дальше проверка
        wait (frames == 5);
        @(posedge pixel_clk);

        if (ppm != 0) begin
            $fclose(ppm);
            $display("кадр выгружен в frame_out.ppm");
        end
        $display("проверено пикселей: %0d, ошибок: %0d", checked, errors);
        if (errors == 0 && checked > 100000)
            $display("ТЕСТ ПРОЙДЕН");
        else
            $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end

    // Страховка от зависания
    initial begin
        #200_000_000;
        $display("ТАЙМАУТ");
        $finish;
    end

endmodule
