// VDAC4 debug5: контроллер по спаду clk, DQ по фронту; такт SDRAM на 90 градусов после рабочего фронта.
//
// sdram.sv
//
// sdram controller implementation for the MiSTer SDRAM, the TN20k etc
//
// Copyright (c) 2024 Till Harbaum <till@harbaum.org>
// Copyright (c) 2026 Mateusz Nalewajski <mateusz@nalewajski.pl>
//
// This source file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This source file is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

// This should work for various RAM sized in 16 or 32 data widths.
//
// Example 1: TN20k internal sdram
// 32 data, 4 dqm, 11 RAS, 2 bank, 8 CAS
//     -> 2^(11+2+8)*(32/8) = 8388608 = 8MB
// address map:  BA:21..20 RAS:19..9 CAS:8..1  data mux: 0
//
// Example 2: MiSTer RAM module
// 16 data, 2 dqm, 13 RAS, 2 bank, 9 CAS
//     -> 2^(13+2+9)*(16/8) = 33554432 = 32MB
// address map:  BA:--  RAS:21..9 CAS:8..0  no data mux

`ifdef YOSYS
`define ITOR(x) $itor(x)
`define RTOI(x) $rtoi(x)
`else
`define ITOR(x) (real'(int'(x)))
`define RTOI(x) (int'(real'(x)))
`endif

`define CEIL(x) `RTOI($ceil(x))

`default_nettype none

module sdram #(
  parameter DATA_WIDTH = 16,
  parameter RAS_WIDTH  = 13,
  parameter CAS_WIDTH  =  9,
  parameter RAM_CLOCK_SPEED  = 85_000_000,
  parameter SYNC_CLOCK_SPEED =  7_080_000,
  parameter CHIP48_BURST = 0,
  parameter SYNC_DELAY = 3,  // delay higher than 4 breaks FX68K build
  parameter VIDEO_PORT = 0,
  parameter VIDEO_STREAM = 0,
  parameter ACK_DELAY = 1
) (
  input  wire clk,
  input  wire reset_n,  // init signal after FPGA config to initialize RAM
  output wire ready,    // ram is ready and has been initialized

  inout  wire [DATA_WIDTH-1:0]   sd_data, // 16/32 bit bidirectional data bus
  output reg  [RAS_WIDTH-1:0]    sd_addr, // multiplexed address bus
  output reg  [DATA_WIDTH/8-1:0] sd_dqm,  // two/four byte masks
  output reg  [1:0]              sd_ba,   // four banks

  output reg  sd_cke,  // clock enable
  output wire sd_cs,   // single chip select
  output wire sd_we,   // write enable
  output wire sd_ras,  // row address select
  output wire sd_cas,  // columns address select

  // chipset interface
  input  wire sync,     // chipset synchronization
  input  wire refresh,  // chipset requests a refresh cycle

  input  wire [15:0] din,     // data input from chipset
  output reg  [15:0] dout,    // data output for chipset
  output reg  [47:0] dout48,  // 64-bit read data output for chipset

  input wire [21:0] addr,  // 22 bit word address for 8MB
  input wire  [1:0] ds,    // upper/lower data strobe
  input wire        cs,    // chipset requests read/write
  input wire        we,    // chipset requests write

  // cpu interface
  input  wire [15:0] p2_din,    // data input from cpu
  output reg  [15:0] p2_dout,   // data output to cpu
  output reg  [47:0] p2_dout48, // upper 48 bits of a 64 bit aligned cpu read

  input  wire [22:0] p2_addr,  // 23 bit word address
  input  wire  [1:0] p2_ds,    // upper/lower data strobe
  input  wire        p2_cs,    // cpu requests read/wrie
  input  wire        p2_we,    // cpu requests write
  output wire        p2_ack,   // cpu ack
  input wire video_cs, video_we,
  input wire video_double, // чтение 32 байт: два пакета BL4 одной строки SDRAM
  input wire video_urgent, // видео отстаёт от графика: важнее CPU (порт 2)
  input wire [20:0] video_addr, // 32-bit word address, 16-byte aligned
  input wire [127:0] video_din,
  output reg [127:0] video_dout,
  output reg video_read_valid,
  output reg [1:0] video_fetch_index,
  output reg video_ack,
  output reg video_single // 8.11.2026: вместе с video_ack — обслужен один блок 16 байт (обращение B)
);

localparam WIDTH32 = (DATA_WIDTH == 32);

// 000 = 1, 001 = 2, 010 = 4, 011 = 8
localparam BURST_LENGTH   = CHIP48_BURST ? 3'b010 : 3'b000;
localparam ACCESS_TYPE    = 1'b0;    // 0 = sequential, 1 = interleaved
localparam CAS_LATENCY    = 3'd2;    // 2/3 allowed
localparam OP_MODE        = 2'b00;   // only 00 (standard operation) allowed
// 7 ноября 2026: одиночная запись, как в рабочей 0.11. Пакетная запись BL4 с маской DQM
// (0.12–0.12.4) ломала Amiga на плате: каждая запись CPU/чипсета/ROM шла пакетом.
// Видео пишет четырьмя командами WRITE подряд по соседним столбцам.
localparam NO_WRITE_BURST = 1'b1;    // 0 = write burst enabled, 1 = only single access write

localparam MODE = {3'b0, NO_WRITE_BURST, OP_MODE, CAS_LATENCY, ACCESS_TYPE, BURST_LENGTH};

// expand to from 22 to 32 address bits internally to be able to drive bigger rams as well
// and shift one bit for 32 bit data bus as addr[0] is used to multiplex between
// both 16 bit data words
localparam ADDR_BASE = WIDTH32 ? 1 : 0;

wire [31:0]    addr32 = { {(10+ADDR_BASE){1'b0}},    addr[21:ADDR_BASE]};
wire [31:0] p2_addr32 = { {( 9+ADDR_BASE){1'b0}}, p2_addr[22:ADDR_BASE]};

reg addr_0;

localparam real CLOCK_PERIOD_NANO_SEC = 1.0e9 / `ITOR(RAM_CLOCK_SPEED);
localparam real SYNC_PERIOD_NANO_SEC  = 1.0e9 / `ITOR(SYNC_CLOCK_SPEED);

// W9864G2GH: 200 us, DQM high, eight AUTO REFRESH; compact sequencer.
localparam real SETTING_INHIBIT_DELAY_MICRO_SEC = 200;

// tRFC - Min autorefresh period
localparam real SETTING_T_RFC_MIN_AUTOREFRESH_PERIOD_NANO_SEC = 60;

// tRP - Min precharge command period
localparam real SETTING_T_RP_MIN_PRECHARGE_CMD_PERIOD_NANO_SEC = 15;

// 8,192 refresh commands every 64ms = 7.8125us, which we round to 7500ns to make sure we hit them all
localparam real SETTING_REFRESH_TIMER_NANO_SEC = 7500;

// Number of cycles for autorefresh duration
localparam CLOCK_CYCLES_FOR_AUTOREFRESH =
    `CEIL(SETTING_T_RFC_MIN_AUTOREFRESH_PERIOD_NANO_SEC / CLOCK_PERIOD_NANO_SEC);

// Number of cycles after reset until we clear command inhibit and start operation
// We add 100 cycles for good measure
localparam CLOCK_CYCLES_UNTIL_INIT_PRECHARGE = 100 +
    `CEIL(SETTING_INHIBIT_DELAY_MICRO_SEC * 1000.0 / CLOCK_PERIOD_NANO_SEC);

// Number of cycles after reset until we are done with precharge
// We add 10 cycles for good measure
localparam CLOCK_CYCLES_UNTIL_INIT_PRECHARGE_END = 10 + CLOCK_CYCLES_UNTIL_INIT_PRECHARGE +
    `CEIL(SETTING_T_RP_MIN_PRECHARGE_CMD_PERIOD_NANO_SEC / CLOCK_PERIOD_NANO_SEC);

localparam CLOCK_CYCLES_UNTIL_REFRESH1_END = CLOCK_CYCLES_UNTIL_INIT_PRECHARGE_END + CLOCK_CYCLES_FOR_AUTOREFRESH;


// tMRD = 2 - Min number of clock cycles between mode set and normal usage
localparam CLOCK_CYCLES_UNTIL_INIT_DONE = CLOCK_CYCLES_UNTIL_REFRESH1_END + 2;

localparam INIT_COUNTER_MAX   = CLOCK_CYCLES_UNTIL_INIT_DONE;
localparam INIT_COUNTER_WIDTH = $clog2(INIT_COUNTER_MAX + 1);

reg [INIT_COUNTER_WIDTH-1:0] init_counter = 0;
reg [4:0] init_refresh_count = 0;

// Number of sync cycles between each autorefresh command
localparam SYNC_CYCLES_PER_REFRESH =
    `CEIL(SETTING_REFRESH_TIMER_NANO_SEC / SYNC_PERIOD_NANO_SEC);

localparam REFRESHCNT_MAX       = SYNC_CYCLES_PER_REFRESH;
localparam REFRESHCNT_MAX_WIDTH = $clog2(REFRESHCNT_MAX + 1);

reg [REFRESHCNT_MAX_WIDTH-1:0] refreshcnt;

// ---------------------------------------------------------------------
// ------------------------ cycle state machine ------------------------
// ---------------------------------------------------------------------

localparam STATE_INIT       = 0;
localparam STATE_RAS        = 1;  // first state in cycle
localparam STATE_RAS_WAIT_0 = 2;
localparam STATE_CAS        = 3;
localparam STATE_CAS_WAIT_0 = 4;
localparam STATE_CAS_WAIT_1 = 5;
localparam STATE_READ_0     = 6;
localparam STATE_READ_1     = 7;
localparam STATE_READ_2     = 8;
localparam STATE_READ_3     = 9;
localparam STATE_LAST       = STATE_READ_3;
localparam STATE_READ_DATA  = STATE_READ_0;

localparam STATE_WIDTH = $clog2(STATE_LAST + 1);

reg [STATE_WIDTH-1:0] state;

wire                  sync_i;
reg  [SYNC_DELAY-1:0] sync_d;

always @(negedge clk, negedge reset_n) begin
  if (!reset_n)
    sync_d <= 0;
  else
    sync_d <= {sync_d[SYNC_DELAY-2:0], sync};
end

assign sync_i = sync_d[SYNC_DELAY-1] && !sync_d[SYNC_DELAY-2];
assign ready  = (state != STATE_INIT);

always @(negedge clk, negedge reset_n) begin
  if (!reset_n) begin
    init_counter <= 0;
    init_refresh_count <= 0;
    state        <= STATE_INIT;

  end else begin
    case (state)
      STATE_INIT: begin
        init_counter <= init_counter + 1;
        // Повторяем один интервал tRFC: не нужны семь широких декодеров таймера.
        if (init_counter == CLOCK_CYCLES_UNTIL_REFRESH1_END[INIT_COUNTER_WIDTH-1:0] && init_refresh_count != 7) begin
          init_counter <= CLOCK_CYCLES_UNTIL_INIT_PRECHARGE_END[INIT_COUNTER_WIDTH-1:0] + 1'b1;
          init_refresh_count <= init_refresh_count + 1'b1;
        end

        if (init_counter == CLOCK_CYCLES_UNTIL_INIT_DONE[INIT_COUNTER_WIDTH-1:0])
          state <= STATE_RAS;
      end
      STATE_RAS:
        if (sync_i) state <= STATE_RAS_WAIT_0;
      STATE_RAS_WAIT_0:
        state <= STATE_CAS;
      STATE_CAS:
        state <= STATE_CAS_WAIT_0;
      STATE_CAS_WAIT_0:
        state <= STATE_CAS_WAIT_1;
      STATE_CAS_WAIT_1:
        state <= STATE_READ_0;
      STATE_READ_0:
        state <= STATE_READ_1;
      STATE_READ_1:
        state <= STATE_READ_2;
      STATE_READ_2:
        state <= STATE_READ_3;
      STATE_READ_3:
        state <= STATE_RAS;
    endcase
    if (state != STATE_INIT) begin
      if (!p2_cs) init_refresh_count <= 0;
      else if (state == STATE_RAS && sync_i && !cs && refreshcnt != 0) begin
        if (VIDEO_PORT && video_cs) init_refresh_count <= init_refresh_count + 1'b1;
        else init_refresh_count <= 0;
      end
    end
  end
end

// ---------------------------------------------------------------------
// ------------------ generate ram control signals ---------------------
// ---------------------------------------------------------------------

// all possible commands
localparam CMD_NOP             = 3'b111;
localparam CMD_ACTIVE          = 3'b011;
localparam CMD_READ            = 3'b101;
localparam CMD_WRITE           = 3'b100;
localparam CMD_BURST_TERMINATE = 3'b110;
localparam CMD_PRECHARGE       = 3'b010;
localparam CMD_AUTO_REFRESH    = 3'b001;
localparam CMD_LOAD_MODE       = 3'b000;

reg [2:0] sd_cmd;   // current command sent to sd ram

// drive control signals according to current command
assign sd_cs  = 1'b0;
assign sd_ras = sd_cmd[2];
assign sd_cas = sd_cmd[1];
assign sd_we  = sd_cmd[0];

reg  [15:0] ram_din;
reg         ram_we;
reg   [1:0] ram_ds;

wire [15:0] ram_dout_lo;
wire [15:0] ram_dout_hi;
wire [15:0] ram_dout;

reg                 sd_dq;
reg [RAS_WIDTH-1:0] sd_addr_cas;

reg p2_ack_i;

generate
  if (WIDTH32) begin
    reg [31:0] dq_q;
    always @(posedge clk) dq_q <= sd_data;
    assign ram_dout_lo = dq_q[15: 0];
    assign ram_dout_hi = dq_q[31:16];
    assign ram_dout    = (addr_0 ? ram_dout_lo : ram_dout_hi);
    reg [31:0] dq_out;
    always @(posedge clk)
      dq_out <= VIDEO_PORT && output_video ? video_word :
                addr_0 ? {16'b0, ram_din} : {ram_din, 16'b0};
    assign sd_data = sd_dq ? dq_out : {DATA_WIDTH{1'bz}};

  end else begin
    assign ram_dout_lo = sd_data;
    assign ram_dout_hi = sd_data;
    assign ram_dout    = sd_data;
    assign sd_data     = sd_dq ? ram_din : {DATA_WIDTH{1'bz}};
  end
endgenerate

localparam PORT_1       = 2'd0;
localparam PORT_2       = 2'd1;
localparam PORT_REFRESH = 2'd2;
localparam PORT_IDLE    = 3'd4;

localparam PORT_VIDEO = 3'd3;
reg [2:0] sdram_port;

reg [1:0] video_word_index;
// 7 ноября 2026: CPU (порт 2) важнее видео, пока видео успевает: при прежнем
// постоянном приоритете видео CPU получал 1 слот из 32, выборка команд из
// ROM/fast RAM почти стояла. Видео обгоняет CPU, только когда отстаёт
// (video_urgent из DMA), и тогда не больше 31 слота подряд.
// Чипсет и Refresh сохраняют свой приоритет. Счётчик считает только
// видеопередачи, которые действительно отложили ожидающий запрос CPU.
// Счётчик начальных Refresh свободен после STATE_INIT.
wire [4:0] video_streak = init_refresh_count;
// 8 ноября 2026: в слоте, который хотят и видео, и CPU, они идут по очереди (video_turn).
// Прежде CPU всегда шёл первым, а видео — только когда уже отстало: на плате в Workbench
// (блиттер занимает нечётные слоты, CPU — чётные) захват пропускал половины строк (призраки
// старого изображения), а чтение опаздывало (чёрные строки). Отстающее видео по-прежнему
// идёт впереди CPU до 31 слота подряд.
// 8 ноября 2026: второе обращение в слоте (B) — чтение видео в его банк (кадровый буфер — банк 3),
// внахлёст с обращением A чипсета или CPU в другой банк. Слот 7 МГц = 12 тактов; команды выходят на
// такт позже своего состояния:
//   A: ACTIVE (такт 2), READ/WRITE с автопрегардом (4), данные READ на DQ 5..8;
//   B: ACTIVE своего банка (5, tRRD 3 такта), READ с автопрегардом (8, tRCD 3), данные 9..12 —
//      те же такты и тот же приём (video_second), что у второго пакета сдвоенного чтения; прегард
//      банка B кончается к ACTIVE следующего слота (такт 13), как и у сдвоенного чтения.
// Запись A (одно слово на такте 4) DQ к 9..12 не занимает. Регенерация (все банки) и обращение A в
// банк видео (копия ROM в банке 3) — без B. Видео-запись по-прежнему только в A. Чтение видео, которое
// в споре с CPU уступало слот, теперь идёт в B того же слота: обслужены оба. Прежде видео делило с
// чипсетом и CPU одно обращение на слот, и при работе блиттера в Workbench не успевало (полосы).
wire [31:0] video_addr32 = {11'd0,video_addr};
localparam BANK_LO = RAS_WIDTH+CAS_WIDTH;
wire [1:0] video_bank = video_addr32[BANK_LO+1:BANK_LO];
wire [1:0] chip_bank  = addr32[BANK_LO+1:BANK_LO];
wire [1:0] p2_bank    = p2_addr32[BANK_LO+1:BANK_LO];
// 8 ноября 2026 (0.13.1, проверка): обращение B выключено. Версия: на 0.12.15 (без B) пользователь видел
// «срыва нет», с 0.12.17 (с B) развёртка в Workbench снова срывается — проверяется связь с нагрузкой SDRAM
// (два ACTIVE на слот). Без B видео делит с чипсетом и CPU одно обращение на слот, как в 0.12.16.
localparam VIDEO_B = 1'b0;
wire video_rd_b = VIDEO_B && VIDEO_PORT && VIDEO_STREAM && video_cs && !video_we;
wire b_p2_ok    = video_rd_b && p2_bank != video_bank;  // CPU в A, чтение видео в B
reg video_b = 1'b0;                       // в этом слоте идёт обращение B
reg [RAS_WIDTH-1:0] vb_row = 0;
reg [CAS_WIDTH-1:0] vb_col = 0;
reg [1:0] vb_bank = 0;
reg video_turn = 1'b1;
wire video_grant = VIDEO_PORT && video_cs && (!p2_cs || (!b_p2_ok && (video_turn || (video_urgent && video_streak != 31))));
wire b_go = video_rd_b && ((cs && !refresh && chip_bank != video_bank) ||
            (!cs && refreshcnt != 0 && !video_grant && p2_cs && p2_bank != video_bank));
always @(negedge clk or negedge reset_n) begin
  if (!reset_n) video_turn <= 1'b1;
  else if (state == STATE_RAS && sync_i && !cs && refreshcnt != 0 && p2_cs && VIDEO_PORT && video_cs)
    video_turn <= !video_grant;   // видео взяло спорный слот — следующий спорный за CPU
end
// Decode the next write source on the arbitration edge. DQ then has
// a single select from this register to its rising-edge I/O register.
reg output_video=0;
always @(negedge clk or negedge reset_n) begin
  if(!reset_n)output_video<=0;
  else if(state==STATE_RAS && sync_i)output_video<=!cs && refreshcnt!=0 && video_grant;
end
wire [31:0] video_word = VIDEO_STREAM ? video_din[31:0]:video_din[video_word_index*32 +: 32];
// Сдвоенное чтение видео (7 ноября 2026): READ на CAS без автопрегарда и
// второй READ столбца +4 с автопрегардом на READ_1, впритык к первому пакету.
// Слова второго пакета приходят на четырёх спадах после READ_3, вне автомата
// состояний; прегард начинается через BL после второго READ, и к ACTIVE
// следующего слота (через 12 тактов) tRP выдержан. DMA просит его только для
// пары, выровненной на 8 столбцов: второй столбец — тот же с битом 2.
reg video_dbl = 0;
reg [2:0] video_second = 0;
always @(negedge clk or negedge reset_n) begin
  if (!reset_n) begin video_ack<=0; video_single<=0; video_dout<=0; video_word_index<=0;video_fetch_index<=0;video_read_valid<=0;video_second<=0; end
  else begin
    video_read_valid<=0;
    if(VIDEO_STREAM && state==STATE_READ_3 && ((sdram_port==PORT_VIDEO && !ram_we && video_dbl) || video_b)) video_second<=4;
    if(VIDEO_PORT && video_b && state==STATE_READ_3) begin video_ack<=~video_ack; video_single<=1'b1; end
    else if(video_second!=0) video_second<=video_second-1'b1;
    if(VIDEO_STREAM && video_second!=0) begin video_dout[31:0]<={ram_dout_hi,ram_dout_lo};video_read_valid<=1; end
    if(state==STATE_LAST) video_fetch_index<=0;
    if(state==STATE_RAS_WAIT_0) video_word_index<=0;
    // Streaming writes cross three registers: capture read address (rising edge),
    // capture BRAM, then DQ OREG. Request the next word before CAS so the pad
    // holds the correct beat across the 270-degree SDRAM clock, including clock/OBUF delays.
    // 8 ноября 2026: каждое слово просится на такт раньше прежнего — в захвате добавлен триггер
    // адреса по фронту (amiga_video_capture.sv, read_address_q), BRAM больше не берёт адрес со спада.
    // Слово 0 (STATE_LAST прошлого слота) и такты WRITE на DQ не меняются.
    if(state==STATE_RAS && sync_i)video_fetch_index<=1;
    if(state==STATE_RAS_WAIT_0)video_fetch_index<=2;
    if(state==STATE_CAS)video_fetch_index<=3;
    if(sdram_port==PORT_VIDEO) begin
      if(state==STATE_CAS_WAIT_0) video_word_index<=1;
      if(state==STATE_CAS_WAIT_1) video_word_index<=2;
      if(state==STATE_READ_0) video_word_index<=3;
      if(!ram_we) begin
        if(VIDEO_STREAM) begin
          if(state==STATE_READ_0 || state==STATE_READ_1 || state==STATE_READ_2 || state==STATE_READ_3)begin
            video_dout[31:0]<={ram_dout_hi,ram_dout_lo};video_read_valid<=1;
          end
        end else begin
        if(state==STATE_READ_0) video_dout[31:0]<={ram_dout_hi,ram_dout_lo};
        if(state==STATE_READ_1) video_dout[63:32]<={ram_dout_hi,ram_dout_lo};
        if(state==STATE_READ_2) video_dout[95:64]<={ram_dout_hi,ram_dout_lo};
        if(state==STATE_READ_3) video_dout[127:96]<={ram_dout_hi,ram_dout_lo};
        end
      end
      if(state==STATE_READ_3) begin video_ack<=~video_ack; video_single<=1'b0; end
    end
  end
end

always @(negedge clk, negedge reset_n) begin
  if (!reset_n)
    sd_cke <= 0;
  else
    sd_cke <= 1;
end

// Вне команд значение не важно: по умолчанию адрес столбца, он уходит на такт раньше READ/WRITE.
reg [RAS_WIDTH-1:0] sd_addr_next;
always @* begin
  sd_addr_next = sd_addr_cas;
  case (state)
    STATE_INIT: begin
      sd_addr_next = {RAS_WIDTH{1'b0}};
      if (init_counter == CLOCK_CYCLES_UNTIL_INIT_PRECHARGE[INIT_COUNTER_WIDTH-1:0])
        sd_addr_next[10] = 1'b1;
      else if (init_counter == CLOCK_CYCLES_UNTIL_REFRESH1_END[INIT_COUNTER_WIDTH-1:0] && init_refresh_count == 7)
        sd_addr_next = MODE;
    end
    STATE_RAS:
      sd_addr_next = cs ? addr32[RAS_WIDTH+CAS_WIDTH-1:CAS_WIDTH] :
        video_grant ? video_addr32[RAS_WIDTH+CAS_WIDTH-1:CAS_WIDTH] : p2_addr32[RAS_WIDTH+CAS_WIDTH-1:CAS_WIDTH];
    STATE_CAS_WAIT_0:
      if (VIDEO_PORT && video_b) sd_addr_next = vb_row;   // ACTIVE обращения B
    default: ;
  endcase
end
always @(negedge clk) sd_addr <= sd_addr_next;

always @(negedge clk) begin
  sd_cmd <= CMD_NOP;
  sd_dq  <= 0;

  case (state)
    STATE_INIT: begin
      sdram_port <= PORT_IDLE;
      video_b <= 1'b0;
      refreshcnt <= SYNC_CYCLES_PER_REFRESH[REFRESHCNT_MAX_WIDTH-1:0];

      if (init_counter == CLOCK_CYCLES_UNTIL_INIT_PRECHARGE[INIT_COUNTER_WIDTH-1:0]) begin
        sd_cmd <= CMD_PRECHARGE;
      end else if (init_counter == CLOCK_CYCLES_UNTIL_INIT_PRECHARGE_END[INIT_COUNTER_WIDTH-1:0]) begin
        sd_cmd <= CMD_AUTO_REFRESH;
      end else if (init_counter == CLOCK_CYCLES_UNTIL_REFRESH1_END[INIT_COUNTER_WIDTH-1:0] && init_refresh_count != 7) begin
        sd_cmd <= CMD_AUTO_REFRESH;
      end else if (init_counter == CLOCK_CYCLES_UNTIL_REFRESH1_END[INIT_COUNTER_WIDTH-1:0] && init_refresh_count == 7) begin
        sd_cmd  <= CMD_LOAD_MODE;
      end
    end
    STATE_RAS: begin
      video_dbl <= 0;
      if (cs) begin

        sd_addr_cas[RAS_WIDTH-1:0] <= {RAS_WIDTH{1'b0}};
        sd_addr_cas[CAS_WIDTH-1:0] <= addr32[CAS_WIDTH-1:0];
        sd_addr_cas[10] <= 1'b1;  // precharge

        sd_ba <= addr32[RAS_WIDTH+CAS_WIDTH+1:RAS_WIDTH+CAS_WIDTH];

        ram_ds  <= ds;
        ram_we  <= we;
        ram_din <= din;

        addr_0  <= addr[0];

      end else if (video_grant) begin
        sd_addr_cas<=0;
        sd_addr_cas[CAS_WIDTH-1:0]<=video_addr32[CAS_WIDTH-1:0];
        sd_addr_cas[10]<=!video_we && !(VIDEO_STREAM && video_double);
        video_dbl<=VIDEO_STREAM && video_double && !video_we;
        sd_ba<=video_addr32[RAS_WIDTH+CAS_WIDTH+1:RAS_WIDTH+CAS_WIDTH];
        ram_ds<=0; ram_we<=video_we; ram_din<=0; addr_0<=0;
      end else if (p2_cs) begin

        sd_addr_cas[RAS_WIDTH-1:0] <= {RAS_WIDTH{1'b0}};
        sd_addr_cas[CAS_WIDTH-1:0] <= p2_addr32[CAS_WIDTH-1:0];
        sd_addr_cas[10] <= 1'b1;  // precharge

        sd_ba <= p2_addr32[RAS_WIDTH+CAS_WIDTH+1:RAS_WIDTH+CAS_WIDTH];

        ram_ds  <= p2_ds;
        ram_we  <= p2_we;
        ram_din <= p2_din;

        addr_0 <= p2_addr[0];
      end

      if (sync_i) begin
        video_b <= b_go;
        vb_row  <= video_addr32[BANK_LO-1:CAS_WIDTH];
        vb_col  <= video_addr32[CAS_WIDTH-1:0];
        vb_bank <= video_bank;
        if (refreshcnt != 0)
          refreshcnt <= refreshcnt - 1;

        if (cs && !refresh) begin
          sdram_port <= PORT_1;
          sd_cmd     <= CMD_ACTIVE;

        end else if (refreshcnt == 0 || (cs && refresh)) begin
          sdram_port <= PORT_REFRESH;
          sd_cmd     <= CMD_NOP;

          refreshcnt <= SYNC_CYCLES_PER_REFRESH[REFRESHCNT_MAX_WIDTH-1:0];

        end else if (video_grant) begin
          sdram_port <= PORT_VIDEO;
          sd_cmd <= CMD_ACTIVE;
        end else if (p2_cs) begin
          sdram_port <= PORT_2;
          sd_cmd     <= CMD_ACTIVE;
        end
      end
    end
    STATE_RAS_WAIT_0: begin
      // Выводы DQ включаются на такт раньше WRITE: шина свободна, а такт SDRAM теперь ближе.
      if (ram_we && (sdram_port == PORT_1 || sdram_port == PORT_2 || sdram_port == PORT_VIDEO))
        sd_dq <= 1;
    end
    STATE_CAS: begin
      case (sdram_port)
        PORT_1: begin
          sd_cmd <= ram_we ? CMD_WRITE : CMD_READ;
          if (ram_we) begin
            sd_dq <= 1;
          end
        end
        PORT_2: begin
          sd_cmd <= ram_we ? CMD_WRITE : CMD_READ;
          if (ram_we) begin
            sd_dq <= 1;
          end
        end
        PORT_VIDEO: begin
          sd_cmd<=ram_we ? CMD_WRITE : CMD_READ;
          sd_dq<=ram_we;
          if(ram_we) sd_addr_cas[1:0]<=2'd1;
        end
        PORT_REFRESH: begin
          sd_cmd <= CMD_AUTO_REFRESH;
        end
        default: ;
      endcase
    end
    // Запись видео: WRITE на CAS, CAS_WAIT_0, CAS_WAIT_1 и READ_0, столбцы +0..+3,
    // по слову за такт (слова меняются так же, как прежние такты пакета);
    // автопрегард на последней. Блок 16 байт выровнен на 4 столбца, поэтому
    // меняются только два младших бита столбца; адрес выходит вместе с командой.
    STATE_CAS_WAIT_0, STATE_CAS_WAIT_1, STATE_READ_0: begin
      if(VIDEO_PORT && video_b && state==STATE_CAS_WAIT_0) begin
        sd_cmd <= CMD_ACTIVE;       // банк B, строка — sd_addr_next
        sd_ba  <= vb_bank;
        sd_addr_cas <= 0;
        sd_addr_cas[CAS_WIDTH-1:0] <= vb_col;
        sd_addr_cas[10] <= 1'b1;   // READ B с автопрегардом
      end
      if(VIDEO_PORT && sdram_port==PORT_VIDEO && ram_we) begin
        sd_dq<=1;
        sd_cmd<=CMD_WRITE;
        if(state==STATE_CAS_WAIT_0) sd_addr_cas[1:0]<=2'd2;
        if(state==STATE_CAS_WAIT_1) begin sd_addr_cas[1:0]<=2'd3; sd_addr_cas[10]<=1'b1; end
      end
      // Второй READ сдвоенного чтения: столбец +4 (бит 2) с автопрегардом,
      // адрес готов на такт раньше команды на READ_1.
      if(VIDEO_PORT && sdram_port==PORT_VIDEO && video_dbl && state==STATE_CAS_WAIT_1) begin
        sd_addr_cas[2]<=1'b1; sd_addr_cas[10]<=1'b1;
      end
    end
    STATE_READ_1: begin
      if(VIDEO_PORT && ((sdram_port==PORT_VIDEO && video_dbl) || video_b)) sd_cmd<=CMD_READ;
    end
    STATE_LAST: begin
      sdram_port <= PORT_IDLE;
    end
    default: ;
  endcase
end

generate
  if (WIDTH32) begin
    // Разрешения приёма: порт и addr_0 не меняются от ACTIVE до конца цикла, поэтому всё известно на такт раньше.
    // dout чипсет забирает через такт после READ_0 (модель: pipeline.csv), он остаётся на полутакте, один мультиплексор и 16 триггеров.
    // Остальное идёт через dq_r, и у этих путей до потребителя целый такт. Подтверждение порта 2 не сдвинуто: p2_ack выходит
    // на READ_3, вместе с последним словом; dout48 чипсет не читает раньше следующего clk7_en, а до него ещё один такт.
    // Данные на такт позже прежних: 47:32 — lo на READ_1, hi на READ_2 при addr_0; 31:16 — на READ_2; 15:0 — lo на READ_2 или hi на READ_3.
    reg [31:0] dq_r;
    reg [1:0] rd0_en, rd1_en, rd2_en;   // [0] — порт 1, [1] — порт 2
    reg [1:0] top_en, low_en;
    wire [15:0] r_lo = dq_r[15:0];
    wire [15:0] r_hi = dq_r[31:16];
    always @(negedge clk) begin
      dq_r <= {ram_dout_hi, ram_dout_lo};
      rd0_en <= {sdram_port == PORT_2, sdram_port == PORT_1} & {2{state == STATE_CAS_WAIT_1}};
      rd1_en <= rd0_en;
      rd2_en <= rd1_en;
      top_en <= rd0_en | (rd1_en & {2{addr_0}});
      low_en <= (rd1_en & {2{~addr_0}}) | (rd2_en & {2{addr_0}});
    end
    always @(negedge clk) begin
      if (rd0_en[0]) dout <= ram_dout;
      if (rd1_en[1]) p2_dout <= addr_0 ? r_lo : r_hi;
      if (top_en[0]) dout48[47:32] <= rd1_en[0] ? r_lo : r_hi;
      if (top_en[1]) p2_dout48[47:32] <= rd1_en[1] ? r_lo : r_hi;
      if (rd2_en[0]) dout48[31:16] <= addr_0 ? r_lo : r_hi;
      if (rd2_en[1]) p2_dout48[31:16] <= addr_0 ? r_lo : r_hi;
      if (low_en[0]) dout48[15:0] <= addr_0 ? r_hi : r_lo;
      if (low_en[1]) p2_dout48[15:0] <= addr_0 ? r_hi : r_lo;
    end
    always @(negedge clk) begin
      sd_dqm <= state == STATE_INIT ? {DATA_WIDTH/8{1'b1}} : 0;

      case (state)
        STATE_CAS: begin
          case (sdram_port)
            PORT_1: if (ram_we) sd_dqm <= (addr_0 ? {2'b11, ram_ds} : {ram_ds, 2'b11});
            PORT_2: if (ram_we) begin
              p2_ack_i <= ~p2_ack_i;
              sd_dqm <= (addr_0 ? {2'b11, ram_ds} : {ram_ds, 2'b11});
            end
            default: ;
          endcase
        end
        STATE_READ_0: begin
          if (sdram_port == PORT_2 && !ram_we && !CHIP48_BURST) p2_ack_i <= ~p2_ack_i;
        end
        STATE_READ_2: begin
          if (sdram_port == PORT_2 && !ram_we && CHIP48_BURST) p2_ack_i <= ~p2_ack_i;
        end
        default: ;
      endcase
    end

  end else begin
    always @(negedge clk) begin
      sd_dqm <= state == STATE_INIT ? {DATA_WIDTH/8{1'b1}} : 0;
      if (state == STATE_READ_DATA && sdram_port == PORT_1) dout <= ram_dout;
      if (state == STATE_READ_DATA && sdram_port == PORT_2) p2_dout <= ram_dout;

      case (state)
        STATE_CAS: begin
          case (sdram_port)
            PORT_1: if (ram_we) sd_dqm <= ram_ds;
            PORT_2: if (ram_we) begin
              sd_dqm <= ram_ds;
              p2_ack_i <= ~p2_ack_i;
            end
            default: ;
          endcase
        end
        STATE_READ_0: begin
          case (sdram_port)
            PORT_1: ;
            PORT_2: if (!ram_we && !CHIP48_BURST) p2_ack_i <= ~p2_ack_i;
            default: ;
          endcase
        end
        STATE_READ_1: begin
          case (sdram_port)
            PORT_1: dout48[47:32] <= ram_dout;
            PORT_2: p2_dout48[47:32] <= ram_dout;
            default: ;
          endcase
        end
        STATE_READ_2: begin
          case (sdram_port)
            PORT_1: dout48[31:16] <= ram_dout;
            PORT_2: p2_dout48[31:16] <= ram_dout;
            default: ;
          endcase
        end
        STATE_READ_3: begin
          case (sdram_port)
            PORT_1: dout48[15: 0] <= ram_dout;
            PORT_2: begin
              if (!ram_we && CHIP48_BURST) p2_ack_i <= ~p2_ack_i;
              p2_dout48[15: 0] <= ram_dout;
            end
            default: ;
          endcase
        end
        default: ;
      endcase
    end
  end
endgenerate

generate
  if (ACK_DELAY == 1) begin
    reg p2_ack_d;

    always @(negedge clk)
      p2_ack_d <= p2_ack_i;

    assign p2_ack = p2_ack_d;

  end else if (ACK_DELAY > 1) begin
    reg [ACK_DELAY-1:0] p2_ack_d;

    always @(negedge clk)
      p2_ack_d <= {p2_ack_d[ACK_DELAY-2:0], p2_ack_i};

    assign p2_ack = p2_ack_d[ACK_DELAY-1];

  end else begin
    assign p2_ack = p2_ack_i;
  end
endgenerate

endmodule
`default_nettype wire
// vim:ts=2 sw=2 tw=120 et
