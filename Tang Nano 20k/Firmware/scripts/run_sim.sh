#!/bin/sh
# Прогон симуляции видеотракта A3.1 (Icarus Verilog).
#   sh scripts/run_sim.sh            — проверка тракта
#   sh scripts/run_sim.sh ppm        — плюс выгрузка кадра в frame_out.ppm
# Ключ -P задаёт фазу чередования SEL: в прошивке по умолчанию 1.
set -e
cd "$(dirname "$0")/.."
IV=${IVERILOG:-iverilog}
VVP=${VVP:-vvp}
"$IV" -g2012 -P tb_ts_video_chain.SEL_PHASE_TB=1 -o tb_ts_video_chain.vvp \
    tb/tb_ts_video_chain.sv \
    rtl/ts_video_chain.sv rtl/ts_mux_capture.sv rtl/ts_line_buffer.sv \
    rtl/ts_video_timing.sv rtl/legacy_palette.sv
if [ "$1" = "ppm" ]; then
    "$VVP" tb_ts_video_chain.vvp +ppm
else
    "$VVP" tb_ts_video_chain.vvp
fi
