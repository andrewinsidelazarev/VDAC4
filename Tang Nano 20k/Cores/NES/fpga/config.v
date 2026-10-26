`define RES_720P
`define GW_IDE
// Tang Nano 20K: 32-bit SDRAM, four byte lanes and eight column bits.
`define NANO
package configPackage;
    localparam SDRAM_DATA_WIDTH=32, SDRAM_ROW_WIDTH=11,
               SDRAM_COL_WIDTH=8, SDRAM_BANK_WIDTH=2;
endpackage
