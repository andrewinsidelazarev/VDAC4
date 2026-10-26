// Обложка и метки MP3 в PLAYER.ELF (player_cover.h): CoverParser и CoverThumb
// прошивки. Задача обложек ведёт экран на HDMI Tang (screen.cpp): шаг раз в 20 мс. Сведение JPEG
// (у картинки 1000×1000 — секунды) — разовой задачей ниже всех (приоритет 0, 1.29): экран и звук
// его не ждут, а её стек внутренней памяти ESP занят, только пока она сводит.
//
// Разбор (cover_feed) и метки идут на ядре обмена, из команд Z80; задача
// обложки сводит собранную картинку. Новый файл (cover_reset) прерывает
// сведение прежней: её миниатюра уже не нужна.
//
// В модуле ESP нет конструкторов статических объектов: разбор живёт в
// статическом буфере и создаётся в cover_begin.
#include <new>
#include <string.h>

#include "player_cover.h"
#include "screen.h"
#include "zifi/cover_art.hpp"

using zifi::CoverParser;
using zifi::CoverPicture;
using zifi::CoverThumb;

namespace {

const zm_core_t* core = nullptr;
alignas(CoverParser) unsigned char parserSpace[sizeof(CoverParser)];
CoverParser* parser = nullptr;
void* lock = nullptr;
void* wake = nullptr;
uint8_t* thumb = nullptr;              // готовая миниатюра, под lock
volatile uint8_t thumbState = THUMB_NONE;
volatile bool cancel = false;          // сведение прежней картинки — прочь
volatile bool stopping = false;
volatile bool taskDone = true;
volatile bool jobRunning = false;      // разовая задача сведения работает
uint32_t generation = 0;               // номер файла, под lock
uint8_t* image = nullptr;              // картинка к сведению, под lock
size_t imageSize = 0;
uint32_t imageGeneration = 0;
uint8_t* picture = nullptr;            // картинка файлом, собирается (ядро обмена)
uint32_t pictureTotal = 0;
uint32_t pictureReceived = 0;
uint32_t fedBytes = 0;                 // байтов метки прислал плагин к этому файлу (ядро обмена)
uint16_t* uiCover = nullptr;           // обложка экрана CoverPicture::kSide², под lock
uint16_t uiSerial = 0;                 // её номер; 0 — нет (под lock)
uint16_t uiCounter = 0;

void* alloc(size_t size) { return core->alloc(size, ZM_MEM_PSRAM); }

void release(void* pointer) {
  if (pointer != nullptr) {
    core->free(pointer);
  }
}

void coverYield() { core->delay(1); }

// Экран оверлеем Tang есть (ядро ABI 1.5 и Tang с оверлеями): обложку и для него.
bool uiWanted() {
  return core->size >= offsetof(zm_core_t, ui_available) + sizeof(core->ui_available) &&
         core->ui_available != nullptr && core->ui_available() != 0;
}

// Свести одну картинку из очереди (image), если она есть. Выход: false — очередь пуста.
bool decodeNext() {
  core->mutex_lock(lock);
  uint8_t* picture = image;
  const size_t size = imageSize;
  const uint32_t pictureGeneration = imageGeneration;
  image = nullptr;
  // A queued replacement must not clear cancellation while the previous
  // decoder is still running. Only one decoder (the job) runs at a time.
  if (picture != nullptr && !stopping) cancel = false;
  core->mutex_unlock(lock);
  if (picture == nullptr) {
    return false;
  }
  {
    const uint32_t started = core->millis();
    uint8_t* work = static_cast<uint8_t*>(alloc(CoverThumb::kBytes));
    zifi::CoverDecodeInfo info;
    int code = 1;                      // нет памяти
    // Обложка экрана — в своём буфере (живёт до выгрузки): сводится в work2, отдаётся под lock
    const bool forUi = uiWanted();
    if (forUi && uiCover == nullptr) {
      uiCover = static_cast<uint16_t*>(
          alloc(sizeof(uint16_t) * CoverPicture::kSide * CoverPicture::kSide));
    }
    uint16_t* uiWork = forUi && uiCover != nullptr
        ? static_cast<uint16_t*>(alloc(sizeof(uint16_t) * CoverPicture::kSide * CoverPicture::kSide))
        : nullptr;
    bool uiDone = false;
    if (work != nullptr) {
      CoverThumb sprite(alloc, release);
      CoverPicture big(alloc, release);
      if (uiWork != nullptr) {
        sprite.attach(&big);
      }
      code = zifi::decodeCoverJpeg(picture, size, sprite, info, &cancel, coverYield);
      if (code == 0) {
        sprite.finish(work);
        uiDone = uiWork != nullptr && big.finish(uiWork);
      }
    }
    release(picture);
    core->mutex_lock(lock);
    const bool current = pictureGeneration == generation;
    if (current) {
      if (code == 0) {
        memcpy(thumb, work, CoverThumb::kBytes);
      }
      thumbState = code == 0 ? THUMB_READY : THUMB_NONE;
      if (uiDone) {
        memcpy(uiCover, uiWork, sizeof(uint16_t) * CoverPicture::kSide * CoverPicture::kSide);
        if (++uiCounter == 0) uiCounter = 1;
        uiSerial = uiCounter;
      }
    }
    core->mutex_unlock(lock);
    release(uiWork);
    release(work);
    if (code == 0) {
      core->log("player: cover JPEG %ux%u, %u KB, scale 1/%u, %u ms%s", (unsigned)info.width,
                (unsigned)info.height, (unsigned)(size >> 10), 1u << info.scale,
                (unsigned)(core->millis() - started), current ? "" : " (file changed)");
    } else {
      // 1xx — ROM prepare, 2xx — ROM decode/cancel, 3xx+ — software JPEG.
      // 1 и 2 — нет памяти.
      core->log("player: cover JPEG %ux%u, %u KB not decoded: code %d", (unsigned)info.width,
                (unsigned)info.height, (unsigned)(size >> 10), code);
    }
  }
  return true;
}

bool claimDecoder();

// Разовая задача сведения: всё, что пришло в очередь, пока она работает, — тоже она.
void decodeJob(void*) {
  while (!stopping && decodeNext()) {
  }
  jobRunning = false;
}

void coverTask(void*) {
  while (!stopping) {
    if (core->signal_wait(wake, screen_tick(core->millis())) != ZM_OK && image == nullptr) {
      continue;
    }
    if (image == nullptr || stopping || !claimDecoder()) {
      continue;
    }
    // Ядро 0, приоритет 0 — ниже экрана (1) и декодера (5); стека нет — сводит эта задача сама
    if (core->task_start(decodeJob, nullptr, 6144, 0, 0) != ZM_OK) {
      decodeNext();
      jobRunning = false;
    }
  }
  taskDone = true;
}

// Собранная картинка — в очередь сведения; wakeTask — разбудить задачу обложки (она заведёт
// разовую задачу сведения).
void decodeLater(uint8_t* data, size_t size, bool wakeTask = true) {
  core->mutex_lock(lock);
  release(image);
  image = data;
  imageSize = size;
  imageGeneration = ++generation;
  cancel = true;
  thumbState = THUMB_PENDING;
  core->mutex_unlock(lock);
  if (wakeTask) core->signal_give(wake);
}

// Сводить может только один: задача сведения или тот, кто её заменяет. true — теперь я.
bool claimDecoder() {
  core->mutex_lock(lock);
  const bool mine = !jobRunning;
  if (mine) jobRunning = true;
  core->mutex_unlock(lock);
  return mine;
}

void dropPicture() {
  release(picture);
  picture = nullptr;
  pictureTotal = 0;
  pictureReceived = 0;
}

void copyField(char* out, size_t size, const char* text) {
  strncpy(out, text, size - 1);
  out[size - 1] = 0;
}

}  // namespace

extern "C" int cover_begin(const zm_core_t* c) {
  core = c;
  lock = core->mutex_create();
  wake = core->signal_create();
  thumb = static_cast<uint8_t*>(alloc(CoverThumb::kBytes));
  if (lock == nullptr || wake == nullptr || thumb == nullptr) {
    return ZM_NO_MEMORY;
  }
  parser = new (parserSpace) CoverParser(alloc, release);
  stopping = false;
  taskDone = false;
  if (screen_begin(core) != ZM_OK) {
    core->log("player: no screen on Tang (core older than ABI 1.5)");
  }
  // Ядро 0, приоритет 1: на ядре 1 обмен с Evo — loopTask прошивки с приоритетом 1, и задача
  // выше него (1.26: ядро 1, приоритет 2) на время показа экрана (весь холст — 115 200 слов в
  // Tang) и сведения JPEG его останавливала: плагин не дожидался ответа (ESP не отвечает).
  // На ядре 0 выше этой задачи — декодер (приоритет 5).
  if (core->task_start(coverTask, nullptr, 6144, 1, 0) != ZM_OK) {
    taskDone = true;
    screen_end();
    parser->~CoverParser();
    parser = nullptr;
    return ZM_ERROR;
  }
  return ZM_OK;
}

extern "C" void cover_end(void) {
  if (parser == nullptr) {
    return;
  }
  core->mutex_lock(lock);
  stopping = true;
  cancel = true;
  core->mutex_unlock(lock);
  core->signal_give(wake);
  for (int wait = 0; wait < 100 && (!taskDone || jobRunning); ++wait) {
    core->delay(10);
  }
  if (taskDone && !jobRunning) {
    screen_end();                        // задача вышла: экран — прочь (иначе его снимет ядро)
  }
  core->mutex_lock(lock);
  release(image);
  image = nullptr;
  core->mutex_unlock(lock);
  dropPicture();
  parser->~CoverParser();
  parser = nullptr;
}

extern "C" void cover_reset(void) {
  if (parser == nullptr) {
    return;
  }
  core->mutex_lock(lock);
  ++generation;
  cancel = true;
  thumbState = THUMB_NONE;
  uiSerial = 0;
  fedBytes = 0;
  release(image);
  image = nullptr;
  core->mutex_unlock(lock);
  dropPicture();
  parser->reset();
}

extern "C" uint8_t cover_feed(uint32_t offset, const uint8_t* data, size_t length,
                              uint32_t* want) {
  if (parser == nullptr) {
    *want = 0;
    return COVER_NONE;
  }
  const bool wanted = parser->result() == CoverParser::kWant;
  fedBytes += static_cast<uint32_t>(length);
  if (offset == 0 && wanted) {
    core->log("player: cover: tag from the file, %u bytes in the first piece", (unsigned)length);
  }
  const CoverParser::Result result = parser->feed(offset, data, length);
  if (wanted && result == CoverParser::kReceived) {
    size_t size = 0;
    uint8_t* taken = parser->takeImage(size);
    if (taken != nullptr) {
      decodeLater(taken, size);
    }
  } else if (wanted && result == CoverParser::kNone) {
    core->log("player: cover: none (%s)", parser->reason());
  }
  if (wanted && result != CoverParser::kWant) {
    const zifi::CoverTags& tags = parser->tags();
    core->log("player: tags: title %u, artist %u, album %u, year %s", (unsigned)strlen(tags.title),
              (unsigned)strlen(tags.artist), (unsigned)strlen(tags.album), tags.year);
  }
  *want = parser->want();
  return static_cast<uint8_t>(result);
}

extern "C" uint8_t cover_picture(uint32_t total, uint32_t offset, const uint8_t* data,
                                 size_t length, uint32_t* received) {
  *received = 0;
  if (parser == nullptr || total < 4 || total > COVER_PICTURE_MAX) {
    return COVER_NONE;
  }
  if (offset == 0) {
    core->log("player: cover: file from the folder, %u bytes", (unsigned)total);
    dropPicture();
    picture = static_cast<uint8_t*>(alloc(total));
    if (picture == nullptr) {
      core->log("player: cover file of %u bytes: no memory", (unsigned)total);
      return COVER_NONE;
    }
    pictureTotal = total;
  }
  if (picture == nullptr || total != pictureTotal || offset != pictureReceived ||
      length > total - pictureReceived) {
    *received = pictureReceived;
    return picture == nullptr ? COVER_NONE : COVER_WANT;   // не то место: Z80 пришлёт заново
  }
  memcpy(picture + offset, data, length);
  pictureReceived += static_cast<uint32_t>(length);
  *received = pictureReceived;
  if (pictureReceived < pictureTotal) {
    return COVER_WANT;
  }
  if (picture[0] != 0xFF || picture[1] != 0xD8 || picture[2] != 0xFF) {
    core->log("player: cover file of %u bytes is not JPEG", (unsigned)total);
    dropPicture();
    return COVER_NONE;
  }
  uint8_t* done = picture;
  const size_t size = pictureTotal;
  picture = nullptr;
  pictureTotal = pictureReceived = 0;
  decodeLater(done, size);
  return COVER_RECEIVED;
}

extern "C" int cover_image(const uint8_t* data, size_t size) {
  if (parser == nullptr || size < 4 || size > COVER_PICTURE_MAX) {
    return 0;
  }
  if (data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF) {
    core->log("player: cover from the decoder, %u bytes: not JPEG", (unsigned)size);
    return 0;
  }
  uint8_t* copy = static_cast<uint8_t*>(alloc(size));
  if (copy == nullptr) {
    core->log("player: cover from the decoder, %u bytes: no memory", (unsigned)size);
    return 0;
  }
  memcpy(copy, data, size);
  core->log("player: cover from the decoder, %u bytes", (unsigned)size);
  decodeLater(copy, size);
  return 1;
}

// Картинка из сети — сводит та же задача поиска (online_job: ядро 0, приоритет 0): её стек уже есть,
// а на вторую задачу внутренней памяти, пока жива эта, не хватало (1.29 на плате).
extern "C" int cover_image_here(const uint8_t* data, size_t size) {
  if (parser == nullptr || size < 4 || size > COVER_PICTURE_MAX) {
    return 0;
  }
  if (data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF) {
    core->log("player: cover from the network, %u bytes: not JPEG", (unsigned)size);
    return 0;
  }
  uint8_t* copy = static_cast<uint8_t*>(alloc(size));
  if (copy == nullptr) {
    core->log("player: cover from the network, %u bytes: no memory", (unsigned)size);
    return 0;
  }
  memcpy(copy, data, size);
  const bool mine = claimDecoder();
  decodeLater(copy, size, !mine);        // сводит другой — пусть его разбудят
  if (mine) {
    while (!stopping && decodeNext()) {
    }
    jobRunning = false;
  }
  return 1;
}

extern "C" uint8_t cover_thumb(uint8_t part, uint8_t* out) {
  part = static_cast<uint8_t>(part % COVER_THUMB_PARTS);
  if (parser == nullptr) {
    return THUMB_NONE;
  }
  core->mutex_lock(lock);
  const uint8_t state = thumbState;
  if (state == THUMB_READY) {
    memcpy(out, thumb + part * COVER_THUMB_PART, COVER_THUMB_PART);
  }
  core->mutex_unlock(lock);
  return state;
}

extern "C" uint32_t cover_epoch(void) {
  if (parser == nullptr) {
    return 0;
  }
  core->mutex_lock(lock);
  const uint32_t epoch = generation;
  core->mutex_unlock(lock);
  return epoch;
}

extern "C" int cover_present(void) {
  if (parser == nullptr) {
    return 1;                            // обложек нет вовсе — и из сети не искать
  }
  core->mutex_lock(lock);
  const bool busy = thumbState != THUMB_NONE || image != nullptr;
  core->mutex_unlock(lock);
  // Метку плагин ещё досылает (картинка в ней может быть), картинка из папки идёт
  const bool coming = (fedBytes != 0 && parser->result() == CoverParser::kWant) || picture != nullptr;
  return busy || coming ? 1 : 0;
}

extern "C" void cover_ui_clear(void) {
  if (parser == nullptr) {
    return;
  }
  core->mutex_lock(lock);
  uiSerial = 0;
  core->mutex_unlock(lock);
}

extern "C" uint16_t cover_ui_serial(void) {
  if (parser == nullptr) {
    return 0;
  }
  core->mutex_lock(lock);
  const uint16_t serial = uiSerial;
  core->mutex_unlock(lock);
  return serial;
}

// Обложка экрана, сведённая по площади к side×side (side ≤ kSide; больше — ближайшей точкой).
extern "C" int cover_ui_copy(uint16_t* out, uint16_t side) {
  if (parser == nullptr || out == nullptr || side == 0) {
    return 0;
  }
  constexpr uint32_t kSide = CoverPicture::kSide;
  core->mutex_lock(lock);
  const bool have = uiSerial != 0 && uiCover != nullptr;
  if (have) {
    for (uint32_t oy = 0; oy < side; ++oy) {
      uint32_t y0 = oy * kSide / side;
      uint32_t y1 = (oy + 1) * kSide / side;
      if (y1 <= y0) y1 = y0 + 1;
      for (uint32_t ox = 0; ox < side; ++ox) {
        uint32_t x0 = ox * kSide / side;
        uint32_t x1 = (ox + 1) * kSide / side;
        if (x1 <= x0) x1 = x0 + 1;
        uint32_t red = 0, green = 0, blue = 0;
        for (uint32_t y = y0; y < y1; ++y) {
          for (uint32_t x = x0; x < x1; ++x) {
            const uint16_t p = uiCover[y * kSide + x];
            red += p >> 11;
            green += (p >> 5) & 63;
            blue += p & 31;
          }
        }
        const uint32_t n = (y1 - y0) * (x1 - x0);
        out[oy * side + ox] = static_cast<uint16_t>((((red + n / 2) / n) << 11) |
                                                    (((green + n / 2) / n) << 5) |
                                                    ((blue + n / 2) / n));
      }
    }
  }
  core->mutex_unlock(lock);
  return have ? 1 : 0;
}

extern "C" void cover_tags(player_tags_t* out) {
  memset(out, 0, sizeof(*out));
  if (parser == nullptr) {
    return;
  }
  const zifi::CoverTags& tags = parser->tags();
  copyField(out->title, sizeof(out->title), tags.title);
  copyField(out->artist, sizeof(out->artist), tags.artist);
  copyField(out->album, sizeof(out->album), tags.album);
  copyField(out->year, sizeof(out->year), tags.year);
  copyField(out->track, sizeof(out->track), tags.track);
}
