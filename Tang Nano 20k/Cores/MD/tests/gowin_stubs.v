// Пустышки примитивов Gowin для моделей Verilator (03.11.2026): сериализатор и дифференциальный
// буфер HDMI модели не нужны — кадр снимается до TMDS (md_video: rgb, de, hs, vs).
module OSER10(output Q, input D0, D1, D2, D3, D4, D5, D6, D7, D8, D9, input PCLK, FCLK, RESET);
assign Q = D0;
endmodule
module ELVDS_OBUF(input I, output O, output OB);
assign O = I;
assign OB = ~I;
endmodule
