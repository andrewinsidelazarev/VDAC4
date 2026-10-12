// Хост-обвязка src/spi_link_core.cpp для tests/test_spi_link_host.py.
//
// Читает команды со stdin построчно и отвечает одной строкой на каждую:
//   begin <от Z80> <к Z80>  -> "ok" либо "fail"; ёмкости очередей в байтах
//   request <hex 60 байт>   -> "<hex ответа>" либо "none"
//   write <hex>             -> "<принято байт>"
//   read <сколько>          -> "<hex>" либо "-"
//   state <число>           -> "<hex 4 байта>"; число — байт запуска
//   stats                   -> "turns repeats bad short from to pending space
//                               blocks badblocks gaveup puts badputs"
//   bulk <0/1/2/3>          -> "ok"; состояние DMA для следующих запросов:
//                              нет, свободен, блок не прочитан, блок вернулся
//   bulkdata                -> "<hex 512 байтов>"; блок, положенный ядром
//   putavail <0/1>          -> "ok"; можно ли открыть приём блока от Z80
//   putdata <hex>|-         -> "ok"; что «пришло» через DMA к следующим запросам
// и для индикатора (include/zifi/status_led.hpp):
//   led_reset               -> "ok"; индикатор как после включения
//   led <мс> <канал 0/1> <обработано> <отвергнуто> <чтений> <тактов SCK> <спадов CS>
//                           -> "<красный> <зелёный> <синий>"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "zifi/spi_link_core.hpp"
#include "zifi/status_led.hpp"

namespace {

std::vector<uint8_t> fromHex(const char* text) {
  std::vector<uint8_t> bytes;
  for (size_t index = 0; text[index] != 0 && text[index + 1] != 0; index += 2) {
    char pair[3] = {text[index], text[index + 1], 0};
    bytes.push_back(static_cast<uint8_t>(strtoul(pair, nullptr, 16)));
  }
  return bytes;
}

void printHex(const uint8_t* data, size_t length) {
  if (length == 0) {
    printf("-\n");
    return;
  }
  for (size_t index = 0; index < length; ++index) {
    printf("%02x", data[index]);
  }
  printf("\n");
}

}  // namespace

int main() {
  // Команда request несёт 120 шестнадцатеричных цифр, write — до 8192.
  static char line[20000];
  std::vector<uint8_t> fromStorage;
  std::vector<uint8_t> toStorage;
  zifi::SpiLinkCore* core = nullptr;
  zifi::StatusLedLogic led;
  zifi::SpiLinkCore::BulkState bulk = zifi::SpiLinkCore::kBulkOff;
  static uint8_t bulkBuffer[zifi::SpiLinkCore::kBulkSize];
  zifi::SpiLinkCore::PutInput put;
  std::vector<uint8_t> putData;

  while (fgets(line, sizeof(line), stdin) != nullptr) {
    line[strcspn(line, "\r\n")] = 0;
    char command[16] = {};
    const char* rest = "";
    if (sscanf(line, "%15s", command) < 1) {
      continue;
    }
    const char* space = strchr(line, ' ');
    if (space != nullptr) {
      rest = space + 1;
    }

    if (strcmp(command, "begin") == 0) {
      unsigned from = 0;
      unsigned to = 0;
      sscanf(rest, "%u %u", &from, &to);
      delete core;
      core = new zifi::SpiLinkCore();
      fromStorage.assign(from, 0);
      toStorage.assign(to, 0);
      const bool ok =
          core->begin(fromStorage.data(), from, toStorage.data(), to);
      printf(ok ? "ok\n" : "fail\n");
    } else if (strcmp(command, "led_reset") == 0) {
      led = zifi::StatusLedLogic();
      printf("ok\n");
    } else if (strcmp(command, "led") == 0) {
      unsigned now = 0;
      unsigned ready = 0;
      zifi::StatusLedLogic::Inputs inputs = {};
      unsigned values[5] = {};
      sscanf(rest, "%u %u %u %u %u %u %u", &now, &ready, &values[0], &values[1],
             &values[2], &values[3], &values[4]);
      inputs.linkReady = ready != 0;
      inputs.handled = values[0];
      inputs.rejected = values[1];
      inputs.reads = values[2];
      inputs.clocks = values[3];
      inputs.selects = values[4];
      const zifi::StatusLedLogic::Color color = led.update(now, inputs);
      printf("%u %u %u\n", static_cast<unsigned>(color.red),
             static_cast<unsigned>(color.green),
             static_cast<unsigned>(color.blue));
    } else if (core == nullptr) {
      printf("no core\n");
    } else if (strcmp(command, "request") == 0) {
      std::vector<uint8_t> request = fromHex(rest);
      request.resize(zifi::SpiLinkCore::kTurnSize, 0);
      uint8_t response[zifi::SpiLinkCore::kTurnSize] = {};
      size_t length = 0;
      if (core->handleRequest(request.data(), response, length, bulk,
                              bulkBuffer, put)) {
        printHex(response, length);
      } else {
        printf("none\n");
      }
    } else if (strcmp(command, "bulk") == 0) {
      bulk = static_cast<zifi::SpiLinkCore::BulkState>(atoi(rest));
      printf("ok\n");
    } else if (strcmp(command, "putavail") == 0) {
      put.available = atoi(rest) != 0;
      printf("ok\n");
    } else if (strcmp(command, "putdata") == 0) {
      if (strcmp(rest, "-") == 0) {
        putData.clear();
        put.received = nullptr;
        put.receivedLength = 0;
      } else {
        putData = fromHex(rest);
        put.received = putData.data();
        put.receivedLength = putData.size();
      }
      printf("ok\n");
    } else if (strcmp(command, "bulkdata") == 0) {
      printHex(bulkBuffer, sizeof(bulkBuffer));
    } else if (strcmp(command, "write") == 0) {
      const std::vector<uint8_t> data = fromHex(rest);
      printf("%u\n", static_cast<unsigned>(core->write(data.data(), data.size())));
    } else if (strcmp(command, "read") == 0) {
      const size_t wanted = static_cast<size_t>(strtoul(rest, nullptr, 10));
      std::vector<uint8_t> data(wanted == 0 ? 1 : wanted);
      const size_t count = core->read(data.data(), wanted);
      printHex(data.data(), count);
    } else if (strcmp(command, "state") == 0) {
      uint8_t word[zifi::SpiLinkCore::kStateSize] = {};
      core->stateWord(word, static_cast<uint8_t>(strtoul(rest, nullptr, 10)));
      printHex(word, sizeof(word));
    } else if (strcmp(command, "stats") == 0) {
      const zifi::SpiLinkCore::Stats stats = core->stats();
      printf("%u %u %u %u %u %u %u %u %u %u %u %u %u\n",
             static_cast<unsigned>(stats.turns),
             static_cast<unsigned>(stats.repeats),
             static_cast<unsigned>(stats.badRequests),
             static_cast<unsigned>(stats.shortAccepts),
             static_cast<unsigned>(stats.bytesFromZ80),
             static_cast<unsigned>(stats.bytesToZ80),
             static_cast<unsigned>(core->pendingToZ80()),
             static_cast<unsigned>(core->writeSpace()),
             static_cast<unsigned>(stats.bulkBlocks),
             static_cast<unsigned>(stats.bulkBad),
             static_cast<unsigned>(stats.bulkGaveUp),
             static_cast<unsigned>(stats.putBlocks),
             static_cast<unsigned>(stats.putBad));
    } else {
      printf("unknown\n");
    }
    fflush(stdout);
  }
  delete core;
  return 0;
}
