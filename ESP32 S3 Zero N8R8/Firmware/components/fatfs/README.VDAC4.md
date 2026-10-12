# fatfs — копия компонента ESP-IDF 6.0.2 с exFAT

Компонент `fatfs` из ESP-IDF v6.0.2 (`components/fatfs`, FatFs R0.15 с исправлениями
Espressif), без тестов. ESP-IDF берёт компонент проекта вместо своего.

Отличия только в `src/ffconf.h`: `FF_FS_EXFAT` равен 1, а булевы настройки
`FF_USE_FASTSEEK`, `FF_USE_LABEL`, `FF_USE_DYN_BUFFER` равны 0 и при выключенном пункте
menuconfig (код exFAT берёт их в выражениях, а невыставленный пункт в `sdkconfig.h` не
определён). В ESP-IDF он жёстко 0, пункта в
menuconfig для него нет, а флешки в разъёме USB модуля платы VDAC4 бывают
отформатированы в exFAT (кэш ресурсов музыки, `src/usb_drive.cpp`). Длинные имена,
которые exFAT требует, включены настройкой `CONFIG_FATFS_LFN_HEAP`.

При переходе на другую версию ESP-IDF копию нужно взять из неё заново и повторить
эти правки.

Лицензии: FatFs — лицензия ChaN (текст в начале `src/ff.c`), остальное — Apache-2.0
(Espressif).
