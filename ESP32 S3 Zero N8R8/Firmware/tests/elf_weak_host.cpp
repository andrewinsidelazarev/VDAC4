// Слабые символы загрузчика ELF на ПК (0.1.122, задание TSafari): настоящая elf_weak_undefined
// (тест вырезает её из components/elf_loader/src/esp_elf.c). Слабый неопределённый символ — адрес 0
// вместо ошибки, сильный неопределённый и слабый определённый — как прежде. Затем — таблица .dynsym
// настоящего пробного модуля modules/hello/hello.app.elf: слабые неопределённые в нём ровно
// vdac4_probe_absent и cbrt (их перемещения GLOB_DAT и JMP_SLOT идут через обе ветки поиска).
// Вызов: elf_weak_host <hello.app.elf>. Вывод: «ok <сценарий>» на строку.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

#include "esp_elf.h"
#include "private/elf_types.h"

extern "C" bool vdac4_test_weak_undefined(const elf32_sym_t* sym);

namespace {

int failures = 0;

void expect(const char* what, bool ok) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++failures;
  }
}

elf32_sym_t sym(unsigned bind, unsigned type, uint16_t shndx) {
  elf32_sym_t s = {};
  s.info = static_cast<unsigned char>(bind << 4 | type);
  s.shndx = shndx;
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  elf32_sym_t s;
  s = sym(STB_WEAK, STT_NOTYPE, SHN_UNDEF);
  expect("weak_undefined", vdac4_test_weak_undefined(&s));
  s = sym(STB_WEAK, STT_FUNC, SHN_UNDEF);
  expect("weak_undefined_func", vdac4_test_weak_undefined(&s));
  s = sym(STB_GLOBAL, STT_NOTYPE, SHN_UNDEF);
  expect("strong_undefined_stays_error", !vdac4_test_weak_undefined(&s));
  s = sym(STB_WEAK, STT_FUNC, 5);
  expect("weak_defined_in_module", !vdac4_test_weak_undefined(&s));
  s = sym(STB_LOCAL, STT_NOTYPE, SHN_UNDEF);
  expect("local_undefined", !vdac4_test_weak_undefined(&s));
  if (failures) return 1;
  printf("ok weak_rules\n");

  if (argc < 2) return 0;
  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    printf("FAIL open %s\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> b;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) b.insert(b.end(), chunk, chunk + n);
  fclose(f);
  elf32_hdr_t eh;
  memcpy(&eh, b.data(), sizeof(eh));
  std::set<std::string> weak, strong;
  for (unsigned i = 0; i < eh.shnum; ++i) {
    elf32_shdr_t sh;
    memcpy(&sh, &b[eh.shoff + i * sizeof(sh)], sizeof(sh));
    if (sh.type != 11) continue;                       // SHT_DYNSYM
    elf32_shdr_t strs;
    memcpy(&strs, &b[eh.shoff + sh.link * sizeof(strs)], sizeof(strs));
    for (unsigned k = 0; k < sh.size / sizeof(elf32_sym_t); ++k) {
      elf32_sym_t e;
      memcpy(&e, &b[sh.offset + k * sizeof(e)], sizeof(e));
      const char* name = reinterpret_cast<const char*>(&b[strs.offset + e.name]);
      if (!*name || e.shndx != SHN_UNDEF) continue;
      (vdac4_test_weak_undefined(&e) ? weak : strong).insert(name);
    }
  }
  expect("hello_weak_set", weak == std::set<std::string>{"vdac4_probe_absent", "cbrt"});
  expect("hello_strong_has_printf", strong.count("printf") == 1 && strong.count("localtime_r") == 1);
  if (failures) {
    for (const auto& w : weak) printf("  weak: %s\n", w.c_str());
    return 1;
  }
  printf("ok hello_dynsym\n");
  return 0;
}
