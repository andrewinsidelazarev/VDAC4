#pragma once

// Обновлятор WC (плагин WC Update, команды WCU_*) для vdac4_esp_host. Заголовок
// без Arduino и FreeRTOS: его включает код хоста, а заглушки стендов ПК живут
// только в wcu_glue.cpp.

#include <stddef.h>
#include <stdint.h>

namespace wcuhost {

struct Callbacks {
  // Кадр к Z80 целиком (с байтом выравнивания канала SPI), одним куском.
  bool (*writeToZ80)(const uint8_t* data, size_t length);
  // Принятое от Z80 для транспорта моста VFS: не дальше конца текущего кадра,
  // 0 — кадров нет. Зовётся только из главного потока (ядро 1).
  size_t (*readFromZ80)(uint8_t* output, size_t capacity);
  // Событие обновлятора (STATE, ENTRY) — кадр к Z80.
  bool (*sendEvent)(uint8_t command, const uint8_t* data, uint16_t length);
  // HTTPS GET тела целиком в buffer. false и error — нет ответа; иначе status —
  // код HTTP. cancel поднят — бросить приём.
  bool (*httpGet)(const char* host, const char* path, uint8_t* buffer,
                  size_t capacity, size_t& length, uint16_t& status,
                  char* error, size_t errorSize, const volatile bool* cancel);
  // Строка журнала хоста.
  void (*log)(const char* text);
};

// Мост VFS и транспорт: один раз при запуске хоста.
bool begin(const Callbacks& callbacks);

// Сеанс, как WcUpdateService прошивки. Зовутся из сетевого потока хоста (ядро 0).
bool start(const uint8_t* payload, uint16_t length, char* error, size_t errorSize);
bool apply(const uint8_t* payload, uint16_t length);
bool sync();
bool stop(uint32_t timeoutMs);
bool running();
// Попросить сеанс закончиться, не дожидаясь (перезапуск ESP по SYS_RESET).
void requestStop();

// Ядро 1: исполнить заявку сеанса к карте Evo (VfsBridge::pollCore1).
void pollCore1();

}  // namespace wcuhost
