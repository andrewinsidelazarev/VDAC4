"""Инициализация W9864G2GH без изменения рабочего арбитража SDRAM."""


def winbond_startup(text):
    marker = '// W9864G2GH: 200 us, DQM high, eight AUTO REFRESH; compact sequencer.'
    if marker in text:
        return text

    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError(f'Неизвестный шаблон инициализации SDRAM: {old!r}')
        text = text.replace(old, new, 1)

    once('localparam real SETTING_INHIBIT_DELAY_MICRO_SEC = 100;',
         marker + '\nlocalparam real SETTING_INHIBIT_DELAY_MICRO_SEC = 200;')
    last = 'localparam CLOCK_CYCLES_UNTIL_REFRESH2_END = CLOCK_CYCLES_UNTIL_REFRESH1_END + CLOCK_CYCLES_FOR_AUTOREFRESH;'
    once(last, '')
    once('CLOCK_CYCLES_UNTIL_INIT_DONE = CLOCK_CYCLES_UNTIL_REFRESH2_END + 2;',
         'CLOCK_CYCLES_UNTIL_INIT_DONE = CLOCK_CYCLES_UNTIL_REFRESH1_END + 2;')
    once('reg [INIT_COUNTER_WIDTH-1:0] init_counter = 0;',
         'reg [INIT_COUNTER_WIDTH-1:0] init_counter = 0;\nreg [2:0] init_refresh_count = 0;')
    once('init_counter <= 0;', 'init_counter <= 0;\n    init_refresh_count <= 0;')
    once('init_counter <= init_counter + 1;', '''init_counter <= init_counter + 1;
        // Повторяем один интервал tRFC: не нужны семь широких декодеров таймера.
        if (init_counter == CLOCK_CYCLES_UNTIL_REFRESH1_END[INIT_COUNTER_WIDTH-1:0] && init_refresh_count != 7) begin
          init_counter <= CLOCK_CYCLES_UNTIL_INIT_PRECHARGE_END[INIT_COUNTER_WIDTH-1:0] + 1'b1;
          init_refresh_count <= init_refresh_count + 1'b1;
        end''')
    # Декодирование адреса MRS и самой команды должно остаться согласованным.
    text = text.replace('init_counter == CLOCK_CYCLES_UNTIL_REFRESH2_END[INIT_COUNTER_WIDTH-1:0]',
                        'init_counter == CLOCK_CYCLES_UNTIL_REFRESH1_END[INIT_COUNTER_WIDTH-1:0] && init_refresh_count == 7')
    once('''end else if (init_counter == CLOCK_CYCLES_UNTIL_REFRESH1_END[INIT_COUNTER_WIDTH-1:0]) begin
        sd_cmd <= CMD_AUTO_REFRESH;''',
         '''end else if (init_counter == CLOCK_CYCLES_UNTIL_REFRESH1_END[INIT_COUNTER_WIDTH-1:0] && init_refresh_count != 7) begin
        sd_cmd <= CMD_AUTO_REFRESH;''')
    if text.count('sd_dqm <= 0;') != 2:
        raise ValueError('Ожидались два значения DQM по умолчанию (16/32 bit)')
    text = text.replace('sd_dqm <= 0;',
                        "sd_dqm <= state == STATE_INIT ? {DATA_WIDTH/8{1'b1}} : 0;")
    return text
