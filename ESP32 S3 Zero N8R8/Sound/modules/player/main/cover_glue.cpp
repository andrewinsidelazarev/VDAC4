// Обложка и метки MP3 в PLAYER.ELF (player_cover.h): CoverParser и CoverThumb
// прошивки, сведение JPEG — в своей задаче, как в AudioApp прошивки.
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
#include "zifi/cover_art.hpp"

using zifi::CoverParser;
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
uint32_t generation = 0;               // номер файла, под lock
uint8_t* image = nullptr;              // картинка к сведению, под lock
size_t imageSize = 0;
uint32_t imageGeneration = 0;
uint8_t* picture = nullptr;            // картинка файлом, собирается (ядро обмена)
uint32_t pictureTotal = 0;
uint32_t pictureReceived = 0;

void* alloc(size_t size) { return core->alloc(size, ZM_MEM_PSRAM); }

void release(void* pointer) {
  if (pointer != nullptr) {
    core->free(pointer);
  }
}

void coverYield() { core->delay(1); }

void coverTask(void*) {
  while (!stopping) {
    if (core->signal_wait(wake, 200) != ZM_OK) {
      continue;
    }
    core->mutex_lock(lock);
    uint8_t* picture = image;
    const size_t size = imageSize;
    const uint32_t pictureGeneration = imageGeneration;
    image = nullptr;
    // A queued replacement must not clear cancellation while the previous
    // decoder is still running. Only this worker starts a new decode.
    if (picture != nullptr && !stopping) cancel = false;
    core->mutex_unlock(lock);
    if (picture == nullptr) {
      continue;
    }
    const uint32_t started = core->millis();
    uint8_t* work = static_cast<uint8_t*>(alloc(CoverThumb::kBytes));
    zifi::CoverDecodeInfo info;
    int code = 1;                      // нет памяти
    if (work != nullptr) {
      CoverThumb sprite(alloc, release);
      code = zifi::decodeCoverJpeg(picture, size, sprite, info, &cancel, coverYield);
      if (code == 0) {
        sprite.finish(work);
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
    }
    core->mutex_unlock(lock);
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
  taskDone = true;
}

// Собранная картинка — задаче обложки.
void decodeLater(uint8_t* data, size_t size) {
  core->mutex_lock(lock);
  release(image);
  image = data;
  imageSize = size;
  imageGeneration = ++generation;
  cancel = true;
  thumbState = THUMB_PENDING;
  core->mutex_unlock(lock);
  core->signal_give(wake);
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
  if (core->task_start(coverTask, nullptr, 6144, 1, 0) != ZM_OK) {
    taskDone = true;
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
  for (int wait = 0; wait < 50 && !taskDone; ++wait) {
    core->delay(10);
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
