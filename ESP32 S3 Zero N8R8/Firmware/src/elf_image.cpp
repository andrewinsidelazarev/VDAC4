#include "zifi/elf_image.hpp"

#include <string.h>

namespace zifi {
namespace {

constexpr uint16_t kMachineXtensa = 94;
constexpr size_t kHeaderSize = 52;
constexpr size_t kSectionSize = 40;
constexpr size_t kSymbolSize = 16;
constexpr size_t kRelaSize = 12;
constexpr uint32_t kMaxSections = 1024;

constexpr uint32_t kShtProgbits = 1;
constexpr uint32_t kShtSymtab = 2;
constexpr uint32_t kShtRela = 4;
constexpr uint32_t kShtNobits = 8;
constexpr uint32_t kShtDynsym = 11;
constexpr uint32_t kShfWrite = 1;
constexpr uint32_t kShfAlloc = 2;
constexpr uint32_t kShfExecinstr = 4;

// Типы релокаций, по которым загрузчик пишет слово по адресу релокации
// (esp_elf_arch_relocate): R_XTENSA_GLOB_DAT, R_XTENSA_JMP_SLOT, R_XTENSA_RELATIVE.
constexpr uint32_t kRelocGlobDat = 3;
constexpr uint32_t kRelocJmpSlot = 4;
constexpr uint32_t kRelocRelative = 5;

uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t read32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct Section {
  uint32_t name, type, flags, addr, offset, size, link;
};

Section sectionAt(const uint8_t* data, uint32_t table, uint32_t index) {
  const uint8_t* p = data + table + index * kSectionSize;
  return Section{read32(p), read32(p + 4), read32(p + 8), read32(p + 12),
                 read32(p + 16), read32(p + 20), read32(p + 24)};
}

// Отрезок [offset, offset + length) целиком в файле (без переполнения).
bool inFile(uint64_t offset, uint64_t length, size_t size) {
  return offset <= size && length <= size - offset;
}

// Строка index таблицы table кончается нулём внутри таблицы.
bool stringOk(const uint8_t* data, const Section& table, uint32_t index) {
  return index < table.size && memchr(data + table.offset + index, 0, table.size - index) != nullptr;
}

bool hasFlags(const Section& s, uint32_t flags) { return (s.flags & flags) == flags; }

// Загружаемые секции, как их выбирает esp_elf_load_section: при повторе имени
// действует последняя.
enum Loaded { kText, kData, kRodata, kRelRo, kBss, kLoadedCount };

struct Range {
  uint32_t addr = 0;
  uint32_t size = 0;
};

}  // namespace

const char* elfImageProblem(const uint8_t* data, size_t size) {
  if (data == nullptr || size < kHeaderSize) {
    return "short header";
  }
  if (memcmp(data, "\x7F" "ELF", 4) != 0) {
    return "no ELF signature";
  }
  if (data[4] != 1 || data[5] != 1) {
    return "not ELF32 little-endian";
  }
  if (read16(data + 18) != kMachineXtensa) {
    return "not Xtensa";
  }
  const uint32_t entry = read32(data + 24);
  const uint32_t table = read32(data + 32);
  const uint16_t entrySize = read16(data + 46);
  const uint16_t count = read16(data + 48);
  const uint16_t namesIndex = read16(data + 50);
  if (entrySize != kSectionSize || count == 0 || count > kMaxSections) {
    return "bad section table";
  }
  if (!inFile(table, static_cast<uint64_t>(count) * kSectionSize, size)) {
    return "section table outside file";
  }
  if (namesIndex >= count) {
    return "bad section name table index";
  }
  for (uint32_t i = 0; i < count; ++i) {
    const Section s = sectionAt(data, table, i);
    if (s.type != kShtNobits && !inFile(s.offset, s.size, size)) {
      return "section outside file";
    }
  }
  const Section names = sectionAt(data, table, namesIndex);
  if (names.type == kShtNobits) {
    return "bad section name table";
  }

  Range loaded[kLoadedCount];
  for (uint32_t i = 0; i < count; ++i) {
    const Section s = sectionAt(data, table, i);
    if (!stringOk(data, names, s.name)) {
      return "bad section name";
    }
    const char* name = reinterpret_cast<const char*>(data + names.offset + s.name);
    if (s.type == kShtProgbits && hasFlags(s, kShfAlloc)) {
      if (hasFlags(s, kShfExecinstr) && strcmp(name, ".text") == 0) {
        // Загрузчик копирует .text, выровненный до 4 байтов.
        const uint64_t aligned = (static_cast<uint64_t>(s.size) + 3) & ~static_cast<uint64_t>(3);
        if (!inFile(s.offset, aligned, size)) {
          return ".text outside file";
        }
        loaded[kText] = Range{s.addr, static_cast<uint32_t>(aligned)};
      } else if (hasFlags(s, kShfWrite) && strcmp(name, ".data") == 0) {
        loaded[kData] = Range{s.addr, s.size};
      } else if (strcmp(name, ".rodata") == 0) {
        loaded[kRodata] = Range{s.addr, s.size};
      } else if (strcmp(name, ".data.rel.ro") == 0) {
        loaded[kRelRo] = Range{s.addr, s.size};
      }
    } else if (s.type == kShtNobits && hasFlags(s, kShfAlloc | kShfWrite) && strcmp(name, ".bss") == 0) {
      loaded[kBss] = Range{s.addr, s.size};
    }
  }
  if (loaded[kText].size == 0) {
    return "no .text";
  }
  if (entry < loaded[kText].addr || entry - loaded[kText].addr >= loaded[kText].size) {
    return "entry outside .text";
  }

  for (uint32_t i = 0; i < count; ++i) {
    const Section rela = sectionAt(data, table, i);
    if (rela.type != kShtRela) {
      continue;
    }
    if (rela.link >= count) {
      return "bad relocation symbol table";
    }
    const Section symbols = sectionAt(data, table, rela.link);
    if ((symbols.type != kShtSymtab && symbols.type != kShtDynsym) || symbols.link >= count) {
      return "bad relocation symbol table";
    }
    const Section strings = sectionAt(data, table, symbols.link);
    if (strings.type == kShtNobits) {
      return "bad symbol string table";
    }
    const uint32_t symbolCount = symbols.size / kSymbolSize;
    const uint32_t relocations = rela.size / kRelaSize;
    for (uint32_t r = 0; r < relocations; ++r) {
      const uint8_t* entryData = data + rela.offset + r * kRelaSize;
      const uint32_t where = read32(entryData);
      const uint32_t info = read32(entryData + 4);
      const uint32_t symbol = info >> 8;
      const uint32_t type = info & 0xFF;
      if (symbol >= symbolCount) {
        return "relocation symbol out of range";
      }
      // Имя символа загрузчик читает почти у всех типов (esp_elf_relocate).
      const uint32_t symbolName = read32(data + symbols.offset + symbol * kSymbolSize);
      if (!stringOk(data, strings, symbolName)) {
        return "bad symbol name";
      }
      if (type == kRelocGlobDat || type == kRelocJmpSlot || type == kRelocRelative) {
        bool inside = false;
        for (const Range& range : loaded) {
          if (range.size >= 4 && where >= range.addr && where - range.addr <= range.size - 4) {
            inside = true;
            break;
          }
        }
        if (!inside) {
          return "relocation outside loaded sections";
        }
      }
    }
  }
  return nullptr;
}

}  // namespace zifi
