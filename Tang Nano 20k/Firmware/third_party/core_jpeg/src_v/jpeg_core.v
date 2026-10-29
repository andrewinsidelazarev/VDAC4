//-----------------------------------------------------------------
//                      Baseline JPEG Decoder
//                             V0.1
//                       Ultra-Embedded.com
//                        Copyright 2020
//
//                   admin@ultra-embedded.com
//-----------------------------------------------------------------
//                      License: Apache 2.0
// This IP can be freely used in commercial projects, however you may
// want access to unreleased materials such as verification environments,
// or test vectors, as well as changes to the IP for integration purposes.
// If this is the case, contact the above address.
// I am interested to hear how and where this IP is used, so please get
// in touch!
//-----------------------------------------------------------------
// Copyright 2020 Ultra-Embedded.com
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//-----------------------------------------------------------------

module jpeg_core
//-----------------------------------------------------------------
// Params
//-----------------------------------------------------------------
#(
     parameter SUPPORT_WRITABLE_DHT = 0
)
//-----------------------------------------------------------------
// Ports
//-----------------------------------------------------------------
(
    // Inputs
     input           clk_i
    ,input           rst_i
    ,input           inport_valid_i
    ,input  [ 31:0]  inport_data_i
    ,input  [  3:0]  inport_strb_i
    ,input           inport_last_i
    ,input           outport_accept_i

    // Outputs
    ,output          inport_accept_o
    ,output          outport_valid_o
    ,output [ 15:0]  outport_width_o
    ,output [ 15:0]  outport_height_o
    ,output [ 15:0]  outport_pixel_x_o
    ,output [ 15:0]  outport_pixel_y_o
    ,output [  7:0]  outport_pixel_r_o
    ,output [  7:0]  outport_pixel_g_o
    ,output [  7:0]  outport_pixel_b_o
    ,output          idle_o
    ,output [  1:0]  outport_mode_o     // VDAC4: 0 серый, 1 4:4:4, 2 4:2:0, 3 4:2:2 (высота MCU)
    ,output [  7:0]  outport_qy_o       // VDAC4: шаг квантования таблицы яркости — сила деблокера
);

wire  [ 15:0]  idct_outport_data_w;
wire           dqt_inport_valid_w;
wire  [ 31:0]  dqt_inport_id_w;
wire  [ 31:0]  output_outport_data_w;
wire  [  5:0]  idct_inport_idx_w;
wire           dqt_inport_eob_w;
wire           img_start_w;
wire  [ 15:0]  img_height_w;
wire           output_inport_accept_w;
wire  [ 15:0]  img_width_w;
wire           dht_cfg_valid_w;
wire           lookup_req_w;
wire           lookup_valid_w;
wire  [  1:0]  img_dqt_table_cb_w;
wire  [  7:0]  dht_cfg_data_w;
wire           img_end_w;
wire  [ 31:0]  idct_inport_id_w;
wire  [  4:0]  lookup_width_w;
wire           idct_inport_accept_w;
wire  [  5:0]  output_inport_idx_w;
wire  [ 15:0]  dqt_outport_data_w;
wire           dqt_inport_blk_space_w;
wire           idct_inport_valid_w;
wire  [  7:0]  dqt_cfg_data_w;
wire  [  1:0]  img_dqt_table_y_w;
wire           idct_inport_eob_w;
wire           dht_cfg_accept_w;
wire  [ 31:0]  output_inport_id_w;
wire           output_inport_valid_w;
wire  [ 15:0]  lookup_input_w;
wire           dqt_cfg_accept_w;
wire  [  7:0]  lookup_value_w;
wire  [  1:0]  lookup_table_w;
wire           dht_cfg_last_w;
wire  [  5:0]  dqt_inport_idx_w;
wire  [  1:0]  img_mode_w;
wire  [  1:0]  img_dqt_table_cr_w;
wire           bb_inport_valid_w;
wire           bb_outport_last_w;
wire           bb_inport_last_w;
wire  [  5:0]  bb_outport_pop_w;
wire  [  7:0]  bb_inport_data_w;
wire           dqt_cfg_valid_w;
wire           bb_outport_valid_w;
wire           bb_inport_accept_w;
wire  [ 31:0]  bb_outport_data_w;
wire           dqt_cfg_last_w;


// VDAC4: регистр байтов таблиц DHT (ниже, у jpeg_dht) — объявлен до подключения разбора
reg        dhq_v_q, dhb_v_q;
reg [8:0]  dhq_q, dhb_q;                 // {last, байт}
wire       dhq_in_accept_w = !dhb_v_q;
wire       dhq_in_take_w   = dht_cfg_valid_w && dhq_in_accept_w;
wire       dhq_out_take_w  = dhq_v_q && dht_cfg_accept_w;

jpeg_input
u_jpeg_input
(
    // Inputs
     .clk_i(clk_i)
    ,.rst_i(rst_i)
    ,.inport_valid_i(inport_valid_i)
    ,.inport_data_i(inport_data_i)
    ,.inport_strb_i(inport_strb_i)
    ,.inport_last_i(inport_last_i)
    ,.dqt_cfg_accept_i(dqt_cfg_accept_w)
    ,.dht_cfg_accept_i(dhq_in_accept_w)
    ,.data_accept_i(bb_inport_accept_w)

    // Outputs
    ,.inport_accept_o(inport_accept_o)
    ,.img_start_o(img_start_w)
    ,.img_end_o(img_end_w)
    ,.img_width_o(img_width_w)
    ,.img_height_o(img_height_w)
    ,.img_mode_o(img_mode_w)
    ,.img_dqt_table_y_o(img_dqt_table_y_w)
    ,.img_dqt_table_cb_o(img_dqt_table_cb_w)
    ,.img_dqt_table_cr_o(img_dqt_table_cr_w)
    ,.dqt_cfg_valid_o(dqt_cfg_valid_w)
    ,.dqt_cfg_data_o(dqt_cfg_data_w)
    ,.dqt_cfg_last_o(dqt_cfg_last_w)
    ,.dht_cfg_valid_o(dht_cfg_valid_w)
    ,.dht_cfg_data_o(dht_cfg_data_w)
    ,.dht_cfg_last_o(dht_cfg_last_w)
    ,.data_valid_o(bb_inport_valid_w)
    ,.data_data_o(bb_inport_data_w)
    ,.data_last_o(bb_inport_last_w)
);


//-----------------------------------------------------------------
// VDAC4: байты таблиц DHT — через регистр на два байта (выход и запасной): таблицы получают байт
// с триггеров, разбор — готовность от запасного. Путь «слово входа → разбор маркеров jpeg_input →
// построение таблиц jpeg_dht» не укладывался в такт 61,875 МГц в пробной сборке ядра видео с
// деблокером записи кадра (−1,8 нс). Таблицы отстают от разбора не больше чем на 2 байта, а до
// первого поиска кода — заголовок SOS (десяток байтов и больше)
//-----------------------------------------------------------------

always @ (posedge clk_i )
if (rst_i)
begin
    dhq_v_q <= 1'b0; dhb_v_q <= 1'b0; dhq_q <= 9'd0; dhb_q <= 9'd0;
end
else if (!dhq_v_q || dhq_out_take_w)
begin
    if (dhb_v_q)
    begin
        dhq_q <= dhb_q; dhq_v_q <= 1'b1; dhb_v_q <= 1'b0;
    end
    else
    begin
        dhq_q <= {dht_cfg_last_w, dht_cfg_data_w}; dhq_v_q <= dhq_in_take_w;
    end
end
else if (dhq_in_take_w)
begin
    dhb_q <= {dht_cfg_last_w, dht_cfg_data_w}; dhb_v_q <= 1'b1;
end

jpeg_dht
#(
     .SUPPORT_WRITABLE_DHT(SUPPORT_WRITABLE_DHT)
)
u_jpeg_dht
(
    // Inputs
     .clk_i(clk_i)
    ,.rst_i(rst_i)
    ,.cfg_valid_i(dhq_v_q)
    ,.cfg_data_i(dhq_q[7:0])
    ,.cfg_last_i(dhq_q[8])
    ,.lookup_req_i(lookup_req_w)
    ,.lookup_table_i(lookup_table_w)
    ,.lookup_input_i(lookup_input_w)

    // Outputs
    ,.cfg_accept_o(dht_cfg_accept_w)
    ,.lookup_valid_o(lookup_valid_w)
    ,.lookup_width_o(lookup_width_w)
    ,.lookup_value_o(lookup_value_w)
);


jpeg_idct
u_jpeg_idct
(
    // Inputs
     .clk_i(clk_i)
    ,.rst_i(rst_i)
    ,.img_start_i(img_start_w)
    ,.img_end_i(img_end_w)
    ,.inport_valid_i(idct_inport_valid_w)
    ,.inport_data_i(idct_outport_data_w)
    ,.inport_idx_i(idct_inport_idx_w)
    ,.inport_eob_i(idct_inport_eob_w)
    ,.inport_id_i(idct_inport_id_w)
    ,.outport_accept_i(output_inport_accept_w)

    // Outputs
    ,.inport_accept_o(idct_inport_accept_w)
    ,.outport_valid_o(output_inport_valid_w)
    ,.outport_data_o(output_outport_data_w)
    ,.outport_idx_o(output_inport_idx_w)
    ,.outport_id_o(output_inport_id_w)
);


jpeg_dqt
u_jpeg_dqt
(
    // Inputs
     .clk_i(clk_i)
    ,.rst_i(rst_i)
    ,.img_start_i(img_start_w)
    ,.img_end_i(img_end_w)
    ,.img_dqt_table_y_i(img_dqt_table_y_w)
    ,.img_dqt_table_cb_i(img_dqt_table_cb_w)
    ,.img_dqt_table_cr_i(img_dqt_table_cr_w)
    ,.cfg_valid_i(dqt_cfg_valid_w)
    ,.cfg_data_i(dqt_cfg_data_w)
    ,.cfg_last_i(dqt_cfg_last_w)
    ,.inport_valid_i(dqt_inport_valid_w)
    ,.inport_data_i(dqt_outport_data_w)
    ,.inport_idx_i(dqt_inport_idx_w)
    ,.inport_id_i(dqt_inport_id_w)
    ,.inport_eob_i(dqt_inport_eob_w)
    ,.outport_accept_i(idct_inport_accept_w)

    // Outputs
    ,.cfg_accept_o(dqt_cfg_accept_w)
    ,.inport_blk_space_o(dqt_inport_blk_space_w)
    ,.outport_valid_o(idct_inport_valid_w)
    ,.outport_data_o(idct_outport_data_w)
    ,.outport_idx_o(idct_inport_idx_w)
    ,.outport_id_o(idct_inport_id_w)
    ,.outport_eob_o(idct_inport_eob_w)
    ,.qs_y_o(outport_qy_o)
);


jpeg_output
u_jpeg_output
(
    // Inputs
     .clk_i(clk_i)
    ,.rst_i(rst_i)
    ,.img_start_i(img_start_w)
    ,.img_end_i(img_end_w)
    ,.img_width_i(img_width_w)
    ,.img_height_i(img_height_w)
    ,.img_mode_i(img_mode_w)
    ,.inport_valid_i(output_inport_valid_w)
    ,.inport_data_i(output_outport_data_w)
    ,.inport_idx_i(output_inport_idx_w)
    ,.inport_id_i(output_inport_id_w)
    ,.outport_accept_i(outport_accept_i)

    // Outputs
    ,.inport_accept_o(output_inport_accept_w)
    ,.outport_valid_o(outport_valid_o)
    ,.outport_width_o(outport_width_o)
    ,.outport_height_o(outport_height_o)
    ,.outport_pixel_x_o(outport_pixel_x_o)
    ,.outport_pixel_y_o(outport_pixel_y_o)
    ,.outport_pixel_r_o(outport_pixel_r_o)
    ,.outport_pixel_g_o(outport_pixel_g_o)
    ,.outport_pixel_b_o(outport_pixel_b_o)
    ,.idle_o(idle_o)
);


jpeg_bitbuffer
u_jpeg_bitbuffer
(
    // Inputs
     .clk_i(clk_i)
    ,.rst_i(rst_i)
    ,.img_start_i(img_start_w)
    ,.img_end_i(img_end_w)
    ,.inport_valid_i(bb_inport_valid_w)
    ,.inport_data_i(bb_inport_data_w)
    ,.inport_last_i(bb_inport_last_w)
    ,.outport_pop_i(bb_outport_pop_w)

    // Outputs
    ,.inport_accept_o(bb_inport_accept_w)
    ,.outport_valid_o(bb_outport_valid_w)
    ,.outport_data_o(bb_outport_data_w)
    ,.outport_last_o(bb_outport_last_w)
);


jpeg_mcu_proc
u_jpeg_mcu_proc
(
    // Inputs
     .clk_i(clk_i)
    ,.rst_i(rst_i)
    ,.img_start_i(img_start_w)
    ,.img_end_i(img_end_w)
    ,.img_width_i(img_width_w)
    ,.img_height_i(img_height_w)
    ,.img_mode_i(img_mode_w)
    ,.inport_valid_i(bb_outport_valid_w)
    ,.inport_data_i(bb_outport_data_w)
    ,.inport_last_i(bb_outport_last_w)
    ,.lookup_valid_i(lookup_valid_w)
    ,.lookup_width_i(lookup_width_w)
    ,.lookup_value_i(lookup_value_w)
    ,.outport_blk_space_i(dqt_inport_blk_space_w)

    // Outputs
    ,.inport_pop_o(bb_outport_pop_w)
    ,.lookup_req_o(lookup_req_w)
    ,.lookup_table_o(lookup_table_w)
    ,.lookup_input_o(lookup_input_w)
    ,.outport_valid_o(dqt_inport_valid_w)
    ,.outport_data_o(dqt_outport_data_w)
    ,.outport_idx_o(dqt_inport_idx_w)
    ,.outport_id_o(dqt_inport_id_w)
    ,.outport_eob_o(dqt_inport_eob_w)
);



// VDAC4: режим цветности — для записи кадра (высота плитки)
assign outport_mode_o = img_mode_w;

endmodule
