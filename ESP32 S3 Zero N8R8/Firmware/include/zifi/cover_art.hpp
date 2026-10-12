#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Обложка MP3 для медиапроигрывателя VDAC4 (Docs/VDAC4.md, «Обложка MP3»).
//
// Длинную метку ID3v2 плагин на ESP не шлёт: пропускает на карте, чтобы звук
// начался сразу. Обложку он потом досылает фоном, кусками с того места метки,
// которое просит ESP (AUDIO_COVER): CoverParser проходит заголовки кадров,
// просит только кадр APIC и собирает картинку. Ничего, кроме смещения и
// данных, Z80 про метку знать не нужно.
//
// Миниатюра (CoverThumb) — 64×64 точки по 4 бита для спрайта TS-Conf поверх
// текстового экрана Wild Commander. В текстовом режиме цвет точки спрайта
// берётся из 16 цветов палитры текста (номер палитры спрайта там не действует,
// video_out.v TS-Conf), поэтому картинка сводится с рассеиванием ошибки к
// цветам WC: 8 цветов ZX на двух яркостях. Цвет 0 у спрайта прозрачен: чёрный —
// цвет 8.
//
// Оба класса однопоточные; разбор идёт на ядре обмена, сведение — в задаче
// обложки (AudioApp, PLAYER.ELF).
//
// Метки (CoverTags): по пути разбор собирает и текстовые кадры — название,
// исполнителя, альбом, год, номер дорожки — и переводит их в CP866 (кодировка
// текста Wild Commander). Поэтому после картинки он идёт по метке дальше, до
// её конца: текстовые кадры бывают и за APIC.
struct CoverTags {
  char title[64];
  char artist[64];
  char album[64];
  char year[8];
  char track[8];
};

class CoverParser {
 public:
  enum Result : uint8_t {
    kWant = 0,      // нужны данные с want()
    kReceived = 1,  // картинка JPEG собрана: takeImage()
    kNone = 2,      // обложки нет или она не годится
  };

  // Больше не берём: PSRAM нужна и модулю, и очереди звука.
  static constexpr uint32_t kMaxImageBytes = 4UL * 1024UL * 1024UL;
  // Текстового кадра читаем не больше: длиннее строки на экране всё равно нет.
  static constexpr uint32_t kMaxTextBytes = 255;

  using Alloc = void* (*)(size_t);
  using Release = void (*)(void*);

  CoverParser(Alloc alloc, Release release) : alloc_(alloc), release_(release) {}
  ~CoverParser() { reset(); }
  CoverParser(const CoverParser&) = delete;
  CoverParser& operator=(const CoverParser&) = delete;

  // Новый файл: всё забыть, ждать данные метки с нуля.
  void reset();
  // Кусок файла с offset. Берётся та его часть, что начинается не позже want();
  // остальное отбрасывается, и Z80 пришлёт нужное снова.
  Result feed(uint32_t offset, const uint8_t* data, size_t length);
  Result result() const { return result_; }
  uint32_t want() const { return want_; }
  // Почему kNone — для журнала наладки.
  const char* reason() const { return reason_; }
  // kReceived: буфер картинки уходит вызывающему (освобождать — release).
  uint8_t* takeImage(size_t& size);
  bool isPng() const { return png_; }
  // Метки, собранные до сих пор (пустые строки — кадра нет).
  const CoverTags& tags() const { return tags_; }

 private:
  enum Phase : uint8_t { kHeader, kExtended, kFrame, kCollect, kText, kDone };

  bool step(uint32_t offset, const uint8_t* data, size_t length);
  bool frameHeader(const uint8_t* header);
  void finishFrame();
  void finishTag(const char* reason);
  void fail(const char* reason);

  Alloc alloc_;
  Release release_;
  Phase phase_ = kHeader;
  Result result_ = kWant;
  const char* reason_ = "";
  uint8_t version_ = 0;
  uint32_t want_ = 0;
  uint32_t tagEnd_ = 0;
  uint32_t frameEnd_ = 0;      // где кончается текущий кадр (следующий заголовок)
  uint32_t dataStart_ = 0;     // начало тела APIC в файле
  uint32_t dataLength_ = 0;
  uint32_t collected_ = 0;
  uint8_t* frame_ = nullptr;   // тело APIC
  uint8_t* image_ = nullptr;   // картинка внутри frame_
  size_t imageSize_ = 0;
  bool png_ = false;
  char* textField_ = nullptr;  // куда пойдёт собираемый текстовый кадр
  size_t textFieldSize_ = 0;
  bool yearField_ = false;     // TYER/TDRC: из даты — только год
  uint8_t text_[kMaxTextBytes];
  CoverTags tags_ = {};
};

// Текст кадра ID3 ([кодировка][строка]) — в CP866: латиница как есть,
// кириллица из UTF-8, UTF-16 и однобайтовой (там она почти всегда в CP1251),
// прочее — «?». Конечные пробелы отрезаются.
void decodeId3Text(const uint8_t* body, size_t length, char* out, size_t size);

class CoverThumb {
 public:
  static constexpr int kSide = 64;
  static constexpr size_t kBytes = kSide * kSide / 2;
  static constexpr uint8_t kBlack = 8;   // цвет 0 у спрайта прозрачен

  // Палитра текста WC (PALLIT в WCVLS.ASM): слова CRAM TS-Conf %0RRRRRGGGGGBBBBB.
  static const uint16_t kWcPalette[16];

  CoverThumb(CoverParser::Alloc alloc, CoverParser::Release release)
      : alloc_(alloc), release_(release) {}
  ~CoverThumb() { end(); }
  CoverThumb(const CoverThumb&) = delete;
  CoverThumb& operator=(const CoverThumb&) = delete;

  // Картинка width×height (после масштаба декодера). Из середины берётся
  // квадрат и сводится к 64×64. false — нет памяти.
  bool begin(uint32_t width, uint32_t height);
  // Прямоугольник RGB888 [left..right]×[top..bottom] включительно, по строкам.
  void addBlock(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
                const uint8_t* rgb);
  // Сведение к палитре WC с рассеиванием ошибки: kBytes байтов, строки сверху
  // вниз, по две точки в байте (левая — в старшей тетраде).
  void finish(uint8_t* output);
  void end();

 private:
  struct Cell {
    uint32_t red, green, blue, count;
  };

  CoverParser::Alloc alloc_;
  CoverParser::Release release_;
  Cell* cells_ = nullptr;
  uint32_t side_ = 0;          // сторона квадрата в точках картинки
  uint32_t left_ = 0;
  uint32_t top_ = 0;
};

// JPEG → CoverThumb: baseline через TJpgDec из ПЗУ; progressive — jpgd,
// рабочие буферы в PSRAM, без полноразмерного RGB-изображения.
// cancel — флаг отмены (новый файл), опрашивается между блоками/строками.
// yield — необязательная передача управления другим задачам при software decode.
// Итог: 0 — миниатюра готова; иначе код отказа (для журнала).
struct CoverDecodeInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t scale = 0;           // 0..3: 1, 1/2, 1/4, 1/8
};
int decodeCoverJpeg(const uint8_t* jpeg, size_t size, CoverThumb& thumb,
                    CoverDecodeInfo& info, const volatile bool* cancel,
                    void (*yield)() = nullptr);

}  // namespace zifi
