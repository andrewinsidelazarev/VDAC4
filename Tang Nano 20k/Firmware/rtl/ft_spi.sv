// SPDX-License-Identifier: Apache-2.0
//
// FT812: приёмник SPI хоста (Z-контроллер Evo). Режим 0, старший бит первым.
//
// SCK Evo — 14 МГц (штатная прошивка TS-Config) или 28 МГц (zxevo_fw_vdac2-spi28):
// пересчитать его тактом ядра (61,875 МГц) нельзя, поэтому приёмник тактируется самим SCK,
// а сбрасывается снятием FT_CS_N. Каждый принятый байт — одно событие для ядра:
//   EV_START — три байта заголовка (команда хоста, адрес чтения или записи);
//   EV_WRITE — байт данных записи;
//   EV_NEXT  — отдан байт чтения: ядру готовить следующий.
// Событие передаётся переключением tg (ft_core синхронизирует его); поля события
// меняются вместе с tg и держатся до следующего, то есть не меньше 8 тактов SCK.
// Конец посылки ядро узнаёт само, по FT_CS_N.
//
// MISO выставляется по фронту SCK, на полтакта раньше обычного режима 0, — как у ESP
// (board::kSpiMisoEarly): задержка буферов платы туда и обратно ~36 нс, и бит,
// выставленный по спаду, Evo читает то верно, то как предыдущий. Байт чтения берётся
// из rd_byte ядра на последнем фронте предыдущего байта: ядро успевает за 8 тактов SCK
// (EV_START или EV_NEXT были на предыдущей границе байта). Вне данных чтения и без выбора
// MISO = 0 — так отвечает bt8xxemu (Bridgetek); на Tang эта линия — ещё и вход WS2812.
module ft_spi (
    input  wire       sck,
    input  wire       cs_n,
    input  wire       mosi,
    output wire       miso,

    output reg        tg,           // переключается на каждом событии
    output reg  [1:0] ev_kind,
    output reg  [7:0] ev_h0,        // EV_START: байты заголовка
    output reg  [7:0] ev_h1,
    output reg  [7:0] ev_b,         // EV_START: третий байт; EV_WRITE: данные
    input  wire [7:0] rd_byte       // домен ядра: следующий байт чтения
);
    localparam logic [1:0] EV_START = 2'd0, EV_WRITE = 2'd1, EV_NEXT = 2'd2;

    reg  [2:0] bitn;
    reg  [2:0] nbyte;               // 0, 1, 2 — заголовок, 3 — первый байт после, 4 — дальше
    reg  [6:0] sh;
    reg  [7:0] h0, h1;
    reg        rd_mode;
    reg        miso_r;
    reg  [7:0] txb;

    wire [7:0] rx = {sh, mosi};
    wire       load = rd_mode && nbyte >= 3'd3;      // на этом фронте bitn == 7 — новый байт

    always @(posedge sck or posedge cs_n) begin
        if (cs_n) begin
            bitn    <= 3'd0;
            nbyte   <= 3'd0;
            rd_mode <= 1'b0;
            miso_r  <= 1'b0;
        end else begin
            bitn <= bitn + 3'd1;
            if (bitn == 3'd7) begin
                if (nbyte != 3'd4)
                    nbyte <= nbyte + 3'd1;
                if (nbyte == 3'd2)
                    rd_mode <= h0[7:6] == 2'b00;
                miso_r <= load ? rd_byte[7] : 1'b0;
            end else begin
                miso_r <= (rd_mode && nbyte >= 3'd4) ? txb[3'd6 - bitn] : 1'b0;
            end
        end
    end

    // Без сброса: tg обязан сохранять значение между посылками.
    always @(posedge sck) begin
        sh <= rx[6:0];
        if (!cs_n && bitn == 3'd7) begin
            if (load)
                txb <= rd_byte;
            case (nbyte)
                3'd0: h0 <= rx;
                3'd1: h1 <= rx;
                3'd2: begin
                    ev_kind <= EV_START;
                    ev_h0   <= h0;
                    ev_h1   <= h1;
                    ev_b    <= rx;
                    tg      <= ~tg;
                end
                default: begin
                    if (h0[7:6] == 2'b10) begin
                        ev_kind <= EV_WRITE;
                        ev_b    <= rx;
                        tg      <= ~tg;
                    end else if (rd_mode) begin
                        ev_kind <= EV_NEXT;
                        tg      <= ~tg;
                    end
                end
            endcase
        end
    end

    // как у триггеров ПЛИС после загрузки (FT_CS_N при этом высокий)
    initial begin
        tg = 1'b0; bitn = 3'd0; nbyte = 3'd0; rd_mode = 1'b0; miso_r = 1'b0;
    end

    assign miso = miso_r;
endmodule
