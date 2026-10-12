#include "zifi/board.hpp"

#if ZIFI_HAS_AUDIO

#include <esp_heap_caps.h>
#include <new>
#include <string.h>

#include "esp32s3/rom/tjpgd.h"
#include "zifi/cover_art.hpp"

// Private scalar decoder: progressive JPEG needs coefficient buffers that
// TJpgDec in the ROM does not have. Its buffers are bounded and live in PSRAM.
#define JPGD_USE_SSE2 0
// The ELF core has no __assert_func. Keep jpgd's runtime format checks;
// debug assertions remain enabled in the host/ASan build.
#if defined(ESP_PLATFORM)
#define JPGD_ASSERT(condition) ((void)0)
#endif
#include "../lib/jpgd/jpgd.cpp"

// Обложка JPEG → миниатюра: TJpgDec R0.01 (ChaN), зашитый в ПЗУ ESP32-S3
// (jd_prepare/jd_decomp, esp32s3.rom.ld). Он понимает baseline JPEG
// YCbCr 4:4:4, 4:2:2 и 4:2:0. Для JDR_FMT3 используется программный jpgd,
// включая progressive JPEG со всеми уточняющими проходами и grayscale.
// Декодер умеет уменьшать сразу в 2, 4 и 8 раз — это почти бесплатно: при 1/8
// от блока 8×8 остаётся одна точка. Масштаб берётся наибольший, при котором
// меньшая сторона ещё не меньше 64 точек.

namespace zifi {
namespace {

// Пул декодера: таблицы Хаффмана и квантования, буфер потока 512 байт, буфер
// MCU. Автор указывает около 3100 байт.
constexpr size_t kPoolBytes = 4096;
constexpr size_t kSoftwareBytes = 4U * 1024U * 1024U;

struct Source {
  const uint8_t* data;
  size_t size;
  size_t position;
  CoverThumb* thumb;
  const volatile bool* cancel;
};

UINT input(JDEC* decoder, BYTE* buffer, UINT count) {
  Source* source = static_cast<Source*>(decoder->device);
  const size_t left = source->size - source->position;
  if (count > left) {
    count = static_cast<UINT>(left);
  }
  if (buffer != nullptr) {
    memcpy(buffer, source->data + source->position, count);
  }
  source->position += count;
  return count;
}

UINT output(JDEC* decoder, void* bitmap, JRECT* rect) {
  Source* source = static_cast<Source*>(decoder->device);
  if (source->cancel != nullptr && *source->cancel) {
    return 0;                          // новый файл: прервать
  }
  source->thumb->addBlock(rect->left, rect->top, rect->right, rect->bottom,
                          static_cast<const uint8_t*>(bitmap));
  return 1;
}

struct SoftwareInput {
  const uint8_t* data;
  size_t size;
  size_t position;
  size_t allocated;
  unsigned checkpoints;
  const volatile bool* cancel;
  void (*yield)();

  bool checkpoint() {
    if ((cancel != nullptr && *cancel) || ++checkpoints > 16384) return false;
    if (yield != nullptr && !(checkpoints & 7)) yield();
    return cancel == nullptr || !*cancel;
  }
  static int read(void* context, uint8_t* out, int count, bool* eof) {
    auto& input = *static_cast<SoftwareInput*>(context);
    if (!input.checkpoint()) return -1;
    size_t n = input.size - input.position;
    if (n > static_cast<size_t>(count)) n = count;
    memcpy(out, input.data + input.position, n);
    input.position += n;
    *eof = input.position == input.size;
    return static_cast<int>(n);
  }
  static void* allocate(void* context, size_t bytes) {
    auto& input = *static_cast<SoftwareInput*>(context);
    if (bytes > kSoftwareBytes - input.allocated) return nullptr;
    void* result = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != nullptr) input.allocated += bytes;
    return result;
  }
  static void release(void*, void* pointer) { heap_caps_free(pointer); }
  static bool check(void* context) { return static_cast<SoftwareInput*>(context)->checkpoint(); }
};

int softwareResult(jpgd::jpeg_decoder& decoder, const volatile bool* cancel) {
  if (cancel != nullptr && *cancel) return 201;
  return decoder.get_error_code() == jpgd::JPGD_NOTENOUGHMEM ? 1 : 300 - decoder.get_error_code();
}

int softwarePixels(jpgd::jpeg_decoder& decoder, SoftwareInput& input, CoverThumb& thumb,
                   CoverDecodeInfo& info, uint8_t* row) {
  if (decoder.begin_decoding() != jpgd::JPGD_SUCCESS) return softwareResult(decoder, input.cancel);
  if (!thumb.begin(info.width, info.height)) return 2;
  const unsigned bytesPerPixel = decoder.get_bytes_per_pixel();
  if (bytesPerPixel != 1 && bytesPerPixel != 4) return 301;
  // Only one RGB row is retained. CoverThumb accumulates directly into 64x64
  // cells; no full-size RGB image competes with the audio buffers.
  for (unsigned y = 0; y < info.height; ++y) {
    if (!input.checkpoint()) return 201;
    const void* decoded = nullptr;
    unsigned length = 0;
    if (decoder.decode(&decoded, &length) != jpgd::JPGD_SUCCESS)
      return softwareResult(decoder, input.cancel);
    if (length < info.width * bytesPerPixel) return 301;
    const uint8_t* pixels = static_cast<const uint8_t*>(decoded);
    for (unsigned x = 0; x < info.width; ++x) {
      row[3*x] = pixels[bytesPerPixel*x];
      row[3*x+1] = pixels[bytesPerPixel*x + (bytesPerPixel == 1 ? 0 : 1)];
      row[3*x+2] = pixels[bytesPerPixel*x + (bytesPerPixel == 1 ? 0 : 2)];
    }
    thumb.addBlock(0, y, info.width - 1, y, row);
  }
  return 0;
}

int decodeSoftware(const uint8_t* jpeg, size_t size, CoverThumb& thumb, CoverDecodeInfo& info,
                   const volatile bool* cancel, void (*yield)()) {
  if (cancel != nullptr && *cancel) return 201;
  SoftwareInput input = {jpeg, size, 0, 0, 0, cancel, yield};
  jpgd::jpeg_decoder_stream stream = {&input, SoftwareInput::read, SoftwareInput::allocate,
                                      SoftwareInput::release, SoftwareInput::check};
  // jpeg_decoder contains input/tables larger than the cover task's stack.
  void* memory = SoftwareInput::allocate(&input, sizeof(jpgd::jpeg_decoder));
  if (memory == nullptr) return 1;
  // Chroma box expansion is sufficient before reduction to a 64x64 sprite;
  // it also handles one-pixel images without linear-filter edge reads.
  auto* decoder = new (memory) jpgd::jpeg_decoder(&stream,
      jpgd::jpeg_decoder::cFlagDisableSIMD | jpgd::jpeg_decoder::cFlagBoxChromaFiltering);
  uint8_t* row = nullptr;
  int result = softwareResult(*decoder, cancel);
  if (decoder->get_error_code() == jpgd::JPGD_SUCCESS) {
    info.width = decoder->get_width();
    info.height = decoder->get_height();
    info.scale = 0;
    row = static_cast<uint8_t*>(SoftwareInput::allocate(&input, info.width * 3));
    result = row != nullptr ? softwarePixels(*decoder, input, thumb, info, row) : 1;
  }
  decoder->~jpeg_decoder();
  heap_caps_free(row);
  heap_caps_free(memory);
  return result;
}

}  // namespace

int decodeCoverJpeg(const uint8_t* jpeg, size_t size, CoverThumb& thumb,
                    CoverDecodeInfo& info, const volatile bool* cancel, void (*yield)()) {
  if (cancel != nullptr && *cancel) return 201;
  void* pool = heap_caps_malloc(kPoolBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (pool == nullptr) {
    return 1;
  }
  Source source = {jpeg, size, 0, &thumb, cancel};
  JDEC decoder = {};
  JRESULT result = jd_prepare(&decoder, input, pool, kPoolBytes, &source);
  if (result != JDR_OK) {
    heap_caps_free(pool);
    if (result == JDR_FMT3) return decodeSoftware(jpeg, size, thumb, info, cancel, yield);
    return 100 + result;
  }
  info.width = decoder.width;
  info.height = decoder.height;
  const uint32_t shorter = info.width < info.height ? info.width : info.height;
  uint8_t scale = 0;
  while (scale < 3 && (shorter >> (scale + 1)) >= static_cast<uint32_t>(CoverThumb::kSide)) {
    ++scale;
  }
  info.scale = scale;
  const uint32_t round = (1U << scale) - 1;
  if (!thumb.begin((info.width + round) >> scale, (info.height + round) >> scale)) {
    heap_caps_free(pool);
    return 2;
  }
  result = jd_decomp(&decoder, output, scale);
  heap_caps_free(pool);
  return result == JDR_OK ? 0 : 200 + result;
}

}  // namespace zifi

#endif  // ZIFI_HAS_AUDIO
