`timescale 1ns/1ps
//
// Поведенческая модель SDR SDRAM 64 Мбит × 32 (как встроенная в GW2AR-18):
// 4 банка × 2048 строк × 256 столбцов. Хранит данные и проверяет протокол:
// mode register (BL=8, CL), tRCD, tRP, tRC, tRAS, tWR, tRFC, интервал регенерации,
// доступ только к открытой строке, конфликт шины данных.
//
// Модель тактируется клоком микросхемы — как на плате, инверсией клока логики.
// Данные чтения — по паспорту: слово выставляется через T_AC после фронта CL−1 от READ
// и держится до T_OH после следующего фронта; между ними на шине 'x. Поэтому неверная
// точка выборки контроллера (rd_phase) даёт 'x или соседнее слово и ловится тестом.
//
module sdram_model #(
    parameter real TCK_NS  = 14.815,
    parameter int  CL      = 2,
    parameter real T_AC    = 6.0,      // доступ от фронта (CL=2, наихудший)
    parameter real T_OH    = 2.5,      // удержание после следующего фронта
    parameter real T_RCD   = 15.0,
    parameter real T_RP    = 15.0,
    parameter real T_RC    = 60.0,
    parameter real T_RAS   = 42.0,
    parameter real T_WR    = 12.0,
    parameter real T_RFC   = 60.0,
    parameter real T_REFI_MAX = 15_625.0   // 4096 регенераций за 64 мс
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
    reg [31:0] mem [0:(1<<21)-1];

    integer errors = 0;
    integer n_act = 0, n_rd = 0, n_wr = 0, n_ref = 0;
    real    max_refi = 0.0;

    // Состояние банков (времена в нс)
    reg        bank_open [0:3];
    reg [10:0] bank_row  [0:3];
    real       t_act     [0:3];
    real       t_pre     [0:3];          // когда банк закрыт (может быть в будущем при автопредзаряде)
    real       t_ref = -1.0e9;
    real       t_last_ref = -1.0;
    reg        mode_set = 1'b0;
    integer    mode_cl = 0;

    // Очередь пакета: одна операция за раз (контроллер их не перекрывает)
    reg        rd_active = 1'b0, wr_active = 1'b0;
    integer    burst_i = 0;
    reg [20:0] burst_base;
    integer    rd_wait = 0;

    task automatic fail(input string msg);
        begin
            if (errors < 20) $display("[SDRAM %0t] ОШИБКА: %0s", $time, msg);
            errors = errors + 1;
        end
    endtask

    integer i;
    initial begin
        for (i = 0; i < 4; i = i + 1) begin
            bank_open[i] = 1'b0;
            bank_row[i]  = '0;
            t_act[i]     = -1.0e9;
            t_pre[i]     = -1.0e9;
        end
        dq_to_ctrl = 'x;
    end

    wire [2:0] cmd = {ras_n, cas_n, we_n};
    localparam [2:0] NOP = 3'b111, ACT = 3'b011, RD = 3'b101, WR = 3'b100,
                     PRE = 3'b010, REF = 3'b001, MRS = 3'b000;

    real now;
    always @(posedge clk) begin
        now = $realtime;

        // ---- продолжающийся пакет записи: слова 1..7 ----
        if (wr_active) begin
            if (!dq_oe) fail("запись: контроллер не ведёт DQ во время пакета");
            mem[burst_base + burst_i] = dq_from_ctrl;
            burst_i = burst_i + 1;
            if (burst_i == 8) wr_active = 1'b0;
        end

        // ---- продолжающийся пакет чтения ----
        if (rd_active) begin
            if (dq_oe) fail("конфликт шины: контроллер ведёт DQ во время чтения");
            if (rd_wait > 0) begin
                rd_wait = rd_wait - 1;
            end
            if (rd_wait == 0) begin
                dq_to_ctrl <= #(T_OH) 'x;
                dq_to_ctrl <= #(T_AC) mem[burst_base + burst_i];
                burst_i = burst_i + 1;
                if (burst_i == 8) rd_active = 1'b0;
            end
        end else if (!wr_active) begin
            dq_to_ctrl <= #(T_OH) 'x;
        end

        if (!cs_n) case (cmd)
        NOP: ;
        MRS: begin
            if (a[2:0] != 3'b011) fail("mode register: длина пакета не 8");
            if (a[3]) fail("mode register: чередующийся пакет");
            mode_cl = a[6:4];
            if (mode_cl != CL) fail("mode register: CL не совпадает с моделью");
            mode_set = 1'b1;
        end
        ACT: begin
            if (!mode_set) fail("ACTIVE до записи mode register");
            if (bank_open[ba] && now < t_pre[ba]) fail("ACTIVE в открытый банк");
            if (now - t_pre[ba] < T_RP - 0.01) fail("нарушен tRP");
            if (now - t_act[ba] < T_RC - 0.01) fail("нарушен tRC");
            if (now - t_ref < T_RFC - 0.01) fail("нарушен tRFC");
            bank_open[ba] = 1'b1;
            bank_row[ba]  = a;
            t_act[ba]     = now;
            t_pre[ba]     = 1.0e18;
            n_act = n_act + 1;
        end
        RD, WR: begin
            if (!bank_open[ba] || now >= t_pre[ba]) fail("READ/WRITE в закрытый банк");
            if (now - t_act[ba] < T_RCD - 0.01) fail("нарушен tRCD");
            if (a[2:0] != 3'b000) fail("пакет не выровнен на 8 слов");
            if (rd_active || wr_active) fail("новый пакет поверх незаконченного");
            burst_base = {bank_row[ba], ba, a[7:0]};
            burst_i = 0;
            if (cmd == WR) begin
                if (!dq_oe) fail("WRITE без данных на DQ");
                if (dqm != 4'h0) fail("WRITE с маской (не ожидается)");
                mem[burst_base] = dq_from_ctrl;
                burst_i = 1;
                wr_active = 1'b1;
                n_wr = n_wr + 1;
                // автопредзаряд: после последнего слова + tWR
                if (a[10]) begin
                    t_pre[ba] = now + 7.0 * TCK_NS + T_WR;
                    if (t_pre[ba] - t_act[ba] < T_RAS) t_pre[ba] = t_act[ba] + T_RAS;
                    bank_open[ba] = 1'b0;
                end
            end else begin
                rd_active = 1'b1;
                rd_wait   = CL - 1;
                n_rd = n_rd + 1;
                if (a[10]) begin
                    t_pre[ba] = now + 8.0 * TCK_NS;
                    if (t_pre[ba] - t_act[ba] < T_RAS) t_pre[ba] = t_act[ba] + T_RAS;
                    bank_open[ba] = 1'b0;
                end
            end
            if (!a[10]) fail("ожидался автопредзаряд (A10=1)");
        end
        PRE: begin
            if (a[10]) begin
                for (i = 0; i < 4; i = i + 1) begin
                    if (bank_open[i] && now - t_act[i] < T_RAS) fail("нарушен tRAS");
                    if (bank_open[i]) t_pre[i] = now;
                    bank_open[i] = 1'b0;
                end
            end else begin
                if (bank_open[ba]) t_pre[ba] = now;
                bank_open[ba] = 1'b0;
            end
        end
        REF: begin
            for (i = 0; i < 4; i = i + 1) begin
                if (bank_open[i] && now < t_pre[i]) fail("REFRESH при открытом банке");
                if (now - t_pre[i] < T_RP - 0.01) fail("REFRESH раньше tRP");
            end
            if (now - t_ref < T_RFC - 0.01) fail("REFRESH раньше tRFC");
            if (mode_set && t_last_ref >= 0.0) begin
                if (now - t_last_ref > max_refi) max_refi = now - t_last_ref;
                if (now - t_last_ref > T_REFI_MAX) fail("слишком большой интервал регенерации");
            end
            if (mode_set) t_last_ref = now;
            t_ref = now;
            n_ref = n_ref + 1;
        end
        default: fail("неизвестная команда");
        endcase
    end

    // Регенерация не должна пропадать надолго и во время обычной работы
    always @(posedge clk) begin
        if (mode_set && t_last_ref >= 0.0 && ($realtime - t_last_ref) > T_REFI_MAX + 100.0) begin
            fail("регенерация пропала");
            t_last_ref = $realtime;    // сообщить один раз на эпизод
        end
    end
endmodule
