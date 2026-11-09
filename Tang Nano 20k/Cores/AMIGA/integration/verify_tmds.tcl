# VDAC4 requires DRIVE=4 on the TMDS clock and all three data pairs.
set amiga_cst_path [file join [file dirname [info script]] fpga amigaVdac4.cst]
set amiga_cst_file [open $amiga_cst_path r]
set amiga_cst_text [read $amiga_cst_file]
close $amiga_cst_file
foreach amiga_tmds_port {O_tmds_clk_p O_tmds_data_p[0] O_tmds_data_p[1] O_tmds_data_p[2]} {
    set amiga_tmds_drives {}
    foreach amiga_cst_line [split $amiga_cst_text "\n"] {
        if {[regexp {^\s*IO_PORT\s+"([^"]+)"\s+([^;]+);} $amiga_cst_line -> amiga_port amiga_attrs] && $amiga_port eq $amiga_tmds_port} {
            if {[regexp {\mDRIVE\s*=\s*([0-9]+)\M} $amiga_attrs -> amiga_drive]} {
                lappend amiga_tmds_drives $amiga_drive
            }
        }
    }
    if {$amiga_tmds_drives ne "4"} {error "VDAC4: $amiga_tmds_port must have exactly one DRIVE=4 constraint"}
}
