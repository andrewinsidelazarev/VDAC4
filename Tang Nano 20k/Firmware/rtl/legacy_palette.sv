// SPDX-License-Identifier: Apache-2.0
//
// Расширение 5 бит на компоненту до 8 бит — точно как в CPLD VDAC2, с учётом PAL_SEL.
//
//   PAL_SEL = 1: значение просто сдвигается влево на три бита (v × 8).
//   PAL_SEL = 0: v ≥ 24 насыщается в 0xFF, иначе round(v × 255 / 24).
//
// Вторая ветвь записана таблицей из 24 констант, а не формулой с делением.
// Так это и было в CPLD, и так оно ложится в несколько LUT: деление на 24
// синтезатор разворачивал в сотни ячеек арифметики с переносом и три блока DSP.
// Значения таблицы получены по той же формуле round(v × 255 / 24) = (v×255+12)/24
// и сверены посимвольно с прежней реализацией.
//
// pal передаётся аргументом функции, а не читается из порта внутри неё: иначе
// список чувствительности непрерывного присваивания не включает pal_sel и симуляция
// расходится с синтезом (выход не пересчитывается, пока не менялись компоненты цвета).
//
module legacy_palette (
    input  wire       pal_sel,
    input  wire [4:0] r5,
    input  wire [4:0] g5,
    input  wire [4:0] b5,
    output wire [7:0] r8,
    output wire [7:0] g8,
    output wire [7:0] b8
);

    function automatic [7:0] expand;
        input       pal;
        input [4:0] value;
        begin
            if (pal)
                expand = {value, 3'b000};
            else begin
                case (value)
                    5'd0:  expand = 8'd0;
                    5'd1:  expand = 8'd11;
                    5'd2:  expand = 8'd21;
                    5'd3:  expand = 8'd32;
                    5'd4:  expand = 8'd43;
                    5'd5:  expand = 8'd53;
                    5'd6:  expand = 8'd64;
                    5'd7:  expand = 8'd74;
                    5'd8:  expand = 8'd85;
                    5'd9:  expand = 8'd96;
                    5'd10: expand = 8'd106;
                    5'd11: expand = 8'd117;
                    5'd12: expand = 8'd128;
                    5'd13: expand = 8'd138;
                    5'd14: expand = 8'd149;
                    5'd15: expand = 8'd159;
                    5'd16: expand = 8'd170;
                    5'd17: expand = 8'd181;
                    5'd18: expand = 8'd191;
                    5'd19: expand = 8'd202;
                    5'd20: expand = 8'd213;
                    5'd21: expand = 8'd223;
                    5'd22: expand = 8'd234;
                    5'd23: expand = 8'd244;
                    default: expand = 8'hFF;   // 24…31 — насыщение, как в CPLD
                endcase
            end
        end
    endfunction

    assign r8 = expand(pal_sel, r5);
    assign g8 = expand(pal_sel, g5);
    assign b8 = expand(pal_sel, b5);

endmodule
