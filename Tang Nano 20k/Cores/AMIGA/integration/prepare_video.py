"""Local video changes, applied after prepare.py has generated the base core.

Upstream remains unchanged. Additional display-window ports only observe the
chipset's DIW state; they do not change the emulated Amiga's raster or CPU.
"""
from pathlib import Path
import re

fp=ROOT/'fpga'
up=ROOT/'upstream/src/minimig-aga'

def replace_once(s,old,new):
    assert s.count(old)==1,(old,s.count(old))
    return s.replace(old,new)

s=(up/'agnus_bitplanedma.v').read_text()
s=replace_once(s,'module agnus_bitplanedma (','module agnus_bitplanedma (\n  output wire vde,')
s=s.replace('// register names and adresses','assign vde=vdiwena;\n\n// register names and adresses',1)
(fp/'agnus_bitplanedma.v').write_text(s)
s=(up/'agnus.v').read_text()
s=re.sub(r'(module agnus\s*\()',r'\1\n output wire vde,',s,count=1)
s=replace_once(s,'agnus_bitplanedma bpd1','agnus_bitplanedma bpd1')
s=re.sub(r'(agnus_bitplanedma bpd1\s*\()',r'\1\n .vde(vde),',s,count=1)
(fp/'agnus.v').write_text(s)
s=(up/'denise.v').read_text()
s=re.sub(r'(module denise\s*\()',r'\1\n output wire display_window,',s,count=1)
s=replace_once(s,'assign blank_out = t_blank;','assign blank_out = t_blank;\nassign display_window=window_del;')
(fp/'denise.v').write_text(s)
s=(fp/'minimig.v').read_text()
s=replace_once(s,'module minimig\n(','module minimig\n(\n output wire video_window,\n input wire video_steal,')
s=replace_once(s,'\t.nrdy(gayle_nrdy & rd_cyc),','\t.nrdy((gayle_nrdy & rd_cyc) | (video_steal & dbs)), // видео отстаёт: CPU ждёт chip RAM')
s=replace_once(s,'\t.nrdy(1\'b0),','\t.nrdy(video_steal & dbs),')
s=s.replace('wire        hbl, hde;','wire        hbl, hde;\nwire video_vde,video_hde;\nassign video_window=video_vde && video_hde;',1)
s=re.sub(r'(agnus AGNUS1\s*\()',r'\1\n .vde(video_vde),',s,count=1)
s=re.sub(r'(denise DENISE1\s*\()',r'\1\n .display_window(video_hde),',s,count=1)
assert '.vde(video_vde)' in s and 'assign video_window=' in s
(fp/'minimig.v').write_text(s)
s=(fp/'nanomig.v').read_text()
s=re.sub(r'(module nanomig\s*\()',r'\1\n output wire video_window,video_lace,video_field,\n input wire video_steal,',s,count=1)
s=replace_once(s,'.field1       (                 ),','.field1       (video_field      ),')
s=replace_once(s,'.lace         (                 ),','.lace         (video_lace       ),')
s=re.sub(r'(minimig minimig\s*\()',r'\1\n .video_window(video_window),.video_steal(video_steal),',s,count=1)
assert '.video_window(video_window)' in s
a=s.index('Amber AMBER')
b=s.index('endmodule',a)
s=s[:a]+'''// Native 14MHz hires stream; line storage/scaling belongs to amiga_video.
assign {r,g,b}={r_in,g_in,b_in};
assign hs=hs_in,vs=vs_in;
'''+s[b:]
(fp/'nanomig.v').write_text(s)
(fp/'sdram.sv').write_text((ROOT/'integration/sdram_video.sv').read_text())
s=(fp/'amigaclks.v').read_text()
s=replace_once(s,'.CLKIN(clk_28_i),','.CLKIN(clk_in),')
s=replace_once(s,'tmdsclk_inst.FCLKIN = "28.5"','tmdsclk_inst.FCLKIN = "27"')
s=replace_once(s,'tmdsclk_inst.IDIV_SEL = 0','tmdsclk_inst.IDIV_SEL = 3')
s=replace_once(s,'tmdsclk_inst.DYN_FBDIV_SEL = "true"','tmdsclk_inst.DYN_FBDIV_SEL = "false"')
s=replace_once(s,'tmdsclk_inst.FBDIV_SEL = 4','tmdsclk_inst.FBDIV_SEL = 54')
s=replace_once(s,'tmdsclk_inst.DYN_ODIV_SEL = "true"','tmdsclk_inst.DYN_ODIV_SEL = "false"')
s=replace_once(s,'tmdsclk_inst.ODIV_SEL = 4','tmdsclk_inst.ODIV_SEL = 2')
s=replace_once(s,".RESETN(1'b1)",".RESETN(video_locked)")
(fp/'amigaclks.v').write_text(s)
s=(fp/'amiga_vdac4.sv').read_text()
s=s.replace('.clk_pixel(),','.clk_pixel(clk_pixel),',1)
s=s.replace('.video_locked()','.video_locked(video_locked)',1)
s=s.replace('wire clk_pixel;','wire clk_pixel,video_locked;',1)
s=s.replace('assign clk_pixel=clk_28m; // one pixel domain; serializer runs at 5x','',1)
s=replace_once(s,'wire hs_n, vs_n; wire [7:0] red, green, blue;', '''wire hs_n, vs_n; wire [7:0] red, green, blue;
wire video_window,video_lace,video_field;
wire [3:0] picture;
wire video_cs,video_we,video_double,video_steal;wire [20:0] video_address;
wire [127:0] video_din,video_dout;wire video_ack,video_read_valid;wire [1:0] video_fetch_index;wire [31:0] video_diagnostic;
// 7 ноября 2026: видео отстаёт (clk85) -> clk28, в nrdy моста CPU minimig.
reg [1:0] video_steal_s=0;wire video_steal_28=video_steal_s[1];
always @(posedge clk_28m) video_steal_s<={video_steal_s[0],video_steal};''')
s=s.replace('.audio_left(audio_left),.audio_right(audio_right),.key_free(key_free),',
    '.picture(picture),.video_diagnostic(video_diagnostic),\n    .audio_left(audio_left),.audio_right(audio_right),.key_free(key_free),',1)
s=s.replace(' .hs(hs_n),',' .video_window(video_window),.video_lace(video_lace),.video_field(video_field),.video_steal(video_steal_28),\n .hs(hs_n),',1)
s=s.replace('.CHIP48_BURST(CHIP48_BURST)', '.VIDEO_PORT(1),.VIDEO_STREAM(1),.CHIP48_BURST(CHIP48_BURST)',1)
s=s.replace('.p2_ack        ( fastram_ready   )','.p2_ack        ( fastram_ready   ),\n    .video_cs(video_cs),.video_we(video_we),.video_double(video_double),.video_urgent(video_steal),.video_addr(video_address),\n    .video_din(video_din),.video_dout(video_dout),.video_ack(video_ack),.video_read_valid(video_read_valid),.video_fetch_index(video_fetch_index)',1)
assert '.VIDEO_PORT(1)' in s
a=s.index('wire [15:0] silent_audio')
s=s[:a]+'''// 720p renderer, independent of the chipset raster. Audio remains on LINK.
wire [2:0] tmds;wire tmds_clock;
reg [2:0] video_reset=7;
always @(posedge clk_pixel) video_reset<={video_reset[1:0],!video_locked};
amiga_video video(.clk28(clk_28m),.clk85(clk_85m),.hclk(clk_pixel),.hclk5(clk_pixel_x5),
 .reset28(rst_28m),.reset85(rst_85m),.hreset(video_reset[2]),
 .hs(hs_n),.vs(vs_n),.lace(video_lace),.field(video_field),.window_in(video_window),
 .rgb_in({red,green,blue}),.picture(picture),
 .osd_wr(osd_wr),.osd_addr(osd_addr),.osd_data(osd_data),.osd_show(osd_show),.osd_sel(osd_sel),
 .osd_band(osd_band),.osd_rule(osd_rule),
 .ram_cs(video_cs),.ram_we(video_we),.ram_double(video_double),.ram_address(video_address),.ram_din(video_din),
 .ram_dout(video_dout),.ram_ack(video_ack),.ram_read_valid(video_read_valid),.ram_fetch_index(video_fetch_index),.diagnostic(video_diagnostic),.steal(video_steal),.raster_debug(video_raster),
 .tmds_clock(tmds_clock),.tmds(tmds));
ELVDS_OBUF tmds_bufds[3:0](.I({tmds_clock,tmds}),.O({O_tmds_clk_p,O_tmds_data_p}),.OB({O_tmds_clk_n,O_tmds_data_n}));
endmodule
`default_nettype wire
'''
s=s.replace('wire [31:0] debug_words [0:35];','wire [5:0] video_raster;\nwire [31:0] debug_words [0:35];')
s=s.replace('assign debug_words[2]=cpu_debug[2]; // live CPU address, no trace RAM',
    'assign debug_words[2]={video_locked,video_reset[2],video_raster,cpu_debug[2][23:0]}; // video PLL/reset/raster and CPU address')
(fp/'amiga_vdac4.sv').write_text(s)
s=(ROOT/'build.tcl').read_text()
for name in ['denise','agnus','agnus_bitplanedma']:
    s=s.replace('add_file upstream/src/minimig-aga/'+name+'.v','add_file fpga/'+name+'.v')
for name in ['amiga_video','amiga_video_capture','amiga_video_cache','amiga_video_dma','amiga_video_geometry','hdmi_tx','tmds_encoder']:
    s=s.replace('add_file fpga/amiga_vdac4.sv','add_file fpga/amiga_vdac4.sv\nadd_file fpga/'+name+'.sv',1)
s=s.replace('set_option -gen_text_timing_rpt 1','set_option -gen_text_timing_rpt 1\nset_option -gen_sdf 1')
s=s.replace('set_option -verilog_std sysv2017','set_option -verilog_std sysv2017\nset_option -global_freq 85.5')
(ROOT/'build.tcl').write_text(s)
s=(fp/'amigaVdac4.sdc').read_text()
s=re.sub(r'create_generated_clock -name clk_hdmi[^\n]+',
    'create_generated_clock -name clk_hdmi -source [get_ports {clk_27m}] -multiply_by 55 -divide_by 4 [get_pins {amigaclks/tmdsclk_inst/CLKOUT}]\n'
    'create_generated_clock -name clk_pixel -source [get_pins {amigaclks/tmdsclk_inst/CLKOUT}] -divide_by 5 [get_pins {amigaclks/clkdiv_inst/CLKOUT}]',s)
s=s.replace('-group [get_clocks {clk85 clk28 clk_hdmi clk_osc}]','-group [get_clocks {clk85 clk28 clk_osc}] -group [get_clocks {clk_hdmi clk_pixel}]')
(fp/'amigaVdac4.sdc').write_text(s)
