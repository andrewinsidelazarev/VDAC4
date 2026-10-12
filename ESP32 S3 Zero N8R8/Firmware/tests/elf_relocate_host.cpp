// Перемещение R_XTENSA_RELATIVE загрузчика ELF на ПК: настоящий
// components/elf_loader/src/arch/esp_elf_xtensa.c и настоящие esp_elf_map_sym и
// elf_remap_text (тест вырезает их из исходников загрузчика). Раскладка секций — как у
// MIDI.ELF 1.7.1, где указатель на конец .rodata становился 0 и ESP сбрасывалась.
// Вывод: «ok <сценарий>» на строку.
#include <stdint.h>
#include <stdio.h>

#include "esp_elf.h"
#include "private/elf_platform.h"

namespace {

int failures = 0;

constexpr uint32_t kText = 0x3C200000;    // .text в буфере команд (адрес шины данных)
constexpr uint32_t kRodata = 0x3C300000;
constexpr uint32_t kBss = 0x3FC90000;
constexpr uint32_t kIbus = 0x06000000;    // SOC_IROM_LOW − SOC_DROM_LOW у ESP32-S3

// Слово в .data со значением value после перемещения RELATIVE.
uint32_t relocate(esp_elf_t& elf, uint32_t* data, int slot, uint32_t value) {
  data[slot] = value;
  elf32_rela_t rela = {};
  rela.offset = elf.sec[ELF_SEC_DATA].v_addr + slot * 4;
  rela.info = 5;                          // R_XTENSA_RELATIVE, без символа
  if (esp_elf_arch_relocate(&elf, &rela, nullptr, 0) != 0) {
    printf("FAIL relocate 0x%x: error\n", value);
    ++failures;
  }
  return data[slot];
}

void expect(const char* what, uint32_t got, uint32_t want) {
  if (got != want) {
    printf("FAIL %s: 0x%08x, want 0x%08x\n", what, got, want);
    ++failures;
  }
}

}  // namespace

int main() {
  uint32_t data[6] = {};
  esp_elf_t elf = {};
  elf.sec[ELF_SEC_TEXT] = {0x8c4, 0, kText, 0x900a};
  elf.sec[ELF_SEC_RODATA] = {0x98d0, 0, kRodata, 0x474};
  elf.sec[ELF_SEC_DATA] = {0xa0a4, 0, reinterpret_cast<uintptr_t>(data), sizeof(data)};
  elf.sec[ELF_SEC_BSS] = {0xa0bc, 0, kBss, 0x244};

  expect("inside_rodata", relocate(elf, data, 0, 0x98d4), kRodata + 4);
  expect("inside_text_goes_to_ibus", relocate(elf, data, 1, 0x8c8), kText + 4 + kIbus);
  expect("rodata_end", relocate(elf, data, 2, 0x98d0 + 0x474), kRodata + 0x474);
  expect("text_end_goes_to_ibus", relocate(elf, data, 3, 0x8c4 + 0x900a), kText + 0x900a + kIbus);
  expect("data_end_is_bss_start", relocate(elf, data, 4, 0xa0bc), kBss);
  expect("gap_is_zero", relocate(elf, data, 5, 0xa000), 0);
  if (failures) return 1;
  printf("ok relative_section_end\n");
  return 0;
}
