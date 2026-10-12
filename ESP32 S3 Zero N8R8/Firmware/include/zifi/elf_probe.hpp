#pragma once

namespace zifi {

// Проверка загрузчика модулей ESP (components/elf_loader) на плате: вшитый
// пробный модуль modules/hello загружается в PSRAM, связывается и выполняется
// один раз при старте; итог — запись «zifi-elf: probe …» в журнале наладки.
// Только сборка VDAC4 с ZIFI_ELF_PROBE; иначе вызов ничего не делает.
void runElfProbe();

}  // namespace zifi
