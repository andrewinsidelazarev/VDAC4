// Стенд md_sdram (03.11.2026): четыре порта со случайными чтениями и записями, у каждого своя
// область и своя теневая копия; каждое прочитанное слово сверяется. Модель микросхемы —
// sdr_chip_model из NanoMig (CL2). Порты MBUS и VDP меняют запросы только на фронтах MCLK
// — в ядре контроллер и порты в одном домене MCLK 54 МГц.
// iverilog -g2012 -o tb.vvp tb_sdram.sv ../fpga/md_sdram.sv sdr_chip_model.v && vvp tb.vvp
`timescale 1ns/1ps
module tb_sdram;
reg clk=0, init_n=0;
always #9.26 clk=~clk;                       // MCLK 54 МГц
wire mclk=clk;

wire [31:0] dq; wire [10:0] a; wire [1:0] ba; wire [3:0] dqm;
wire cs_n, ras_n, cas_n, we_n, cke, ready;
// микросхема — по спаду: такт SDRAM сдвинут (CLKOUTP 135°), команду она принимает между нашими фронтами
sdr_chip_model chip(.clk(~clk), .dq(dq), .addr(a), .dqm(dqm), .ba(ba),
    .cs_n(cs_n), .ras_n(ras_n), .cas_n(cas_n), .we_n(we_n));

reg a_req=0, a_we=0; reg [22:1] a_addr=0; reg [1:0] a_be=0; reg [15:0] a_d=0;
wire a_ack; wire [15:0] a_q;
reg v_req=0, v_we=0; reg [15:1] v_addr=0; reg [1:0] v_be=0; reg [15:0] v_d=0;
wire v_ack; wire [15:0] v_q;
reg v32_req=0; reg [15:2] v32_addr=0; wire v32_ack; wire [31:0] v32_q;
reg h_req=0, h_we=0; reg [22:2] h_addr=0; reg [3:0] h_be=0; reg [31:0] h_d=0;
wire h_ack; wire [31:0] h_q;

md_sdram #(.REFRESH_CYCLES(200)) dut(.clk(clk), .init_n(init_n), .ready(ready),
    .sd_dq(dq), .sd_a(a), .sd_ba(ba), .sd_dqm(dqm), .sd_cs_n(cs_n), .sd_ras_n(ras_n),
    .sd_cas_n(cas_n), .sd_we_n(we_n), .sd_cke(cke),
    .a_req(a_req), .a_ack(a_ack), .a_we(a_we), .a_addr(a_addr), .a_be(a_be), .a_d(a_d), .a_q(a_q),
    .v_req(v_req), .v_ack(v_ack), .v_we(v_we), .v_addr(v_addr), .v_be(v_be), .v_d(v_d), .v_q(v_q),
    .v32_req(v32_req), .v32_ack(v32_ack), .v32_addr(v32_addr), .v32_q(v32_q),
    .h_req(h_req), .h_ack(h_ack), .h_we(h_we), .h_addr(h_addr), .h_be(h_be), .h_d(h_d), .h_q(h_q));

// теневые копии
reg [15:0] sh_a [0:16383];     // MBUS: 8192 слов в банке 0 и 8192 в банке 2 (5F0000)
reg [15:0] sh_v [0:16383];     // VRAM: слова 0..3FFF
reg [31:0] sh_h [0:8191];      // мост: 4096 слов в банке 1 (200000) и 4096 в банке 3 (7F0000)
integer i, errors=0, n_a=0, n_v=0, n_v32=0, n_h=0, reads=0;
initial begin
    for (i=0;i<16384;i=i+1) begin sh_a[i]=0; sh_v[i]=0; end
    for (i=0;i<8192;i=i+1) sh_h[i]=0;
    for (i=0;i<2097152;i=i+1) chip.mem[i]=0;
end

function [22:1] a_map(input [13:0] k);    // слово тени MBUS -> адрес байта /2
    a_map=k[13] ? (22'h2F8000 + k[12:0]) : {9'b0,k[12:0]};
endfunction
function [22:2] h_map(input [12:0] k);
    h_map=k[12] ? (21'h1FC000 + k[11:0]) : (21'h080000 + k[11:0]);
endfunction

// --- MBUS ---
reg [13:0] a_k; reg a_busy=0;
always @(posedge mclk) if (ready) begin
    if (a_busy && a_req==a_ack) begin
        a_busy<=0;
        if (!a_we) begin
            reads=reads+1;
            if (a_q!==sh_a[a_k]) begin errors=errors+1;
                if (errors<10) $display("MBUS: слово %h адрес %h: %h вместо %h", a_k, {a_addr,1'b0}, a_q, sh_a[a_k]); end
        end
    end else if (!a_busy && ($random & 3)==0) begin
        a_k=$random; a_addr<=a_map(a_k); a_we<=$random; a_be<=$random|0; a_d<=$random;
        a_busy<=1; a_req<=~a_req; n_a=n_a+1;
    end
end
// запись в тень — в момент приёма (ack), с масками
always @(a_ack) if (a_we) begin
    if (a_be[1]) sh_a[a_k][15:8]=a_d[15:8];
    if (a_be[0]) sh_a[a_k][7:0]=a_d[7:0];
end

// --- VRAM 16 ---
reg v_busy=0;
always @(posedge mclk) if (ready) begin
    if (v_busy && v_req==v_ack) begin
        v_busy<=0;
        if (!v_we) begin
            reads=reads+1;
            if (v_q!==sh_v[v_addr[14:1]]) begin errors=errors+1;
                if (errors<10) $display("VRAM: слово %h: %h вместо %h", v_addr[14:1], v_q, sh_v[v_addr[14:1]]); end
        end
    end else if (!v_busy && ($random & 3)==0) begin
        v_addr<=$random & 15'h3FFF; v_we<=$random; v_be<=$random; v_d<=$random;
        v_busy<=1; v_req<=~v_req; n_v=n_v+1;
    end
end
always @(v_ack) if (v_we) begin
    if (v_be[1]) sh_v[v_addr[14:1]][15:8]=v_d[15:8];
    if (v_be[0]) sh_v[v_addr[14:1]][7:0]=v_d[7:0];
end

// --- VRAM 32 ---
reg v32_busy=0;
always @(posedge mclk) if (ready) begin
    if (v32_busy && v32_req==v32_ack) begin
        v32_busy<=0; reads=reads+1;
        if (v32_q!=={sh_v[{v32_addr[14:2],1'b1}],sh_v[{v32_addr[14:2],1'b0}]}) begin errors=errors+1;
            if (errors<10) $display("VRAM32: %h: %h", v32_addr, v32_q); end
    end else if (!v32_busy && ($random & 1)==0) begin
        v32_addr<=$random & 14'h1FFF; v32_busy<=1; v32_req<=~v32_req; n_v32=n_v32+1;
    end
end

// --- мост ---
reg [12:0] h_k; reg h_busy=0;
always @(posedge clk) if (ready) begin
    if (h_busy && h_req==h_ack) begin
        h_busy<=0;
        if (!h_we) begin
            reads=reads+1;
            if (h_q!==sh_h[h_k]) begin errors=errors+1;
                if (errors<10) $display("мост: %h: %h вместо %h", h_k, h_q, sh_h[h_k]); end
        end
    end else if (!h_busy && ($random & 7)==0) begin
        h_k=$random; h_addr<=h_map(h_k); h_we<=$random; h_be<=$random; h_d<=$random;
        h_busy<=1; h_req<=~h_req; n_h=n_h+1;
    end
end
always @(h_ack) if (h_we) begin
    if (h_be[3]) sh_h[h_k][31:24]=h_d[31:24];
    if (h_be[2]) sh_h[h_k][23:16]=h_d[23:16];
    if (h_be[1]) sh_h[h_k][15:8]=h_d[15:8];
    if (h_be[0]) sh_h[h_k][7:0]=h_d[7:0];
end

// --- проверки времени команд ---
integer last_act [0:3]; integer last_ref=-1000, cyc=0, tviol=0, clash=0;
// шина данных: микросхема и ПЛИС не ведут её вместе
always @(posedge clk or negedge clk) if (chip.v1 && dut.dq_oe) begin clash=clash+1; if (clash<5) $display("шина данных: ведут обе стороны, такт %0d", cyc); end
initial for (i=0;i<4;i=i+1) last_act[i]=-1000;
always @(posedge clk) begin
    cyc=cyc+1;
    if (!cs_n && {ras_n,cas_n,we_n}==3'b011) begin
        if (cyc-last_act[ba]<4) begin tviol=tviol+1; $display("tRC: банк %0d через %0d тактов", ba, cyc-last_act[ba]); end
        if (cyc-last_ref<4) begin tviol=tviol+1; $display("tRFC: ACT через %0d тактов после REFRESH", cyc-last_ref); end
        last_act[ba]=cyc;
    end
    if (!cs_n && {ras_n,cas_n,we_n}==3'b001) last_ref=cyc;
end

initial begin
    #100 init_n=1;
    wait(ready);
    #(18.52*200000);
    $display("операций: MBUS %0d, VRAM %0d, VRAM32 %0d, мост %0d; чтений сверено %0d; ошибок %0d; нарушений времени %0d; столкновений на шине %0d",
        n_a, n_v, n_v32, n_h, reads, errors, tviol, clash);
    if (errors==0 && tviol==0 && clash==0 && reads>1000) $display("ВСЁ ВЕРНО"); else $display("ОШИБКА");
    $finish;
end
endmodule
