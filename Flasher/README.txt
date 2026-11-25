Первая прошивка VDAC4
=====================

Программа для Windows 10/11 записывает прошивки платы VDAC4 по USB: модуль ESP32-S3 Zero, BL616
модуля Tang Nano 20K и flash ПЛИС Tang. Ставить ничего не нужно: ни Python, ни ESP-IDF, ни Gowin.

Прошивки
--------
При запуске программа сама скачивает свежие прошивки с GitHub (репозиторий andrewinsidelazarev/VDAC4)
в папку firmware рядом с собой и сверяет каждый файл с git (хэш того же коммита). Нет интернета — можно прошить тем, что уже
лежит в папке firmware (окно скажет, откуда эти файлы), или нажать «Скачать снова».

Бэкап
-----
Перед записью каждый шаг читает с устройства всё, что сотрёт, и кладёт в папку Backup рядом с
программой (папка создаётся сама). Имена файлов — с датой и временем, что где — в Backup\backup.txt:
* ESP32-S3 — вся память 8 МБ (прошивка, настройки, файлы); чтение — около полутора минут;
* BL616 — вся память 4 МБ (FPGA Partner, прошивка VDAC4, кэш ядер);
* ПЛИС Tang — отмеченные слоты flash, по 1 МБ: 0 TS-Config, 1 FT812, 3 видео;
* шрифты FT812 — слот 2 flash Tang, 1 МБ.
Только когда бэкап записан и сверен, шаг стирает устройство: ESP32-S3 — целиком, BL616 — всё, кроме
FPGA Partner (прежнюю прошивку, кэш ядер и любые чужие данные), ПЛИС — отмеченные слоты. Затем пишет
новую прошивку и сверяет её. После BL616 программа проверяет, что кэш ядер чистый: ядра лягут туда сами
при первом запуске каждого, ничего форматировать не нужно.

Перед началом
-------------
* Выньте плату VDAC4 из ZX Evolution (или выключите Evo). Питание Evo и USB одновременно не включать.
* Нужен USB-кабель с данными (зарядный без данных не подойдёт).
* Для шага ПЛИС Windows ставит драйвер FTDI сама при первом подключении Tang (нужен интернет). Без
  интернета — установите «CDM driver» с ftdichip.com.

Шаги
----
1. ESP32-S3 Zero: подключить USB к модулю, «Прошить». Не входит в режим прошивки — держать кнопку
   BOOT на модуле при подключении кабеля.
2. BL616: держать кнопку UPDATE на Tang Nano 20K, подключить USB к Tang, отпустить, «Прошить».
3. ПЛИС Tang: переподключить USB к Tang без кнопки, «Прошить» (бэкап, запись и сверка чтением).
4. Шрифты FT812: USB к Tang так же, «Прошить». Без шрифтов ядро FT812 не рисует встроенный текст программ
   для FT812. На новой плате их нет; если на плате они уже есть, шаг можно пропустить.

Каждый шаг можно пропустить или повторить. Журнал — в VDAC4Flasher.log рядом с программой.

VDAC4 first flashing
====================

A Windows 10/11 program that writes the VDAC4 board firmware over USB: the ESP32-S3 Zero module, the
BL616 of the Tang Nano 20K and the Tang FPGA flash. Nothing to install (no Python, ESP-IDF or Gowin).
At start it downloads the current firmware from GitHub (andrewinsidelazarev/VDAC4) into the firmware
folder and checks every file against git (its hash in the same commit). Before writing, each step saves what it will erase into the Backup
folder next to the program (see Backup\backup.txt), then erases (the whole ESP32-S3 flash, the BL616
firmware partition, the checked FPGA slots), writes and verifies. Step 4 writes the FT812 ROM fonts into
slot 2 of the Tang flash (without them the FT812 core draws no built-in text); skip it if the board has them.
Take the board out of the ZX Evolution (or switch the Evo off) first: never run Evo power and USB together.
