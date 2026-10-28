#!/bin/sh
# Подбор сдвига клока SDRAM ядра FT812 (sdram_train, с 1.38) на модели с задержками корпуса
# (tb/tb_sd_train.sv): для каждого набора задержек подбор должен найти участок и встать на середину,
# а случайные запросы после него — пройти без расхождений и нарушений протокола.
#   sh scripts/run_sd_train.sh                       — все наборы (до минуты на набор)
#   sh scripts/run_sd_train.sh one TK TC TD SKEW [SEED]
# Симулятор: Icarus Verilog (IVERILOG/VVP или в PATH).
set -e
cd "$(dirname "$0")/.."
IV=${IVERILOG:-iverilog}
VVP=${VVP:-vvp}
OUT=${SIM_DIR:-sim}/sd_train
mkdir -p "$OUT"

run() {
    echo "=== клок $1 нс, команды $2 нс, данные $3 нс, разброс DQ до $4 нс"
    "$IV" -g2012 -DSIMULATION -s tb_sd_train -Ptb_sd_train.TK=$1 -Ptb_sd_train.TC=$2 \
        -Ptb_sd_train.TD=$3 -Ptb_sd_train.SKEW=$4 -Ptb_sd_train.SEED=${5:-1} \
        -o "$OUT/t_$1_$2_$3_$4.vvp" rtl/sdram_burst.sv tb/sdram_model.sv tb/tb_sd_train.sv
    "$VVP" -n "$OUT/t_$1_$2_$3_$4.vvp" | grep -v '^\[SDRAM' | grep -v '^tb/'
}

if [ "$1" = "one" ]; then run "$2" "$3" "$4" "$5" "$6"; exit 0; fi

run 0 0 0 0
run 1.5 2.5 1.0 0.6 2
run 3.0 1.0 2.5 0.8 3
run 0.5 3.5 3.5 1.0 4
run 4.0 4.0 0.5 0.3 5
run 2.0 0.5 5.0 1.2 6
run 6.0 2.0 2.0 0.5 7
