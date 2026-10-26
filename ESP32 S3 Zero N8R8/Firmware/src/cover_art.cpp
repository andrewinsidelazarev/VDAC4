#include "zifi/cover_art.hpp"

#include <string.h>

namespace zifi {
namespace {

constexpr uint32_t kHeaderBytes = 10;  // и заголовок метки, и заголовок кадра

uint32_t readBe32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

// Четыре байта по семь значащих битов. false — старший бит выставлен.
bool readSyncsafe(const uint8_t* data, uint32_t& value) {
  if (((data[0] | data[1] | data[2] | data[3]) & 0x80) != 0) {
    return false;
  }
  value = (static_cast<uint32_t>(data[0]) << 21) | (static_cast<uint32_t>(data[1]) << 14) |
          (static_cast<uint32_t>(data[2]) << 7) | data[3];
  return true;
}

// Символ Юникода — в CP866; 0 — пропустить (управляющий).
uint8_t toCp866(uint32_t code) {
  if (code < 0x20) {
    return 0;
  }
  if (code < 0x7F) {
    return static_cast<uint8_t>(code);
  }
  if (code >= 0x410 && code <= 0x43F) {
    return static_cast<uint8_t>(0x80 + (code - 0x410));       // А..п
  }
  if (code >= 0x440 && code <= 0x44F) {
    return static_cast<uint8_t>(0xE0 + (code - 0x440));       // р..я
  }
  if (code == 0x401) {
    return 0xF0;                                              // Ё
  }
  if (code == 0x451) {
    return 0xF1;                                              // ё
  }
  if (code == 0xA0) {
    return ' ';
  }
  return '?';
}

// Байт однобайтовой строки ID3: выше 0x7F — CP1251.
uint32_t fromCp1251(uint8_t byte) {
  if (byte < 0x80) {
    return byte;
  }
  if (byte >= 0xC0) {
    return 0x410 + (byte - 0xC0);
  }
  if (byte == 0xA8) {
    return 0x401;
  }
  if (byte == 0xB8) {
    return 0x451;
  }
  return 0xFFFD;
}

}  // namespace

void decodeId3Text(const uint8_t* body, size_t length, char* out, size_t size) {
  if (size == 0) {
    return;
  }
  size_t used = 0;
  auto emit = [&](uint32_t code) {
    const uint8_t c = toCp866(code);
    if (c != 0 && used + 1 < size) {
      out[used++] = static_cast<char>(c);
    }
  };
  if (body != nullptr && length > 1) {
    const uint8_t encoding = body[0];
    const uint8_t* text = body + 1;
    size_t count = length - 1;
    if (encoding == 1 || encoding == 2) {
      bool little = encoding == 1;
      size_t index = 0;
      if (encoding == 1 && count >= 2) {
        if (text[0] == 0xFF && text[1] == 0xFE) {
          index = 2;
        } else if (text[0] == 0xFE && text[1] == 0xFF) {
          little = false;
          index = 2;
        }
      }
      for (; index + 1 < count; index += 2) {
        const uint32_t unit = little ? (text[index] | (text[index + 1] << 8))
                                     : ((text[index] << 8) | text[index + 1]);
        if (unit == 0) {
          break;                       // в 2.4 дальше может быть вторая строка
        }
        emit(unit >= 0xD800 && unit <= 0xDFFF ? 0xFFFD : unit);
      }
    } else if (encoding == 3) {
      for (size_t index = 0; index < count && text[index] != 0;) {
        const uint8_t lead = text[index];
        uint32_t code = 0xFFFD;
        size_t extra = 0;
        if (lead < 0x80) {
          code = lead;
        } else if ((lead & 0xE0) == 0xC0) {
          code = lead & 0x1F;
          extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
          code = lead & 0x0F;
          extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
          code = lead & 0x07;
          extra = 3;
        }
        ++index;
        for (size_t k = 0; k < extra; ++k, ++index) {
          if (index >= count || (text[index] & 0xC0) != 0x80) {
            code = 0xFFFD;
            break;
          }
          code = (code << 6) | (text[index] & 0x3F);
        }
        emit(code);
      }
    } else {
      for (size_t index = 0; index < count && text[index] != 0; ++index) {
        emit(fromCp1251(text[index]));
      }
    }
  }
  while (used > 0 && out[used - 1] == ' ') {
    --used;
  }
  out[used] = 0;
}

void CoverParser::reset() {
  if (frame_ != nullptr) {
    release_(frame_);
  }
  frame_ = nullptr;
  image_ = nullptr;
  imageSize_ = 0;
  phase_ = kHeader;
  result_ = kWant;
  reason_ = "";
  version_ = 0;
  want_ = 0;
  tagEnd_ = 0;
  frameEnd_ = 0;
  dataStart_ = 0;
  dataLength_ = 0;
  collected_ = 0;
  png_ = false;
  textField_ = nullptr;
  textFieldSize_ = 0;
  yearField_ = false;
  tags_ = CoverTags{};
}

void CoverParser::fail(const char* reason) {
  if (frame_ != nullptr) {
    release_(frame_);
  }
  frame_ = nullptr;
  image_ = nullptr;
  imageSize_ = 0;
  reason_ = reason;
  phase_ = kDone;
  result_ = kNone;
}

// Метка пройдена (или дальше её не разобрать): картинка нашлась раньше — она
// и есть итог.
void CoverParser::finishTag(const char* reason) {
  if (image_ != nullptr) {
    phase_ = kDone;
    result_ = kReceived;
    return;
  }
  fail(reason);
}

CoverParser::Result CoverParser::feed(uint32_t offset, const uint8_t* data, size_t length) {
  if (result_ != kWant) {
    return result_;
  }
  if (data == nullptr || length == 0) {
    // Z80 прочитал с want() ноль байтов: файл кончился раньше метки.
    if (offset == want_) {
      finishTag("the file ends inside the tag");
    }
    return result_;
  }
  const uint64_t end = static_cast<uint64_t>(offset) + length;
  while (result_ == kWant && offset <= want_ && want_ < end) {
    const size_t skip = want_ - offset;
    // Кусок с самого want() короче нужного бывает только в конце файла: Z80
    // шлёт с нужного места не меньше килобайта.
    const bool atStart = skip == 0;
    if (!step(want_, data + skip, length - skip)) {
      if (atStart) {
        finishTag("the file ends inside the tag");
      }
      break;
    }
  }
  return result_;
}

// Шаг разбора с позиции at (= want_), доступно length байтов. false — данных
// для шага не хватило.
bool CoverParser::step(uint32_t at, const uint8_t* data, size_t length) {
  switch (phase_) {
    case kHeader: {
      if (length < kHeaderBytes) {
        return false;
      }
      if (memcmp(data, "ID3", 3) != 0) {
        fail("no ID3v2 tag");
        return true;
      }
      version_ = data[3];
      if (version_ != 3 && version_ != 4) {
        fail("ID3v2 version is not 2.3 or 2.4");
        return true;
      }
      const uint8_t flags = data[5];
      uint32_t size = 0;
      if ((flags & 0x80) != 0 || !readSyncsafe(data + 6, size)) {
        fail("unsynchronised or broken tag");
        return true;
      }
      tagEnd_ = kHeaderBytes + size;
      want_ = kHeaderBytes;
      phase_ = (flags & 0x40) != 0 ? kExtended : kFrame;
      return true;
    }
    case kExtended: {
      if (length < 4) {
        return false;
      }
      uint32_t size = 0;
      if (version_ == 3) {
        size = 4 + readBe32(data);     // в 2.3 длина без самих четырёх байтов
      } else if (!readSyncsafe(data, size)) {
        fail("broken extended header");
        return true;
      }
      want_ = at + size;
      phase_ = kFrame;
      return true;
    }
    case kFrame:
      if (at + kHeaderBytes > tagEnd_) {
        finishTag(png_ ? "the picture is PNG" : "no picture in the tag");
        return true;
      }
      if (length < kHeaderBytes) {
        return false;
      }
      return frameHeader(data);
    case kCollect: {
      uint32_t count = dataLength_ - collected_;
      if (count > length) {
        count = static_cast<uint32_t>(length);
      }
      memcpy(frame_ + collected_, data, count);
      collected_ += count;
      want_ += count;
      if (collected_ == dataLength_) {
        finishFrame();
      }
      return true;
    }
    case kText: {
      uint32_t count = dataLength_ - collected_;
      if (count > length) {
        count = static_cast<uint32_t>(length);
      }
      memcpy(text_ + collected_, data, count);
      collected_ += count;
      want_ += count;
      if (collected_ == dataLength_) {
        decodeId3Text(text_, dataLength_, textField_, textFieldSize_);
        if (yearField_ && strlen(textField_) > 4) {
          textField_[4] = 0;           // TDRC: «2003-05-17» — только год
        }
        phase_ = kFrame;
        want_ = frameEnd_;
      }
      return true;
    }
    case kDone:
      break;
  }
  return false;
}

// Заголовок кадра по want_: APIC — собирать, остальное — пропустить.
bool CoverParser::frameHeader(const uint8_t* header) {
  if (header[0] == 0) {
    finishTag(png_ ? "the picture is PNG" : "no picture in the tag");  // дальше заполнитель
    return true;
  }
  uint32_t size = 0;
  if (version_ == 4) {
    // В 2.4 длина кадра семибитная; некоторые программы пишут обычную.
    if (!readSyncsafe(header + 4, size)) {
      size = readBe32(header + 4);
    }
  } else {
    size = readBe32(header + 4);
  }
  const uint64_t end = static_cast<uint64_t>(want_) + kHeaderBytes + size;
  if (end > tagEnd_) {
    finishTag("a frame runs past the tag");
    return true;
  }
  frameEnd_ = static_cast<uint32_t>(end);
  const uint8_t format = header[9];
  // Сжатие, шифрование, рассинхронизация кадра — такую картинку не собрать.
  const bool packed = version_ == 3 ? (format & 0xC0) != 0 : (format & 0x0E) != 0;
  uint32_t start = want_ + kHeaderBytes;
  uint32_t length = size;
  if (version_ == 4 && (format & 0x01) != 0 && length >= 4) {
    start += 4;                        // указатель длины данных
    length -= 4;
  }
  // Текстовые кадры меток: собрать начало (не длиннее kMaxTextBytes).
  char* field = nullptr;
  size_t fieldSize = 0;
  bool year = false;
  if (memcmp(header, "TIT2", 4) == 0) {
    field = tags_.title;
    fieldSize = sizeof(tags_.title);
  } else if (memcmp(header, "TPE1", 4) == 0) {
    field = tags_.artist;
    fieldSize = sizeof(tags_.artist);
  } else if (memcmp(header, "TALB", 4) == 0) {
    field = tags_.album;
    fieldSize = sizeof(tags_.album);
  } else if (memcmp(header, "TYER", 4) == 0 || memcmp(header, "TDRC", 4) == 0) {
    field = tags_.year;
    fieldSize = sizeof(tags_.year);
    year = true;
  } else if (memcmp(header, "TRCK", 4) == 0) {
    field = tags_.track;
    fieldSize = sizeof(tags_.track);
  }
  if (field != nullptr && !packed && length >= 2) {
    textField_ = field;
    textFieldSize_ = fieldSize;
    yearField_ = year;
    dataLength_ = length < kMaxTextBytes ? length : kMaxTextBytes;
    collected_ = 0;
    want_ = start;
    phase_ = kText;
    return true;
  }
  // Картинка — первая подходящая; следующие APIC пропускаются.
  if (memcmp(header, "APIC", 4) != 0 || packed || length < 16 ||
      length > kMaxImageBytes || image_ != nullptr) {
    want_ = frameEnd_;
    return true;
  }
  frame_ = static_cast<uint8_t*>(alloc_(length));
  if (frame_ == nullptr) {
    fail("no memory for the picture");
    return true;
  }
  dataStart_ = start;
  dataLength_ = length;
  collected_ = 0;
  want_ = start;
  phase_ = kCollect;
  return true;
}

// Тело APIC собрано: [кодировка][MIME, 0][вид][описание, 0][картинка].
void CoverParser::finishFrame() {
  const uint8_t* body = frame_;
  const uint32_t length = dataLength_;
  const uint8_t encoding = body[0];
  uint32_t index = 1;
  while (index < length && body[index] != 0) {
    ++index;                           // MIME
  }
  index += 2;                          // ноль и вид картинки
  if (encoding == 1 || encoding == 2) {
    // UTF-16: конец описания — два нулевых байта на чётном месте.
    const uint32_t description = index;
    while (index + 1 < length &&
           !(body[index] == 0 && body[index + 1] == 0 && ((index - description) & 1) == 0)) {
      ++index;
    }
    index += 2;
  } else {
    while (index < length && body[index] != 0) {
      ++index;
    }
    index += 1;
  }
  if (index + 4 <= length && body[index] == 0xFF && body[index + 1] == 0xD8 &&
      body[index + 2] == 0xFF) {
    // Картинка есть; дальше по метке — за текстовыми кадрами.
    image_ = frame_ + index;
    imageSize_ = length - index;
    phase_ = kFrame;
    want_ = frameEnd_;
    return;
  }
  if (index + 4 <= length && memcmp(body + index, "\x89PNG", 4) == 0) {
    png_ = true;
  }
  // Не JPEG: может быть, дальше есть ещё картинка.
  release_(frame_);
  frame_ = nullptr;
  phase_ = kFrame;
  want_ = frameEnd_;
}

uint8_t* CoverParser::takeImage(size_t& size) {
  if (result_ != kReceived || frame_ == nullptr) {
    size = 0;
    return nullptr;
  }
  // Картинка лежит внутри тела кадра: сдвинуть её в начало буфера, чтобы
  // освобождать можно было по тому же адресу.
  memmove(frame_, image_, imageSize_);
  uint8_t* buffer = frame_;
  size = imageSize_;
  frame_ = nullptr;
  image_ = nullptr;
  imageSize_ = 0;
  return buffer;
}

// --- миниатюра ------------------------------------------------------------------

const uint16_t CoverThumb::kWcPalette[16] = {
    0x0000, 0x0010, 0x4000, 0x4010, 0x0200, 0x0210, 0x4200, 0x4210,
    0x0000, 0x0018, 0x6000, 0x6018, 0x0300, 0x0318, 0x6300, 0x6318,
};

bool CoverThumb::begin(uint32_t width, uint32_t height) {
  end();
  if (width == 0 || height == 0) {
    return false;
  }
  // Обложка экрана — по возможности: без памяти на неё миниатюра всё равно будет
  if (picture_ != nullptr && !picture_->begin(width, height)) {
    picture_ = nullptr;
  }
  cells_ = static_cast<Cell*>(alloc_(sizeof(Cell) * kSide * kSide));
  if (cells_ == nullptr) {
    return false;
  }
  memset(cells_, 0, sizeof(Cell) * kSide * kSide);
  side_ = width < height ? width : height;
  left_ = (width - side_) / 2;
  top_ = (height - side_) / 2;
  return true;
}

void CoverThumb::end() {
  if (cells_ != nullptr) {
    release_(cells_);
  }
  cells_ = nullptr;
}

void CoverThumb::addBlock(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
                          const uint8_t* rgb) {
  if (picture_ != nullptr) {
    picture_->addBlock(left, top, right, bottom, rgb);
  }
  if (cells_ == nullptr) {
    return;
  }
  const uint32_t width = right - left + 1;
  for (uint32_t y = top; y <= bottom; ++y) {
    if (y < top_ || y >= top_ + side_) {
      continue;
    }
    // Точка накрывает все клетки, в которые попадает хотя бы частью: при
    // картинке меньше 64 точек пустых клеток не остаётся.
    const uint32_t v = y - top_;
    const uint32_t row0 = v * kSide / side_;
    uint32_t row1 = ((v + 1) * kSide - 1) / side_;
    if (row1 >= static_cast<uint32_t>(kSide)) {
      row1 = kSide - 1;
    }
    const uint8_t* pixel = rgb + static_cast<size_t>(y - top) * width * 3;
    for (uint32_t x = left; x <= right; ++x, pixel += 3) {
      if (x < left_ || x >= left_ + side_) {
        continue;
      }
      const uint32_t u = x - left_;
      const uint32_t column0 = u * kSide / side_;
      uint32_t column1 = ((u + 1) * kSide - 1) / side_;
      if (column1 >= static_cast<uint32_t>(kSide)) {
        column1 = kSide - 1;
      }
      for (uint32_t row = row0; row <= row1; ++row) {
        for (uint32_t column = column0; column <= column1; ++column) {
          Cell& cell = cells_[row * kSide + column];
          cell.red += pixel[0];
          cell.green += pixel[1];
          cell.blue += pixel[2];
          ++cell.count;
        }
      }
    }
  }
}

// --- обложка для экрана ---------------------------------------------------------

void CoverPicture::cells(uint32_t u, uint32_t& first, uint32_t& last) const {
  first = u * kSide / side_;
  last = ((u + 1) * kSide - 1) / side_;
  if (last >= static_cast<uint32_t>(kSide)) {
    last = kSide - 1;
  }
}

bool CoverPicture::begin(uint32_t width, uint32_t height) {
  end();
  if (width == 0 || height == 0) {
    return false;
  }
  side_ = width < height ? width : height;
  left_ = (width - side_) / 2;
  top_ = (height - side_) / 2;
  // Точек на клетку по стороне — не больше 16 (шаг по 15 на клетку и одна на стыке): сумма
  // 255 · 16 · 16 входит в 16 бит
  step_ = (side_ + 15U * kSide - 1U) / (15U * kSide);
  if (step_ == 0) {
    step_ = 1;
  }
  cells_ = static_cast<Cell*>(alloc_(sizeof(Cell) * kSide * kSide));
  hits_ = static_cast<uint8_t*>(alloc_(kSide));
  if (cells_ == nullptr || hits_ == nullptr) {
    end();
    return false;
  }
  memset(cells_, 0, sizeof(Cell) * kSide * kSide);
  memset(hits_, 0, kSide);
  for (uint32_t u = 0; u < side_; u += step_) {
    uint32_t first = 0;
    uint32_t last = 0;
    cells(u, first, last);
    for (uint32_t c = first; c <= last; ++c) {
      ++hits_[c];
    }
  }
  return true;
}

void CoverPicture::end() {
  if (cells_ != nullptr) {
    release_(cells_);
  }
  if (hits_ != nullptr) {
    release_(hits_);
  }
  cells_ = nullptr;
  hits_ = nullptr;
}

void CoverPicture::addBlock(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
                            const uint8_t* rgb) {
  if (cells_ == nullptr) {
    return;
  }
  const uint32_t width = right - left + 1;
  for (uint32_t y = top; y <= bottom; ++y) {
    if (y < top_ || y >= top_ + side_ || (y - top_) % step_ != 0) {
      continue;
    }
    uint32_t row0 = 0;
    uint32_t row1 = 0;
    cells(y - top_, row0, row1);
    const uint8_t* pixel = rgb + static_cast<size_t>(y - top) * width * 3;
    for (uint32_t x = left; x <= right; ++x, pixel += 3) {
      if (x < left_ || x >= left_ + side_ || (x - left_) % step_ != 0) {
        continue;
      }
      uint32_t column0 = 0;
      uint32_t column1 = 0;
      cells(x - left_, column0, column1);
      for (uint32_t row = row0; row <= row1; ++row) {
        for (uint32_t column = column0; column <= column1; ++column) {
          Cell& cell = cells_[row * kSide + column];
          cell.red = static_cast<uint16_t>(cell.red + pixel[0]);
          cell.green = static_cast<uint16_t>(cell.green + pixel[1]);
          cell.blue = static_cast<uint16_t>(cell.blue + pixel[2]);
        }
      }
    }
  }
}

bool CoverPicture::finish(uint16_t* output) {
  if (cells_ == nullptr) {
    return false;
  }
  for (int row = 0; row < kSide; ++row) {
    for (int column = 0; column < kSide; ++column) {
      const Cell& cell = cells_[row * kSide + column];
      uint32_t count = static_cast<uint32_t>(hits_[row]) * hits_[column];
      if (count == 0) {
        count = 1;
      }
      const uint32_t red = (cell.red + count / 2) / count;
      const uint32_t green = (cell.green + count / 2) / count;
      const uint32_t blue = (cell.blue + count / 2) / count;
      output[row * kSide + column] =
          static_cast<uint16_t>(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
    }
  }
  return true;
}

void CoverThumb::finish(uint8_t* output) {
  memset(output, 0, kBytes);
  if (cells_ == nullptr) {
    return;
  }
  // Цвета палитры в единицах 0..255 (код CRAM из 31).
  int palette[16][3];
  for (int index = 0; index < 16; ++index) {
    const uint16_t word = kWcPalette[index];
    palette[index][0] = ((word >> 10) & 31) * 255 / 31;
    palette[index][1] = ((word >> 5) & 31) * 255 / 31;
    palette[index][2] = (word & 31) * 255 / 31;
  }
  // Самый яркий цвет WC — 24 из 31: белое картинки сводится к нему, а не
  // копит ошибку, которую нечем погасить.
  const int top = palette[15][0];
  // Ошибка текущей и следующей строки, с полем в одну клетку по краям.
  int error[2][kSide + 2][3];
  memset(error, 0, sizeof(error));
  for (int row = 0; row < kSide; ++row) {
    int (*current)[3] = error[row & 1];
    int (*next)[3] = error[(row + 1) & 1];
    memset(next, 0, sizeof(error[0]));
    const bool forward = (row & 1) == 0;   // змейкой: полосы не тянутся в одну сторону
    for (int step = 0; step < kSide; ++step) {
      const int column = forward ? step : kSide - 1 - step;
      const Cell& cell = cells_[row * kSide + column];
      const uint32_t count = cell.count != 0 ? cell.count : 1;
      int value[3] = {
          static_cast<int>(cell.red / count) * top / 255,
          static_cast<int>(cell.green / count) * top / 255,
          static_cast<int>(cell.blue / count) * top / 255,
      };
      // Почти чёрное и почти белое (фон обложек, шум JPEG) — ровно чёрным и
      // белым, без ошибки: иначе она копится и рассыпается цветными точками,
      // ведь между чёрным и тёмными цветами WC — половина шкалы.
      const int high = value[0] > value[1] ? (value[0] > value[2] ? value[0] : value[2])
                                              : (value[1] > value[2] ? value[1] : value[2]);
      const int low = value[0] < value[1] ? (value[0] < value[2] ? value[0] : value[2])
                                               : (value[1] < value[2] ? value[1] : value[2]);
      const bool flat = high < top / 6 || low > top - top / 8;
      for (int channel = 0; channel < 3; ++channel) {
        value[channel] += flat ? 0 : current[column + 1][channel] / 16;
        if (value[channel] < 0) {
          value[channel] = 0;
        } else if (value[channel] > 255) {
          value[channel] = 255;
        }
      }
      int best = kBlack;
      int bestDistance = 0x7FFFFFFF;
      for (int index = 1; index < 16; ++index) {   // 0 у спрайта прозрачен
        const int red = value[0] - palette[index][0];
        const int green = value[1] - palette[index][1];
        const int blue = value[2] - palette[index][2];
        const int distance = 3 * red * red + 4 * green * green + 2 * blue * blue;
        if (distance < bestDistance) {
          bestDistance = distance;
          best = index;
        }
      }
      // Флойд — Стейнберг: 7/16 вперёд, 3/16, 5/16, 1/16 — в следующую строку.
      const int ahead = forward ? 1 : -1;
      for (int channel = 0; channel < 3; ++channel) {
        const int rest = flat ? 0 : value[channel] - palette[best][channel];
        current[column + 1 + ahead][channel] += rest * 7;
        next[column + 1 - ahead][channel] += rest * 3;
        next[column + 1][channel] += rest * 5;
        next[column + 1 + ahead][channel] += rest;
      }
      uint8_t& byte = output[row * (kSide / 2) + column / 2];
      byte |= (column & 1) == 0 ? static_cast<uint8_t>(best << 4) : static_cast<uint8_t>(best);
    }
  }
}

}  // namespace zifi
