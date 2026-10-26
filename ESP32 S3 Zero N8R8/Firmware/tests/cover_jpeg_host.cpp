// Real software JPEG decoder with tracked PSRAM and a controllable ROM boundary.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <esp_heap_caps.h>
#include "esp32s3/rom/tjpgd.h"
#include "zifi/cover_art.hpp"

static std::unordered_map<void*, size_t> blocks;
static size_t liveBytes, peakBytes;
static unsigned allocations, failAllocation, yields, cancelYield;
static volatile bool cancelled;
static int prepareResult = JDR_FMT3;
static unsigned prepareCalls, decodeCalls;

void* heap_caps_malloc(size_t size, uint32_t) {
  if (++allocations == failAllocation) return nullptr;
  void* p = malloc(size);
  if (p) { blocks[p] = size; liveBytes += size; if (liveBytes > peakBytes) peakBytes = liveBytes; }
  return p;
}
void heap_caps_free(void* p) {
  if (!p) return;
  auto i = blocks.find(p);
  if (i == blocks.end()) std::abort();
  liveBytes -= i->second; blocks.erase(i); free(p);
}
static void* allocate(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void yield() { if (++yields == cancelYield) cancelled = true; }

JRESULT jd_prepare(JDEC* d, UINT(*)(JDEC*, BYTE*, UINT), void*, UINT, void* source) {
  ++prepareCalls; d->device = source; d->width = d->height = 8;
  return static_cast<JRESULT>(prepareResult);
}
JRESULT jd_decomp(JDEC* d, UINT(*out)(JDEC*, void*, JRECT*), BYTE) {
  ++decodeCalls;
  uint8_t pixels[8*8*3]; memset(pixels, 127, sizeof(pixels));
  JRECT rect = {0, 7, 0, 7};
  return out(d, pixels, &rect) ? JDR_OK : JDR_INTR;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) return 2;
  if (argc > 3) cancelYield = _wtoi(argv[3]);
  if (argc > 4) failAllocation = _wtoi(argv[4]);
  if (argc > 5) prepareResult = _wtoi(argv[5]);
  // argv[6] — файл обложки экрана (CoverPicture 256×256 RGB565): сводится вместе с миниатюрой
  const wchar_t* picturePath = argc > 6 ? argv[6] : nullptr;
  FILE* file = _wfopen(argv[1], L"rb"); if (!file) return 3;
  fseek(file, 0, SEEK_END); size_t size = ftell(file); rewind(file);
  std::vector<uint8_t> data(size); fread(data.data(), 1, size, file); fclose(file);
  zifi::CoverDecodeInfo info;
  uint8_t out[zifi::CoverThumb::kBytes] = {};
  auto start = std::chrono::steady_clock::now();
  int result;
  int pictureDone = 0;
  static uint16_t picture[zifi::CoverPicture::kSide * zifi::CoverPicture::kSide];
  {
    zifi::CoverThumb thumb(allocate, heap_caps_free);
    zifi::CoverPicture big(allocate, heap_caps_free);
    if (picturePath != nullptr) thumb.attach(&big);
    result = zifi::decodeCoverJpeg(data.data(), data.size(), thumb, info, &cancelled, yield);
    if (result == 0) thumb.finish(out);
    if (result == 0 && picturePath != nullptr) pictureDone = big.finish(picture) ? 1 : 0;
  }
  if (picturePath != nullptr) {
    FILE* p = _wfopen(picturePath, L"wb");
    if (!p) return 6;
    fwrite(picture, sizeof(uint16_t), zifi::CoverPicture::kSide * zifi::CoverPicture::kSide, p);
    fclose(p);
  }
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
  file = _wfopen(argv[2], L"wb"); if (!file) return 4;
  fwrite(out, 1, sizeof(out), file); fclose(file);
  printf("{\"result\":%d,\"picture\":%d,\"width\":%u,\"height\":%u,\"scale\":%u,\"allocations\":%u,"
         "\"peak_bytes\":%zu,\"live_bytes\":%zu,\"yields\":%u,\"cancelled\":%s,"
         "\"prepare\":%u,\"rom_decode\":%u,\"microseconds\":%lld}\n",
         result, pictureDone, info.width, info.height, info.scale, allocations, peakBytes, liveBytes,
         yields, cancelled ? "true" : "false", prepareCalls, decodeCalls, (long long)us);
  return liveBytes ? 5 : 0;
}
