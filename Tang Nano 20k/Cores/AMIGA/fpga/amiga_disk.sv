// SPDX-License-Identifier: GPL-3.0-or-later
// Почтовый ящик секторов дискеты DF0 и диска IDE HD0. Порты синхронны clk (28,5 МГц).
// Адреса хоста — номера СЛОВ. Сброс гостя отменяет обмен, образ остаётся подключённым.
//
// Чтение: гость просит сектор, хост кладёт 128 слов в окно приёма и подтверждает номером запроса,
// ящик отдаёт гостю 512 байт. Запись: ящик забирает у гостя 512 байт в окно передачи и ждёт, пока
// хост их прочитает и подтвердит номером запроса; только после этого гость получает done.
module amiga_disk (
    input wire clk, rst_n, guest_reset,
    input wire wr_stb, input wire [17:0] wr_addr, input wire [31:0] wr_data,
    input wire [12:0] rd_addr, output reg [31:0] rd_data,
    input wire rd_clk, output reg [31:0] tx_q,
    input wire [7:0] request_rd, request_wr, input wire [31:0] request_sector,
    output reg [7:0] image_mounted, output reg [63:0] image_size, output reg floppy_wrprot,
    output wire busy, output reg done, byte_strobe,
    output reg [8:0] byte_addr, output wire [7:0] byte_data, input wire [7:0] guest_data
);
    localparam IDLE=0, WAIT_HOST=1, SEND=2, FINISH=3, FETCH=4;
    localparam [3:0] FETCH_LAST=15;     // такт на байт: IDE читает слова через окно управления ide.v
    reg [2:0] state;
    reg [3:0] pace;
    reg [31:0] sector, token, completed, df0_size;
    reg [7:0] drive;
    reg [2:0] unit;                     // 0 — DF0, 4 — HD0
    reg writing, error;
    reg [127:0] filled;
    reg [31:0] rx [0:127];              // хост -> гость
    reg [31:0] rx_q;
    reg [31:0] tx [0:127];              // гость -> хост
    reg [23:0] tx_low;
    wire payload = wr_addr >= 18'h1400 && wr_addr < 18'h1480;
    wire tx_we = state==FETCH && pace==FETCH_LAST && byte_addr[1:0]==2'd3;
    assign busy = state != IDLE;
    assign byte_data = rx_q >> (8 * byte_addr[1:0]);
    always @(posedge clk) begin
        if (wr_stb && payload && state==WAIT_HOST && !writing && !guest_reset)
            rx[wr_addr[6:0]] <= wr_data;
        rx_q <= rx[byte_addr[8:2]];
    end
    always @(posedge clk) begin
        if (state==FETCH && pace==FETCH_LAST) tx_low <= {guest_data, tx_low[23:8]};
        if (tx_we) tx[byte_addr[8:2]] <= {guest_data, tx_low};
    end
    always @(posedge rd_clk) tx_q <= tx[rd_addr[6:0]];
    // Номер сектора дискеты проверяется здесь, границы HDF — у хоста.
    wire floppy = |drive[3:0];
    // Дискету могли снять, пока шёл запрос: ответ всё равно нужен гостю, защиту записи соблюдает хост.
    wire reply_ok = wr_data==token && (writing || &filled) && (!floppy || (drive==8'd1 && sector<1760));
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state<=IDLE; pace<=0; sector<=0; token<=0; completed<=0; df0_size<=0;
            drive<=0; unit<=0; writing<=0; error<=0; filled<=0; floppy_wrprot<=1;
            image_mounted<=0; image_size<=0; done<=0; byte_strobe<=0; byte_addr<=0;
        end else begin
            image_mounted<=0; done<=0; byte_strobe<=0;
            if (wr_stb && wr_addr==18'h65) unit<=wr_data[2:0];
            if (wr_stb && wr_addr==18'h67) floppy_wrprot<=wr_data[0];
            // Импульс сообщает гостю о смене носителя, в том числе во время сброса.
            if (wr_stb && wr_addr==18'h60 && state==IDLE) begin
                if (unit==0 ? (wr_data==0 || wr_data==901120) : unit==4) begin
                    image_size<={32'b0,wr_data}; image_mounted[unit]<=1; error<=0;
                    if (unit==0) df0_size<=wr_data;
                end else error<=1;
            end
            if (guest_reset) begin
                state<=IDLE; filled<=0; pace<=0; byte_addr<=0; error<=0;
            end else begin
                case (state)
                    IDLE: if (|request_rd || |request_wr) begin
                        sector<=request_sector;
                        drive<=request_rd | request_wr; writing<=|request_wr;
                        token<=token+1'b1; filled<=0; pace<=0; byte_addr<=0;
                        state<=|request_wr ? FETCH : WAIT_HOST;
                    end
                    FETCH: begin
                        pace<=pace+1'b1;
                        if (pace==FETCH_LAST) begin
                            if (byte_addr==511) state<=WAIT_HOST;
                            else byte_addr<=byte_addr+1'b1;
                        end
                    end
                    WAIT_HOST: begin
                        if (wr_stb && payload) filled[wr_addr[6:0]]<=1;
                        if (wr_stb && wr_addr==18'h64) begin
                            // Неполный или устаревший ответ не должен дойти до памяти гостя.
                            if (reply_ok) begin
                                state<=writing ? FINISH : SEND; byte_addr<=0; pace<=0;
                            end else error<=1;
                        end
                    end
                    SEND: begin
                        pace<=pace+1'b1;
                        if (pace[1:0]==2) byte_strobe<=1;
                        if (pace[1:0]==3) begin
                            if (byte_addr==511) state<=FINISH;
                            else byte_addr<=byte_addr+1'b1;
                        end
                    end
                    FINISH: begin state<=IDLE; done<=1; completed<=token; end
                    default: state<=IDLE;
                endcase
            end
        end
    end
    always @* begin
        rd_data=0;
        case (rd_addr)
            13'h60: rd_data=df0_size;
            13'h61: rd_data={16'b0,drive,3'b0,writing&&state==WAIT_HOST,writing,error,busy,state==WAIT_HOST};
            13'h62: rd_data=sector;
            13'h63: rd_data=token;
            13'h64: rd_data=completed;
            13'h65: rd_data={29'b0,unit};
            13'h66: rd_data=32'h41444632; // ADF2: запись и HD0, независимо от VERSION ядра
            13'h67: rd_data={31'b0,floppy_wrprot};
            default: ;
        endcase
    end
endmodule
