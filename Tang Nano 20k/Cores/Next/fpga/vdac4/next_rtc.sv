// SPDX-License-Identifier: Apache-2.0
//
// Часы DS1307 для ядра ZX Next на VDAC4: ведомый I2C с адресом 0x68 на линиях портов 0x103B (SCL) и
// 0x113B (SDA) ядра. NextZXOS (RTC.SYS) читает время и подпись «ZX» в ОЗУ часов 0x3E…0x3F: без подписи
// часов как бы нет. Время приходит при старте ядра от ESP по LINK — это часы Gluk Evo, прочитанные
// плагином WC VDAC4 Cores (команда TIME слоя NEXT.ELF): load + time {день недели, часы, минуты, секунды}
// + date {год, месяц, число}, всё в BCD. Дальше часы идут сами (секунда — CLK_HZ тактов), записи
// NextZXOS (.date, .time) принимаются, как у DS1307; указатель регистров после 0x3F переходит на 0x00.
// Пока время не загружено, секунды стоят (бит CH = 1) и подписи нет — как без часов.
//
// Ведущий на шине один — процессор Next (порты по фронту CLK_28), поэтому линии синхронны такту и
// синхронизатор не нужен. Часы меняют SDA только при низком SCL; старт и стоп ловятся по SDA ведущего.
module next_rtc #(
    parameter int CLK_HZ = 28125000
) (
    input  wire        clk,
    input  wire        scl,             // линия SCL (1 — отпущена)
    input  wire        sda_m,           // SDA ведущего (1 — отпущена)
    output reg         sda_s = 1'b1,    // SDA часов (0 — тянут вниз)
    input  wire        load,            // такт: загрузить время из time_in / date_in
    input  wire [31:0] time_in,         // {день недели 1…7, часы, минуты, секунды}, BCD
    input  wire [23:0] date_in,         // {год 00…99, месяц, число}, BCD
    output wire [31:0] time_out,        // текущее время — для чтения по LINK
    output wire [23:0] date_out,
    output wire        valid            // время загружено (подпись «ZX» записана)
);
    reg [7:0] r [0:63];                 // 0…7 — время и управление, 8…63 — ОЗУ часов
    reg [7:0] snap [0:7];               // время на момент старта: чтение не разорвёт перенос
    reg       valid_r = 1'b0;
    integer   i;
    initial begin
        for (i = 0; i < 64; i = i + 1) r[i] = 8'h00;
        r[0] = 8'h80;                   // CH: часы стоят
        r[3] = 8'h01;
        r[4] = 8'h01;
        r[5] = 8'h01;
        for (i = 0; i < 8; i = i + 1) snap[i] = 8'h00;
    end

    assign time_out = {r[3], r[2], r[1], r[0]};
    assign date_out = {r[6], r[5], r[4]};
    assign valid    = valid_r;

    // --- BCD -------------------------------------------------------------------------
    // следующее значение после v: после last — first и перенос
    function automatic [8:0] bcd_next(input [7:0] v, input [7:0] last, input [7:0] first);
        if (v >= last)
            bcd_next = {1'b1, first};
        else if (v[3:0] >= 4'd9)
            bcd_next = {1'b0, v[7:4] + 4'd1, 4'd0};
        else
            bcd_next = {1'b0, v + 8'd1};
    endfunction

    // високосный год 2000…2099: год mod 4 = (2 × десятки + единицы) mod 4
    function automatic leap(input [7:0] y);
        reg [4:0] s;
        s = {3'd0, y[4], 1'b0} + {1'b0, y[3:0]};
        leap = (s[1:0] == 2'd0);
    endfunction

    function automatic [7:0] month_last(input [7:0] m, input [7:0] y);
        case (m)
            8'h02:                      month_last = leap(y) ? 8'h29 : 8'h28;
            8'h04, 8'h06, 8'h09, 8'h11: month_last = 8'h30;
            default:                    month_last = 8'h31;
        endcase
    endfunction

    // --- секунда ---------------------------------------------------------------------
    reg [24:0] pre = 25'd0;
    wire       tick = (pre == CLK_HZ - 1);

    // --- I2C -------------------------------------------------------------------------
    localparam [2:0] IDLE = 3'd0, RX = 3'd1, RXACK = 3'd2, TX = 3'd3, TXACK = 3'd4;
    reg [2:0] st = IDLE;
    reg [3:0] nb = 4'd0;                // принято или выставлено битов текущего байта
    reg [7:0] sh = 8'h00;
    reg       addr_ph = 1'b0;           // следующий байт — адрес
    reg       ptr_ph = 1'b0;            // следующий байт записи — номер регистра
    reg       rw = 1'b0;
    reg       nack = 1'b1;
    reg [5:0] ptr = 6'd0;
    reg       scl_d = 1'b1, sda_d = 1'b1;

    function automatic [7:0] rd(input [5:0] a);
        rd = (a < 6'd8) ? snap[a[2:0]] : r[a];
    endfunction

    always @(posedge clk) begin : clock
        reg [8:0] n;
        reg [7:0] b;
        // ход часов
        pre <= tick ? 25'd0 : pre + 25'd1;
        if (tick && !r[0][7]) begin
            n = bcd_next({1'b0, r[0][6:0]}, 8'h59, 8'h00);
            r[0] <= n[7:0];
            if (n[8]) begin
                n = bcd_next(r[1], 8'h59, 8'h00);
                r[1] <= n[7:0];
                if (n[8]) begin
                    n = bcd_next({2'b00, r[2][5:0]}, 8'h23, 8'h00);
                    r[2] <= n[7:0];
                    if (n[8]) begin
                        r[3] <= (r[3] >= 8'h07 || r[3] == 8'h00) ? 8'h01 : r[3] + 8'h01;
                        n = bcd_next(r[4], month_last(r[5], r[6]), 8'h01);
                        r[4] <= n[7:0];
                        if (n[8]) begin
                            n = bcd_next(r[5], 8'h12, 8'h01);
                            r[5] <= n[7:0];
                            if (n[8]) begin
                                n = bcd_next(r[6], 8'h99, 8'h00);
                                r[6] <= n[7:0];
                            end
                        end
                    end
                end
            end
        end

        // шина
        scl_d <= scl;
        sda_d <= sda_m;
        if (scl && scl_d && sda_d && !sda_m) begin          // старт (и повторный старт)
            st <= RX;
            nb <= 4'd0;
            addr_ph <= 1'b1;
            sda_s <= 1'b1;
            for (i = 0; i < 8; i = i + 1)
                snap[i] <= r[i];
        end else if (scl && scl_d && !sda_d && sda_m) begin  // стоп
            st <= IDLE;
            sda_s <= 1'b1;
        end else if (scl && !scl_d) begin                     // фронт SCL: бит ведущего
            if (st == RX) begin
                sh <= {sh[6:0], sda_m};
                nb <= nb + 4'd1;
            end else if (st == TXACK) begin
                nack <= sda_m;
            end
        end else if (!scl && scl_d) begin                     // спад SCL: ход часов на шине
            case (st)
                RX:
                    if (nb == 4'd8) begin
                        if (addr_ph) begin
                            if (sh[7:1] == 7'h68) begin
                                rw <= sh[0];
                                ptr_ph <= !sh[0];
                                sda_s <= 1'b0;                // ACK
                                st <= RXACK;
                            end else begin
                                st <= IDLE;                   // не нам
                            end
                        end else if (ptr_ph) begin
                            ptr <= sh[5:0];
                            ptr_ph <= 1'b0;
                            sda_s <= 1'b0;
                            st <= RXACK;
                        end else begin
                            r[ptr] <= sh;
                            if (ptr == 6'd0)
                                pre <= 25'd0;                 // запись секунд — счёт секунды заново
                            ptr <= ptr + 6'd1;
                            sda_s <= 1'b0;
                            st <= RXACK;
                        end
                    end
                RXACK: begin
                    addr_ph <= 1'b0;
                    if (addr_ph && rw) begin                  // чтение: первый байт
                        b = rd(ptr);
                        sh <= b;
                        sda_s <= b[7];
                        ptr <= ptr + 6'd1;
                        nb <= 4'd1;
                        st <= TX;
                    end else begin
                        sda_s <= 1'b1;
                        nb <= 4'd0;
                        st <= RX;
                    end
                end
                TX:
                    if (nb == 4'd8) begin
                        sda_s <= 1'b1;                        // ACK — ведущему
                        st <= TXACK;
                    end else begin
                        sda_s <= sh[4'd7 - nb];
                        nb <= nb + 4'd1;
                    end
                TXACK:
                    if (!nack) begin                          // ACK: следующий байт
                        b = rd(ptr);
                        sh <= b;
                        sda_s <= b[7];
                        ptr <= ptr + 6'd1;
                        nb <= 4'd1;
                        st <= TX;
                    end else begin
                        sda_s <= 1'b1;
                        st <= IDLE;
                    end
                default: ;
            endcase
        end

        // время от ESP (часы Evo): поверх всего
        if (load) begin
            r[0] <= {1'b0, time_in[6:0]};
            r[1] <= {1'b0, time_in[14:8]};
            r[2] <= {2'b00, time_in[21:16]};
            r[3] <= time_in[31:24];
            r[4] <= date_in[7:0];
            r[5] <= date_in[15:8];
            r[6] <= date_in[23:16];
            r[7] <= 8'h00;
            r[62] <= 8'h5A;                                   // 'Z'
            r[63] <= 8'h58;                                   // 'X'
            pre <= 25'd0;
            valid_r <= 1'b1;
        end
    end
endmodule
