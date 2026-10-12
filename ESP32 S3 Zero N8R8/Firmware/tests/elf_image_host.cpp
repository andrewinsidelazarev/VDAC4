// Проверка образа ELF модуля (src/elf_image.cpp) на ПК.
// Вызов: elf_image_host <ELF>... — настоящие модули обязаны пройти; затем каждый
// портится (обрезка, поля заголовка и секций, релокации, случайные байты), и
// проверка обязана не выйти за образ: он лежит вплотную к странице без доступа,
// чтение за концом роняет программу. Вывод: «ok <сценарий>» на строку.
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <random>
#include <string>
#include <vector>

#include "zifi/elf_image.hpp"

namespace {

int failures = 0;

void fail(const char* what, const std::string& detail) {
  printf("FAIL %s: %s\n", what, detail.c_str());
  ++failures;
}

// Образ в конце доступной памяти: следующая страница без доступа.
const char* checkGuarded(const std::vector<uint8_t>& image) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const size_t page = info.dwPageSize;
  const size_t body = (image.size() + page - 1) / page * page;
  uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, body + page, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READWRITE));
  DWORD old = 0;
  VirtualProtect(base + body, page, PAGE_NOACCESS, &old);
  uint8_t* data = base + body - image.size();
  if (!image.empty()) {
    memcpy(data, image.data(), image.size());
  }
  const char* problem = zifi::elfImageProblem(data, image.size());
  VirtualFree(base, 0, MEM_RELEASE);
  return problem;
}

std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> data;
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return data;
  }
  fseek(f, 0, SEEK_END);
  data.resize(static_cast<size_t>(ftell(f)));
  fseek(f, 0, SEEK_SET);
  if (!data.empty() && fread(data.data(), 1, data.size(), f) != data.size()) {
    data.clear();
  }
  fclose(f);
  return data;
}

uint32_t get32(const std::vector<uint8_t>& d, size_t at) {
  return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}
uint16_t get16(const std::vector<uint8_t>& d, size_t at) { return static_cast<uint16_t>(d[at] | (d[at + 1] << 8)); }
void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) d[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void put16(std::vector<uint8_t>& d, size_t at, uint16_t v) {
  d[at] = static_cast<uint8_t>(v);
  d[at + 1] = static_cast<uint8_t>(v >> 8);
}

// Порча обязана быть отвергнута.
void expectRefused(const char* what, const std::vector<uint8_t>& image, const char* path) {
  if (checkGuarded(image) == nullptr) {
    fail(what, path);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::vector<uint8_t>> modules;
  for (int i = 1; i < argc; ++i) {
    std::vector<uint8_t> image = readFile(argv[i]);
    const char* problem = image.empty() ? "unreadable" : checkGuarded(image);
    if (problem != nullptr) {
      fail("real_module_is_valid", std::string(argv[i]) + ": " + problem);
    } else {
      modules.push_back(image);
    }
  }
  if (modules.empty()) {
    fail("real_module_is_valid", "no modules");
    return 1;
  }
  if (failures == 0) printf("ok real_modules_are_valid\n");

  const int before = failures;
  for (size_t m = 0; m < modules.size(); ++m) {
    const char* path = argv[m + 1];
    const std::vector<uint8_t>& good = modules[m];
    // Обрезка: только сигнатура, заголовок, середина, без последнего байта.
    for (size_t cut : {size_t(0), size_t(4), size_t(51), size_t(52), good.size() / 2, good.size() - 1}) {
      std::vector<uint8_t> image(good.begin(), good.begin() + cut);
      if (cut < good.size()) {
        // Без последнего байта образ может остаться годным, только если его не читают.
        const char* problem = checkGuarded(image);
        if (cut <= 52 && problem == nullptr) fail("truncated_header", path);
      }
    }
    const uint32_t table = get32(good, 32);
    const uint16_t count = get16(good, 48);
    std::vector<uint8_t> image = good;
    put32(image, 32, static_cast<uint32_t>(good.size()));
    expectRefused("section_table_past_end", image, path);
    image = good;
    put32(image, 32, 0xFFFFFFF0u);
    expectRefused("section_table_offset_overflow", image, path);
    image = good;
    put16(image, 50, count);
    expectRefused("name_table_index", image, path);
    image = good;
    put16(image, 18, 3);
    expectRefused("other_machine", image, path);
    image = good;
    put32(image, 24, 0xFFFFFFF0u);
    expectRefused("entry_outside_text", image, path);
    for (uint32_t i = 0; i < count; ++i) {
      const size_t s = table + i * 40;
      const uint32_t type = get32(good, s + 4);
      if (type != 8) {                                   // не .bss
        image = good;
        put32(image, s + 16, static_cast<uint32_t>(good.size()) - 1);
        put32(image, s + 20, 0x100);
        if (get32(good, s + 20) != 0) expectRefused("section_past_end", image, path);
      }
      image = good;
      put32(image, s, 0x7FFFFFFF);
      expectRefused("section_name_index", image, path);
      if (type == 4) {                                   // SHT_RELA
        const uint32_t offset = get32(good, s + 16);
        const uint32_t size = get32(good, s + 20);
        if (size >= 12) {
          image = good;
          put32(image, offset + 4, (0xFFFFFFu << 8) | (get32(good, offset + 4) & 0xFF));
          expectRefused("relocation_symbol", image, path);
          image = good;
          put32(image, offset + 4, (get32(good, offset + 4) & ~0xFFu) | 5);
          put32(image, offset, 0xFFFFFFF0u);
          expectRefused("relocation_target", image, path);
        }
        image = good;
        put32(image, s + 24, count + 5);
        expectRefused("relocation_link", image, path);
      }
    }
    // Случайная порча: проверка не обязана отвергнуть, но не смеет выйти за образ.
    std::mt19937 random(static_cast<uint32_t>(1234 + m));
    for (int round = 0; round < 3000; ++round) {
      image = good;
      const int flips = 1 + static_cast<int>(random() % 8);
      for (int f = 0; f < flips; ++f) {
        const size_t at = random() % (random() % 4 == 0 ? image.size() : (table + count * 40u < image.size()
                                                                             ? table + count * 40u
                                                                             : image.size()));
        image[at] = static_cast<uint8_t>(random());
      }
      if (random() % 5 == 0) image.resize(random() % image.size());
      checkGuarded(image);
    }
  }
  if (failures == before) printf("ok damaged_images_are_refused_within_bounds\n");
  return failures == 0 ? 0 : 1;
}
