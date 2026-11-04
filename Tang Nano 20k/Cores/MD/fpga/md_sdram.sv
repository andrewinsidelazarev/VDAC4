// SPDX-License-Identifier: GPL-3.0-or-later
// SDRAM Mega Drive для VDAC4 (03.11.2026): встроенная SDRAM GW2AR-18 (2M x 32: 4 банка, строка
// 11 бит, столбец 8 бит), такт — сам MCLK 54 МГц: у GW2AR-18 в этом корпусе только две PLL (вторая —
// HDMI), а MCLK и такт Z80 должны быть CLKOUT и CLKOUTD одной PLL. Порты MBUS и VDP — в том же домене.
//
// Доступ — «ACT, через такт READA/WRITEA» (строка закрывается сама), начинается в любом такте, где
// можно: шина команд свободна (не такт CAS прошлого доступа), банк отдохнул (4 такта от его ACT —
// 74 нс, больше tRC), запись — не раньше 3 тактов после чтения (его данные ещё на шине). Порядок:
// MBUS (68000 и Z80: от задержки зависит, будут ли такты ожидания) > видеопамять 16 бит > 32 бита >
// мост ESP. Видеопамять — банк 3, ROM, ОЗУ и SRAM — банки 0–2, так они не мешают друг другу.
// Модель (tests/md_model.sv, задержка памяти MD_MEM_DELAY) показала: с подтверждением MBUS через 2–5
// тактов циклы 68000 к ROM и ОЗУ идут без ожидания (3 импульса такта процессора на цикл, как с
// идеальной памятью); прежняя схема с жёсткими окнами (в среднем 9,3 такта) давала 4–6 импульсов
// у 77 % циклов — 68000 медленнее на 12 %.
//
// Время шины (54 МГц, T = 18,5 нс, CL2, tRCD — 1 такт). Такт микросхемы — CLKOUTP той же PLL со
// сдвигом 135°: микросхема принимает команду через 0,375 T после нашего фронта (команда держится
// с 3 нс после фронта до следующего: запас 4 нс до приёма). Данные чтения CL2 выходят после
// следующего её фронта: на шине с ~1,67 T до ~2,5 T после фронта загрузки READ — защёлкиваются
// регистром в IOB (dq_in) нашим фронтом через 2 T, отдаются порту ещё через такт. Итог для MBUS:
// запрос (фронт E0) — ACT загружен в такте 0, READ — в такте 1, dq_in — на фронте конца такта 3,
// подтверждение — на фронте конца такта 4: как память с задержкой 5.
// «Загружена в такте t» — регистр команды получает её на фронте в конце такта t.
//
// Обновление: когда пора — новые доступы не начинаются, банки отдыхают, REFRESH, 5 тактов (92 нс)
// без доступов. Каждые 7,4 мкс (4096 строк за 64 мс требуют 15,6).
// Порты — переключатели: req != ack — есть заявка; ack = req — запись принята (в такте CAS) или
// данные чтения в q.
`default_nettype none
module md_sdram #(
    parameter REFRESH_CYCLES = 400
)(
    input  wire        clk,             // MCLK 54 МГц
    input  wire        init_n,          // 0 — PLL ещё не готова
    output reg         ready=0,

    inout  wire [31:0] sd_dq,
    output reg  [10:0] sd_a=0,
    output reg  [1:0]  sd_ba=0,
    output reg  [3:0]  sd_dqm=4'b1111,
    output wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n,
    output wire        sd_cke,

    // MBUS: 16 бит, адрес байта; [1] — половина 32-битного слова; банки 0–2
    input  wire        a_req, output reg a_ack=0,
    input  wire        a_we,
    input  wire [22:1] a_addr,
    input  wire [1:0]  a_be,            // {UDS, LDS}
    input  wire [15:0] a_d,
    output reg  [15:0] a_q=0,

    // видеопамять (банк 3)
    input  wire        v_req, output reg v_ack=0,
    input  wire        v_we,
    input  wire [15:1] v_addr,
    input  wire [1:0]  v_be,
    input  wire [15:0] v_d,
    output reg  [15:0] v_q=0,
    input  wire        v32_req, output reg v32_ack=0,
    input  wire [15:2] v32_addr,
    output reg  [31:0] v32_q=0,

    // мост ESP: 32-битные слова, любой банк
    input  wire        h_req, output reg h_ack=0,
    input  wire        h_we,
    input  wire [22:2] h_addr,
    input  wire [3:0]  h_be,
    input  wire [31:0] h_d,
    output reg  [31:0] h_q=0
);

localparam [3:0] CMD_NOP=4'b0111, CMD_ACT=4'b0011, CMD_READ=4'b0101, CMD_WRITE=4'b0100,
                 CMD_PRE=4'b0010, CMD_REF=4'b0001, CMD_MODE=4'b0000;
// A10 —, A9 запись пакетом, A8:7 режим, A6:4 CL2, A3 последовательный, A2:0 длина пакета 1
localparam [10:0] MODE=11'b0_0_00_010_0_000;

reg [3:0] cmd=CMD_NOP;
assign {sd_cs_n,sd_ras_n,sd_cas_n,sd_we_n}=cmd;
assign sd_cke=1'b1;

reg [31:0] dq_out=0;
reg dq_oe=0;
assign sd_dq=dq_oe ? dq_out : 32'bz;
reg [31:0] dq_in=0;                     // регистр в IOB (ireg_in_iob)
always @(posedge clk) dq_in<=sd_dq;

// --- запуск: 200 мкс после PLL, PRECHARGE ALL, два REFRESH, MODE ---
reg [14:0] init_cnt=0;
reg [2:0] init_step=0;
reg init_done=0;

// --- обновление ---
reg [9:0] ref_cnt=0;
reg ref_due=0;
reg [2:0] ref_busy=0;

// --- доступы ---
localparam [1:0] P_M=0, P_V=1, P_V32=2, P_H=3;
reg [2:0] age [0:3];                    // тактов от ACT банка (до 7)
reg [2:0] rd_age=7;                     // тактов от ACT последнего чтения
reg cas_due=0;                          // следующий такт — CAS начатого доступа
reg c_we=0, c_half=0;
reg [1:0] c_port=0, c_bank=0;
reg [7:0] c_col=0;
reg [31:0] c_data=0;
reg [3:0] c_mask=0;
// чтения в пути: стадия 1 — READ загружен в прошлом такте, 2, 3 — данные в dq_in
reg [2:0] rv=0;
reg [1:0] rport [1:3];
reg rhalf [1:3];
integer i;
initial for (i=0;i<4;i=i+1) age[i]=3'd7;

wire m_pend=a_req!=a_ack, v_pend=v_req!=v_ack, v32_pend=v32_req!=v32_ack, h_pend=h_req!=h_ack;
wire [1:0] h_bank=h_addr[22:21];
wire [1:0] a_bank=a_addr[22:21];
function automatic [3:0] mask16(input half, input [1:0] be);
    mask16=half ? {~be,2'b11} : {2'b11,~be};
endfunction
// можно ли начать доступ: банк отдохнул (4 такта от ACT), запись — через 3 такта после чтения.
// Явными проводами, не функцией: функция, читающая age не через аргументы, в непрерывном
// присваивании не пересчитывается при смене age (симулятор — по аргументам).
wire [3:0] rested={age[3]>=3'd4,age[2]>=3'd4,age[1]>=3'd4,age[0]>=3'd4};
wire wr_ok=rd_age>=3'd3;
wire can=!cas_due && init_done && !ref_due && ref_busy==0;
wire go_m=can && m_pend && rested[a_bank] && (!a_we || wr_ok);
wire go_v=can && !go_m && v_pend && rested[3] && (!v_we || wr_ok);
wire go_v32=can && !go_m && !go_v && v32_pend && rested[3];
wire go_h=can && !go_m && !go_v && !go_v32 && h_pend && rested[h_bank] && (!h_we || wr_ok);
wire go=go_m|go_v|go_v32|go_h;
wire [1:0] g_bank=go_m ? a_bank : go_h ? h_bank : 2'd3;
wire [10:0] g_row=go_m ? a_addr[20:10] : go_v ? {5'b0,v_addr[15:10]} : go_v32 ? {5'b0,v32_addr[15:10]} : h_addr[20:10];
wire g_we=go_m ? a_we : go_v ? v_we : go_v32 ? 1'b0 : h_we;

always @(posedge clk) begin
    cmd<=CMD_NOP;
    dq_oe<=0;
    sd_dqm<=4'b0000;
    if (!init_n) begin
        init_cnt<=0; init_step<=0; init_done<=0; ready<=0; ref_cnt<=0; ref_due<=0; ref_busy<=0;
        cas_due<=0; rv<=0; rd_age<=3'd7; sd_dqm<=4'b1111;
        for (i=0;i<4;i=i+1) age[i]<=3'd7;
    end else if (!init_done) begin
        sd_dqm<=4'b1111;
        init_cnt<=init_cnt+1'b1;
        if (&init_cnt[13:0]) begin             // каждые 303 мкс (первый шаг — после 200 мкс)
            init_step<=init_step+1'b1;
            case (init_step)
                3'd0: begin cmd<=CMD_PRE; sd_a[10]<=1'b1; end
                3'd1, 3'd2: cmd<=CMD_REF;
                3'd3: begin cmd<=CMD_MODE; sd_a<=MODE; sd_ba<=0; end
                3'd4: begin init_done<=1; ready<=1; end
                default: ;
            endcase
        end
    end else begin
        for (i=0;i<4;i=i+1) if (age[i]!=3'd7) age[i]<=age[i]+3'd1;
        if (rd_age!=3'd7) rd_age<=rd_age+3'd1;
        if (ref_busy!=0) ref_busy<=ref_busy-3'd1;
        if (ref_cnt==REFRESH_CYCLES) begin ref_cnt<=0; ref_due<=1; end
        else ref_cnt<=ref_cnt+1'b1;

        // обновление: все банки отдохнули, чтений в пути нет
        if (ref_due && !cas_due && rv==0 && age[0]>=3'd5 && age[1]>=3'd5 && age[2]>=3'd5 && age[3]>=3'd5) begin
            cmd<=CMD_REF; ref_due<=0; ref_busy<=3'd5;
        end

        // ACT нового доступа
        if (go) begin
            cmd<=CMD_ACT; sd_ba<=g_bank; sd_a<=g_row;
            age[g_bank]<=3'd0;
            if (!g_we) rd_age<=3'd0;
            cas_due<=1; c_bank<=g_bank; c_we<=g_we;
            if (go_m) begin
                c_port<=P_M; c_col<=a_addr[9:2]; c_half<=a_addr[1]; c_data<={a_d,a_d}; c_mask<=mask16(a_addr[1],a_be);
            end else if (go_v) begin
                c_port<=P_V; c_col<=v_addr[9:2]; c_half<=v_addr[1]; c_data<={v_d,v_d}; c_mask<=mask16(v_addr[1],v_be);
            end else if (go_v32) begin
                c_port<=P_V32; c_col<=v32_addr[9:2]; c_half<=1'b0;
            end else begin
                c_port<=P_H; c_col<=h_addr[9:2]; c_half<=1'b0; c_data<=h_d; c_mask<=~h_be;
            end
        end

        // CAS начатого в прошлом такте
        rv<={rv[1:0],1'b0};
        if (cas_due) begin
            cas_due<=0;
            cmd<=c_we ? CMD_WRITE : CMD_READ;
            sd_ba<=c_bank; sd_a<={3'b100,c_col};            // A10 — закрыть строку после доступа
            if (c_we) begin
                dq_out<=c_data; dq_oe<=1; sd_dqm<=c_mask;
                case (c_port)
                    P_M: a_ack<=a_req;
                    P_V: v_ack<=v_req;
                    P_H: h_ack<=h_req;
                    default: ;
                endcase
            end else begin
                rv[0]<=1'b1; rport[1]<=c_port; rhalf[1]<=c_half;
            end
        end
        rport[2]<=rport[1]; rhalf[2]<=rhalf[1];
        rport[3]<=rport[2]; rhalf[3]<=rhalf[2];

        // данные чтения: READ загружен три такта назад, dq_in защёлкнут на прошлом фронте
        if (rv[2]) case (rport[3])
            P_M:   begin a_q<=rhalf[3] ? dq_in[31:16] : dq_in[15:0]; a_ack<=a_req; end
            P_V:   begin v_q<=rhalf[3] ? dq_in[31:16] : dq_in[15:0]; v_ack<=v_req; end
            P_V32: begin v32_q<=dq_in; v32_ack<=v32_req; end
            P_H:   begin h_q<=dq_in; h_ack<=h_req; end
        endcase
    end
end

endmodule
`default_nettype wire
