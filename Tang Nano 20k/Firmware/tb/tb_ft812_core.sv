`timescale 1ns/1ps

module tb_ft812_core;
    reg clk = 0;
    reg reset_n = 1;
    reg bus_valid = 0;
    reg bus_write = 0;
    reg [21:0] bus_addr = 0;
    reg [7:0] bus_wdata = 0;
    wire bus_ready;
    wire bus_rvalid;
    wire [7:0] bus_rdata;
    reg frame_tick = 0;
    wire int_n;
    integer errors = 0;

    always #8 clk = ~clk;

    ft812_core dut (
        .clk(clk), .reset_n(reset_n),
        .bus_valid(bus_valid), .bus_write(bus_write), .bus_addr(bus_addr),
        .bus_wdata(bus_wdata), .bus_ready(bus_ready),
        .bus_rvalid(bus_rvalid), .bus_rdata(bus_rdata),
        .ramg_valid(), .ramg_write(), .ramg_addr(), .ramg_wdata(),
        .ramg_ready(1'b1), .ramg_rvalid(1'b0), .ramg_rdata(8'h00),
        .frame_tick(frame_tick), .int_n(int_n), .dl_clk(clk),
        .dl_read_addr(13'd0), .dl_read_data(),
        .dl_debug_addr(13'd0), .dl_debug_data(),
        .cmd_read_ptr_out(), .cmd_write_ptr_out()
    );

    task write_byte(input [21:0] addr, input [7:0] data);
        begin
            @(negedge clk);
            bus_addr = addr;
            bus_wdata = data;
            bus_write = 1;
            bus_valid = 1;
            @(negedge clk);
            bus_valid = 0;
        end
    endtask

    task read_byte(input [21:0] addr, output [7:0] data);
        begin
            @(negedge clk);
            bus_addr = addr;
            bus_write = 0;
            bus_valid = 1;
            @(negedge clk);
            bus_valid = 0;
            wait (bus_rvalid);
            data = bus_rdata;
        end
    endtask

    reg [7:0] value;
    initial begin
        #1 reset_n = 0;
        #31 reset_n = 1;

        read_byte(22'h302000, value);
        if (value !== 8'h7C) begin
            $display("FAIL REG_ID=%02x", value);
            errors = errors + 1;
        end

        write_byte(22'h300123, 8'hA6);
        read_byte(22'h300123, value);
        if (value !== 8'hA6) begin
            $display("FAIL RAM_DL=%02x", value);
            errors = errors + 1;
        end

        write_byte(22'h3020AC, 8'h01);
        write_byte(22'h3020B0, 8'h01);
        write_byte(22'h302054, 8'h02);
        @(negedge clk) frame_tick = 1;
        @(negedge clk) frame_tick = 0;
        #1;
        if (int_n !== 1'b0) begin
            $display("FAIL swap interrupt not asserted");
            errors = errors + 1;
        end
        read_byte(22'h3020A8, value);
        if (value !== 8'h01) begin
            $display("FAIL INT_FLAGS=%02x", value);
            errors = errors + 1;
        end
        @(posedge clk); #1;
        if (int_n !== 1'b1) begin
            $display("FAIL interrupt not cleared");
            errors = errors + 1;
        end

        write_byte(22'h302578, 8'h11);
        write_byte(22'h302579, 8'h22);
        read_byte(22'h3020FC, value);
        if (value !== 8'h02) begin
            $display("FAIL CMDB pointer=%02x", value);
            errors = errors + 1;
        end

        if (errors == 0)
            $display("PASS tb_ft812_core");
        else
            $fatal(1, "tb_ft812_core errors=%0d", errors);
        $finish;
    end
endmodule
