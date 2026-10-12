#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_AUDIO

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "zifi/spsc_ring.hpp"

namespace zifi {

// Звук платы VDAC4: I2S -> GY-PCM5102. Проигрывает поток MP3, который Z80
// читает с карты и присылает кадрами AUDIO_DATA, трекерные модули (XM, MOD,
// S3M — файл целиком в PSRAM, компонент tracker) и тестовые тоны для проверки
// ЦАП и разводки каналов.
//
// Вызовы open/feed/close/tone/status делает цикл протокола (ядро 1). Декодер и
// вывод работают в своей задаче на ядре 0; между ними — кольцо байтов MP3 (у
// модуля — его буфер) и очередь команд.
class AudioPlayer {
 public:
  static constexpr uint8_t kChannelLeft = 0x01;
  static constexpr uint8_t kChannelRight = 0x02;
  static constexpr uint8_t kChannelBoth = kChannelLeft | kChannelRight;
  static constexpr size_t kStatusSize = 32;
  // Самый длинный модуль: файл и его разобранный контекст живут в PSRAM вместе.
  static constexpr uint32_t kModuleMaxBytes = 3 * 1024 * 1024;

  enum State : uint8_t {
    kStateIdle = 0,      // тишина
    kStatePlaying = 1,   // идёт поток MP3 или модуль
    kStateDraining = 2,  // поток закрыт, доигрывается накопленное
    kStateTone = 3,      // тестовый тон
    kStateOffline = 4,   // I2S не запустился
    kStateLoading = 5,   // модуль принимается или разбирается
  };

  // Что играет (AUDIO_OPEN, байт 0 тела; поле формата в записи состояния).
  enum Format : uint8_t {
    kFormatNone = 0,
    kFormatMp3 = 1,
    kFormatMod = 2,
    kFormatXm = 3,
    kFormatS3m = 4,
    kFormatModule = 0x80,  // при открытии: трекерный модуль, вид — по содержимому
  };

  enum FeedResult : uint8_t {
    kFeedFull = 0,      // места нет, повторить позже
    kFeedAccepted = 1,
    kFeedClosed = 2,    // поток не открыт
  };

  AudioPlayer();

  // usePsram — класть ли очередь MP3 во внешнюю память. playChime — проиграть
  // при запуске три коротких тона: левый канал, правый, оба.
  bool begin(bool usePsram, bool playChime);
  bool ready() const { return ready_; }
  // Идёт ли сейчас звук: поток MP3, его остаток или тон. В это время нельзя
  // писать во flash: запись останавливает оба ядра на десятки миллисекунд, а
  // запас I2S — 87 мс.
  bool sounding() const {
    const uint8_t state = state_.load();
    return state == kStatePlaying || state == kStateDraining ||
           state == kStateTone;
  }

  bool open();
  // Модуль длиной size байт: дальше feed() кладёт файл в буфер, и когда он
  // принят целиком, задача разбирает его и начинает играть.
  bool openModule(uint32_t size);
  FeedResult feed(const uint8_t* data, size_t length);
  void close(bool drain);
  // Модуль: перейти на delta позиций таблицы порядка (с начала позиции).
  // position — куда перешли. false — модуль не играет.
  bool seek(int8_t delta, uint16_t& position);
  // Поток MP3 после перемотки: выбросить очередь и состояние декодера; дальше
  // Z80 шлёт файл с нового места. positionMs — время этого места.
  bool flush(uint32_t positionMs);
  // Ответ AUDIO_INFO: [формат][каналы][длина LE16][название, 0].
  size_t info(uint8_t* output, size_t capacity) const;
  bool tone(uint8_t channels, uint16_t hertz, uint16_t milliseconds,
            uint8_t level);
  void setVolume(uint8_t volume);
  // Пауза потока MP3: очередь и декодер сохраняются, в ЦАП идёт тишина.
  void pause(bool on);

  // Тест линий ЦАП под питанием. Для каждой из линий BCK, LRCK, DIN — восемь
  // напряжений на выводе ESP (в единицах 20 мВ): четыре, когда вывод выдаёт
  // единицу, и четыре, когда он только подтянут вверх; в каждой четвёрке две
  // остальные линии стоят в 00, 01, 10 и 11 (порядок остальных: BCK, LRCK,
  // DIN без самой линии). Свободная линия даёт около 3,3 В во всех восьми;
  // линия, которую сажает другая, — меньше при нуле на той. Трогает только
  // эти три вывода, на время теста I2S с них снят. Пока идёт звук, не
  // выполняется (false). Занимает около 30 мс.
  static constexpr size_t kProbeBytes = 24;
  bool probeLines(uint8_t* output, size_t capacity);

  size_t freeSpace() const;

  // Вывод PCM для приложений ESP (zm_app.h): приложение само декодирует и
  // отдаёт отсчёты float, плеер ставит их в очередь (та же, что у MP3) и
  // выводит. pcmWrite ждёт места до kPcmWriteWaitMs и возвращает принятые
  // кадры; вызывать из задачи приложения, не из задачи звука.
  bool pcmOpen(uint32_t rate, uint8_t channels);
  size_t pcmWrite(const float* frames, size_t count);
  uint32_t pcmQueuedMs() const;
  void pcmFlush();
  void pcmClose();
  uint16_t i2sFaults() const { return i2sFaults_.load(); }
  // Громкость 0..255 в шкале AUDIO_VOLUME.
  uint8_t volume() const;

  // Запись состояния для ответа AUDIO_STATUS (формат — Docs/VDAC4.md).
  size_t status(uint8_t* output, size_t capacity) const;
  // Короткое описание для SYS_INFO: "idle", "44100x2", "tone", "off".
  void describe(char* output, size_t capacity) const;
  // Наладка: посмотреть, что на самом деле идёт на выводах BCK, LRCK и DATA, и
  // записать в журнал наладки. Занимает около 50 мс; из задачи звука не
  // вызывать. Возвращает false, если I2S ещё не запущен.
  bool logPads() const;
  // Наладка без I2S: напряжение на каждом из трёх выводов без нагрузки, с
  // подтяжкой и когда его ведёт выход GPIO. Показывает, чем нагружена линия.
  // Выводы после проверки свободны; при работающем I2S не вызывать.
  static void logPadVoltages();

 private:
  // Сигнал при включении: пауза перед ним, три тона и две паузы между ними.
  static constexpr size_t kMaxToneSteps = 6;
  // Входной буфер декодера: кадр MP3 не длиннее 1441 байта, с запасом на
  // «резервуар бит» предыдущих кадров.
  static constexpr size_t kInputBytes = 4096;
  // 1152 отсчёта на канал — самый длинный кадр MPEG-1.
  static constexpr size_t kPcmSamples = 1152 * 2;

  struct ToneStep {
    uint8_t channels;
    uint8_t level;
    uint16_t hertz;
    uint16_t milliseconds;
  };

  struct Command {
    uint8_t type;
    uint8_t argument;
    uint8_t stepCount;
    ToneStep steps[kMaxToneSteps];
    uint8_t* data;   // OpenModule: буфер файла, задача становится его владельцем
    uint32_t value;  // OpenModule: длина; Flush: время, мс; Seek: шаг со знаком
  };

  enum CommandType : uint8_t {
    kCommandOpen,
    kCommandClose,
    kCommandTone,
    kCommandOpenModule,
    kCommandStartModule,
    kCommandSeek,
    kCommandFlush,
    kCommandPcmOpen,
    kCommandPcmFlush,
    kCommandPcmClose,
  };
  enum Mode : uint8_t {
    kModeIdle,
    kModeStream,
    kModeTone,
    kModeLoading,
    kModeModule,
    kModePcm,
  };
  static constexpr uint32_t kPcmWriteWaitMs = 500;
  // Отсчётов за один проход модуля (на канал).
  static constexpr size_t kModuleFrames = 512;

  static void taskEntry(void* context);
  void taskLoop();
  bool startI2s();
  void apply(const Command& command);
  void streamStep();
  void toneStep();
  void moduleStep();
  void pcmStep();
  void startModule();
  void releaseModule();
  bool waitOpened(const Command& command);
  void starve();
  void finishStream();
  void consume(size_t count);
  void setOutputRate(uint32_t sourceRate);
  void silenceOutput();
  void watchStep();
  void play(size_t frames);
  void writeI2s(size_t bytes);
  bool sendTone(const ToneStep* steps, size_t count);

  void* channel_;        // i2s_chan_handle_t
  void* decoder_;        // HMP3Decoder
  TaskHandle_t task_;
  QueueHandle_t commands_;
  SemaphoreHandle_t opened_;
  SpscByteRing input_;
  uint8_t* inputStorage_;
  uint8_t* in_;
  int16_t* pcm_;
  size_t inLength_;
  size_t skip_;
  bool id3Checked_;
  bool starving_;
  // Сколько байтов должно набраться в очереди, прежде чем декодер начнёт
  // (или продолжит после опустошения) их разбирать.
  size_t bufferTarget_;
  Mode mode_;
  uint32_t outputRate_;
  uint8_t repeat_;
  ToneStep steps_[kMaxToneSteps];
  size_t stepCount_;
  size_t stepIndex_;
  uint32_t stepFrames_;
  uint32_t stepPosition_;
  float phase_;
  // Модуль. Буфер файла: пишет ядро 1 (feed), владеет и освобождает задача.
  uint8_t* moduleFile_;        // у задачи
  uint32_t moduleFileSize_;
  uint8_t* moduleWrite_;       // у ядра 1: куда класть файл
  uint32_t moduleWriteSize_;
  uint32_t moduleWritten_;
  void* tracker_;              // xm_context_t*
  float* mix_;                 // стерео float от трекера
  char title_[33];
  // Вывод PCM приложения: частота источника (после неё — repeat_), каналы.
  std::atomic<uint32_t> pcmRate_;
  uint8_t pcmChannels_;

  volatile bool ready_;
  std::atomic<bool> streamOpen_;
  std::atomic<bool> moduleOpen_;   // feed() кладёт в буфер модуля
  std::atomic<uint8_t> format_;
  std::atomic<uint16_t> position_; // модуль: позиция таблицы порядка
  std::atomic<uint16_t> length_;   // модуль: длина таблицы порядка
  std::atomic<uint8_t> row_;
  std::atomic<uint8_t> moduleChannels_;
  std::atomic<uint8_t> state_;
  std::atomic<bool> finished_;
  std::atomic<bool> decoded_;
  std::atomic<bool> buffering_;  // очередь набирается, звука пока нет
  std::atomic<bool> paused_;
  std::atomic<uint16_t> volume_;
  std::atomic<uint32_t> sampleRate_;
  std::atomic<uint8_t> channels_;
  std::atomic<uint16_t> bitrateKbps_;
  std::atomic<uint32_t> framesPlayed_;
  // Хвост в буферах DMA: когда была последняя запись в I2S и сколько звука (мс) DMA
  // держит после неё. pcmQueuedMs прибавляет недоигранный остаток — иначе PLAYER
  // закрывал вывод по пустому кольцу, и конец песни (до 87 мс) обрезался.
  std::atomic<uint32_t> lastWriteMs_{0};
  std::atomic<uint32_t> dmaTailMs_{0};
  std::atomic<uint32_t> playedMs_;
  std::atomic<uint16_t> decodeErrors_;
  std::atomic<uint16_t> underruns_;
  // Сколько раз драйвер I2S не принял отсчёты (за запуск прошивки).
  std::atomic<uint16_t> i2sFaults_;
  // Наблюдение за выводом во время звука (журнал раз в 2 с, watchStep):
  // буфер DMA доигран, а нового нет — в ЦАП ушла тишина (прерывание I2S).
  std::atomic<uint32_t> dmaUnderflows_;
  uint32_t watchStartMs_;      // 0 — окно не идёт
  uint32_t watchUnderflows_;   // dmaUnderflows_ в начале окна
  uint32_t watchFrames_;       // кадров отдано в I2S за окно
  uint32_t watchWrites_;
  uint32_t watchMaxUs_;        // самая долгая запись в I2S
  uint32_t watchSlow_;         // записей дольше 20 мс
  uint16_t watchPeak_;         // наибольший |отсчёт| после громкости
  // Итог тона (сигнал при включении, TONE) — одной записью журнала в конце:
  // недоливы DMA (в ЦАП ушла пустота), самый большой разрыв между записями в
  // I2S (задачу звука не пускали к процессору) и самая долгая запись.
  uint32_t toneStartMs_;       // время запуска тона от включения
  uint32_t toneUnderflows_;    // dmaUnderflows_ в начале тона
  uint32_t toneWrites_;
  uint32_t toneMaxUs_;         // самая долгая запись в I2S
  uint32_t toneMaxGapUs_;      // самый большой разрыв между записями
  int64_t toneLastUs_;         // конец предыдущей записи, 0 — записей не было
};

}  // namespace zifi

#endif  // ZIFI_HAS_AUDIO
