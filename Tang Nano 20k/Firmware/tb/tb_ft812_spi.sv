`timescale 1ns/1ps

module tb_ft812_spi;
    reg reset_n = 1;
    reg spi_sck = 0;
    reg spi_cs_n = 1;
    reg spi_mosi = 0;
    wire spi_miso;
    wire spi_miso_oe;
    reg clk_sys = 0;
    wire bus_valid;
    wire bus_write;
    wire [21:0] bus_addr;
    wire [7:0] bus_wdata;
    reg bus_rvalid = 0;
    reg [7:0] bus_rdata = 0;
    wire request_overflow;
    wire read_underflow;
    reg [7:0] memory [0:65535];
    integer errors = 0;

    always #7.716 clk_sys = ~clk_sys; // 64.8 MHz

    ft812_spi dut (
        .reset_n(reset_n), .spi_sck(spi_sck), .spi_cs_n(spi_cs_n),
        .spi_mosi(spi_mosi), .spi_miso(spi_miso), .spi_miso_oe(spi_miso_oe),
        .clk_sys(clk_sys), .bus_valid(bus_valid), .bus_write(bus_write),
        .bus_addr(bus_addr), .bus_wdata(bus_wdata), .bus_ready(1'b1),
        .bus_rvalid(bus_rvalid), .bus_rdata(bus_rdata),
        .request_overflow(request_overflow), .read_underflow(read_underflow)
    );

    always @(posedge clk_sys) begin
        bus_rvalid <= 0;
        if (bus_valid) begin
            if (bus_write)
                memory[bus_addr[15:0]] <= bus_wdata;
            else begin
                bus_rdata <= memory[bus_addr[15:0]];
                bus_rvalid <= 1;
            end
        end
    end

    task spi_byte(input [7:0] tx, output [7:0] rx);
        integer i;
        begin
            rx = 0;
            for (i = 7; i >= 0; i = i - 1) begin
                spi_mosi = tx[i];
                #70;
                spi_sck = 1;
                #35;
                rx[i] = spi_miso;
                #35;
                spi_sck = 0;
            end
        end
    endtask

    task begin_transaction;
        begin
            spi_sck = 0;
            spi_cs_n = 0;
            #100;
        end
    endtask

    task end_transaction;
        begin
            #70;
            spi_cs_n = 1;
            #300;
        end
    endtask

    reg [7:0] rx;
    initial begin
        $dumpfile("tb_ft812_spi.vcd");
        $dumpvars(0, tb_ft812_spi);
        memory[16'h1234] = 8'hA5;
        memory[16'h1235] = 8'h5A;
        #1 reset_n = 0;
        #99;
        reset_n = 1;
        #200;

        begin_transaction();
        spi_byte(8'h80, rx);
        spi_byte(8'h12, rx);
        spi_byte(8'h30, rx);
        spi_byte(8'hDE, rx);
        spi_byte(8'hAD, rx);
        end_transaction();
        #1000;
        if (memory[16'h1230] !== 8'hDE || memory[16'h1231] !== 8'hAD) begin
            $display("FAIL write: %02x %02x", memory[16'h1230], memory[16'h1231]);
            errors = errors + 1;
        end

        begin_transaction();
        spi_byte(8'h00, rx);
        spi_byte(8'h12, rx);
        spi_byte(8'h34, rx);
        spi_byte(8'h00, rx); // dummy
        spi_byte(8'h00, rx);
        if (rx !== 8'hA5) begin
            $display("FAIL read0: %02x", rx);
            errors = errors + 1;
        end
        spi_byte(8'h00, rx);
        if (rx !== 8'h5A) begin
            $display("FAIL read1: %02x", rx);
            errors = errors + 1;
        end
        end_transaction();

        if (request_overflow || read_underflow) begin
            $display("FAIL status overflow=%0d underflow=%0d", request_overflow, read_underflow);
            errors = errors + 1;
        end
        if (errors == 0)
            $display("PASS tb_ft812_spi");
        else
            $fatal(1, "tb_ft812_spi errors=%0d", errors);
        $finish;
    end
endmodule
