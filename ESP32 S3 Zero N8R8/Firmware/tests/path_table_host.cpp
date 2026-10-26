// Хост-обвязка src/path_table.cpp для tests/test_path_table.py.
//
// Читает команды со stdin построчно и отвечает на каждую:
//   text               -> следующие строки до «.» — текст path.cfg, разобрать; ответ "<число записей>"
//   set NAME SER CLU   -> "ok" либо "bad"
//   get NAME SER       -> "<кластер hex>" либо "none"
//   dump               -> текст format(), затем строка "."
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "zifi/path_table.hpp"

int main() {
  static zifi::PathTable table;
  static char out[zifi::PathTable::kMaxText];
  char line[512];
  while (fgets(line, sizeof(line), stdin) != nullptr) {
    line[strcspn(line, "\r\n")] = 0;
    char command[16] = {};
    char name[64] = {};
    unsigned long serial = 0;
    unsigned long cluster = 0;
    if (sscanf(line, "%15s", command) < 1) {
      continue;
    }
    if (strcmp(command, "text") == 0) {
      std::string text;
      while (fgets(line, sizeof(line), stdin) != nullptr && strcmp(line, ".\n") != 0 &&
             strcmp(line, ".\r\n") != 0) {
        text += line;
      }
      table.parse(text.data(), text.size());
      printf("%u\n", static_cast<unsigned>(table.count()));
    } else if (strcmp(command, "set") == 0) {
      sscanf(line, "%*s %63s %lx %lx", name, &serial, &cluster);
      printf("%s\n", table.set(name, static_cast<uint32_t>(serial), static_cast<uint32_t>(cluster))
                         ? "ok"
                         : "bad");
    } else if (strcmp(command, "get") == 0) {
      sscanf(line, "%*s %63s %lx", name, &serial);
      uint32_t found = 0;
      if (table.find(name, static_cast<uint32_t>(serial), found)) {
        printf("%08X\n", static_cast<unsigned>(found));
      } else {
        printf("none\n");
      }
    } else if (strcmp(command, "dump") == 0) {
      const size_t length = table.format(out, sizeof(out));
      fwrite(out, 1, length, stdout);
      printf(".\n");
    }
    fflush(stdout);
  }
  return 0;
}
