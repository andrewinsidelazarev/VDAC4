// Снимок F5/F6 (ядро 0.6, 03.11.2026): состояние таймеров YM (md_ym_stub st_q/st_we). Экземпляр A работает с
// записями в таймеры; на такте T его st_q грузится в B; дальше оба получают одни и те же записи — флаги
// (dout без занятости) и st_q обязаны совпадать на каждом такте. Так же — снова, через несколько переполнений.
// Вокруг загрузки такт ФМ стоит, как в ядре (FM_CLKEN на время снимка — SNAP_ACT).
// iverilog -g2012 -o ym.vvp ym_stub_state_tb.sv ../fpga/md_ym_stub.sv && vvp ym.vvp
`timescale 1ns/1ps
module ym_stub_state_tb;
reg clk = 0, cen = 0, rst = 1, wr = 0, st_we = 0, hold = 0;
reg [1:0] addr = 0; reg [7:0] din = 0;
wire [7:0] da, db; wire [63:0] qa, qb; wire sa, sb;
md_ym_stub A(.clk(clk), .rst(rst), .cen(cen), .addr(addr), .wr(wr), .din(din), .dout(da), .synth(sa),
             .st_we(1'b0), .st_d(64'b0), .st_q(qa));
md_ym_stub B(.clk(clk), .rst(rst), .cen(cen), .addr(addr), .wr(wr), .din(din), .dout(db), .synth(sb),
             .st_we(st_we), .st_d(qa), .st_q(qb));
always #5 clk = ~clk;
integer cyc = 0, errors = 0, checked = 0, loads = 0, flags = 0;
reg loaded = 0;
always @(posedge clk) begin
    cyc <= cyc + 1;
    cen <= !hold && (cyc % 7) == 0;                 // FM_CLKEN = MCLK/7
    if (loaded && !st_we) begin
        checked <= checked + 1;
        if (qa !== qb || da[1:0] !== db[1:0] || sa !== sb) begin
            errors <= errors + 1;
            if (errors < 5) $display("ОШИБКА такт %0d: A %016h/%02h B %016h/%02h", cyc, qa, da, qb, db);
        end
        if (da[1:0] != 0) flags <= flags + 1;
    end
end
task ym(input [1:0] p, input [7:0] r, input [7:0] v);
    begin
        @(negedge clk) begin addr = {p[0], 1'b0}; din = r; wr = 1; end
        @(negedge clk) wr = 0;
        @(negedge clk) begin addr = {p[0], 1'b1}; din = v; wr = 1; end
        @(negedge clk) wr = 0;
    end
endtask
task snap;
    begin
        @(negedge clk) hold = 1;
        repeat (3) @(negedge clk);
        @(negedge clk) st_we = 1;
        @(negedge clk) st_we = 0;
        repeat (3) @(negedge clk);
        hold = 0;
        loaded = 1; loads = loads + 1;
    end
endtask
initial begin
    repeat (10) @(negedge clk); rst = 0;
    ym(0, 8'h24, 8'hF0); ym(0, 8'h25, 8'h02); ym(0, 8'h26, 8'hE8);
    ym(0, 8'h27, 8'h0F);                            // оба таймера идут, флаги разрешены
    repeat (12345) @(negedge clk);
    snap;                                           // B догоняет A
    repeat (40000) @(negedge clk);
    ym(0, 8'h27, 8'h3F);                            // сброс флагов, таймеры идут дальше
    repeat (777) @(negedge clk);
    ym(0, 8'h2A, 8'h80);                            // адрес меняется (ЦАП)
    snap;                                           // повторная загрузка посреди счёта
    repeat (60000) @(negedge clk);
    ym(0, 8'h27, 8'h15);
    repeat (30000) @(negedge clk);
    if (errors == 0 && flags > 0 && checked > 100000)
        $display("ВСЁ ВЕРНО: загрузок %0d, тактов сверено %0d, тактов с флагами %0d", loads, checked, flags);
    else $display("ОШИБКА: расхождений %0d, сверено %0d, флагов %0d", errors, checked, flags);
    $finish;
end
endmodule
