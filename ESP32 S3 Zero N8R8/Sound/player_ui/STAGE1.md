# VDAC4 Media Player — картинки этапа 1

Дизайн утверждён пользователем. Этап 2 выполнен: [результаты в README](README.md#этап-2).
Все основные PNG — **1280×720**, точное увеличение RGB565
640×360 вдвое. Дополнение учтено: модель SDK ABI 1.5 и совместимый begin/end/render.

Быстрый просмотр: [галерея всех состояний](previews/index.html),
[обзорный лист](previews/00_overview.png),
[главный экран MP3](previews/01_mp3.png).

| Картинки | Что показано |
|---|---|
| [01 MP3](previews/01_mp3.png) | Обложка, все метки, спектр, 1:23 / 3:45, Track 3 of 10 |
| [02 Кириллица](previews/02_cyrillic.png) | Длинные название и исполнитель CP866, многоточие |
| [03 XM / 8](previews/03_xm_8_channels.png), [04 XM / 32](previews/04_xm_32_channels.png) | Каналы, Samples / Effects, нет обложки |
| [05 VGZ / OPL3](previews/05_vgz_opl3.png), [06 MIDI](previews/06_midi.png) | Формат микросхемы, Instruments / Tracks |
| [07 Радио](previews/07_radio_icy.png), [08 Станции](previews/08_radio_stations.png) | ICY, время без длительности, выделенная станция |
| [09 Загрузка 45%](previews/09_loading_45.png), [10 Buffering](previews/10_buffering.png) | Передача файла и набор очереди |
| [11 Пауза](previews/11_paused.png) | Затухший спектр и состояние PAUSED |
| [12 Пропуск файла](previews/12_error_skipping.png), [13 Обрыв потока](previews/13_error_stream_closed.png) | Оба требуемых текста ошибок |
| [14 Справка файлов](previews/14_help_files.png), [15 Справка радио](previews/15_help_radio.png) | Клавиши H, включая отличие радио и модулей |
| [16 Подробности](previews/16_details.png) | File, State, Format, Sent, Queue, Faults, Volume |
| [17 Нет файла](previews/17_idle.png), [18 Доигрывание](previews/18_finishing.png) | Оставшиеся состояния модели |
| [19 Температура 70°C](previews/19_temperature_70.png), [20 Температура 75°C](previews/20_temperature_75.png) | Предупреждение и тревога |
| [21 Без меток](previews/21_filename_fallback.png), [22 Без длительности](previews/22_unknown_duration.png) | Имя файла; время больше часа |
| [23 FLAC](previews/23_flac.png), [24 Connecting](previews/24_connecting.png), [25 Без анализатора](previews/25_view_none.png) | Дополнительные варианты формата и вида |
| [26 Alpha 4](previews/26_fade_alpha_4.png), [27 Alpha 8](previews/27_fade_alpha_8.png), [28 Alpha 12](previews/28_fade_alpha_12.png) | Появление поверх WC по формуле Tang |
| [29 Фон WC](previews/29_wc_background.png) | Нарисованный фон для трёх кадров появления |

[Анимация спектра — APNG](previews/spectrum_25fps.apng): 100 кадров × 40 мс = 4 с.
Она также показана в начале HTML-галереи. Спектр из локального PCM WAV 8 кГц;
доступный фрагмент 2,812375 с зациклен, выше 4 кГц полосы пустые. Звука в APNG нет.

Сделаны `player_ui.h/.cpp`, совместимый `zifi/player_view.hpp`, таблицы и генератор
собственного шрифта с MIT-лицензией, `pui_preview` (Python + исполняемый стенд C++),
README. Всего **29 полноразмерных PNG + обзорный лист + APNG**.

Внешний вид, собственный шрифт, обложка 192×192 и многоточие вместо бегущей строки
сохранены. На этапе 2 все 125 исходных RGB565-кадров повторены побайтно; эти
картинки не менялись. Проверки ПК, host-тест обвязки, Xtensa-сборка, размеры и
расчёт времени CPU приведены в README. Физическая плата не использовалась.
