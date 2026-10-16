// Модель SPI-flash (режим 0) для тестбенча fb_flash: 0x03 чтение, 0x9F JEDEC ID, 0x05 состояние,
// 0x06/0x04 разрешение/запрет записи, 0x02 запись страницы (только 1 → 0, адрес в пределах
// страницы 256 байт), 0x20 стирание сектора 4 КБ. Пока идёт запись или стирание (WIP),
// принимается только 0x05. Память — два сектора: 0x000000 и сектор настроек SET_ADDR;
// остальные адреса читаются узором.
// Нарушения протокола считаются в errors: команда при WIP, запись без WREN, CS неактивен
// меньше T_CSH_NS, команда оборвана на середине байта.
`timescale 1ns/1ps
module spi_flash_model #(
    parameter logic [23:0] SET_ADDR = 24'h7F_F000,
    parameter real         T_PP_NS  = 20_000.0,
    parameter real         T_SE_NS  = 100_000.0,
    parameter real         T_CSH_NS = 50.0
) (
    input  wire sck,
    input  wire cs_n,
    input  wire mosi,
    output reg  miso
);
    reg [7:0] mem_low [0:4095];
    reg [7:0] mem_set [0:4095];
    integer   errors = 0;
    integer   reads_set = 0;     // байтов прочитано из сектора настроек

    reg [7:0]  cmd;
    reg [23:0] addr;
    reg [7:0]  sh;
    integer    nbits;
    reg [7:0]  cur;
    integer    oidx;
    reg        wel = 1'b0, wip = 1'b0;
    realtime   cs_rise = -1000.0;
    realtime   wip_ns = 0.0;

    // Конец записи или стирания — отдельным процессом: CS тем временем переключается
    // опросом состояния
    always @(posedge wip) begin
        #(wip_ns) wip = 1'b0;
    end

    initial begin : init
        integer i;
        for (i = 0; i < 4096; i = i + 1) begin mem_low[i] = 8'hFF; mem_set[i] = 8'hFF; end
        miso = 1'b1;
    end

    function automatic [7:0] rd(input [23:0] a);
        if (a[23:12] == SET_ADDR[23:12]) rd = mem_set[a[11:0]];
        else if (a[23:12] == 12'd0)      rd = mem_low[a[11:0]];
        else                             rd = a[7:0] ^ 8'h5A;
    endfunction

    task automatic prog(input [23:0] a, input [7:0] v);
        if (a[23:12] == SET_ADDR[23:12]) mem_set[a[11:0]] = mem_set[a[11:0]] & v;
        else if (a[23:12] == 12'd0)      mem_low[a[11:0]] = mem_low[a[11:0]] & v;
    endtask

    always @(negedge cs_n) begin
        if ($realtime - cs_rise < T_CSH_NS) begin
            errors = errors + 1;
            $display("[%0t] flash: CS неактивен %0.1f нс", $time, $realtime - cs_rise);
        end
        nbits = 0; oidx = 0; cmd = 8'h00; addr = '0;
    end

    always @(posedge sck) if (!cs_n) begin
        sh = {sh[6:0], mosi};
        nbits = nbits + 1;
        if (nbits == 8) begin
            cmd = sh;
            if (wip && cmd != 8'h05) begin
                errors = errors + 1;
                $display("[%0t] flash: команда %02h во время записи", $time, cmd);
            end
        end else if (nbits > 8 && nbits <= 32) begin
            addr = {addr[22:0], mosi};
        end
        // данные записи страницы
        if (cmd == 8'h02 && nbits > 32 && (nbits - 32) % 8 == 0 && !wip) begin
            if (!wel) begin
                errors = errors + 1;
                $display("[%0t] flash: запись без WREN", $time);
            end else
                prog({addr[23:8], 8'(addr[7:0] + (nbits - 40) / 8)}, sh);
        end
    end

    always @(negedge sck) if (!cs_n) begin
        integer start;
        start = (cmd == 8'h03) ? 32 : (cmd == 8'h9F || cmd == 8'h05) ? 8 : -1;
        if (start >= 0 && nbits >= start) begin
            if ((nbits - start) % 8 == 0) begin
                case (cmd)
                    8'h03: begin
                        cur = rd(addr + oidx);
                        if ((addr + oidx) >> 12 == SET_ADDR >> 12) reads_set = reads_set + 1;
                    end
                    8'h9F: cur = (oidx % 3 == 0) ? 8'hEF : (oidx % 3 == 1) ? 8'h40 : 8'h17;
                    default: cur = {6'd0, wel, wip};
                endcase
                oidx = oidx + 1;
            end
            miso = cur[7 - (nbits - start) % 8];
        end
    end

    always @(posedge cs_n) begin
        cs_rise = $realtime;
        miso = 1'b1;
        if (nbits % 8 != 0) begin
            errors = errors + 1;
            $display("[%0t] flash: CS снят на %0d-м бите", $time, nbits);
        end
        if (!wip) begin
            case (cmd)
                8'h06: if (nbits == 8) wel = 1'b1;
                8'h04: if (nbits == 8) wel = 1'b0;
                8'h02: if (wel && nbits > 32) begin
                    wel = 1'b0; wip_ns = T_PP_NS; wip = 1'b1;
                end
                8'h20: if (nbits == 32) begin
                    if (!wel) begin
                        errors = errors + 1;
                        $display("[%0t] flash: стирание без WREN", $time);
                    end else begin : erase
                        integer i;
                        for (i = 0; i < 4096; i = i + 1)
                            if (addr[23:12] == SET_ADDR[23:12]) mem_set[i] = 8'hFF;
                            else if (addr[23:12] == 12'd0)   mem_low[i] = 8'hFF;
                        wel = 1'b0; wip_ns = T_SE_NS; wip = 1'b1;
                    end
                end
                default: ;
            endcase
        end
    end
endmodule
