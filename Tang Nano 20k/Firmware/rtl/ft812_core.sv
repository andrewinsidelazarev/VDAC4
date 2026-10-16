// SPDX-License-Identifier: Apache-2.0
// FT812 address space, register state, RAM_DL and RAM_CMD.
// RAM_G is deliberately external: the Tang Nano hard SDRAM controller owns it.
module ft812_core (
    input  wire        clk,
    input  wire        reset_n,

    input  wire        bus_valid,
    input  wire        bus_write,
    input  wire [21:0] bus_addr,
    input  wire [7:0]  bus_wdata,
    output wire        bus_ready,
    output wire        bus_rvalid,
    output wire [7:0]  bus_rdata,

    output wire        ramg_valid,
    output wire        ramg_write,
    output wire [19:0] ramg_addr,
    output wire [7:0]  ramg_wdata,
    input  wire        ramg_ready,
    input  wire        ramg_rvalid,
    input  wire [7:0]  ramg_rdata,

    input  wire        frame_tick,
    output wire        int_n,
    input  wire        dl_clk,
    input  wire [12:0] dl_read_addr,
    output reg  [7:0]  dl_read_data,
    input  wire [12:0] dl_debug_addr,
    output wire [7:0]  dl_debug_data,
    output wire [15:0] cmd_read_ptr_out,
    output wire [15:0] cmd_write_ptr_out
);
    localparam [21:0] ROM_CHIPID      = 22'h0C0000;
    localparam [21:0] RAM_DL_BASE     = 22'h300000;
    localparam [21:0] RAM_REG_BASE    = 22'h302000;
    localparam [21:0] RAM_CMD_BASE    = 22'h308000;

    localparam [21:0] REG_ID          = 22'h302000;
    localparam [21:0] REG_FRAMES      = 22'h302004;
    localparam [21:0] REG_CLOCK       = 22'h302008;
    localparam [21:0] REG_FREQUENCY   = 22'h30200C;
    localparam [21:0] REG_CPURESET    = 22'h302020;
    localparam [21:0] REG_HCYCLE      = 22'h30202C;
    localparam [21:0] REG_HOFFSET     = 22'h302030;
    localparam [21:0] REG_HSIZE       = 22'h302034;
    localparam [21:0] REG_HSYNC0      = 22'h302038;
    localparam [21:0] REG_HSYNC1      = 22'h30203C;
    localparam [21:0] REG_VCYCLE      = 22'h302040;
    localparam [21:0] REG_VOFFSET     = 22'h302044;
    localparam [21:0] REG_VSIZE       = 22'h302048;
    localparam [21:0] REG_VSYNC0      = 22'h30204C;
    localparam [21:0] REG_VSYNC1      = 22'h302050;
    localparam [21:0] REG_DLSWAP      = 22'h302054;
    localparam [21:0] REG_PCLK        = 22'h302070;
    localparam [21:0] REG_GPIOX_DIR   = 22'h302098;
    localparam [21:0] REG_GPIOX       = 22'h30209C;
    localparam [21:0] REG_INT_FLAGS   = 22'h3020A8;
    localparam [21:0] REG_INT_EN      = 22'h3020AC;
    localparam [21:0] REG_INT_MASK    = 22'h3020B0;
    localparam [21:0] REG_CMD_READ    = 22'h3020F8;
    localparam [21:0] REG_CMD_WRITE   = 22'h3020FC;
    localparam [21:0] REG_CMD_DL      = 22'h302100;
    localparam [21:0] REG_SPI_WIDTH   = 22'h302188;
    localparam [21:0] REG_CMDB_SPACE  = 22'h302574;
    localparam [21:0] REG_CMDB_WRITE  = 22'h302578;

    reg [7:0] ram_dl [0:8191];
    reg [7:0] ram_cmd [0:4095];

    reg [31:0] frames;
    reg [31:0] clock_counter;
    reg [31:0] frequency;
    reg [15:0] hcycle, hoffset, hsize, hsync0, hsync1;
    reg [15:0] vcycle, voffset, vsize, vsync0, vsync1;
    reg [7:0] cpureset;
    reg [7:0] dlswap;
    reg [7:0] pclk;
    reg [15:0] gpiox_dir;
    reg [15:0] gpiox;
    reg [7:0] int_flags;
    reg [7:0] int_en;
    reg [7:0] int_mask;
    reg [15:0] cmd_read_ptr;
    reg [15:0] cmd_write_ptr;
    reg [15:0] cmd_dl_ptr;
    reg [7:0] spi_width;

    reg internal_rvalid;
    reg [7:0] internal_rdata;

    wire in_ramg = bus_addr < 22'h100000;
    wire in_ramdl = bus_addr >= RAM_DL_BASE && bus_addr < RAM_DL_BASE + 8192;
    wire in_ramcmd = bus_addr >= RAM_CMD_BASE && bus_addr < RAM_CMD_BASE + 4096;
    // FT81x treats a continuous write starting at REG_CMDB_WRITE as a stream.
    wire in_cmdb_stream = bus_addr >= REG_CMDB_WRITE &&
                          bus_addr < REG_CMDB_WRITE + 4096;
    wire internal_target = !in_ramg;
    wire [11:0] cmd_occupancy = cmd_write_ptr[11:0] - cmd_read_ptr[11:0];
    wire [15:0] cmdb_space = 16'd4092 - {4'b0, cmd_occupancy};

    assign ramg_valid = bus_valid && in_ramg;
    assign ramg_write = bus_write;
    assign ramg_addr = bus_addr[19:0];
    assign ramg_wdata = bus_wdata;
    assign bus_ready = internal_target ? 1'b1 : ramg_ready;
    assign bus_rvalid = internal_rvalid | ramg_rvalid;
    assign bus_rdata = ramg_rvalid ? ramg_rdata : internal_rdata;
    assign int_n = !(int_en[0] && |(int_flags & int_mask));
    assign dl_debug_data = ram_dl[dl_debug_addr];
    assign cmd_read_ptr_out = cmd_read_ptr;
    assign cmd_write_ptr_out = cmd_write_ptr;

    function automatic [7:0] byte32;
        input [31:0] value;
        input [1:0] offset;
        begin
            case (offset)
                2'd0: byte32 = value[7:0];
                2'd1: byte32 = value[15:8];
                2'd2: byte32 = value[23:16];
                default: byte32 = value[31:24];
            endcase
        end
    endfunction

    function automatic [7:0] byte16;
        input [15:0] value;
        input offset;
        begin
            byte16 = offset ? value[15:8] : value[7:0];
        end
    endfunction

    task write16;
        inout [15:0] value;
        input offset;
        input [7:0] data;
        begin
            if (offset)
                value[15:8] <= data;
            else
                value[7:0] <= data;
        end
    endtask

    task write32;
        inout [31:0] value;
        input [1:0] offset;
        input [7:0] data;
        begin
            case (offset)
                2'd0: value[7:0] <= data;
                2'd1: value[15:8] <= data;
                2'd2: value[23:16] <= data;
                2'd3: value[31:24] <= data;
            endcase
        end
    endtask

    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            frames <= 0;
            clock_counter <= 0;
            frequency <= 32'd60_000_000;
            hcycle <= 16'd858;
            hoffset <= 16'd122;
            hsize <= 16'd720;
            hsync0 <= 16'd0;
            hsync1 <= 16'd62;
            vcycle <= 16'd525;
            voffset <= 16'd36;
            vsize <= 16'd480;
            vsync0 <= 16'd0;
            vsync1 <= 16'd6;
            cpureset <= 0;
            dlswap <= 0;
            pclk <= 0;
            gpiox_dir <= 0;
            gpiox <= 0;
            int_flags <= 0;
            int_en <= 0;
            int_mask <= 0;
            cmd_read_ptr <= 0;
            cmd_write_ptr <= 0;
            cmd_dl_ptr <= 0;
            spi_width <= 0;
            internal_rvalid <= 0;
            internal_rdata <= 0;
            dl_read_data <= 0;
        end else begin
            clock_counter <= clock_counter + 1'b1;
            internal_rvalid <= 0;
            if (frame_tick) begin
                frames <= frames + 1'b1;
                if (dlswap != 0) begin
                    dlswap <= 0;
                    int_flags[0] <= 1'b1;
                end
            end

            if (bus_valid && bus_ready && internal_target) begin
                if (bus_write) begin
                    if (in_ramdl) begin
                        ram_dl[bus_addr[12:0]] <= bus_wdata;
                    end else if (in_ramcmd) begin
                        ram_cmd[bus_addr[11:0]] <= bus_wdata;
                    end else if (in_cmdb_stream) begin
                        ram_cmd[cmd_write_ptr[11:0]] <= bus_wdata;
                        cmd_write_ptr <= {4'b0, cmd_write_ptr[11:0] + 1'b1};
                    end else if (bus_addr >= REG_FREQUENCY && bus_addr < REG_FREQUENCY+4) begin
                        write32(frequency, bus_addr[1:0], bus_wdata);
                    end else if (bus_addr >= REG_HCYCLE && bus_addr < REG_HCYCLE+2) begin
                        write16(hcycle, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_HOFFSET && bus_addr < REG_HOFFSET+2) begin
                        write16(hoffset, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_HSIZE && bus_addr < REG_HSIZE+2) begin
                        write16(hsize, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_HSYNC0 && bus_addr < REG_HSYNC0+2) begin
                        write16(hsync0, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_HSYNC1 && bus_addr < REG_HSYNC1+2) begin
                        write16(hsync1, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_VCYCLE && bus_addr < REG_VCYCLE+2) begin
                        write16(vcycle, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_VOFFSET && bus_addr < REG_VOFFSET+2) begin
                        write16(voffset, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_VSIZE && bus_addr < REG_VSIZE+2) begin
                        write16(vsize, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_VSYNC0 && bus_addr < REG_VSYNC0+2) begin
                        write16(vsync0, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_VSYNC1 && bus_addr < REG_VSYNC1+2) begin
                        write16(vsync1, bus_addr[0], bus_wdata);
                    end else if (bus_addr == REG_CPURESET) begin
                        cpureset <= bus_wdata;
                    end else if (bus_addr == REG_DLSWAP) begin
                        dlswap <= bus_wdata;
                    end else if (bus_addr == REG_PCLK) begin
                        pclk <= bus_wdata;
                    end else if (bus_addr >= REG_GPIOX_DIR && bus_addr < REG_GPIOX_DIR+2) begin
                        write16(gpiox_dir, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_GPIOX && bus_addr < REG_GPIOX+2) begin
                        write16(gpiox, bus_addr[0], bus_wdata);
                    end else if (bus_addr == REG_INT_EN) begin
                        int_en <= bus_wdata;
                    end else if (bus_addr == REG_INT_MASK) begin
                        int_mask <= bus_wdata;
                    end else if (bus_addr >= REG_CMD_READ && bus_addr < REG_CMD_READ+2) begin
                        write16(cmd_read_ptr, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_CMD_WRITE && bus_addr < REG_CMD_WRITE+2) begin
                        write16(cmd_write_ptr, bus_addr[0], bus_wdata);
                    end else if (bus_addr >= REG_CMD_DL && bus_addr < REG_CMD_DL+2) begin
                        write16(cmd_dl_ptr, bus_addr[0], bus_wdata);
                    end else if (bus_addr == REG_SPI_WIDTH) begin
                        spi_width <= bus_wdata;
                    end
                end else begin
                    internal_rvalid <= 1'b1;
                    if (in_ramdl) begin
                        internal_rdata <= ram_dl[bus_addr[12:0]];
                    end else if (in_ramcmd) begin
                        internal_rdata <= ram_cmd[bus_addr[11:0]];
                    end else if (bus_addr >= ROM_CHIPID && bus_addr < ROM_CHIPID+4) begin
                        case (bus_addr[1:0])
                            2'd0: internal_rdata <= 8'h08;
                            2'd1: internal_rdata <= 8'h12;
                            default: internal_rdata <= 8'h00;
                        endcase
                    end else if (bus_addr == REG_ID) begin
                        internal_rdata <= 8'h7C;
                    end else if (bus_addr >= REG_FRAMES && bus_addr < REG_FRAMES+4) begin
                        internal_rdata <= byte32(frames, bus_addr[1:0]);
                    end else if (bus_addr >= REG_CLOCK && bus_addr < REG_CLOCK+4) begin
                        internal_rdata <= byte32(clock_counter, bus_addr[1:0]);
                    end else if (bus_addr >= REG_FREQUENCY && bus_addr < REG_FREQUENCY+4) begin
                        internal_rdata <= byte32(frequency, bus_addr[1:0]);
                    end else if (bus_addr >= REG_HCYCLE && bus_addr < REG_HCYCLE+2) begin
                        internal_rdata <= byte16(hcycle, bus_addr[0]);
                    end else if (bus_addr >= REG_HOFFSET && bus_addr < REG_HOFFSET+2) begin
                        internal_rdata <= byte16(hoffset, bus_addr[0]);
                    end else if (bus_addr >= REG_HSIZE && bus_addr < REG_HSIZE+2) begin
                        internal_rdata <= byte16(hsize, bus_addr[0]);
                    end else if (bus_addr >= REG_HSYNC0 && bus_addr < REG_HSYNC0+2) begin
                        internal_rdata <= byte16(hsync0, bus_addr[0]);
                    end else if (bus_addr >= REG_HSYNC1 && bus_addr < REG_HSYNC1+2) begin
                        internal_rdata <= byte16(hsync1, bus_addr[0]);
                    end else if (bus_addr >= REG_VCYCLE && bus_addr < REG_VCYCLE+2) begin
                        internal_rdata <= byte16(vcycle, bus_addr[0]);
                    end else if (bus_addr >= REG_VOFFSET && bus_addr < REG_VOFFSET+2) begin
                        internal_rdata <= byte16(voffset, bus_addr[0]);
                    end else if (bus_addr >= REG_VSIZE && bus_addr < REG_VSIZE+2) begin
                        internal_rdata <= byte16(vsize, bus_addr[0]);
                    end else if (bus_addr >= REG_VSYNC0 && bus_addr < REG_VSYNC0+2) begin
                        internal_rdata <= byte16(vsync0, bus_addr[0]);
                    end else if (bus_addr >= REG_VSYNC1 && bus_addr < REG_VSYNC1+2) begin
                        internal_rdata <= byte16(vsync1, bus_addr[0]);
                    end else if (bus_addr == REG_CPURESET) begin
                        internal_rdata <= cpureset;
                    end else if (bus_addr == REG_DLSWAP) begin
                        internal_rdata <= dlswap;
                    end else if (bus_addr == REG_PCLK) begin
                        internal_rdata <= pclk;
                    end else if (bus_addr >= REG_GPIOX_DIR && bus_addr < REG_GPIOX_DIR+2) begin
                        internal_rdata <= byte16(gpiox_dir, bus_addr[0]);
                    end else if (bus_addr >= REG_GPIOX && bus_addr < REG_GPIOX+2) begin
                        internal_rdata <= byte16(gpiox, bus_addr[0]);
                    end else if (bus_addr == REG_INT_FLAGS) begin
                        internal_rdata <= int_flags;
                        int_flags <= 0;
                    end else if (bus_addr == REG_INT_EN) begin
                        internal_rdata <= int_en;
                    end else if (bus_addr == REG_INT_MASK) begin
                        internal_rdata <= int_mask;
                    end else if (bus_addr >= REG_CMD_READ && bus_addr < REG_CMD_READ+2) begin
                        internal_rdata <= byte16(cmd_read_ptr, bus_addr[0]);
                    end else if (bus_addr >= REG_CMD_WRITE && bus_addr < REG_CMD_WRITE+2) begin
                        internal_rdata <= byte16(cmd_write_ptr, bus_addr[0]);
                    end else if (bus_addr >= REG_CMD_DL && bus_addr < REG_CMD_DL+2) begin
                        internal_rdata <= byte16(cmd_dl_ptr, bus_addr[0]);
                    end else if (bus_addr == REG_SPI_WIDTH) begin
                        internal_rdata <= spi_width;
                    end else if (bus_addr >= REG_CMDB_SPACE && bus_addr < REG_CMDB_SPACE+2) begin
                        internal_rdata <= byte16(cmdb_space, bus_addr[0]);
                    end else begin
                        internal_rdata <= 8'h00;
                    end
                end
            end
        end
    end


    // Second RAM_DL port for the display-list renderer.
    always @(posedge dl_clk)
        dl_read_data <= ram_dl[dl_read_addr];
endmodule
