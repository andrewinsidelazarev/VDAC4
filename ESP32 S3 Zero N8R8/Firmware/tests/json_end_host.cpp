// Стенд JsonEnd (0.1.162): конец JSON ответа API GitHub в потоке кусками любой длины.
#include <stdio.h>
#include <string.h>

#include <string>

#include "zifi/json_end.hpp"

static int failures = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++failures;
  }
}

// весь поток по кускам step байт: где документ кончился (позиция после последней скобки) или -1
static long endOf(const std::string& stream, size_t step) {
  zifi::JsonEnd end;
  size_t at = 0;
  while (at < stream.size()) {
    const size_t n = stream.size() - at < step ? stream.size() - at : step;
    size_t used = 0;
    if (end.feed(reinterpret_cast<const uint8_t*>(stream.data()) + at, n, used)) {
      return static_cast<long>(at + used);
    }
    at += n;
  }
  return -1;
}

int main() {
  const std::string tree =
      "{\"sha\":\"4f2\",\"url\":\"https://api.github.com/x\",\"tree\":[{\"path\":\"build/vdac4/VDAC4UPD.WMF\","
      "\"mode\":\"100644\",\"type\":\"blob\",\"sha\":\"aa\",\"size\":16640},{\"path\":\"odd {name} [x]\\\" \\\\\","
      "\"type\":\"tree\"}],\"truncated\":false}";
  for (size_t step : {1u, 2u, 3u, 7u, 64u, 4096u}) {
    check(endOf(tree, step) == static_cast<long>(tree.size()), "tree ends at its last brace");
    check(endOf(tree + "\n", step) == static_cast<long>(tree.size()), "trailing newline is not the document");
    check(endOf("  \r\n" + tree + "GARBAGE", step) == static_cast<long>(tree.size()) + 4, "leading spaces");
    check(endOf(tree.substr(0, tree.size() - 1), step) == -1, "cut document never ends");
  }
  check(endOf("[1,[2,{\"a\":\"]\"}],3]", 5) == 19, "array with a bracket inside a string");
  check(endOf("{\"message\":\"Not Found\",\"status\":\"404\"}", 3) == 38, "404 answer");
  check(endOf("<html>{}</html>", 4) == -1, "not JSON: no end here");
  check(endOf("", 1) == -1, "empty");
  if (failures == 0) {
    printf("OK\n");
  }
  return failures == 0 ? 0 : 1;
}
