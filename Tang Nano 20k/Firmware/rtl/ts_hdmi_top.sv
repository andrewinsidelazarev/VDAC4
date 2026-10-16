// SPDX-License-Identifier: Apache-2.0
//
// Базовая прошивка Tang Nano 20K для carrier A3.1: видео TS-Config → HDMI.
//
// НАЗНАЧЕНИЕ
//   Принять с ZX Evolution через TS-Port40 штатный видеопоток базового режима
//   TS-Config (M_SEL=0), пришедший на carrier двумя фазами по восемь линий,
//   и вывести его по HDMI с максимально исправленной развёрткой, какая достижима
//   без кадрового буфера.
//
// ЧТО ИМЕННО ИСПРАВЛЯЕТСЯ
//   Вход (расчёт из HDL TS-Config, docs/tsconfig-tang20k-consolidated-notes.md, 10.1):
//     PCLK 28 МГц, строка 896 отсчётов → 31,25 кГц, кадр 640 строк → 48,828125 Гц.
//   Выход:
//     пиксельный клок 28 МГц, взятый из того же PCLK (PLL ×5 и CLKDIV ÷5);
//     строка 896 = 64 синхро + 68 задний + 720 актив + 44 передний;
//     кадр  640 = 5 синхро + 39 задний + 576 актив + 20 передний;
//     импульсы сгенерированы заново, полярность отрицательная — как в 576p50.
//   Итог: строчная частота ровно 31,25 кГц (совпадает с 576p50), активное окно
//   ровно 720×576, длительности импульсов и полок стандартные, дрожание входных
//   синхроимпульсов на выход не проходит.
//
// ЧЕГО ЭТА ВЕРСИЯ НЕ ДЕЛАЕТ
//   Кадровая частота остаётся входной — 48,83 Гц вместо 50,00 Гц. Поднять её можно
//   только кадровым буфером в SDRAM с повтором кадров; здесь его намеренно нет.
//   Также нет: масштабирования, деинтерлейса, режима M_SEL=1 (выводится чёрное поле),
//   звука по HDMI (поток DVI-совместимый, без пакетов).
//
// ПОЧЕМУ ГЕНЛОК, А НЕ СОБСТВЕННЫЙ КВАРЦ
//   Весь тракт тактируется входным PCLK, поэтому выходной кадр по длительности
//   строго равен входному: строчный буфер на две строки не переполняется и не
//   опустошается, подстройка не нужна. Кварц 27 МГц Tang не используется.
//   При пропадании PCLK обе PLL теряют захват и выход гаснет — ожидаемое поведение.
//
// ЧТО ОБЯЗАТЕЛЬНО ПРОВЕРИТЬ НА ЖЕЛЕЗЕ
//   • SAMPLE_DELAY: запас setup/hold на выходе мультиплексоров не измерен;
//   • H_IN_OFFSET / V_IN_OFFSET: положение активного окна входа взято расчётным;
//   • HS_ACTIVE_LOW / VS_ACTIVE_LOW: полярность синхроимпульсов Evo не измерена;
//   • примет ли конкретный монитор кадровую 48,83 Гц.
//
module ts_hdmi_top #(
    parameter int H_IN_OFFSET   = 131,
    parameter int V_IN_OFFSET   = 45,
    parameter int SAMPLE_DELAY  = 1,
    parameter bit HS_ACTIVE_LOW = 1'b1,
    parameter bit VS_ACTIVE_LOW = 1'b1,
    parameter bit SEL_PHASE     = 1'b1
) (
    // --- Видео с carrier ------------------------------------------------------
    input  wire        pclk_in,     // FPGA80 (GCLKT_0): PCLK Evo, ~28 МГц
    input  wire [7:0]  mux_d,       // FPGA27/28/29/30/41/42/48/54
    input  wire        hsync_in,    // FPGA15
    input  wire        vsync_in,    // FPGA16
    input  wire        m_sel,       // FPGA73: 0 — видео TS-Config, 1 — режим FT812
    output wire        mux_sel,     // FPGA56 → вывод 1 U1/U2

    // --- Обратные линии: в этой версии неактивны ------------------------------
    output wire        ft_int_n,    // FPGA74 → U11 (аппаратно разрешён только при M_SEL=1)
    output wire        link_irq_n,  // FPGA85 → ESP

    // --- Бортовое ---------------------------------------------------------------
    // Кнопки Tang не используются: их активный уровень на модуле не закладывается,
    // сброс — только по захвату PLL.
    output wire        pa_en,       // FPGA51: усилитель Tang выключен, его выводы I2S
                                    // заняты линиями MUX_D7 и MUX_SEL

    // --- HDMI --------------------------------------------------------------------
    output wire        tmds_clk_p,
    output wire        tmds_clk_n,
    output wire [2:0]  tmds_data_p,
    output wire [2:0]  tmds_data_n
);

    // ======================================================================
    // Тактирование: всё от входного PCLK
    //   rPLL №1: ×5 = 140 МГц (последовательный клок OSER10), CLKDIV ÷5 → 28 МГц пикселей;
    //   rPLL №2: ×2 =  56 МГц (домен приёма, два такта на пиксель).
    // ======================================================================
    wire serial_clk, pixel_clk, clk_cap;
    wire pll_tmds_lock, pll_cap_lock;
    wire pll_reset = 1'b0;

    ts_pll_tmds pll_tmds (
        .clkin(pclk_in), .reset(pll_reset), .clkout(serial_clk), .lock(pll_tmds_lock)
    );
    ts_pll_capture pll_capture (
        .clkin(pclk_in), .reset(pll_reset), .phase(4'd0), .clkout(clk_cap), .lock(pll_cap_lock)
    );
    ts_clkdiv5 clkdiv (
        .hclkin(serial_clk), .resetn(pll_tmds_lock), .clkout(pixel_clk)
    );

    // Общий сброс снимается, когда обе PLL захватили частоту.
    wire locked = pll_tmds_lock & pll_cap_lock;

    reg [3:0] rst_cap_sr, rst_pix_sr;
    always_ff @(posedge clk_cap or negedge locked)
        if (!locked) rst_cap_sr <= '0; else rst_cap_sr <= {rst_cap_sr[2:0], 1'b1};
    always_ff @(posedge pixel_clk or negedge locked)
        if (!locked) rst_pix_sr <= '0; else rst_pix_sr <= {rst_pix_sr[2:0], 1'b1};

    wire rst_cap_n = rst_cap_sr[3];
    wire rst_pix_n = rst_pix_sr[3];

    // ======================================================================
    // Режим порта: при M_SEL=1 на линиях RGB не видео, выводим чёрное поле,
    // но синхроимпульсы продолжаем гнать, чтобы монитор не терял захват.
    // ======================================================================
    reg m_sel_meta, m_sel_sync;
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            m_sel_meta <= 1'b0;
            m_sel_sync <= 1'b0;
        end else begin
            m_sel_meta <= m_sel;
            m_sel_sync <= m_sel_meta;
        end
    end

    // ======================================================================
    // Видеотракт
    // ======================================================================
    wire [7:0] vid_r, vid_g, vid_b;
    wire       vid_de, vid_hs, vid_vs;

    ts_video_chain #(
        .H_IN_OFFSET(H_IN_OFFSET), .V_IN_OFFSET(V_IN_OFFSET),
        .SAMPLE_DELAY(SAMPLE_DELAY),
        .HS_ACTIVE_LOW(HS_ACTIVE_LOW), .VS_ACTIVE_LOW(VS_ACTIVE_LOW),
        .SEL_PHASE(SEL_PHASE)
    ) chain (
        .clk_cap(clk_cap), .rst_cap_n(rst_cap_n),
        .pixel_clk(pixel_clk), .rst_pix_n(rst_pix_n),
        .mux_d(mux_d), .hsync_in(hsync_in), .vsync_in(vsync_in),
        .blank_video(m_sel_sync), .mux_sel(mux_sel),
        .out_r(vid_r), .out_g(vid_g), .out_b(vid_b),
        .out_de(vid_de), .out_hs(vid_hs), .out_vs(vid_vs)
    );

    // ======================================================================
    // DVI-совместимый поток TMDS: синхроимпульсы едут в управляющих битах синего канала.
    // ======================================================================
    wire [9:0] tmds_blue, tmds_green, tmds_red;

    tmds_encoder enc_blue (
        .pixel_clk(pixel_clk), .reset_n(rst_pix_n), .video_data(vid_b),
        .control_data({vid_vs, vid_hs}), .data_enable(vid_de), .tmds(tmds_blue)
    );
    tmds_encoder enc_green (
        .pixel_clk(pixel_clk), .reset_n(rst_pix_n), .video_data(vid_g),
        .control_data(2'b00), .data_enable(vid_de), .tmds(tmds_green)
    );
    tmds_encoder enc_red (
        .pixel_clk(pixel_clk), .reset_n(rst_pix_n), .video_data(vid_r),
        .control_data(2'b00), .data_enable(vid_de), .tmds(tmds_red)
    );

    tmds_serializer_gowin serializer (
        .pixel_clk(pixel_clk), .serial_clk(serial_clk), .reset(~rst_pix_n),
        .channel0(tmds_blue), .channel1(tmds_green), .channel2(tmds_red),
        .tmds_clk_p(tmds_clk_p), .tmds_clk_n(tmds_clk_n),
        .tmds_data_p(tmds_data_p), .tmds_data_n(tmds_data_n)
    );

    // ======================================================================
    // Безопасные уровни прочих линий carrier.
    //   FT_INT_N и LINK_IRQ_N активны низким — держим высокими (неактивны).
    //   Усилитель звука Tang выключен: его выводы I2S заняты под MUX_D7 и MUX_SEL.
    //   Остальные линии carrier (SPI, LINK_IO, MISO) в этой прошивке не объявлены
    //   и остаются входами — на них действуют штатные нагрузки платы Tang.
    // ======================================================================
    assign ft_int_n   = 1'b1;
    assign link_irq_n = 1'b1;
    assign pa_en      = 1'b0;

endmodule
