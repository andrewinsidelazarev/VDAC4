// SPDX-License-Identifier: GPL-3.0-or-later
// Заглушка YM2612 для VDAC4 (03.11.2026). Звук ФМ синтезирует ESP по записям в порты; здесь остаётся
// только то, что процессоры читают обратно: таймеры A и B с флагами и флаг занятости.
//
// Деления тактов как у микросхемы (YM3438_APL, JT12): cen — такт ФМ (MCLK/7, 7,67 МГц), внутри /6 —
// такт синтеза (1,278 МГц), 24 слота — один отсчёт (53,27 кГц). Таймер A прибавляет 1 каждый отсчёт,
// таймер B — каждые 16 отсчётов. Флаг ставится только при разрешении (биты 2 и 3 регистра 27), как
// в MAME и Genesis Plus GX. Занятость — 32 такта синтеза после записи данных, как в jt12_mmr.
// Запись в порт данных части I действует, только если адрес писали в часть I (MAME, «verified on
// real YM2608»); то же для части II.
// Снимок F5/F6 (ядро 0.6): st_q — всё состояние таймеров (кроме занятости), st_we — загрузить из st_d.
module md_ym_stub(
    input            clk,       // MCLK
    input            rst,       // сброс YM вместе с Z80
    input            cen,       // FM_CLKEN
    input      [1:0] addr,      // A1 — часть I/II, A0 — 0 адрес, 1 данные
    input            wr,        // запись в порт, один такт
    input      [7:0] din,
    output     [7:0] dout,      // статус: занят, флаги B и A
    output           synth,     // такт синтеза — для меток времени очереди
    input            st_we,     // снимок: загрузить состояние
    input     [63:0] st_d,
    output    [63:0] st_q       // {1'b0, slot, div6, флаги B A, разрешения B A, пуск B A, tb_pre, tb_cnt,
                                //  tb_val, ta_cnt, ta_val, часть, адрес}
);

reg [2:0] div6=0;
reg [4:0] slot=0;
assign synth=cen && div6==3'd5;
wire zero=synth && slot==5'd23;
always @(posedge clk) begin
    if (st_we) begin div6<=st_d[57:55]; slot<=st_d[62:58]; end
    else if (rst) begin div6<=0; slot<=0; end
    else if (cen) begin
        div6<=synth ? 3'd0 : div6+3'd1;
        if (synth) slot<=zero ? 5'd0 : slot+5'd1;
    end
end

reg [7:0] reg_addr=0;
reg reg_part=0;
reg [9:0] ta_val=0, ta_cnt=0;
reg [7:0] tb_val=0, tb_cnt=0;
reg [3:0] tb_pre=0;
reg load_a=0, load_b=0, en_a=0, en_b=0, flag_a=0, flag_b=0;
reg busy=0;
reg [4:0] busy_cnt=0;
wire data_wr=wr && addr[0] && addr[1]==reg_part;
wire part1=data_wr && !reg_part;
always @(posedge clk) begin
    if (st_we) begin
        {flag_b,flag_a,en_b,en_a,load_b,load_a,tb_pre,tb_cnt,tb_val,ta_cnt,ta_val,reg_part,reg_addr}<=st_d[54:0];
        busy<=0; busy_cnt<=0;
    end else if (rst) begin
        reg_addr<=0; reg_part<=0; load_a<=0; load_b<=0; en_a<=0; en_b<=0;
        flag_a<=0; flag_b<=0; busy<=0; busy_cnt<=0; tb_pre<=0;
    end else begin
        if (wr && !addr[0]) begin reg_addr<=din; reg_part<=addr[1]; end
        if (data_wr) begin busy<=1; busy_cnt<=0; end
        else if (busy && synth) begin
            busy_cnt<=busy_cnt+5'd1;
            if (busy_cnt==5'd31) busy<=0;
        end
        if (zero) begin
            tb_pre<=tb_pre+4'd1;
            if (load_a) begin
                if (&ta_cnt) begin ta_cnt<=ta_val; if (en_a) flag_a<=1; end
                else ta_cnt<=ta_cnt+10'd1;
            end
            if (load_b && &tb_pre) begin
                if (&tb_cnt) begin tb_cnt<=tb_val; if (en_b) flag_b<=1; end
                else tb_cnt<=tb_cnt+8'd1;
            end
        end
        if (part1) case (reg_addr)
            8'h24: ta_val[9:2]<=din;
            8'h25: ta_val[1:0]<=din[1:0];
            8'h26: tb_val<=din;
            8'h27: begin
                load_a<=din[0]; load_b<=din[1];
                if (din[0] && !load_a) ta_cnt<=ta_val;
                if (din[1] && !load_b) tb_cnt<=tb_val;
                en_a<=din[2]; en_b<=din[3];
                if (din[4]) flag_a<=0;
                if (din[5]) flag_b<=0;
            end
            default: ;
        endcase
    end
end
assign dout={busy,5'b0,flag_b,flag_a};
assign st_q={1'b0,slot,div6,flag_b,flag_a,en_b,en_a,load_b,load_a,tb_pre,tb_cnt,tb_val,ta_cnt,ta_val,reg_part,reg_addr};

endmodule
