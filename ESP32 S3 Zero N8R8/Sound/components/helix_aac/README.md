# Helix AAC для AAC.ELF

Декодер HE-AAC с фиксированной точкой RealNetworks (Helix DNA, 2005): AAC-LC
MPEG-2/MPEG-4, SBR (HE-AAC v1), потоки ADTS, ADIF и сырые блоки. Параметрического
стерео (HE-AAC v2) нет — такой поток звучит моно; нет Main/SSR/LTP. Подробно —
`readme_helix.txt` (оригинальный readme).

Источник — `src/libhelix-aac` из
[pschatzmann/arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix),
коммит `a84de91d34f279cf4d32e44bbc884055ba84c092` (2026-10-05). Лицензия файлов
декодера — RealNetworks Public Source License (RPSL) или RealNetworks Community
Source License (RCSL), см. шапки файлов; обёртки arduino-libhelix (C++, GPL-3) не
берутся.

Правки порта (больше ничего не менялось):

- `aaccommon.h`: вместо `ConfigHelix.h` и `utils/helix_pgm.h` — `helix_aac_port.h`
  (таблицы — обычные константы, `helix_malloc`/`helix_free` — функции модуля,
  `AAC_ENABLE_SBR`);
- `buffers.c`: без `hlxclib/stdlib.h` и `utils/helix_memory.h`;
- `sbr.c`: состояние SBR — через `helix_malloc`/`helix_free`, без `printf`.

Собирается с `-O2 -DNDEBUG` (`CMakeLists.txt`); на ПК — в стенде
`tests/test_player_host.py`.
