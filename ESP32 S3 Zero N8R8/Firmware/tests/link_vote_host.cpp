// Стенд проверенного чтения LINK (0.1.168, include/zifi/link_vote.hpp) на ПК.
//
// Настоящего LINK здесь нет: «чтение» — подставная функция, которая по очереди отдаёт заранее
// заготовленные значения. Так можно точно задать, какое чтение испорчено, и проверить:
//   - сколько обменов тратит readVoted (два, если слово не менялось, три — если менялось);
//   - что одно испорченное чтение из трёх отбрасывается и у постоянного регистра, и у счётчика;
//   - что при отказе обмена слова приёмника не тронуты;
//   - что выброс (0.1.169: два чтения совпали, третье нет) находится, а смена счётчика — нет;
//   - что median3 совпадает с «серединой после сортировки» на миллионе случайных троек.
// Печатает OK или строки FAIL — их проверяет tests/test_link_vote_host.py.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <random>
#include <vector>

#include "zifi/link_vote.hpp"

using zifi::link_vote::Outcome;
using zifi::link_vote::median3;
using zifi::link_vote::readVoted;

static int failures = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++failures;
  }
}

// Подставной LINK: каждый вызов — одно чтение count слов. reads[n] — что вернёт чтение номер n
// (по слову на каждое из count мест). failAt — номер чтения, которое «не прошло» (-1 — все прошли).
struct FakeLink {
  std::vector<std::vector<uint32_t>> reads;
  int failAt = -1;
  int calls = 0;

  bool operator()(uint32_t* out) {
    const int n = calls++;
    if (n == failAt || n >= static_cast<int>(reads.size())) {
      return false;
    }
    for (size_t i = 0; i < reads[n].size(); ++i) {
      out[i] = reads[n][i];
    }
    return true;
  }
};

// Одно слово: прочитать через подставной LINK с данной последовательностью значений.
static Outcome readOne(std::vector<uint32_t> values, uint32_t* word, int* calls, int failAt = -1) {
  FakeLink link;
  for (uint32_t v : values) {
    link.reads.push_back({v});
  }
  link.failAt = failAt;
  const Outcome outcome = readVoted(link, word, 1);
  *calls = link.calls;
  return outcome;
}

static void single_word() {
  uint32_t word = 0;
  int calls = 0;

  // постоянный регистр, чтения целые: хватает двух обменов
  check(readOne({7, 7, 999}, &word, &calls) == Outcome::kAgreed, "static: agreed");
  check(word == 7 && calls == 2, "static: value 7 in two reads");

  // постоянный регистр, испорчено второе чтение: третье и медиана
  check(readOne({7, 0x8007, 7}, &word, &calls) == Outcome::kMedian, "static, bad 2nd: median");
  check(word == 7 && calls == 3, "static, bad 2nd: value 7 in three reads");

  // испорчено первое чтение (случай с платы 24.11.2026: FIFO_FREE 0x858 при пределе 0x800)
  check(readOne({0x858, 0x100, 0x100}, &word, &calls) == Outcome::kMedian, "bad 1st: median");
  check(word == 0x100, "bad 1st: 0x858 dropped");

  // испорчено третье чтение: первые два и так разные (счётчик), медиана — меньшее из целых
  readOne({100, 102, 0}, &word, &calls);
  check(word == 100, "counter, low outlier: 100");
  readOne({100, 102, 0xFFFFFFFF}, &word, &calls);
  check(word == 102, "counter, high outlier: 102");

  // счётчик меняется сам, чтения целые: медиана — одно из настоящих значений (среднее по времени)
  readOne({100, 101, 102}, &word, &calls);
  check(word == 101, "counter rising: 101");
  readOne({102, 101, 100}, &word, &calls);
  check(word == 101, "counter falling: 101");

  // счётчик с выбросом посередине (как FIFO_FREE): выброс уходит в край ряда
  readOne({100, 0x858, 102}, &word, &calls);
  check(word == 102, "counter, 0x858 in the middle: 102");
}

static void failures_untouched() {
  for (int failAt = 0; failAt < 3; ++failAt) {
    uint32_t word = 0xDEADBEEF;
    int calls = 0;
    // чтения 0 и 1 разные — значит, дойдёт и до третьего, и отказ любого из трёх виден
    const Outcome outcome = readOne({1, 2, 3}, &word, &calls, failAt);
    check(outcome == Outcome::kFailed, "failed read: kFailed");
    check(word == 0xDEADBEEF, "failed read: words untouched");
    check(calls == failAt + 1, "failed read: stops at once");
  }
}

static void bad_arguments() {
  FakeLink link;
  link.reads = {{1}, {1}, {1}};
  uint32_t words[17] = {};
  check(readVoted(link, words, 0) == Outcome::kFailed, "count 0 refused");
  check(readVoted(link, words, 17) == Outcome::kFailed, "count 17 refused");
  check(readVoted(link, nullptr, 1) == Outcome::kFailed, "null words refused");
  check(link.calls == 0, "refused before any read");
}

static void sixteen_words() {
  // 16 слов: слово 3 испорчено во втором чтении, слово 9 — счётчик, остальные постоянны
  FakeLink link;
  for (int n = 0; n < 3; ++n) {
    std::vector<uint32_t> r(16);
    for (int i = 0; i < 16; ++i) {
      r[i] = 0x1000U + static_cast<uint32_t>(i);
    }
    if (n == 1) {
      r[3] = 0xBAD0BAD0U;
    }
    r[9] = 500U + static_cast<uint32_t>(n);
    link.reads.push_back(r);
  }
  uint32_t words[16] = {};
  check(readVoted(link, words, 16) == Outcome::kMedian, "16 words: median");
  bool same = true;
  for (int i = 0; i < 16; ++i) {
    const uint32_t want = i == 9 ? 501U : 0x1000U + static_cast<uint32_t>(i);
    same = same && words[i] == want;
  }
  check(same, "16 words: each word on its own");
  check(link.calls == 3, "16 words: three reads");
}

static void median_matches_sort() {
  // median3 против «отсортировать и взять середину»: случайные тройки и крайние значения
  std::mt19937 random(12345);
  const uint32_t edges[] = {0U, 1U, 0x7FFFFFFFU, 0x80000000U, 0xFFFFFFFEU, 0xFFFFFFFFU};
  bool ok = true;
  for (uint32_t a : edges) {
    for (uint32_t b : edges) {
      for (uint32_t c : edges) {
        uint32_t s[3] = {a, b, c};
        std::sort(s, s + 3);
        ok = ok && median3(a, b, c) == s[1];
      }
    }
  }
  for (int i = 0; i < 1000000; ++i) {
    const uint32_t a = random(), b = random(), c = random() & (i & 1 ? 0xFFFFFFFFU : 0xFFU);
    uint32_t s[3] = {a, b, c};
    std::sort(s, s + 3);
    ok = ok && median3(a, b, c) == s[1];
  }
  check(ok, "median3 equals the middle of the sorted three");
}

static void one_bad_stays_between_good() {
  // Главное свойство: если испорчено одно чтение из трёх, итог лежит между двумя целыми
  // (включительно) — каким бы ни было испорченное и на каком бы месте оно ни стояло.
  std::mt19937 random(777);
  bool ok = true;
  for (int i = 0; i < 300000; ++i) {
    const uint32_t g1 = random() & 0xFFFU, g2 = random() & 0xFFFU, bad = random();
    const uint32_t low = std::min(g1, g2), high = std::max(g1, g2);
    const int place = i % 3;
    uint32_t r[3] = {g1, g2, g2};
    r[place] = bad;
    r[(place + 1) % 3] = g1;
    r[(place + 2) % 3] = g2;
    const uint32_t m = median3(r[0], r[1], r[2]);
    ok = ok && m >= low && m <= high;
  }
  check(ok, "one bad read of three: result between the two good ones");
}

// Выброс (0.1.169) — для счётчика в команде «tang»: два чтения из трёх совпали, третье нет.
static Outcome readOutlier(std::vector<std::vector<uint32_t>> reads, uint32_t* words, size_t count,
                           zifi::link_vote::Outlier* outlier) {
  FakeLink link;
  link.reads = reads;
  return readVoted(link, words, count, outlier);
}

static void outliers() {
  zifi::link_vote::Outlier o;
  uint32_t w[2] = {};

  // постоянный регистр, испорчено второе чтение: выброс — второе, принято общее значение
  readOutlier({{7}, {0x8007}, {7}}, w, 1, &o);
  check(o.found && o.index == 0 && o.rejected == 0x8007 && o.accepted == 7, "outlier in 2nd read");

  // испорчено первое (случай с платы): отброшено 0x858
  readOutlier({{0x858}, {0x100}, {0x100}}, w, 1, &o);
  check(o.found && o.rejected == 0x858 && o.accepted == 0x100, "outlier in 1st read");

  // счётчик, все три разные: третье чтение было, но выброса нет — его не отличить от смены
  readOutlier({{100}, {101}, {102}}, w, 1, &o);
  check(!o.found, "changing counter: no outlier");
  readOutlier({{100}, {0x858}, {102}}, w, 1, &o);
  check(!o.found, "counter with a bad read: not counted (all three differ)");

  // два чтения совпали сразу: третьего нет, выброса нет
  readOutlier({{5}, {5}}, w, 1, &o);
  check(!o.found, "agreed: no outlier");

  // блок из двух слов: слово 0 — счётчик (из-за него третье чтение), слово 1 постоянное и
  // испорчено в третьем чтении — выброс в слове 1, адрес считает прошивка как address + 4*index
  readOutlier({{10, 0x55}, {11, 0x55}, {12, 0xFF55}}, w, 2, &o);
  check(o.found && o.index == 1 && o.rejected == 0xFF55 && o.accepted == 0x55 && w[1] == 0x55,
        "block: outlier in word 1, 3rd read");

  // в блоке слово, у которого все три чтения равны, выбросом не считается
  readOutlier({{1, 9}, {2, 9}, {3, 9}}, w, 2, &o);
  check(!o.found, "block: equal word is not an outlier");

  // без указателя на выброс работает как раньше
  check(readOutlier({{7}, {0x8007}, {7}}, w, 1, nullptr) == Outcome::kMedian && w[0] == 7,
        "outlier pointer may be null");

  // прежний итог в той же переменной стирается в начале каждого чтения
  readOutlier({{7}, {0x8007}, {7}}, w, 1, &o);
  readOutlier({{5}, {5}}, w, 1, &o);
  check(!o.found, "outlier reset on each read");
}

int main() {
  single_word();
  outliers();
  failures_untouched();
  bad_arguments();
  sixteen_words();
  median_matches_sort();
  one_bad_stays_between_good();
  if (failures == 0) {
    printf("OK\n");
  }
  return failures == 0 ? 0 : 1;
}
