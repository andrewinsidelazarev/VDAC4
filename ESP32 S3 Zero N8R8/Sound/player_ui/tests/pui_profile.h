#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>

// Эти счётчики вставляются только в производную копию исходника в build-pc.
namespace pui_profile {
struct Counts {
  uint64_t put, stores, dither, background, placeholder, round, restore;
  uint64_t glyph, alpha, hash, copied, formats;
};
inline Counts counts{};
inline int format(char* out, size_t size, const char* fmt, ...) {
  ++counts.formats;
  va_list args; va_start(args, fmt);
  const int n = std::vsnprintf(out, size, fmt, args);
  va_end(args); return n;
}
}
