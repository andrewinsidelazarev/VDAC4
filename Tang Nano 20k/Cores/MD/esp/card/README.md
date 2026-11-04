# Карточка Mega Drive

Оригинальный пиксель-арт из геометрических примитивов Pillow. Палитра, подиум,
тень и направление проекции согласованы с NES, NEXT и AMIGA из
`cores_ui/tools/generate_art.py`. Надписей и логотипов в рисунке нет.

- `image.png`: 256 × 128, RGBA, alpha только 0/255.
- `icon.png`: 56 × 28, ближайший сосед, alpha только 0/255.
- `make_art.py`: воспроизводимый исходник обоих рисунков.
- `card.json`: метаданные; синий акцент `#70B8FF` (RGB565 `0x75DF`).
- `card.bin`: 3982 байта, формат VD4CARD v1, CRC32 `569c0fca`.

## Пересборка

PowerShell; запись только в каталог карточки и временный каталог на E:.
Python и Pillow уже установлены в указанном окружении.

```powershell
$card = 'E:\zx\Tang Nano 20k VDAC\VDAC4\Tang Nano 20k\Cores\MD\esp\card'
$tools = 'E:\zx\Tang Nano 20k VDAC\VDAC4\ESP32 S3 Zero N8R8\Network\VDAC4 Cores\cores_ui\tools'
$python = 'E:\zx\esp\pio-venv\Scripts\python.exe'
$env:TEMP = 'E:\zx\cache\tmp\codex_md_card'
$env:TMP = $env:TEMP
$env:TMPDIR = $env:TEMP
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:PYTHONUTF8 = '1'
& $python -B "$card\make_art.py"
& $python -B "$tools\make_card.py" "$card\card.json" "$card\icon.png" "$card\image.png" "$card\card.bin"
```

Существующий `MD/esp/tools/build.ps1` уже подхватывает `card.bin`.

## Проверка и превью

Все стенды, логи, тестовая копия `MD.ELF` и PNG находятся в
`E:\zx\cache\tmp\codex_md_card`:

- `menu_md.png`: настоящее меню 640 × 360, увеличенное строго вдвое;
  выбрана большая карточка Mega Drive.
- `menu_nes.png`: верхняя часть того же списка NES / NEXT / MD / AMIGA.
- `menus_all_four.png`: четыре состояния настоящего меню.
- `art_comparison.png`: рисунки и значки четырёх систем на общем тёмном фоне.
- `verification.json`: размеры, хеши и результаты проверок.
- `verify_preview.py`, `preview_md.cpp`: воспроизводимый стенд проверки и рендера.

Рендер использован без изменений. Он показывает три строки одновременно:
при выборе MD видны NEXT, MD и AMIGA; NES доступна прокруткой вверх.
Размер BIN в превью — демонстрационные 886 KiB из штатного стенда;
это не измерение файла прошивки MD.

Пройдены проверки размеров PNG, двоичной прозрачности, общей палитры,
побайтово воспроизводимой сборки, CRC32 и сохранения исходных секций ELF.
`check_module_symbols.py`: доступны все 15 внешних символов.
`check_elf_pointers.py`: проверено 349 адресов, подозрительных 0.
Штатный `elfImageProblem()` принимает тестовый ELF.
Настоящий `cui_card_read_elf()` нашёл карточку через адаптер с секторами
по 512 байт: 55 запросов, максимальный запрос 512 байт, пересечений границ нет.

Исходный ELF сохранён без изменений; тестовый ELF имеет размер 65840 байт.
69 прочитанных исходных файлов и эталонов совпали по SHA-256 до и после проверки.
На HDMI ничего не выводилось.
