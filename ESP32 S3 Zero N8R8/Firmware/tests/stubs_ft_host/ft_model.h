#pragma once
// 0.1.180: модель окна FT812 ядра Tang 1.29+ для стенда ft_host на ПК (tests/ft_host_host.cpp).
// Заменяет ESP-IDF (журнал, таймер, FreeRTOS) и LINK (tang::read/write/writeBurst/writeBurstDma):
// очередь записи окна на 512 слов уходит в память FT812 со своей скоростью, пока по LINK идут слова.
//
// Время — в «словах линии»: одно слово по LINK — 1 единица (~0,4 мкс), обмен — ещё накладные расходы.
// Очередь сливается drain слов за единицу. Слово в полную очередь теряется (FT_STAT 14), как у ядра.
#include <stdint.h>
#include <stdio.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
inline void vTaskDelay(TickType_t) {}

struct FtModel {
  // Память FT812 и окно
  std::map<uint32_t, uint32_t> mem;
  std::deque<uint32_t> queue;
  uint32_t queueAddress = 0;     // куда ляжет первое слово очереди
  uint32_t readAddress = 0;      // FT_DATA при чтении
  bool lost = false;             // FT_STAT 14
  // Скорость
  double drain = 1.0;            // слов очереди в память за единицу времени
  double credit = 0;
  uint64_t now = 0;
  static constexpr unsigned kExchange = 12;   // обмен по LINK: CS, команда, адрес, запуск (~5 мкс)
  static constexpr unsigned kDmaStart = 3;    // транзакция DMA: дескриптор и пуск
  // DMA и учёт
  bool dmaPrepared = false;
  int failDmaCall = -1;          // номер вызова writeBurstDma, который «не кончится за срок»
  int dmaCalls = 0;
  int dmaTransactions = 0;
  uint64_t dmaWords = 0;
  int burstCalls = 0;
  int longBurstsWithDma = 0;     // writeBurst длиннее 16 слов при взятых буферах DMA
  int overflows = 0;             // слово пришло в полную очередь
  int overCredit = 0;            // пачку послали длиннее свободных мест на её начало
  std::vector<std::string> errors;

  void advance(unsigned units) {
    for (unsigned i = 0; i < units; ++i) {
      ++now;
      credit += drain;
      while (credit >= 1.0 && !queue.empty()) {
        mem[queueAddress] = queue.front();
        queue.pop_front();
        queueAddress += 4;
        credit -= 1.0;
      }
      if (queue.empty()) credit = 0;   // запаса впрок у пустой очереди нет
    }
  }
  void push(uint32_t word) {
    if (queue.size() >= 512) {
      lost = true;
      ++overflows;
    } else {
      queue.push_back(word);
    }
    advance(1);
  }
  uint32_t freeWords() const { return 512U - static_cast<uint32_t>(queue.size()); }
};

inline FtModel g_ft;

inline int64_t esp_timer_get_time() { return static_cast<int64_t>(g_ft.now * 2 / 5); }   // 0,4 мкс
