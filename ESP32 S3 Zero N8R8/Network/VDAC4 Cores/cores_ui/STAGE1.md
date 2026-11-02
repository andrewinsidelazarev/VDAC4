# VDAC4 Cores — принятый дизайн с подписью Manage

Пользователь принял этап 1: «Да, принимаю». Единственная визуальная правка —
`Manage - <клавиша>` вместо `Own ESP layer`. Этапы 2 и 3 выполнены, результаты — в README.
В этапе 3 карточки NES/NEXT/AMIGA читаются из ELF самих ядер; дизайн сохранён.

Открыть **[галерею всех состояний](previews/index.html)** или
[обзорный лист](previews/00_overview.png).
Каждый отдельный PNG — 1280×720, точное удвоение холста RGB565 640×360.

| Кадры | Что смотреть |
|---|---|
| [01 — NES](previews/01_menu_nes.png) | Основной экран; реальные три ядра по 886 KB, Manage - F1 |
| [02 — NEXT](previews/02_menu_next.png) | Корпус ZX Spectrum Next, Manage - F1 |
| [03 — AMIGA](previews/03_menu_amiga.png) | Корпус A1200 для NanoMig AGA, Manage - F11 |
| [04 — много ядер](previews/04_many_unknown_shared.png) | 12 записей; неизвестное имя, общий слой ESP, клавиатура недоступна |
| [05 — C64](previews/05_many_future_c64.png) | Нет карточки: общий FPGA core, C64.BIN; Manage скрыт |
| [06 — одно ядро](previews/06_single_core.png) | Единственная запись, Manage - F1 |
| [07 — ESP](previews/07_connecting.png), [08 — SD](previews/08_reading_sd.png) | Подключение и чтение карты Tang |
| [09 — слой ESP](previews/09_starting_esp.png) | Подготовка запуска |
| [10 — 45 %](previews/10_cache_45_percent.png) | Копирование в кэш BL616 с известным процентом |
| [11 — без процентов](previews/11_loading_unknown.png), [12 — ПЛИС](previews/12_programming_fpga.png) | Ожидание без выдуманной шкалы |
| [13 — нет ядер](previews/13_error_no_cores.png) | Папка /CORES пуста |
| [14 — ответ ESP](previews/14_error_esp_text.png) | Длинный текст ошибки с переносом |
| [15 — ядро не запустилось](previews/15_error_core.png) | Ошибка запуска |
| [16 — обновление NES.ELF](previews/16_error_update_nes.png) | Точное требование версии 0.7+ |
| [17 — нет SD](previews/17_error_no_sd.png), [18 — слой ESP](previews/18_error_esp_layer.png) | Карта отсутствует / слой не стартовал |
| [19 — 180 с](previews/19_error_timeout.png), [20 — связь потеряна](previews/20_error_esp_lost.png) | Тайм-аут и потеря ответа ESP |
| [21 — нет ESP](previews/21_error_no_esp.png), [22 — ESP не отвечает](previews/22_error_no_answer.png) | Ошибки подключения |
| [23 — alpha 4](previews/23_fade_alpha_4.png), [24 — alpha 8](previews/24_fade_alpha_8.png), [25 — alpha 12](previews/25_fade_alpha_12.png) | Появление поверх WC по формуле Tang |
| [26 — фон WC](previews/26_wc_background.png) | Разрешённый макет из плеера; не снимок платы |
| [27 — без карточки](previews/27_no_card_no_manage.png) | CUSTOM_CORE, общий вид, Manage скрыт |

**[Анимация APNG](previews/motion_25fps.apng)** — 4 секунды, 100 кадров по 40 мс:
NES → NEXT → AMIGA → NEXT → NES, затем запуск и загрузка.
Если просмотрщик показывает только первый кадр, открыть `previews/index.html` в браузере.

Композиция, рисунки и анимация сохранены. 121 кадр совпадает с этапом 2
полностью, включая всю анимацию и 01–04/06. Смысловые изменения: 05 больше
не использует встроенные метаданные C64; 27 показывает отсутствие карточки.
Формат и создание новых карточек — в [CARD.md](CARD.md). Управление экраном выбора остаётся
↑/↓, Enter, Esc; во время загрузки — F12. Manage сообщает клавишу управления
запущенным ядром. Проверки, размеры и команда запуска — в [README](README.md).
