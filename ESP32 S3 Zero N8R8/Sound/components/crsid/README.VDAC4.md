# cRSID 1.58 для VDAC4

Эмулятор C64 и SID (6510, CIA, VIC, SID 6581/8580, до 4 SID) — библиотека `libcRSID` из cRSID 1.58,
автор Hermit (Mihaly Horvath), релиз 15.04.2026: <https://csdb.dk/release/?id=261057>,
архив `cRSID-1.58-with-source-code.tar.gz` (SHA-256
`98bc2da927b65ab8781b46dc5341b1113967b0b61aa05406877c0e5dcffa8b9d`; копия — `E:\zx\cRSID`).

Лицензия автора (`README.txt`): «License is still WTF, you can use the code in any way you like, but
I'd like to be mentioned as the original author.» Автор — Hermit (Mihaly Horvath).

Взято без изменений, кроме одного места: `libcRSID.c`, `libcRSID.h`, `Config.h`, `Optimize.h`,
`C64/*`, `host/file.c`, `host/audio.c`, `host/md5.c`, `host/host.h`. Правка (04.11.2026): в
`C64/CPU.c`, в `wr()` и `wr2()` перед записью процессора — `CRSID_SID_WRITE_HOOK(address)`, если он
определён (без него cRSID тот же): SID.ELF догоняет по нему огибающие ADSR пакетом; `README.txt` — библиотеки, `README.cRSID.txt` — всего
cRSID. SDL, графика, утилиты и музыка из архива не нужны. Собирает `Sound/modules/sid` (SID.ELF):
`main/sid_source.c` включает `libcRSID.c` одним файлом, без `CRSID_PLATFORM_PC`.
