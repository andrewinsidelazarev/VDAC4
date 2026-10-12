# stb_vorbis для OGG.ELF

Декодер Ogg Vorbis [stb_vorbis](https://github.com/nothings/stb) v1.22 (Sean
Barrett, public domain / MIT) — файл `stb_vorbis.c` без правок (SHA-256
начинается с `4c7cb2ff1f7011e9`, взят 2026-10-05).

Настройки модуля — `stb_vorbis_vdac4.h`: только pushdata (поток кусками), без
stdio и целочисленного вывода, до 8 каналов. Ограничения stb_vorbis: нет floor 0
(файлы до 2004 года), цепочки потоков (новая песня интернет-радио Ogg — новый
поток) он сам не разбирает — это делает OGG.ELF: на странице BOS открывает
декодер заново.

Память: вся — из буфера, который даёт модуль (`stb_vorbis_alloc`, PSRAM), без
malloc и alloca. Собирается с `-O2 -DNDEBUG` (`CMakeLists.txt`); на ПК — в стенде
`tests/test_player_host.py`.
