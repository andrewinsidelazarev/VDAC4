// Потактовая модель SDR SDRAM 64 Мбит × 32 для стенда Verilator (tb/vsys): 4 банка × 2048 строк
// × 256 столбцов, BL = 8, CL = 2 — то же поведение данных, что у tb/sdram_model.sv, без проверок
// времени (их держит тот тестбенч в iverilog; здесь — скорость).
//
// Клок модели — клок микросхемы (инверсия клока логики, как в ts_fb_hdmi_top). Слово чтения
// выставляется на фронте CL−1 после READ и держится до следующего фронта: у tb/sdram_model оно
// действительно с T_AC после этого фронта до T_OH после следующего, и обе рабочие точки выборки
// контроллера (0 — фронт логики E2, 1 — спад E2,5 через dq_neg) видят здесь то же слово.
//
// Память открыта C++ (public_flat_rw): стенд кладёт в неё RAM_G до запуска и читает кадры FT.
module sdram_cyc #(
    parameter int CL = 2
) (
    input  wire        clk,
    input  wire        cs_n,
    input  wire        ras_n,
    input  wire        cas_n,
    input  wire        we_n,
    input  wire [10:0] a,
    input  wire [1:0]  ba,
    input  wire [3:0]  dqm,
    input  wire [31:0] dq_from_ctrl,
    input  wire        dq_oe,
    output reg  [31:0] dq_to_ctrl
);
    reg [31:0] mem [0:(1<<21)-1] /*verilator public_flat_rw*/;

    // счётчики команд — загрузка шины (стенд читает их сам)
    reg [31:0] n_act /*verilator public_flat_rd*/, n_rd /*verilator public_flat_rd*/,
               n_wr /*verilator public_flat_rd*/, n_ref /*verilator public_flat_rd*/;

    reg [10:0] bank_row [0:3];
    reg        wr_active;
    reg [3:0]  burst_i;
    reg [20:0] burst_base;
    // пакеты чтения — конвейером (READ каждые 8 тактов, у CL 3 — поверх двух последних слов):
    // до двух пакетов, номер фронта первого слова
    reg [31:0] ecnt;
    reg [20:0] rq_base [0:1];
    reg [31:0] rq_t0 [0:1];
    reg [1:0]  rq_n;

    initial begin
        wr_active = 1'b0; burst_i = 4'd0; ecnt = 32'd0; rq_n = 2'd0;
        n_act = 0; n_rd = 0; n_wr = 0; n_ref = 0;
        dq_to_ctrl = 32'd0;
    end

    function automatic [31:0] masked(input [31:0] old, input [31:0] d, input [3:0] m);
        for (int b = 0; b < 4; b++)
            masked[8*b +: 8] = m[b] ? old[8*b +: 8] : d[8*b +: 8];
    endfunction

    wire [2:0] cmd = {ras_n, cas_n, we_n};
    localparam [2:0] ACT = 3'b011, RD = 3'b101, WR = 3'b100, REF = 3'b001;

    always @(posedge clk) begin
        // продолжающийся пакет записи: слова 1…7
        if (wr_active) begin
            mem[burst_base + 21'(burst_i)] <= masked(mem[burst_base + 21'(burst_i)], dq_from_ctrl, dqm);
            burst_i = burst_i + 4'd1;
            if (burst_i == 4'd8) wr_active = 1'b0;
        end
        // пакеты чтения: слово, чей фронт настал
        ecnt = ecnt + 32'd1;
        if (rq_n != 2'd0 && ecnt >= rq_t0[0]) begin
            dq_to_ctrl <= mem[rq_base[0] + 21'(ecnt - rq_t0[0])];
            if (ecnt - rq_t0[0] == 32'd7) begin
                rq_base[0] = rq_base[1];
                rq_t0[0]   = rq_t0[1];
                rq_n       = rq_n - 2'd1;
            end
        end
        if (!cs_n) case (cmd)
            ACT: begin bank_row[ba] = a; n_act <= n_act + 1; end
            RD, WR: begin
                burst_base = {bank_row[ba], ba, a[7:0]};
                if (cmd == WR) begin
                    mem[{bank_row[ba], ba, a[7:0]}] <= masked(mem[{bank_row[ba], ba, a[7:0]}], dq_from_ctrl, dqm);
                    burst_i   = 4'd1;
                    wr_active = 1'b1;
                    n_wr <= n_wr + 1;
                end else begin
                    rq_base[rq_n[0]] = burst_base;
                    rq_t0[rq_n[0]]   = ecnt + 32'(CL - 1);
                    rq_n = rq_n + 2'd1;
                    n_rd <= n_rd + 1;
                end
            end
            REF: n_ref <= n_ref + 1;
            default: ;
        endcase
    end
endmodule
