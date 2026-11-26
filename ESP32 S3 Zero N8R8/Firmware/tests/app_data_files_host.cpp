// Стенд файлов данных приложений на флешке (src/app_data_files.cpp, 0.1.165) на ПК; с 0.1.166 — и
// файлов на карте Evo, которые читаются по запросу у программы Z80 (её заменяет FakeZ80).
//
// Каталог «флешки» — временная папка ПК: тот же переносимый код (stdio, stat, utime, rename), что в
// прошивке работает поверх FatFs. Каждый сценарий печатает «ok имя» или причину неудачи и выход с кодом 1.

#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <string>
#include <vector>

#include "zifi/app_data_files.hpp"

using zifi::AppDataFiles;

namespace {

std::string g_root;
constexpr uint64_t kLots = 1ull << 40;   // «места много»

uint32_t fatStamp(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute) {
  const uint32_t date = ((year - 1980) << 9) | (month << 5) | day;
  const uint32_t time = (hour << 11) | (minute << 5);
  return time | (date << 16);
}

std::vector<uint8_t> pattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>(seed + i * 13 + (i >> 11));
  }
  return data;
}

std::string pathOf(const char* app, const char* name) { return g_root + "\\" + app + "\\" + name; }
bool exists(const char* app, const char* name) { return _access(pathOf(app, name).c_str(), 0) == 0; }

void removeAll() {
  for (const char* app : {"Q2", "DOOM"}) {
    for (const char* name : {"PAK0.PAK", "PAK1.PAK", "WRITING.TMP"}) {
      remove(pathOf(app, name).c_str());
    }
  }
}

bool fail(const char* scenario, const char* why) {
  printf("FAIL %s: %s\n", scenario, why);
  return false;
}

// Доставить файл кусками parts (последний — остаток).
bool deliver(AppDataFiles& files, const char* app, const char* name, const std::vector<uint8_t>& data,
             uint32_t stamp, std::initializer_list<uint32_t> parts) {
  if (files.begin(app, name, static_cast<uint32_t>(data.size()), stamp, kLots) != AppDataFiles::kSend) {
    return false;
  }
  uint32_t at = 0;
  for (uint32_t part : parts) {
    if (!files.put(at, data.data() + at, part)) {
      return false;
    }
    at += part;
  }
  if (at < data.size() && !files.put(at, data.data() + at, static_cast<uint32_t>(data.size()) - at)) {
    return false;
  }
  return files.commit();
}

bool deliveredFileReadsBack() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  const std::vector<uint8_t> data = pattern(18969, 3);          // не кратно 4 КБ, больше куска
  const uint32_t stamp = fatStamp(2026, 11, 23, 20, 15);
  if (!deliver(files, "q2", "pak0.pak", data, stamp, {10000, 8192})) {
    return fail("delivered_file_reads_back", "delivery failed");
  }
  if (!exists("Q2", "PAK0.PAK") || exists("Q2", "WRITING.TMP")) {
    return fail("delivered_file_reads_back", "file is not under its upper-case name or temp left");
  }
  if (files.begin("Q2", "PAK0.PAK", static_cast<uint32_t>(data.size()), stamp, kLots) !=
      AppDataFiles::kHave) {
    return fail("delivered_file_reads_back", "fresh copy is asked again");
  }
  void* file = nullptr;
  uint32_t size = 0;
  if (files.open("Q2", "pak0.pak", &file, &size) != AppDataFiles::kOk || size != data.size()) {
    return fail("delivered_file_reads_back", "open failed or wrong size");
  }
  std::vector<uint8_t> back(data.size());
  uint32_t got = 0;
  // кусками разной длины и не по порядку
  if (files.read(file, 12000, back.data() + 12000, 6969, &got) != AppDataFiles::kOk || got != 6969 ||
      files.read(file, 0, back.data(), 12000, &got) != AppDataFiles::kOk || got != 12000 ||
      back != data) {
    return fail("delivered_file_reads_back", "read back differs");
  }
  uint8_t tail[100];
  if (files.read(file, 18900, tail, 100, &got) != AppDataFiles::kOk || got != 69 ||
      memcmp(tail, data.data() + 18900, 69) != 0) {
    return fail("delivered_file_reads_back", "read over the end is not cut at the end");
  }
  if (files.read(file, 18969, tail, 10, &got) != AppDataFiles::kOk || got != 0) {
    return fail("delivered_file_reads_back", "read past the end is not empty");
  }
  files.close(file);
  return true;
}

bool putOutOfOrderIsRefused() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  const std::vector<uint8_t> data = pattern(5000, 9);
  if (files.begin("Q2", "PAK0.PAK", 5000, fatStamp(2026, 1, 2, 3, 4), kLots) != AppDataFiles::kSend) {
    return fail("put_out_of_order_is_refused", "begin");
  }
  if (files.put(100, data.data(), 100) || !files.put(0, data.data(), 3000) ||
      files.put(1000, data.data(), 10) || files.put(3000, data.data(), 2001) || files.received() != 3000) {
    return fail("put_out_of_order_is_refused", "wrong piece accepted");
  }
  files.drop();
  return true;
}

bool incompleteDeliveryLeavesNothing() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  const std::vector<uint8_t> data = pattern(9000, 5);
  if (files.begin("Q2", "PAK1.PAK", 9000, fatStamp(2026, 2, 3, 4, 5), kLots) != AppDataFiles::kSend ||
      !files.put(0, data.data(), 4000)) {
    return fail("incomplete_delivery_leaves_nothing", "begin/put");
  }
  if (files.commit() || exists("Q2", "PAK1.PAK") || exists("Q2", "WRITING.TMP")) {
    return fail("incomplete_delivery_leaves_nothing", "half a file was committed or temp left");
  }
  // и брошенная доставка (новый MOD_QUERY или drop) не трогает прежнюю копию
  if (!deliver(files, "Q2", "PAK1.PAK", data, fatStamp(2026, 2, 3, 4, 5), {})) {
    return fail("incomplete_delivery_leaves_nothing", "full delivery");
  }
  const std::vector<uint8_t> other = pattern(7000, 77);
  if (files.begin("Q2", "PAK1.PAK", 7000, fatStamp(2026, 3, 3, 4, 5), kLots) != AppDataFiles::kSend ||
      !files.put(0, other.data(), 1000)) {
    return fail("incomplete_delivery_leaves_nothing", "second begin");
  }
  files.drop();
  void* file = nullptr;
  uint32_t size = 0;
  if (exists("Q2", "WRITING.TMP") || files.open("Q2", "PAK1.PAK", &file, &size) != AppDataFiles::kOk ||
      size != 9000) {
    return fail("incomplete_delivery_leaves_nothing", "dropped delivery touched the old copy");
  }
  files.close(file);
  return true;
}

bool newerCardFileIsSentAgain() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  const std::vector<uint8_t> data = pattern(3000, 1);
  if (!deliver(files, "Q2", "PAK0.PAK", data, fatStamp(2026, 5, 6, 7, 8), {})) {
    return fail("newer_card_file_is_sent_again", "delivery");
  }
  if (files.begin("Q2", "PAK0.PAK", 3000, fatStamp(2026, 5, 6, 7, 10), kLots) != AppDataFiles::kSend) {
    return fail("newer_card_file_is_sent_again", "newer file on the card is not asked");
  }
  files.drop();
  if (files.begin("Q2", "PAK0.PAK", 3001, fatStamp(2026, 5, 6, 7, 8), kLots) != AppDataFiles::kSend) {
    return fail("newer_card_file_is_sent_again", "other length is not asked");
  }
  files.drop();
  if (files.begin("Q2", "PAK0.PAK", 3000, fatStamp(2025, 5, 6, 7, 8), kLots) != AppDataFiles::kHave) {
    return fail("newer_card_file_is_sent_again", "older card file is asked");
  }
  return true;
}

bool roomCountsTheOldCopy() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  if (files.begin("Q2", "PAK0.PAK", 10000, fatStamp(2026, 1, 1, 0, 0), 9999) != AppDataFiles::kNoRoom) {
    return fail("room_counts_the_old_copy", "no room is not reported");
  }
  const std::vector<uint8_t> data = pattern(4000, 2);
  if (!deliver(files, "Q2", "PAK0.PAK", data, fatStamp(2026, 1, 1, 0, 0), {})) {
    return fail("room_counts_the_old_copy", "delivery");
  }
  // 10000 байт при 6000 свободных и прежней копии 4000 — влезает
  if (files.begin("Q2", "PAK0.PAK", 10000, fatStamp(2026, 1, 2, 0, 0), 6000) != AppDataFiles::kSend) {
    return fail("room_counts_the_old_copy", "the old copy's room is not counted");
  }
  files.drop();
  return true;
}

bool badNamesAreRefused() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  const uint32_t stamp = fatStamp(2026, 1, 1, 0, 0);
  for (const char* app : {"", "TOOLONGAPP", "A/B", "A.B", "..", "Q 2"}) {
    if (files.begin(app, "PAK0.PAK", 10, stamp, kLots) != AppDataFiles::kBadName) {
      return fail("bad_names_are_refused", app);
    }
  }
  for (const char* name : {"", "WRITING.TMP", "ABCDEFGHI.PAK", "PAK0.LONG", "A.B.C", ".PAK", "..",
                           "..\\X", "A/B.PAK"}) {
    if (files.begin("Q2", name, 10, stamp, kLots) != AppDataFiles::kBadName) {
      return fail("bad_names_are_refused", name);
    }
    void* file = reinterpret_cast<void*>(1);
    if (files.open("Q2", name, &file, nullptr) != AppDataFiles::kNotFound || file != nullptr) {
      return fail("bad_names_are_refused", "open of a bad name");
    }
  }
  if (files.begin("Q2", "PAK0.PAK", 0, stamp, kLots) != AppDataFiles::kBadName) {
    return fail("bad_names_are_refused", "zero length");
  }
  void* file = nullptr;
  if (files.open("Q2", "NONE.PAK", &file, nullptr) != AppDataFiles::kNotFound) {
    return fail("bad_names_are_refused", "missing file opens");
  }
  return true;
}

bool interleavedHandlesReadTheirFiles() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  const std::vector<uint8_t> a = pattern(9000, 11);
  const std::vector<uint8_t> b = pattern(7000, 99);
  if (!deliver(files, "Q2", "PAK0.PAK", a, fatStamp(2026, 1, 1, 0, 0), {}) ||
      !deliver(files, "Q2", "PAK1.PAK", b, fatStamp(2026, 1, 1, 0, 0), {}) ||
      !deliver(files, "DOOM", "PAK0.PAK", b, fatStamp(2026, 1, 1, 0, 0), {})) {
    return fail("interleaved_handles_read_their_files", "delivery");
  }
  void* fa = nullptr;
  void* fb = nullptr;
  void* fd = nullptr;
  if (files.open("Q2", "PAK0.PAK", &fa, nullptr) != AppDataFiles::kOk ||
      files.open("Q2", "PAK1.PAK", &fb, nullptr) != AppDataFiles::kOk ||
      files.open("DOOM", "PAK0.PAK", &fd, nullptr) != AppDataFiles::kOk) {
    return fail("interleaved_handles_read_their_files", "open");
  }
  uint8_t x[600];
  uint32_t got = 0;
  for (uint32_t at = 0; at < 6000; at += 600) {
    if (files.read(fa, at, x, 600, &got) != AppDataFiles::kOk || memcmp(x, a.data() + at, 600) != 0 ||
        files.read(fb, at, x, 600, &got) != AppDataFiles::kOk || memcmp(x, b.data() + at, 600) != 0 ||
        files.read(fd, 6000 - at, x, 600, &got) != AppDataFiles::kOk ||
        memcmp(x, b.data() + 6000 - at, 600) != 0) {
      return fail("interleaved_handles_read_their_files", "wrong data after switching files");
    }
  }
  files.closeAll();
  if (files.read(fa, 0, x, 10, &got) != AppDataFiles::kError) {
    return fail("interleaved_handles_read_their_files", "closed handle still reads");
  }
  return true;
}

bool handlesRunOut() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  const std::vector<uint8_t> a = pattern(100, 1);
  if (!deliver(files, "Q2", "PAK0.PAK", a, fatStamp(2026, 1, 1, 0, 0), {})) {
    return fail("handles_run_out", "delivery");
  }
  std::vector<void*> opened;
  for (size_t i = 0; i < AppDataFiles::kMaxOpen; ++i) {
    void* file = nullptr;
    if (files.open("Q2", "PAK0.PAK", &file, nullptr) != AppDataFiles::kOk) {
      return fail("handles_run_out", "fewer handles than kMaxOpen");
    }
    opened.push_back(file);
  }
  void* extra = nullptr;
  if (files.open("Q2", "PAK0.PAK", &extra, nullptr) != AppDataFiles::kError || extra != nullptr) {
    return fail("handles_run_out", "more handles than kMaxOpen");
  }
  files.close(opened[3]);
  if (files.open("Q2", "PAK0.PAK", &extra, nullptr) != AppDataFiles::kOk) {
    return fail("handles_run_out", "closed handle is not reused");
  }
  files.closeAll();
  return true;
}

// --- 0.1.166: файлы на карте Evo, которые читаются по запросу у программы Z80 ----------------------

// «Программа Z80» стенда: разбирает событие, как ESP App SPG, и отвечает кусками через AppDataRequest
// (как задача обмена прошивки). Попутно проверяет обещания AppDataFiles: смещение кратно 512, длина
// 1…32768, не за концом файла; и подмешивает куски, которые должны пройти мимо: прежнего номера
// запроса и повтор уже принятого.
struct FakeZ80 {
  const std::vector<uint8_t>* files[4] = {};
  zifi::AppDataRequest request;
  uint8_t seq = 0;
  unsigned requests = 0;
  uint32_t piece = 8192;        // кусок: 8192 — быстрый путь DMA, 1016 — кадры
  bool failNext = false;        // следующий запрос — «сектор не прочитался»
  const char* bad = nullptr;    // нарушение, которое увидела «Z80»
};

zifi::AppDataRequest::Result sendPiece(FakeZ80& z, uint8_t seq, uint32_t at, const uint8_t* data,
                                       size_t length) {
  std::vector<uint8_t> body(5 + length);
  body[0] = seq;
  for (int i = 0; i < 4; ++i) {
    body[1 + i] = static_cast<uint8_t>(at >> (8 * i));
  }
  if (length != 0) {
    memcpy(body.data() + 5, data, length);
  }
  return z.request.piece(body.data(), body.size());
}

bool fakeFetch(void* context, uint8_t index, uint32_t offset, void* data, uint32_t count) {
  FakeZ80& z = *static_cast<FakeZ80*>(context);
  ++z.requests;
  const std::vector<uint8_t>* file = index < 4 ? z.files[index] : nullptr;
  if (file == nullptr) {
    z.bad = "unknown file index";
    return false;
  }
  if (offset % 512 != 0 || count == 0 || count > 32768 || offset + count > file->size()) {
    z.bad = "request is not whole sectors inside the file";
    return false;
  }
  const uint8_t seq = ++z.seq;
  z.request.begin(seq, offset, count, static_cast<uint8_t*>(data));
  uint8_t event[zifi::AppDataRequest::kEventBytes];
  zifi::AppDataRequest::event(event, seq, index, offset, count);
  // Z80 видит только событие
  const uint8_t gotSeq = event[0];
  const uint32_t at = event[2] | event[3] << 8 | event[4] << 16 | static_cast<uint32_t>(event[5]) << 24;
  const uint32_t n = event[6] | event[7] << 8;
  if (event[1] != index || at != offset || n != count) {
    z.bad = "event fields";
    return false;
  }
  const uint8_t* source = file->data() + at;
  if (sendPiece(z, static_cast<uint8_t>(gotSeq - 1), at, source, 16) != zifi::AppDataRequest::kIgnored) {
    z.bad = "a piece of the previous request was taken";
    return false;
  }
  if (z.failNext) {
    z.failNext = false;
    if (sendPiece(z, gotSeq, at, nullptr, 0) != zifi::AppDataRequest::kFailed || !z.request.failed()) {
      z.bad = "an empty piece is not a failure";
    }
    z.request.end();
    return false;
  }
  for (uint32_t done = 0; done < n;) {
    const uint32_t length = n - done < z.piece ? n - done : z.piece;
    const auto result = sendPiece(z, gotSeq, at + done, source + done, length);
    done += length;
    if (result != (done == n ? zifi::AppDataRequest::kDone : zifi::AppDataRequest::kTaken)) {
      z.bad = "piece not taken";
      return false;
    }
    if (done < n && sendPiece(z, gotSeq, at, source, length) != zifi::AppDataRequest::kIgnored) {
      z.bad = "a repeated piece was taken twice";
      return false;
    }
  }
  if (sendPiece(z, gotSeq, at + n, source, 1) != zifi::AppDataRequest::kIgnored) {
    z.bad = "a piece after the end was taken";
    return false;
  }
  z.request.end();
  return z.request.complete();
}

// Случайные чтения файла на карте (длина не кратна сектору) — байт в байт; DMA и кадры; чтение за конец
// обрезается; короткие подряд берутся из блока; отказ Z80 — kError, следующее чтение снова работает.
bool remoteReadsMatchTheCard() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  std::vector<uint8_t> block(AppDataFiles::kRemoteBlock);
  FakeZ80 z;
  const std::vector<uint8_t> pak = pattern(300001, 9);
  const std::vector<uint8_t> small = pattern(700, 77);
  z.files[2] = &pak;
  z.files[3] = &small;
  files.setFetch(fakeFetch, &z, block.data());
  if (!files.addRemote("q2", "pak0.pak", static_cast<uint32_t>(pak.size()), fatStamp(2026, 1, 1, 0, 0), 2) ||
      !files.addRemote("Q2", "PAK1.PAK", static_cast<uint32_t>(small.size()), fatStamp(2026, 1, 1, 0, 0), 3)) {
    return fail("remote_reads_match_the_card", "addRemote");
  }
  void* a = nullptr;
  void* b = nullptr;
  uint32_t size = 0;
  if (files.open("Q2", "PAK0.PAK", &a, &size) != AppDataFiles::kOk || size != pak.size() ||
      files.open("q2", "pak1.pak", &b, &size) != AppDataFiles::kOk || size != small.size()) {
    return fail("remote_reads_match_the_card", "open of a registered file");
  }
  std::vector<uint8_t> out(80000);
  uint32_t seed = 12345;
  for (int i = 0; i < 400; ++i) {
    seed = seed * 1103515245u + 12345u;
    z.piece = (seed >> 7) % 3 == 0 ? 1016 : 8192;
    const uint32_t offset = (seed >> 3) % pak.size();
    const uint32_t kinds[] = {1 + (seed >> 9) % 100, 100 + (seed >> 11) % 5000, 20000 + (seed >> 13) % 60000};
    const uint32_t count = kinds[(seed >> 20) % 3];
    uint32_t got = 0;
    if (files.read(a, offset, out.data(), count, &got) != AppDataFiles::kOk) {
      return fail("remote_reads_match_the_card", z.bad != nullptr ? z.bad : "read failed");
    }
    const uint32_t want = count < pak.size() - offset ? count : static_cast<uint32_t>(pak.size() - offset);
    if (got != want || memcmp(out.data(), pak.data() + offset, got) != 0) {
      return fail("remote_reads_match_the_card", "wrong bytes");
    }
    if (i % 7 == 0) {                          // другой файл вперемешку
      const uint32_t at = static_cast<uint32_t>(i % 650);
      const uint32_t left = static_cast<uint32_t>(small.size()) - at;
      if (files.read(b, at, out.data(), 100, &got) != AppDataFiles::kOk || got != (left < 100 ? left : 100) ||
          memcmp(out.data(), small.data() + at, got) != 0) {
        return fail("remote_reads_match_the_card", "second file");
      }
    }
  }
  // короткие чтения подряд (каталог pak): 16 КБ по 64 байта — не больше трёх запросов
  z.piece = 8192;
  const unsigned before = z.requests;
  for (uint32_t at = 100000; at < 100000 + 16384; at += 64) {
    uint32_t got = 0;
    if (files.read(a, at, out.data(), 64, &got) != AppDataFiles::kOk || got != 64 ||
        memcmp(out.data(), pak.data() + at, 64) != 0) {
      return fail("remote_reads_match_the_card", "short reads");
    }
  }
  if (z.requests - before > 3) {
    return fail("remote_reads_match_the_card", "short reads are not served from the block");
  }
  // отказ программы Z80 — kError и сколько успели; следующее чтение — снова верно
  z.failNext = true;
  uint32_t got = 0;
  if (files.read(a, 4096, out.data(), 5000, &got) != AppDataFiles::kError || got != 0) {
    return fail("remote_reads_match_the_card", "a Z80 failure is not an error");
  }
  if (files.read(a, 4096, out.data(), 5000, &got) != AppDataFiles::kOk || got != 5000 ||
      memcmp(out.data(), pak.data() + 4096, 5000) != 0) {
    return fail("remote_reads_match_the_card", "read after a failure");
  }
  if (z.bad != nullptr) {
    return fail("remote_reads_match_the_card", z.bad);
  }
  files.closeAll();
  return true;
}

// Учёт: файл на учёте открывается вместо копии на флешке; forgetRemote — снова с флешки; без обработчика
// — не открывается; полный учёт отдаёт места чужих приложений; плохие имена — отказ; после clearRemote —
// нет; новый учёт не отдаёт блок прежнего файла под тем же номером.
bool remoteRegistry() {
  removeAll();
  AppDataFiles files(g_root.c_str());
  const std::vector<uint8_t> local = pattern(5000, 1);
  if (!deliver(files, "Q2", "PAK0.PAK", local, fatStamp(2026, 1, 1, 0, 0), {})) {
    return fail("remote_registry", "delivery");
  }
  // PAK1.PAK — только на карте Evo (на учёте): без обработчика не открывается, с ним — у Z80
  const std::vector<uint8_t> card = pattern(9000, 2);
  if (!files.addRemote("Q2", "PAK1.PAK", static_cast<uint32_t>(card.size()), 0, 0)) {
    return fail("remote_registry", "addRemote");
  }
  void* file = nullptr;
  if (files.open("Q2", "PAK1.PAK", &file, nullptr) != AppDataFiles::kNotFound) {
    return fail("remote_registry", "a registered file opens without a fetch handler");
  }
  std::vector<uint8_t> block(AppDataFiles::kRemoteBlock);
  FakeZ80 z;
  z.files[0] = &card;
  files.setFetch(fakeFetch, &z, block.data());
  uint32_t size = 0;
  std::vector<uint8_t> out(9000);
  uint32_t got = 0;
  if (files.open("Q2", "PAK1.PAK", &file, &size) != AppDataFiles::kOk || size != card.size() ||
      files.read(file, 10, out.data(), 100, &got) != AppDataFiles::kOk || memcmp(out.data(), card.data() + 10, 100) != 0) {
    return fail("remote_registry", "registered file is not read from the Z80");
  }
  files.close(file);
  // 0.1.175: PAK0.PAK и на флешке, и на учёте (другой длины) — читается флешка
  files.addRemote("Q2", "PAK0.PAK", static_cast<uint32_t>(card.size()), 0, 1);
  if (files.open("Q2", "PAK0.PAK", &file, &size) != AppDataFiles::kOk || size != local.size() ||
      files.read(file, 10, out.data(), 100, &got) != AppDataFiles::kOk || memcmp(out.data(), local.data() + 10, 100) != 0) {
    return fail("remote_registry", "the drive copy does not go before the registration");
  }
  files.close(file);
  files.forgetRemote("q2", "pak1.pak");
  if (files.open("Q2", "PAK1.PAK", &file, &size) != AppDataFiles::kNotFound) {
    return fail("remote_registry", "a file opens after forgetRemote");
  }
  // другой файл под тем же номером: блок прежнего не годится
  const std::vector<uint8_t> other = pattern(9000, 99);
  files.addRemote("Q2", "PAK1.PAK", static_cast<uint32_t>(card.size()), 0, 0);
  if (files.open("Q2", "PAK1.PAK", &file, nullptr) != AppDataFiles::kOk ||
      files.read(file, 20, out.data(), 50, &got) != AppDataFiles::kOk) {
    return fail("remote_registry", "reopen");
  }
  z.files[0] = &other;
  files.addRemote("Q2", "PAK1.PAK", static_cast<uint32_t>(other.size()), 0, 0);
  if (files.read(file, 30, out.data(), 50, &got) != AppDataFiles::kOk ||
      memcmp(out.data(), other.data() + 30, 50) != 0) {
    return fail("remote_registry", "the block of the previous registration was reused");
  }
  files.closeAll();
  // плохие имена
  if (files.addRemote("Q2/..", "PAK0.PAK", 10, 0, 0) || files.addRemote("Q2", "WRITING.TMP", 10, 0, 0) ||
      files.addRemote("Q2", "PAK0.PAK", 0, 0, 0) || files.addRemote("TOOLONGAPP", "A.B", 10, 0, 0)) {
    return fail("remote_registry", "bad names are registered");
  }
  // полный учёт: места другого приложения уходят, своих больше kMaxRemote — нет
  files.clearRemote();
  char name[13];
  for (size_t i = 0; i < AppDataFiles::kMaxRemote; ++i) {
    snprintf(name, sizeof(name), "F%u.DAT", static_cast<unsigned>(i));
    if (!files.addRemote("DOOM", name, 10, 0, static_cast<uint8_t>(i))) {
      return fail("remote_registry", "registry smaller than kMaxRemote");
    }
  }
  for (size_t i = 0; i < AppDataFiles::kMaxRemote; ++i) {
    snprintf(name, sizeof(name), "G%u.DAT", static_cast<unsigned>(i));
    if (!files.addRemote("Q2", name, 10, 0, static_cast<uint8_t>(i))) {
      return fail("remote_registry", "other app's entries are not evicted");
    }
  }
  if (files.addRemote("Q2", "EXTRA.DAT", 10, 0, 9)) {
    return fail("remote_registry", "more than kMaxRemote files of one app");
  }
  if (files.open("DOOM", "F0.DAT", &file, nullptr) != AppDataFiles::kNotFound) {
    return fail("remote_registry", "evicted entry still opens");
  }
  files.clearRemote();
  if (files.open("Q2", "G0.DAT", &file, nullptr) != AppDataFiles::kNotFound) {
    return fail("remote_registry", "entry opens after clearRemote");
  }
  return true;
}

// Один запрос и куски: свой номер, ровно за принятым, не за длиной; без данных — отказ; после end — мимо.
bool remoteRequestPieces() {
  using zifi::AppDataRequest;
  std::vector<uint8_t> dest(3000, 0);
  const std::vector<uint8_t> data = pattern(3000, 5);
  FakeZ80 z;
  z.request.begin(7, 4096, 3000, dest.data());
  auto send = [&](uint8_t seq, uint32_t at, const uint8_t* bytes, size_t length) {
    return sendPiece(z, seq, at, bytes, length);
  };
  if (send(8, 4096, data.data(), 100) != AppDataRequest::kIgnored ||
      send(7, 4097, data.data(), 100) != AppDataRequest::kIgnored ||
      send(7, 4096, data.data(), 3001) != AppDataRequest::kIgnored) {
    return fail("remote_request_pieces", "foreign, misplaced or too long piece taken");
  }
  if (send(7, 4096, data.data(), 1016) != AppDataRequest::kTaken ||
      send(7, 4096 + 1016, data.data() + 1016, 1984) != AppDataRequest::kDone || !z.request.complete() ||
      memcmp(dest.data(), data.data(), 3000) != 0) {
    return fail("remote_request_pieces", "two pieces");
  }
  z.request.begin(9, 0, 512, dest.data());
  if (send(9, 0, nullptr, 0) != AppDataRequest::kFailed || !z.request.failed() || z.request.complete()) {
    return fail("remote_request_pieces", "empty piece");
  }
  z.request.begin(10, 0, 512, dest.data());
  z.request.end();
  if (send(10, 0, data.data(), 512) != AppDataRequest::kIgnored) {
    return fail("remote_request_pieces", "piece after end taken");
  }
  uint8_t event[AppDataRequest::kEventBytes];
  AppDataRequest::event(event, 0xAB, 3, 0x12345600, 32768);
  const uint8_t want[] = {0xAB, 3, 0x00, 0x56, 0x34, 0x12, 0x00, 0x80};
  if (memcmp(event, want, sizeof(want)) != 0) {
    return fail("remote_request_pieces", "event layout");
  }
  return true;
}

// --- поиск на флешке (0.1.175) ---------------------------------------------------------------------

namespace fs = std::filesystem;

// Положить файл rel (путь от корня, через «/») с содержимым data; папки создаются.
bool putFile(const char* rel, const std::vector<uint8_t>& data) {
  const fs::path path = fs::u8path(g_root) / fs::u8path(rel);
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  FILE* out = fopen(path.string().c_str(), "wb");
  if (out == nullptr) {
    return false;
  }
  const bool ok = fwrite(data.data(), 1, data.size(), out) == data.size();
  return fclose(out) == 0 && ok;
}

void removeTree(const char* rel) {
  std::error_code error;
  fs::remove_all(fs::u8path(g_root) / fs::u8path(rel), error);
}

struct LogLines {
  std::vector<std::string> lines;
  size_t count(const char* part) const {
    size_t n = 0;
    for (const std::string& line : lines) {
      n += line.find(part) != std::string::npos ? 1 : 0;
    }
    return n;
  }
};

void keepLine(void* context, const char* line) { static_cast<LogLines*>(context)->lines.push_back(line); }

// Открыть name приложения Q2 и прочитать целиком: совпало с want — true.
bool opensAs(AppDataFiles& files, const char* name, const std::vector<uint8_t>& want) {
  void* file = nullptr;
  uint32_t size = 0;
  if (files.open("Q2", name, &file, &size) != AppDataFiles::kOk) {
    return false;
  }
  std::vector<uint8_t> out(size);
  uint32_t got = 0;
  const bool same = size == want.size() &&
                    files.read(file, 0, out.data(), size, &got) == AppDataFiles::kOk && got == size &&
                    memcmp(out.data(), want.data(), size) == 0;
  files.close(file);
  return same;
}

bool notFound(AppDataFiles& files, const char* name) {
  void* file = nullptr;
  return files.open("Q2", name, &file, nullptr) == AppDataFiles::kNotFound;
}

// Файл ищется в папке приложения и во всех вложенных в корень на любой глубине (регистр не важен);
// первая папка, где нашёлся файл, — «домашняя»: остальные файлы — только из неё; найденное и
// ненайденное помнится до closeAll; без флешки (том 0) не ищется; в журнал — строка на файл.
bool driveSearch() {
  removeAll();
  for (const char* dir : {"Q2", "AAA", "X", "MUSIC"}) {
    removeTree(dir);
  }
  const std::vector<uint8_t> deep0 = pattern(3000, 5);
  const std::vector<uint8_t> deep1 = pattern(2000, 6);
  const std::vector<uint8_t> app0 = pattern(1500, 7);
  const std::vector<uint8_t> demo0 = pattern(1000, 8);
  if (!putFile("X/Y/Z/BASEQ2/PAK0.PAK", deep0) || !putFile("X/Y/Z/BASEQ2/pak1.pak", deep1) ||
      !putFile("MUSIC/TRACK.MOD", pattern(100, 9))) {
    return fail("drive_search", "setup");
  }
  AppDataFiles files(g_root.c_str());
  LogLines log;
  files.setLog(keepLine, &log);
  // 1. глубоко вложенная папка; имя на флешке строчными — тоже находится
  if (!opensAs(files, "PAK0.PAK", deep0) || !opensAs(files, "pak1.pak", deep1)) {
    return fail("drive_search", "files four folders deep are not read");
  }
  // 2. одна строка на файл за запуск, как ни открывай
  for (int i = 0; i < 5; ++i) {
    opensAs(files, "PAK0.PAK", deep0);
  }
  if (log.count("data file Q2/PAK0.PAK: USB drive") != 1 || log.count("data file Q2/PAK1.PAK: USB drive") != 1 ||
      log.count("data files of Q2: USB drive folder") != 1 || log.count("BASEQ2") < 3) {
    return fail("drive_search", "not one log line per file");
  }
  // 3. папка приложения — первая; домашняя — Q2: PAK1.PAK из другой папки не берётся
  if (!putFile("Q2/PAK0.PAK", app0)) {
    return fail("drive_search", "setup Q2");
  }
  files.closeAll();
  if (!opensAs(files, "PAK0.PAK", app0) || !notFound(files, "PAK1.PAK")) {
    return fail("drive_search", "the app folder is not first or files are mixed across folders");
  }
  // 4. по алфавиту: AAA раньше X; домашняя — AAA, PAK1.PAK из X не смешивается с её PAK0.PAK
  removeTree("Q2");
  if (!putFile("AAA/PAK0.PAK", demo0)) {
    return fail("drive_search", "setup AAA");
  }
  files.closeAll();
  if (!opensAs(files, "PAK0.PAK", demo0) || !notFound(files, "PAK1.PAK")) {
    return fail("drive_search", "folders are not taken in order or files are mixed across folders");
  }
  // 5. ненайденное помнится до closeAll: новый файл виден только в следующем запуске
  if (!notFound(files, "PAK5.PAK") || !putFile("AAA/PAK5.PAK", demo0) || !notFound(files, "PAK5.PAK")) {
    return fail("drive_search", "a missing file is searched again in the same run");
  }
  files.closeAll();
  if (!opensAs(files, "PAK5.PAK", demo0)) {
    return fail("drive_search", "after closeAll the new file is not found");
  }
  // 6. флешки нет (том 0) — не ищется; вставили (новый номер) — снова
  files.closeAll();
  files.setVolume(0);
  if (!notFound(files, "PAK0.PAK")) {
    return fail("drive_search", "searched without a drive");
  }
  files.setVolume(7);
  if (!opensAs(files, "PAK0.PAK", demo0)) {
    return fail("drive_search", "not found after the drive came back");
  }
  // 7. папка с именем файла — не файл
  files.closeAll();
  removeTree("AAA");
  std::error_code error;
  fs::create_directories(fs::u8path(g_root) / "Q2" / "PAK2.PAK", error);
  if (!notFound(files, "PAK2.PAK")) {
    return fail("drive_search", "a folder named like the file is opened");
  }
  // 8. файла нет нигде — одна строка в журнал
  log.lines.clear();
  notFound(files, "PAK9.PAK");
  notFound(files, "PAK9.PAK");
  if (log.count("data file Q2/PAK9.PAK: neither on the USB drive nor registered") != 1) {
    return fail("drive_search", "a missing file is logged more than once");
  }
  files.closeAll();
  for (const char* dir : {"Q2", "AAA", "X", "MUSIC"}) {
    removeTree(dir);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: app_data_files_host <folder>\n");
    return 2;
  }
  g_root = argv[1];
  struct Scenario {
    const char* name;
    bool (*run)();
  };
  const Scenario scenarios[] = {
      {"delivered_file_reads_back", deliveredFileReadsBack},
      {"put_out_of_order_is_refused", putOutOfOrderIsRefused},
      {"incomplete_delivery_leaves_nothing", incompleteDeliveryLeavesNothing},
      {"newer_card_file_is_sent_again", newerCardFileIsSentAgain},
      {"room_counts_the_old_copy", roomCountsTheOldCopy},
      {"bad_names_are_refused", badNamesAreRefused},
      {"interleaved_handles_read_their_files", interleavedHandlesReadTheirFiles},
      {"handles_run_out", handlesRunOut},
      {"remote_reads_match_the_card", remoteReadsMatchTheCard},
      {"remote_registry", remoteRegistry},
      {"remote_request_pieces", remoteRequestPieces},
      {"drive_search", driveSearch},
  };
  for (const Scenario& scenario : scenarios) {
    if (!scenario.run()) {
      return 1;
    }
    printf("ok %s\n", scenario.name);
  }
  removeAll();
  return 0;
}
