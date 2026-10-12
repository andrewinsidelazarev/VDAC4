// Стенд компонента tracker на настоящих файлах (test_tracker_files_host.py):
// каждый файл грузится так же, как в AudioPlayer::startModule — вид по
// содержимому, контекст создан, буфер файла сразу освобождён, — и играет
// заданное число секунд тем же xm_render. Строка на файл:
//   <путь>\t<вид>\t<итог>\t<каналов>\t<позиций>\t<мс загрузки>\t<мс звука>\t<пик>\t<невыровненных>
//   \t<длительность прогоном, мс>\t<мс прогона>
// Вид: xm/s3m/mod/none; итог — код создания контекста (не меньше нуля — загружен).
// Невыровненных — 16-битных отсчётов с нечётного адреса: ESP32-S3 на них падает.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <vector>

#include "tracker.h"

namespace {

void* hostAlloc(size_t size) { return malloc(size); }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: tracker_files_host <seconds> <file>...\n");
    return 2;
  }
  const double seconds = atof(argv[1]);
  const uint32_t rate = 44100;
  for (int arg = 2; arg < argc; ++arg) {
    FILE* input = fopen(argv[arg], "rb");
    if (input == nullptr) {
      printf("%s\tnone\topen\t0\t0\t0\t0\t0\n", argv[arg]);
      continue;
    }
    fseek(input, 0, SEEK_END);
    const long length = ftell(input);
    fseek(input, 0, SEEK_SET);
    // Как в прошивке: буфер файла — отдельная куча, после разбора — free.
    uint8_t* file = static_cast<uint8_t*>(malloc(static_cast<size_t>(length)));
    fread(file, 1, static_cast<size_t>(length), input);
    fclose(input);
    const size_t size = static_cast<size_t>(length);

    xm_context_t* context = nullptr;
    const char* kind = "none";
    int result = -1;
    size_t needed = 0;
    const auto started = std::chrono::steady_clock::now();
    if (size >= 17 && memcmp(file, "Extended Module:", 16) == 0) {
      kind = "xm";
      result = xm_create_context_safe(&context, file, size, rate, hostAlloc);
    } else if (size >= 48 && memcmp(file + 44, "SCRM", 4) == 0) {
      kind = "s3m";
      result = s3m_create_context_safe(&context, file, size, rate, hostAlloc, &needed);
    } else if (mod_check_sanity_preload(file, size) == 0) {
      kind = "mod";
      result = mod_create_context_safe(&context, file, size, rate, hostAlloc, &needed);
    }
    const auto loaded = std::chrono::steady_clock::now();
    free(file);
    long renderMs = 0;
    long scanMs = 0;
    uint32_t duration = 0;
    float peak = 0;
    unsigned channels = 0;
    unsigned positions = 0;
    unsigned misaligned = 0;
    if (result >= 0 && context != nullptr) {
      for (uint16_t i = 0; i < context->module.num_instruments; ++i) {
        const xm_instrument_t& instrument = context->module.instruments[i];
        for (uint16_t j = 0; j < instrument.num_samples; ++j) {
          const xm_sample_t& sample = instrument.samples[j];
          if (sample.bits == 16 && (reinterpret_cast<uintptr_t>(sample.data16) & 1) != 0) {
            ++misaligned;
          }
        }
      }
      channels = xm_get_number_of_channels(context);
      positions = xm_get_module_length(context);
      xm_set_max_loop_count(context, 1);
      const auto scanStart = std::chrono::steady_clock::now();
      duration = xm_scan_song(context, nullptr, 3600000);
      scanMs = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - scanStart).count());
      std::vector<float> mix(512 * 2);
      const size_t blocks = static_cast<size_t>(seconds * rate / 512);
      for (size_t block = 0; block < blocks && xm_get_loop_count(context) == 0; ++block) {
        xm_render(context, mix.data(), 512);
        for (float value : mix) {
          peak = fabsf(value) > peak ? fabsf(value) : peak;
        }
      }
      renderMs = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - loaded).count());
      xm_free_context(context);
    }
    if (result < 0 && strcmp(kind, "s3m") == 0) {
      fprintf(stderr, "%s: %s\n", argv[arg], s3m_get_last_error());   // почему S3M не взят
    }
    printf("%s\t%s\t%d\t%u\t%u\t%ld\t%ld\t%.3f\t%u\t%u\t%ld\n", argv[arg], kind, result, channels,
           positions,
           static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
               loaded - started).count()),
           renderMs, peak, misaligned, static_cast<unsigned>(duration), scanMs);
    fflush(stdout);
  }
  return 0;
}
