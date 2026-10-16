// SPDX-License-Identifier: Apache-2.0
// FT81x-compatible single-SPI transport. Mode 0, 24-bit address header.
// Requests cross into clk_sys through a dual-clock FIFO. Read responses use
// a one-outstanding-request toggle mailbox; the FT81x dummy byte provides the
// system side with at least eight SCK periods to fetch the first byte.
module ft812_spi #(
    parameter integer REQUEST_FIFO_BITS = 5
) (
    input  wire        reset_n,
    input  wire        spi_sck,
    input  wire        spi_cs_n,
    input  wire        spi_mosi,
    output wire        spi_miso,
    output wire        spi_miso_oe,

    input  wire        clk_sys,
    output reg         bus_valid,
    output reg         bus_write,
    output reg  [21:0] bus_addr,
    output reg  [7:0]  bus_wdata,
    input  wire        bus_ready,
    input  wire        bus_rvalid,
    input  wire [7:0]  bus_rdata,

    output reg         request_overflow,
    output reg         read_underflow
);
    // {write, address[21:0], data[7:0], reserved}
    wire [31:0] request_wr_data;
    wire        request_wr_en;
    wire        request_wr_full;
    reg         request_rd_en;
    wire [31:0] request_rd_data;
    wire        request_rd_empty;

    reg  [2:0]  bus_state;

    reg  [7:0] rx_shift;
    reg  [7:0] tx_shift;
    reg        miso_reg;
    reg  [2:0] bit_count;
    reg  [15:0] byte_count;
    reg         transaction_write;
    reg  [21:0] transaction_addr;
    reg  [7:0] header0;
    reg  [7:0] header1;

    reg         response_toggle_sys;
    reg  [7:0]  response_data_sys;
    reg         response_toggle_sck1;
    reg         response_toggle_sck2;
    reg         response_seen_sck;
    reg  [7:0]  response_data_sck1;
    reg  [7:0]  response_data_sck2;

    wire [7:0] received_byte = {rx_shift[6:0], spi_mosi};
    wire response_available = response_toggle_sck2 != response_seen_sck;
    wire request_boundary = (bit_count == 3'd7) &&
        ((transaction_write && byte_count >= 16'd3) ||
         (!transaction_write && byte_count >= 16'd2));
    wire [21:0] request_address =
        (!transaction_write && byte_count == 16'd2) ?
            {header0[5:0], header1, received_byte} :
        (!transaction_write) ? transaction_addr + 1'b1 :
            transaction_addr;

    assign request_wr_en = !spi_cs_n && request_boundary && !request_wr_full;
    assign request_wr_data = {
        transaction_write,
        request_address,
        transaction_write ? received_byte : 8'h00,
        1'b0
    };

    assign spi_miso = miso_reg;
    assign spi_miso_oe = !spi_cs_n;

    async_fifo #(
        .WIDTH(32),
        .ADDR_BITS(REQUEST_FIFO_BITS)
    ) request_fifo (
        .wr_clk(spi_sck),
        .wr_reset_n(reset_n),
        .wr_en(request_wr_en),
        .wr_data(request_wr_data),
        .wr_full(request_wr_full),
        .rd_clk(clk_sys),
        .rd_reset_n(reset_n),
        .rd_en(request_rd_en),
        .rd_data(request_rd_data),
        .rd_empty(request_rd_empty)
    );

    // SPI clock domain.
    always @(posedge spi_sck or posedge spi_cs_n or negedge reset_n) begin
        if (!reset_n) begin
            rx_shift <= 0;
            tx_shift <= 0;
            bit_count <= 0;
            byte_count <= 0;
            transaction_write <= 0;
            transaction_addr <= 0;
            header0 <= 0;
            header1 <= 0;
            request_overflow <= 0;
            read_underflow <= 0;
            response_toggle_sck1 <= 0;
            response_toggle_sck2 <= 0;
            response_seen_sck <= 0;
            response_data_sck1 <= 0;
            response_data_sck2 <= 0;
        end else if (spi_cs_n) begin
            rx_shift <= 0;
            tx_shift <= 0;
            bit_count <= 0;
            byte_count <= 0;
            transaction_write <= 0;
            transaction_addr <= 0;
        end else begin
            response_toggle_sck1 <= response_toggle_sys;
            response_toggle_sck2 <= response_toggle_sck1;
            response_data_sck1 <= response_data_sys;
            response_data_sck2 <= response_data_sck1;
            rx_shift <= {rx_shift[6:0], spi_mosi};

            if (bit_count == 3'd7) begin
                bit_count <= 0;
                byte_count <= byte_count + 1'b1;
                case (byte_count)
                    16'd0: begin
                        header0 <= received_byte;
                        transaction_write <= received_byte[7];
                    end
                    16'd1: header1 <= received_byte;
                    16'd2: begin
                        transaction_addr <= {header0[5:0], header1, received_byte};
                        if (!transaction_write) begin
                            if (request_wr_full) begin
                                request_overflow <= 1'b1;
                            end
                        end
                    end
                    default: begin
                        if (transaction_write) begin
                            if (!request_wr_full) begin
                                transaction_addr <= transaction_addr + 1'b1;
                            end else begin
                                request_overflow <= 1'b1;
                            end
                        end else begin
                            // byte 3 is the mandatory dummy byte. At its end,
                            // and at every following byte boundary, load the
                            // completed response and request the next address.
                            if (response_available) begin
                                tx_shift <= response_data_sck2;
                                response_seen_sck <= response_toggle_sck2;
                            end else begin
                                tx_shift <= 8'hEE;
                                read_underflow <= 1'b1;
                            end
                            transaction_addr <= transaction_addr + 1'b1;
                            if (request_wr_full) begin
                                request_overflow <= 1'b1;
                            end
                        end
                    end
                endcase
            end else begin
                bit_count <= bit_count + 1'b1;
            end
        end
    end

    // CPOL=0/CPHA=0: change MISO on the falling edge so it is stable before
    // the master's next rising sampling edge.
    always @(negedge spi_sck or posedge spi_cs_n or negedge reset_n) begin
        if (!reset_n || spi_cs_n)
            miso_reg <= 1'b0;
        else
            miso_reg <= tx_shift[7-bit_count];
    end

    // System clock domain: convert queued packets to a conventional request.
    always @(posedge clk_sys or negedge reset_n) begin
        if (!reset_n) begin
            request_rd_en <= 0;
            bus_state <= 0;
            bus_valid <= 0;
            bus_write <= 0;
            bus_addr <= 0;
            bus_wdata <= 0;
            response_toggle_sys <= 0;
            response_data_sys <= 0;
        end else begin
            request_rd_en <= 0;
            case (bus_state)
                3'd0: begin
                    bus_valid <= 0;
                    if (!request_rd_empty) begin
                        request_rd_en <= 1'b1;
                        bus_state <= 3'd1;
                    end
                end
                3'd1: begin
                    // async_fifo registers rd_data on this clock.
                    bus_state <= 3'd2;
                end
                3'd2: begin
                    bus_write <= request_rd_data[31];
                    bus_addr <= request_rd_data[30:9];
                    bus_wdata <= request_rd_data[8:1];
                    bus_valid <= 1'b1;
                    bus_state <= 3'd3;
                end
                3'd3: begin
                    if (bus_ready) begin
                        bus_valid <= 0;
                        bus_state <= bus_write ? 3'd0 : 3'd4;
                    end
                end
                3'd4: begin
                    if (bus_rvalid) begin
                        response_data_sys <= bus_rdata;
                        response_toggle_sys <= ~response_toggle_sys;
                        bus_state <= 3'd0;
                    end
                end
                default: bus_state <= 3'd0;
            endcase
        end
    end
endmodule
