// SPDX-License-Identifier: Apache-2.0
//
// Модель microSD (SDHC) в режиме SPI для стендов моста fb_sdspi: режим 0 (байт — старшим битом
// вперёд, MOSI берётся по фронту SCK, MISO меняется по спаду), CS высокий — MISO = 1.
// Команды: CMD0 (нужен CRC 0x95), CMD8 (CRC 0x87, эхо шаблона), CMD55 + ACMD41 (готова со второго
// раза), CMD58 (OCR: SDHC, готова), CMD16, CMD17 (R1, NAC байтов 0xFF, маркер 0xFE, 512 байтов,
// CRC16; блок вне карты — маркер ошибки 0x08), CMD18 (R1, затем блоки подряд, как у CMD17, пока
// не придёт CMD12; за последним блоком карты — маркер ошибки 0x08), CMD12 (поток обрывается: байт
// мусора, NCR, R1 0x00 и BUSY_STOP байтов «занята»), CMD24 (ждёт маркер 0xFE, 512 байтов и CRC16 —
// ответ 0xE5 «принято», BUSY байтов 0x00 — занята; CRC не тот — 0xEB, блок не пишется).
// bad_crc_blk — у этого блока карта отдаёт неверный CRC16 (стенд проверки CRC в мосте).
// Прочие — R1 0x04 (недопустимая команда). Ответ — через один байт 0xFF после команды (NCR).
//
module sd_card_model #(
    parameter int BLOCKS = 16,
    parameter int NAC    = 40,           // байтов 0xFF до маркера данных
    parameter int BUSY   = 30,           // байтов «занята» после записи блока
    parameter int BUSY_STOP = 6          // байтов «занята» после CMD12
) (
    input  wire cs_n,
    input  wire sck,
    input  wire mosi,
    output reg  miso
);
    reg [7:0] mem [0:BLOCKS*512-1];
    integer   i;
    initial begin
        for (i = 0; i < BLOCKS * 512; i = i + 1) mem[i] = (i * 7 + (i >> 9) * 13 + 5) & 8'hFF;
        miso = 1'b1;
    end

    // очередь ответа
    reg [7:0] q [0:4095];
    integer   qh = 0, qt = 0;
    // счётчики команд (для стенда)
    integer   n_cmd = 0, n_read = 0, n_write = 0, n_bad = 0;
    integer   streaming = 0, sblk = 0;    // CMD18: идёт поток, следующий блок
    integer   bad_crc_blk = -1;
    // функции, а не задачи: их зовёт и функция pop
    function automatic void push(input [7:0] b);
        begin q[qt % 4096] = b; qt = qt + 1; end
    endfunction
    // блок с маркером (CMD17, CMD18): NAC байтов 0xFF, 0xFE, данные, CRC16; вне карты — 0x08
    function automatic void push_block(input integer blk);
        integer k;
        reg [15:0] c;
        begin
            for (k = 0; k < NAC; k = k + 1) push(8'hFF);
            if (blk >= BLOCKS) push(8'h08);
            else begin
                push(8'hFE);
                for (k = 0; k < 512; k = k + 1) push(mem[blk * 512 + k]);
                c = crc16(blk * 512);
                if (blk == bad_crc_blk) c = c ^ 16'h0100;
                push(c[15:8]); push(c[7:0]);
                n_read = n_read + 1;
            end
        end
    endfunction
    function automatic [7:0] pop;
        begin
            if (qh == qt && streaming != 0) begin
                // CMD18: следующий блок — когда прежний отдан
                push_block(sblk);
                if (sblk >= BLOCKS) streaming = 0;
                sblk = sblk + 1;
            end
            if (qh == qt) pop = 8'hFF;
            else begin pop = q[qh % 4096]; qh = qh + 1; end
        end
    endfunction

    function automatic [15:0] crc16(input integer base);
        integer k, b;
        reg [15:0] c;
        begin
            c = 16'd0;
            for (k = 0; k < 512; k = k + 1)
                for (b = 7; b >= 0; b = b - 1)
                    c = (c[15] ^ mem[base + k][b]) ? {c[14:0], 1'b0} ^ 16'h1021 : {c[14:0], 1'b0};
            crc16 = c;
        end
    endfunction


    // приём команд и данных
    reg [7:0]  cmd [0:5];
    integer    cn = 0;
    reg        idle = 1'b1, app = 1'b0;
    integer    acmd41 = 0;
    integer    wstate = 0, wn = 0, wblk = 0;   // 0 — команды, 1 — ждём маркер, 2 — данные
    reg [7:0]  wbuf [0:513];

    task automatic exec;
        reg [5:0]  idx;
        reg [31:0] arg;
        integer    k, base;
        reg [15:0] c;
        begin
            idx = cmd[0][5:0];
            arg = {cmd[1], cmd[2], cmd[3], cmd[4]};
            n_cmd = n_cmd + 1;
            if (idx == 6'd12) begin
                // стоп потока: то, что карта ещё не отдала, пропадает; первый байт — мусор
                streaming = 0;
                qh = qt;
                push(8'h3C);
            end
            push(8'hFF);                                     // NCR
            if (app && idx == 6'd41) begin
                acmd41 = acmd41 + 1;
                if (acmd41 >= 2) idle = 1'b0;
                push({7'd0, idle});
            end else case (idx)
                6'd0: begin
                    if (cmd[5] != 8'h95) begin push(8'h09); n_bad = n_bad + 1; end
                    else begin idle = 1'b1; acmd41 = 0; push(8'h01); end
                end
                6'd8: begin
                    if (cmd[5] != 8'h87) begin push(8'h09); n_bad = n_bad + 1; end
                    else begin push({7'd0, idle}); push(8'h00); push(8'h00); push({4'd0, arg[11:8]}); push(arg[7:0]); end
                end
                6'd55: push({7'd0, idle});
                6'd58: begin push({7'd0, idle}); push(8'hC0); push(8'hFF); push(8'h80); push(8'h00); end
                6'd16: push({7'd0, idle});
                6'd17: begin
                    push(8'h00);
                    push_block(arg);                              // вне карты — маркер ошибки
                end
                6'd18: begin
                    push(8'h00);
                    streaming = 1; sblk = arg;                    // блоки — в pop, по мере выдачи
                end
                6'd12: begin
                    push(8'h00);
                    for (k = 0; k < BUSY_STOP; k = k + 1) push(8'h00);
                end
                6'd24: begin
                    push(8'h00);
                    wstate = 1; wblk = arg;
                end
                default: push({5'd0, 1'b1, 1'b0, idle});    // 0x04 | idle
            endcase
            if (!(idx == 6'd55)) app = 1'b0; else app = 1'b1;
        end
    endtask

    task automatic got(input [7:0] b);
        integer k;
        reg [15:0] c, rc;
        begin
            if (wstate == 1) begin
                if (b == 8'hFE) begin wstate = 2; wn = 0; end
            end else if (wstate == 2) begin
                wbuf[wn] = b;
                wn = wn + 1;
                if (wn == 514) begin
                    wstate = 0;
                    // CRC16 принятого
                    c = 16'd0;
                    for (k = 0; k < 512; k = k + 1) begin : crc_b
                        integer bb;
                        for (bb = 7; bb >= 0; bb = bb - 1)
                            c = (c[15] ^ wbuf[k][bb]) ? {c[14:0], 1'b0} ^ 16'h1021 : {c[14:0], 1'b0};
                    end
                    rc = {wbuf[512], wbuf[513]};
                    if (rc != c || wblk >= BLOCKS) begin
                        push(8'hEB);                         // ошибка CRC
                        n_bad = n_bad + 1;
                    end else begin
                        for (k = 0; k < 512; k = k + 1) mem[wblk * 512 + k] = wbuf[k];
                        push(8'hE5);                         // принято
                        for (k = 0; k < BUSY; k = k + 1) push(8'h00);
                        n_write = n_write + 1;
                    end
                end
            end else if (cn == 0) begin
                if (b[7:6] == 2'b01) begin cmd[0] = b; cn = 1; end
            end else begin
                cmd[cn] = b;
                cn = cn + 1;
                if (cn == 6) begin cn = 0; exec(); end
            end
        end
    endtask

    // байты: приём по фронту, выдача по спаду
    reg [7:0] rsr, osr;
    integer   re = 0, fe = 0;
    always @(negedge cs_n) begin re = 0; fe = 0; osr = 8'hFF; miso = 1'b1; end
    always @(posedge cs_n) miso = 1'b1;
    always @(posedge sck) if (!cs_n) begin
        rsr = {rsr[6:0], mosi};
        re = re + 1;
        if (re % 8 == 0) got(rsr);
    end
    always @(negedge sck) if (!cs_n) begin
        fe = fe + 1;
        if (fe % 8 == 0) osr = pop();
        else osr = {osr[6:0], 1'b1};
        miso = osr[7];
    end
endmodule
