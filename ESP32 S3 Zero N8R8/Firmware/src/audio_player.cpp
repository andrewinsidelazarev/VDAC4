#include "zifi/audio_player.hpp"

#if ZIFI_HAS_AUDIO

#include <Arduino.h>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <driver/pulse_cnt.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <math.h>
#include <mp3dec.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>
#include <soc/i2s_reg.h>
#include <soc/rtc_io_struct.h>
#include <soc/soc.h>
#include <stdio.h>
#include <string.h>
#include <tracker.h>

#include "zifi/bringup_log.hpp"
#include "zifi/protocol.hpp"

namespace zifi {
namespace {

constexpr char kTag[] = "zifi-audio";
// Выше сетевой задачи ZiFi (приоритет 2), ниже Wi-Fi и lwIP: звук не должен
// заикаться из-за опроса сокетов, но и сеть он не задерживает.
constexpr UBaseType_t kTaskPriority = 6;
constexpr BaseType_t kTaskCore = 0;
constexpr uint32_t kTaskStackBytes = 8192;
// Около 16 секунд MP3 при 128 кбит/с (6,5 с при 320): Z80 читает карту
// рывками и отвлекается на клавиши и экран, но подаёт быстрее, чем звук
// играет. Больше не нужно: при переходе к другому файлу накопленное пропадает.
constexpr size_t kRingPsramBytes = 256 * 1024;
constexpr size_t kRingInternalBytes = 16 * 1024;
// Звук начинается, когда в очереди набралось столько байтов самого потока
// (метка ID3 к этому времени уже пропущена): запас на рывки подачи. После
// опустошения очереди — столько, чтобы пауза была одна, а не дробь из коротких.
constexpr size_t kStartBufferBytes = 128 * 1024;
constexpr size_t kRefillBufferBytes = 48 * 1024;
constexpr uint32_t kDefaultRate = 44100;
// PCM5102 берёт тактовую из BCK (вывод SCK на земле), и ниже 1 МГц его PLL не
// работает. При 64 тактах BCK на отсчёт это 16 кГц; более редкие потоки
// выводятся с удвоением каждого отсчёта.
constexpr uint32_t kMinDacRate = 16000;
// Буферы DMA I2S: 8 x 480 кадров — около 87 мс запаса на 44,1 кГц.
constexpr uint32_t kDmaDescriptors = 8;
constexpr uint32_t kDmaFrames = 480;
constexpr uint32_t kI2sWriteTimeoutMs = 1000;
constexpr uint32_t kCommandTimeoutMs = 500;
constexpr uint32_t kStartTimeoutMs = 2000;
constexpr size_t kToneBlockFrames = 256;
constexpr uint32_t kToneRampFrames = 220;     // около 5 мс: тон без щелчков
constexpr uint16_t kFullVolume = 256;
// Столько отказов I2S за запуск попадает в журнал наладки.
constexpr uint16_t kI2sFaultReports = 8;
constexpr uint8_t kDefaultVolume = 200;
constexpr float kTwoPi = 6.2831853f;

i2s_chan_handle_t channelOf(void* channel) {
  return static_cast<i2s_chan_handle_t>(channel);
}

// Все буферы DMA остановленного канала — нулями: после запуска в ЦАП не уйдёт
// ничего из того, что лежало в них до остановки.
void preloadSilence(i2s_chan_handle_t channel) {
  static const uint8_t kZeros[512] = {};
  size_t loaded = 0;
  do {
    if (i2s_channel_preload_data(channel, kZeros, sizeof(kZeros), &loaded) != ESP_OK) {
      break;
    }
  } while (loaded != 0);
}

i2s_std_clk_config_t clockFor(uint32_t rate) {
  i2s_std_clk_config_t clock = {};
  clock.sample_rate_hz = rate;
  clock.clk_src = I2S_CLK_SRC_DEFAULT;
  clock.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  clock.bclk_div = 8;
  return clock;
}

uint16_t volumeScale(uint8_t volume) {
  // 255 — без ослабления (256/256), чтобы полная громкость не теряла разряд.
  return volume == 255 ? kFullVolume : volume;
}

// Команды звука в журнале наладки: по ним видно, дошла ли команда до ESP и чем
// кончилось проигрывание. Клавишу тона держат, и команда идёт каждый кадр,
// поэтому записей за один запуск — не больше kAudioReports.
constexpr uint32_t kAudioReports = 40;
std::atomic<uint32_t> g_audioReports{0};
// Записи наблюдения за выводом (раз в 2 с звука): не больше этого за запуск.
constexpr uint32_t kWatchReports = 90;
constexpr uint32_t kWatchPeriodMs = 2000;
constexpr uint32_t kSlowWriteUs = 20000;
std::atomic<uint32_t> g_watchReports{0};

bool audioReportAllowed() {
  return g_audioReports.fetch_add(1) < kAudioReports;
}

// Что видно на выводе: куда он подключён и что на нём происходит.
struct PadView {
  uint32_t outSignal;    // номер сигнала в матрице GPIO, выведенного на вывод
  bool outputEnabled;
  uint32_t samples;      // сколько раз прочитан уровень
  uint32_t highs;        // из них высоких
  uint32_t changes;      // перемен уровня между соседними чтениями
  uint32_t sampleUs;     // за какое время
  int counted;           // фронтов по счётчику PCNT; -1 — счётчик не запустился
  uint32_t countUs;
};

// Уровень на выводе читается подряд, без пауз: около 20 млн раз в секунду, так
// что и BCK (2,8 МГц) виден без пропусков.
void samplePad(int pin, uint32_t samples, PadView& view) {
  const uint32_t mask = 1U << pin;
  uint32_t highs = 0;
  uint32_t changes = 0;
  uint32_t previous = GPIO.in & mask;
  const int64_t started = esp_timer_get_time();
  for (uint32_t index = 0; index < samples; ++index) {
    const uint32_t level = GPIO.in & mask;
    if (level != 0) {
      ++highs;
    }
    if (level != previous) {
      ++changes;
      previous = level;
    }
  }
  view.sampleUs = static_cast<uint32_t>(esp_timer_get_time() - started);
  view.samples = samples;
  view.highs = highs;
  view.changes = changes;
}

// Фронты на выводе за короткое окно. PCNT слушает вывод через матрицу GPIO,
// выходу I2S это не мешает; счётчик создаётся на время измерения.
void countPadEdges(int pin, uint32_t windowUs, PadView& view) {
  view.counted = -1;
  view.countUs = 0;
  pcnt_unit_config_t unitConfig = {};
  unitConfig.low_limit = -1;
  unitConfig.high_limit = 30000;
  pcnt_unit_handle_t unit = nullptr;
  if (pcnt_new_unit(&unitConfig, &unit) != ESP_OK) {
    return;
  }
  pcnt_chan_config_t channelConfig = {};
  channelConfig.edge_gpio_num = pin;
  channelConfig.level_gpio_num = -1;
  pcnt_channel_handle_t channel = nullptr;
  if (pcnt_new_channel(unit, &channelConfig, &channel) == ESP_OK) {
    pcnt_channel_set_edge_action(channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_channel_set_level_action(channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    if (pcnt_unit_enable(unit) == ESP_OK) {
      pcnt_unit_clear_count(unit);
      pcnt_unit_start(unit);
      const int64_t started = esp_timer_get_time();
      esp_rom_delay_us(windowUs);
      int count = 0;
      pcnt_unit_get_count(unit, &count);
      view.countUs = static_cast<uint32_t>(esp_timer_get_time() - started);
      view.counted = count;
      pcnt_unit_stop(unit);
      pcnt_unit_disable(unit);
    }
    pcnt_del_channel(channel);
  }
  pcnt_del_unit(unit);
}

// Напряжение на выводе по АЦП модуля, мВ. Без калибровки, с точностью около
// десятой: здесь важно отличить ноль от трёх вольт. Шкала при ослаблении 12 дБ —
// примерно до 3,1 В.
int padMillivolts(adc_oneshot_unit_handle_t unit, adc_channel_t channel) {
  esp_rom_delay_us(500);
  int raw = 0;
  if (adc_oneshot_read(unit, channel, &raw) != ESP_OK) {
    return -1;
  }
  return raw * 3100 / 4095;
}

// Драйвер I2S: доигранный буфер вернулся в полную очередь — писатель его не
// забрал, и DMA выдаёт тишину (буферы чистятся после отправки).
bool IRAM_ATTR onSendOverflow(i2s_chan_handle_t, i2s_event_data_t*, void* context) {
  static_cast<std::atomic<uint32_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
  return false;
}

// Сила выходов I2S: наибольшая. Линия BCK временами нагружена (тест линий:
// высокий уровень то 3,1 В, то около 1 В), сильный выход держит уровень выше.
constexpr gpio_drive_cap_t kI2sPadDrive = GPIO_DRIVE_CAP_3;

void driveI2sPads() {
  gpio_set_drive_capability(static_cast<gpio_num_t>(board::kI2sBckPin), kI2sPadDrive);
  gpio_set_drive_capability(static_cast<gpio_num_t>(board::kI2sLrckPin), kI2sPadDrive);
  gpio_set_drive_capability(static_cast<gpio_num_t>(board::kI2sDataPin), kI2sPadDrive);
}

// Сигнала матрицы GPIO с таким номером нет: вывод после проверки остаётся свободным.
constexpr uint32_t kNoSignal = 0xFFFFFFFF;

// Что с самим выводом: напряжение на нём без нагрузки, с подтяжкой и когда его
// ведёт выход GPIO. Замкнутый на землю вывод не поднимается ни в одном случае;
// у вывода со сгоревшим выходом подтяжка работает. В конце выводу возвращается
// его сигнал signal.
void measurePad(const char* tag, const char* name, int pin, uint32_t signal) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  adc_unit_t unitId = ADC_UNIT_1;
  adc_channel_t channel = ADC_CHANNEL_0;
  adc_oneshot_unit_handle_t unit = nullptr;
  adc_oneshot_unit_init_cfg_t unitConfig = {};
  adc_oneshot_chan_cfg_t channelConfig = {};
  channelConfig.atten = ADC_ATTEN_DB_12;
  channelConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_oneshot_io_to_channel(pin, &unitId, &channel) != ESP_OK) {
    bringupLog(tag, "pad %s voltage: GPIO%d has no ADC channel", name, pin);
    return;
  }
  unitConfig.unit_id = unitId;
  if (adc_oneshot_new_unit(&unitConfig, &unit) != ESP_OK) {
    bringupLog(tag, "pad %s voltage: ADC is busy", name);
    return;
  }
  // Настройка канала отключает у вывода и вход, и выход, и подтяжки.
  if (adc_oneshot_config_channel(unit, channel, &channelConfig) == ESP_OK) {
    const int floating = padMillivolts(unit, channel);
    gpio_pullup_en(gpio);
    const int pulledUp = padMillivolts(unit, channel);
    gpio_pullup_dis(gpio);
    gpio_set_level(gpio, 1);
    gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT);
    const int high = padMillivolts(unit, channel);
    const int reads = (GPIO.in >> pin) & 1;
    gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_3);
    const int highStrong = padMillivolts(unit, channel);
    gpio_set_drive_capability(gpio, kI2sPadDrive);
    gpio_set_level(gpio, 0);
    const int low = padMillivolts(unit, channel);
    bringupLog(tag,
               "pad %s voltage, mV: free %d, pull-up %d, driven high %d (reads %d), high at full drive %d, driven low %d",
               name, floating, pulledUp, high, reads, highStrong, low);
  }
  adc_oneshot_del_unit(unit);
  if (signal == kNoSignal) {
    gpio_set_direction(gpio, GPIO_MODE_DISABLE);
    return;
  }
  gpio_input_enable(gpio);
  esp_rom_gpio_connect_out_signal(pin, signal, false, false);
}

// Только наблюдение: вывод остаётся за I2S, ничего на нём не переключается.
// Замеры с переключением вывода делает тест линий (probeLines) по команде и
// только в тишине.
void logPad(const char* tag, const char* name, int pin, uint32_t windowUs) {
  PadView view = {};
  view.outSignal = GPIO.func_out_sel_cfg[pin].func_sel;
  view.outputEnabled = (GPIO.enable & (1U << pin)) != 0;
  // Чтобы прочитать уровень на выходе, у вывода должен быть включён вход.
  gpio_input_enable(static_cast<gpio_num_t>(pin));
  samplePad(pin, 20000, view);
  countPadEdges(pin, windowUs, view);
  const uint32_t sampledHz =
      view.sampleUs == 0
          ? 0
          : static_cast<uint32_t>(static_cast<uint64_t>(view.changes) * 500000 /
                                  view.sampleUs);
  bringupLog(tag,
             "pad %s GPIO%d: signal %u, output %s, %u%% high, %u changes in %u us (%u Hz), counter %d in %u us",
             name, pin, static_cast<unsigned>(view.outSignal),
             view.outputEnabled ? "on" : "OFF",
             static_cast<unsigned>(view.highs * 100 / view.samples),
             static_cast<unsigned>(view.changes),
             static_cast<unsigned>(view.sampleUs),
             static_cast<unsigned>(sampledHz), view.counted,
             static_cast<unsigned>(view.countUs));
}

}  // namespace

AudioPlayer::AudioPlayer()
    : channel_(nullptr),
      decoder_(nullptr),
      task_(nullptr),
      commands_(nullptr),
      opened_(nullptr),
      input_(),
      inputStorage_(nullptr),
      in_(nullptr),
      pcm_(nullptr),
      inLength_(0),
      skip_(0),
      id3Checked_(false),
      starving_(false),
      bufferTarget_(0),
      mode_(kModeIdle),
      outputRate_(0),
      repeat_(1),
      steps_{},
      stepCount_(0),
      stepIndex_(0),
      stepFrames_(0),
      stepPosition_(0),
      phase_(0.0f),
      moduleFile_(nullptr),
      moduleFileSize_(0),
      moduleWrite_(nullptr),
      moduleWriteSize_(0),
      moduleWritten_(0),
      tracker_(nullptr),
      mix_(nullptr),
      title_{},
      pcmRate_(0),
      pcmChannels_(2),
      ready_(false),
      streamOpen_(false),
      moduleOpen_(false),
      format_(kFormatNone),
      position_(0),
      length_(0),
      row_(0),
      moduleChannels_(0),
      state_(kStateOffline),
      finished_(false),
      decoded_(false),
      buffering_(false),
      paused_(false),
      volume_(volumeScale(kDefaultVolume)),
      sampleRate_(0),
      channels_(0),
      bitrateKbps_(0),
      framesPlayed_(0),
      playedMs_(0),
      decodeErrors_(0),
      underruns_(0),
      i2sFaults_(0),
      dmaUnderflows_(0),
      watchStartMs_(0),
      watchUnderflows_(0),
      watchFrames_(0),
      watchWrites_(0),
      watchMaxUs_(0),
      watchSlow_(0),
      watchPeak_(0),
      toneStartMs_(0),
      toneUnderflows_(0),
      toneWrites_(0),
      toneMaxUs_(0),
      toneMaxGapUs_(0),
      toneLastUs_(0) {}

bool AudioPlayer::begin(bool usePsram, bool playChime) {
  if (task_ != nullptr) {
    return ready_;
  }
  size_t ringBytes = kRingInternalBytes;
  if (usePsram) {
    inputStorage_ = static_cast<uint8_t*>(heap_caps_malloc(
        kRingPsramBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (inputStorage_ != nullptr) {
      ringBytes = kRingPsramBytes;
    }
  }
  if (inputStorage_ == nullptr) {
    inputStorage_ = static_cast<uint8_t*>(heap_caps_malloc(
        kRingInternalBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  in_ = static_cast<uint8_t*>(
      heap_caps_malloc(kInputBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  pcm_ = static_cast<int16_t*>(heap_caps_malloc(
      kPcmSamples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  mix_ = static_cast<float*>(heap_caps_malloc(
      kModuleFrames * 2 * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  commands_ = xQueueCreate(4, sizeof(Command));
  opened_ = xSemaphoreCreateBinary();
  if (inputStorage_ == nullptr || in_ == nullptr || pcm_ == nullptr ||
      mix_ == nullptr || commands_ == nullptr || opened_ == nullptr ||
      !input_.attach(inputStorage_, ringBytes)) {
    ESP_LOGE(kTag, "no memory for audio");
    return false;
  }
  // I2S запускает сама задача звука: его прерывание остаётся на ядре 0 и не
  // мешает обмену с Z80 на ядре 1.
  if (xTaskCreatePinnedToCore(taskEntry, "zifi-audio", kTaskStackBytes, this,
                              kTaskPriority, &task_, kTaskCore) != pdPASS) {
    task_ = nullptr;
    ESP_LOGE(kTag, "audio task was not created");
    return false;
  }
  xSemaphoreTake(opened_, pdMS_TO_TICKS(kStartTimeoutMs));
  if (ready_ && playChime) {
    // Левый канал, правый, затем оба: разводка каналов и сам ЦАП проверяются
    // на слух после включения, ещё без единой программы на Spectrum. Сначала
    // пауза: усилитель и монитор просыпаются позже, чем запускается ESP.
    static constexpr ToneStep kChime[kMaxToneSteps] = {
        {0, 0, 440, 1500},
        {kChannelLeft, 128, 660, 400},
        {0, 0, 440, 150},
        {kChannelRight, 128, 880, 400},
        {0, 0, 440, 150},
        {kChannelBoth, 128, 1320, 500},
    };
    sendTone(kChime, kMaxToneSteps);
  }
  bringupLog(kTag, "I2S %s, boot chime %s", ready_ ? "ready" : "FAILED",
             ready_ && playChime ? "queued" : "skipped");
  return ready_;
}

void AudioPlayer::taskEntry(void* context) {
  static_cast<AudioPlayer*>(context)->taskLoop();
}

bool AudioPlayer::startI2s() {
  i2s_chan_config_t channelConfig = {};
  channelConfig.id = I2S_NUM_0;
  channelConfig.role = I2S_ROLE_MASTER;
  // 8 x 480 отсчётов — около 87 мс запаса на 44,1 кГц.
  channelConfig.dma_desc_num = kDmaDescriptors;
  channelConfig.dma_frame_num = kDmaFrames;
  // Когда данных нет, в ЦАП идут нули, а тактовые не останавливаются: PCM5102
  // не теряет захват и не щёлкает на каждом старте звука.
  channelConfig.auto_clear_after_cb = true;

  i2s_chan_handle_t channel = nullptr;
  esp_err_t error = i2s_new_channel(&channelConfig, &channel, nullptr);
  if (error != ESP_OK) {
    ESP_LOGE(kTag, "i2s_new_channel: %s", esp_err_to_name(error));
    return false;
  }

  i2s_std_config_t config = {};
  config.clk_cfg = clockFor(kDefaultRate);
  // Формат Philips I2S (перемычка FMT модуля в L), 16 бит в 32-битном слоте:
  // BCK = 64 fs.
  config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
  config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  config.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
  config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  config.slot_cfg.ws_width = 32;
  config.slot_cfg.ws_pol = false;
  config.slot_cfg.bit_shift = true;
  config.slot_cfg.left_align = true;
  config.slot_cfg.big_endian = false;
  config.slot_cfg.bit_order_lsb = false;
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = static_cast<gpio_num_t>(board::kI2sBckPin);
  config.gpio_cfg.ws = static_cast<gpio_num_t>(board::kI2sLrckPin);
  config.gpio_cfg.dout = static_cast<gpio_num_t>(board::kI2sDataPin);
  config.gpio_cfg.din = I2S_GPIO_UNUSED;

  error = i2s_channel_init_std_mode(channel, &config);
  if (error == ESP_OK) {
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_send_q_ovf = onSendOverflow;
    i2s_channel_register_event_callback(channel, &callbacks, &dmaUnderflows_);
    error = i2s_channel_enable(channel);
  }
  if (error != ESP_OK) {
    ESP_LOGE(kTag, "i2s start: %s", esp_err_to_name(error));
    i2s_del_channel(channel);
    return false;
  }
  channel_ = channel;
  outputRate_ = kDefaultRate;
  driveI2sPads();
  gpio_drive_cap_t drive = GPIO_DRIVE_CAP_DEFAULT;
  gpio_get_drive_capability(static_cast<gpio_num_t>(board::kI2sBckPin), &drive);
  ESP_LOGI(kTag, "I2S ready: BCK=%d LRCK=%d DATA=%d, drive %d", board::kI2sBckPin,
           board::kI2sLrckPin, board::kI2sDataPin, static_cast<int>(drive));
  return true;
}

void AudioPlayer::setOutputRate(uint32_t sourceRate) {
  repeat_ = sourceRate < kMinDacRate ? 2 : 1;
  const uint32_t wanted = sourceRate * repeat_;
  if (wanted == outputRate_) {
    return;
  }
  const i2s_std_clk_config_t clock = clockFor(wanted);
  const esp_err_t stopped = i2s_channel_disable(channelOf(channel_));
  // Недоигранное в DMA на новой частоте прозвучало бы хрипом.
  preloadSilence(channelOf(channel_));
  const esp_err_t changed =
      i2s_channel_reconfig_std_clock(channelOf(channel_), &clock);
  if (changed == ESP_OK) {
    outputRate_ = wanted;
  }
  const esp_err_t started = i2s_channel_enable(channelOf(channel_));
  if (stopped != ESP_OK || changed != ESP_OK || started != ESP_OK) {
    // Канал мог остаться выключенным: тактовых нет, и звука не будет.
    bringupLog(kTag, "I2S rate %u Hz: stop %s, set %s, start %s",
               static_cast<unsigned>(wanted), esp_err_to_name(stopped),
               esp_err_to_name(changed), esp_err_to_name(started));
  }
}

// Звук оборван (стоп, выход из плеера, перемотка): то, что уже ушло в буферы
// DMA (до 8 x 480 кадров, около 87 мс), не доигрывать. Канал на мгновение
// останавливается, буферы заполняются нулями, тактовые идут снова.
void AudioPlayer::silenceOutput() {
  const esp_err_t stopped = i2s_channel_disable(channelOf(channel_));
  preloadSilence(channelOf(channel_));
  const esp_err_t started = i2s_channel_enable(channelOf(channel_));
  if (stopped != ESP_OK || started != ESP_OK) {
    bringupLog(kTag, "I2S silence: stop %s, start %s", esp_err_to_name(stopped),
               esp_err_to_name(started));
  }
}

void AudioPlayer::writeI2s(size_t bytes) {
  uint16_t peak = watchPeak_;
  for (size_t index = 0; index < bytes / sizeof(int16_t); ++index) {
    const int32_t value = pcm_[index];
    const uint16_t magnitude = static_cast<uint16_t>(value < 0 ? -value : value);
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  watchPeak_ = peak;
  size_t written = 0;
  const int64_t started = esp_timer_get_time();
  const esp_err_t error = i2s_channel_write(channelOf(channel_), pcm_, bytes,
                                            &written, kI2sWriteTimeoutMs);
  const uint32_t spent = static_cast<uint32_t>(esp_timer_get_time() - started);
  ++watchWrites_;
  watchFrames_ += static_cast<uint32_t>(written / 4);
  if (spent > watchMaxUs_) {
    watchMaxUs_ = spent;
  }
  if (spent > kSlowWriteUs) {
    ++watchSlow_;
  }
  if (written != 0 && outputRate_ != 0) {
    // Запись возвращается, когда в DMA нашлось место, — то есть DMA снова полон.
    lastWriteMs_.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
    dmaTailMs_.store(kDmaDescriptors * kDmaFrames * 1000U / outputRate_);
  }
  if (error == ESP_OK && written == bytes) {
    return;
  }
  // Отсчёты не ушли в ЦАП, хотя программа считает, что звук идёт.
  uint16_t faults = i2sFaults_.load();
  if (faults != 0xFFFF) {
    i2sFaults_.store(++faults);
  }
  if (faults <= kI2sFaultReports) {
    bringupLog(kTag, "I2S write failed: %s, %u of %u bytes",
               esp_err_to_name(error), static_cast<unsigned>(written),
               static_cast<unsigned>(bytes));
  }
}

void AudioPlayer::taskLoop() {
  const bool started = startI2s();
  state_.store(started ? kStateIdle : kStateOffline);
  ready_ = started;
  xSemaphoreGive(opened_);
  if (!started) {
    vTaskDelete(nullptr);
    return;
  }
  for (;;) {
    Command command;
    // В тишине и пока модуль принимается задача спит до команды; нули в ЦАП
    // драйвер шлёт сам.
    TickType_t wait =
        mode_ == kModeIdle || mode_ == kModeLoading ? portMAX_DELAY : 0;
    while (xQueueReceive(commands_, &command, wait) == pdTRUE) {
      apply(command);
      wait = 0;
    }
    if (mode_ == kModeStream) {
      streamStep();
    } else if (mode_ == kModeTone) {
      toneStep();
    } else if (mode_ == kModeModule) {
      moduleStep();
    } else if (mode_ == kModePcm) {
      pcmStep();
    }
    watchStep();
  }
}

// Раз в 2 с звука — в журнал, что уходит в ЦАП: пик отсчётов, кадры, записи в
// I2S и самая долгая из них, недоливы DMA (каждый — буфер тишины, около
// 11 мс), фронты на LRCK и DATA за 2 мс (DATA без фронтов — в ЦАП нули).
// Записи во flash во время звука отложены (blocksFlashWrites), в журнал они
// попадут после.
void AudioPlayer::watchStep() {
  const bool sounding = (mode_ == kModeStream || mode_ == kModeModule || mode_ == kModePcm) &&
                        !paused_.load() && !buffering_.load();
  const uint32_t now = millis();
  if (!sounding || watchStartMs_ == 0) {
    watchStartMs_ = sounding ? (now == 0 ? 1 : now) : 0;
    watchUnderflows_ = dmaUnderflows_.load();
    watchFrames_ = watchWrites_ = watchMaxUs_ = watchSlow_ = 0;
    watchPeak_ = 0;
    return;
  }
  if (static_cast<uint32_t>(now - watchStartMs_) < kWatchPeriodMs) {
    return;
  }
  const uint32_t underflows = dmaUnderflows_.load();
  if (g_watchReports.fetch_add(1) < kWatchReports) {
    // Фронты (подъёмы): BCK за 200 мкс (2,82 МГц — около 564), LRCK и DATA за 2 мс
    // (LRCK 44,1 кГц — около 88).
    PadView bck = {};
    PadView lrck = {};
    PadView data = {};
    countPadEdges(board::kI2sBckPin, 200, bck);
    countPadEdges(board::kI2sLrckPin, 2000, lrck);
    countPadEdges(board::kI2sDataPin, 2000, data);
    // Строка журнала — до 136 знаков: коротко.
    bringupLog(kTag, "play %s %us pk%u fr%u/%ums w%u max%uus udf%u flt%u v%u BCK%d LR%d D%d",
               mode_ == kModeStream ? "mp3" : (mode_ == kModeModule ? "mod" : "pcm"),
               static_cast<unsigned>(playedMs_.load() / 1000), static_cast<unsigned>(watchPeak_),
               static_cast<unsigned>(watchFrames_), static_cast<unsigned>(now - watchStartMs_),
               static_cast<unsigned>(watchWrites_), static_cast<unsigned>(watchMaxUs_),
               static_cast<unsigned>(underflows - watchUnderflows_),
               static_cast<unsigned>(i2sFaults_.load()), static_cast<unsigned>(volume_.load()),
               bck.counted, lrck.counted, data.counted);
  }
  watchStartMs_ = millis();
  if (watchStartMs_ == 0) {
    watchStartMs_ = 1;
  }
  watchUnderflows_ = dmaUnderflows_.load();
  watchFrames_ = watchWrites_ = watchMaxUs_ = watchSlow_ = 0;
  watchPeak_ = 0;
}

void AudioPlayer::apply(const Command& command) {
  switch (command.type) {
    case kCommandPcmOpen:
      // Приложение ESP выводит готовые отсчёты: прочие режимы — прочь.
      releaseModule();
      streamOpen_.store(false);
      moduleOpen_.store(false);
      input_.discard(input_.available());
      pcmChannels_ = command.argument;
      pcmRate_.store(command.value);
      setOutputRate(command.value);
      sampleRate_.store(command.value);
      channels_.store(pcmChannels_);
      paused_.store(false);
      finished_.store(false);
      buffering_.store(false);
      mode_ = kModePcm;
      state_.store(kStatePlaying);
      xSemaphoreGive(opened_);
      break;

    case kCommandPcmFlush:
      if (mode_ == kModePcm) {
        input_.discard(input_.available());
        silenceOutput();
      }
      xSemaphoreGive(opened_);
      break;

    case kCommandPcmClose:
      if (mode_ == kModePcm) {
        input_.discard(input_.available());
        silenceOutput();
        mode_ = kModeIdle;
        state_.store(kStateIdle);
        pcmRate_.store(0);
      }
      xSemaphoreGive(opened_);
      break;

    case kCommandOpen:
      releaseModule();
      format_.store(kFormatMp3);
      input_.discard(input_.available());
      if (decoder_ != nullptr) {
        MP3FreeDecoder(decoder_);
      }
      decoder_ = MP3InitDecoder();
      inLength_ = 0;
      skip_ = 0;
      id3Checked_ = false;
      starving_ = false;
      // Не больше половины очереди: иначе при малой очереди (нет PSRAM)
      // предзагрузка не закончилась бы никогда.
      bufferTarget_ = kStartBufferBytes < input_.capacity() / 2
                          ? kStartBufferBytes
                          : input_.capacity() / 2;
      buffering_.store(true);
      paused_.store(false);
      finished_.store(false);
      decoded_.store(false);
      sampleRate_.store(0);
      channels_.store(0);
      bitrateKbps_.store(0);
      framesPlayed_.store(0);
      playedMs_.store(0);
      decodeErrors_.store(0);
      underruns_.store(0);
      mode_ = decoder_ != nullptr ? kModeStream : kModeIdle;
      state_.store(decoder_ != nullptr ? kStatePlaying : kStateIdle);
      xSemaphoreGive(opened_);
      break;

    case kCommandClose:
      if (command.argument != 0 &&
          (mode_ == kModeStream || mode_ == kModeModule)) {
        // Модуль принят целиком: «доиграть» — до конца песни.
        state_.store(kStateDraining);
      } else {
        input_.discard(input_.available());
        releaseModule();
        if (mode_ != kModeIdle && mode_ != kModeLoading) {
          silenceOutput();               // выход из плеера: ни звука после
        }
        mode_ = kModeIdle;
        state_.store(kStateIdle);
        buffering_.store(false);
        paused_.store(false);
      }
      break;

    case kCommandOpenModule:
      // Буфер файла выделило ядро 1 и сейчас будет его заполнять; разбирать
      // его задача начнёт по StartModule.
      releaseModule();
      input_.discard(input_.available());
      moduleFile_ = command.data;
      moduleFileSize_ = command.value;
      format_.store(kFormatNone);
      title_[0] = 0;
      position_.store(0);
      length_.store(0);
      row_.store(0);
      moduleChannels_.store(0);
      finished_.store(false);
      decoded_.store(false);
      paused_.store(false);
      buffering_.store(true);
      sampleRate_.store(0);
      channels_.store(0);
      bitrateKbps_.store(0);
      framesPlayed_.store(0);
      playedMs_.store(0);
      decodeErrors_.store(0);
      underruns_.store(0);
      mode_ = kModeLoading;
      state_.store(kStateLoading);
      xSemaphoreGive(opened_);
      break;

    case kCommandStartModule:
      if (mode_ == kModeLoading) {
        startModule();
      }
      break;

    case kCommandSeek:
      if (mode_ == kModeModule && tracker_ != nullptr) {
        auto* context = static_cast<xm_context_t*>(tracker_);
        const int32_t length = xm_get_module_length(context);
        uint8_t index = 0;
        xm_get_position(context, &index, nullptr, nullptr, nullptr);
        int32_t target = static_cast<int32_t>(index) +
                         static_cast<int8_t>(command.value & 0xFF);
        if (target < 0) {
          target = 0;
        }
        if (target >= length) {
          target = length > 0 ? length - 1 : 0;
        }
        xm_seek(context, static_cast<uint8_t>(target), 0, 0);
        silenceOutput();
        position_.store(static_cast<uint16_t>(target));
        row_.store(0);
      }
      xSemaphoreGive(opened_);
      break;

    case kCommandFlush:
      // Перемотка MP3: всё, что лежит в очереди, — уже не с того места. Декодер
      // заново: его «резервуар бит» ссылается на прежние кадры.
      if (mode_ == kModeStream) {
        input_.discard(input_.available());
        silenceOutput();
        if (decoder_ != nullptr) {
          MP3FreeDecoder(decoder_);
        }
        decoder_ = MP3InitDecoder();
        inLength_ = 0;
        skip_ = 0;
        id3Checked_ = true;              // середина файла: метки ID3 там нет
        starving_ = false;
        bufferTarget_ = kRefillBufferBytes < input_.capacity() / 2
                            ? kRefillBufferBytes
                            : input_.capacity() / 2;
        buffering_.store(true);
        const uint32_t rate = sampleRate_.load();
        framesPlayed_.store(rate == 0 ? 0
                                      : static_cast<uint32_t>(
                                            static_cast<uint64_t>(command.value) *
                                            rate / 1000U));
        playedMs_.store(command.value);
        if (decoder_ == nullptr) {
          mode_ = kModeIdle;
          state_.store(kStateIdle);
        }
      }
      xSemaphoreGive(opened_);
      break;

    case kCommandTone:
      releaseModule();
      buffering_.store(false);
      paused_.store(false);
      input_.discard(input_.available());
      stepCount_ = command.stepCount <= kMaxToneSteps ? command.stepCount
                                                      : kMaxToneSteps;
      memcpy(steps_, command.steps, sizeof(steps_));
      stepIndex_ = 0;
      stepPosition_ = 0;
      setOutputRate(kDefaultRate);
      mode_ = kModeTone;
      state_.store(kStateTone);
      toneStartMs_ = millis();
      toneUnderflows_ = dmaUnderflows_.load();
      toneWrites_ = toneMaxUs_ = toneMaxGapUs_ = 0;
      toneLastUs_ = 0;
      break;

    default:
      break;
  }
}

// Отдать модуль: контекст трекера и буфер файла. Только задача звука.
void AudioPlayer::releaseModule() {
  if (tracker_ != nullptr) {
    xm_free_context(static_cast<xm_context_t*>(tracker_));
    tracker_ = nullptr;
  }
  if (moduleFile_ != nullptr) {
    heap_caps_free(moduleFile_);
    moduleFile_ = nullptr;
  }
  moduleFileSize_ = 0;
}

static void* moduleAlloc(size_t size) {
  void* memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return memory != nullptr ? memory : malloc(size);
}

// Файл принят целиком: разобрать (вид модуля — по содержимому) и играть.
void AudioPlayer::startModule() {
  xm_context_t* context = nullptr;
  uint8_t format = kFormatNone;
  const uint8_t* file = moduleFile_;
  const uint32_t size = moduleFileSize_;
  int result = -1;
  size_t needed = 0;
  if (file != nullptr && size >= 17 &&
      memcmp(file, "Extended Module:", 16) == 0) {
    format = kFormatXm;
    result = xm_create_context_safe(&context, moduleFile_, size, kDefaultRate,
                                    moduleAlloc);
  } else if (file != nullptr && size >= 48 && memcmp(file + 44, "SCRM", 4) == 0) {
    format = kFormatS3m;
    result = s3m_create_context_safe(&context, moduleFile_, size, kDefaultRate,
                                     moduleAlloc, &needed);
  } else if (file != nullptr && mod_check_sanity_preload(file, size) == 0) {
    format = kFormatMod;
    result = mod_create_context_safe(&context, moduleFile_, size, kDefaultRate,
                                     moduleAlloc, &needed);
  }
  // Контекст держит свои копии нот и отсчётов: файл больше не нужен.
  heap_caps_free(moduleFile_);
  moduleFile_ = nullptr;
  moduleFileSize_ = 0;
  if (result < 0 || context == nullptr) {
    if (context != nullptr) {
      xm_free_context(context);
    }
    decodeErrors_.fetch_add(1);
    finished_.store(true);
    buffering_.store(false);
    mode_ = kModeIdle;
    state_.store(kStateIdle);
    bringupLog(kTag, "module of %u bytes was not loaded: format %u, result %d",
               static_cast<unsigned>(size), static_cast<unsigned>(format), result);
    return;
  }
  tracker_ = context;
  // Песня играет один раз: дальше — следующий файл панели.
  xm_set_max_loop_count(context, 1);
  snprintf(title_, sizeof(title_), "%s", xm_get_module_name(context));
  length_.store(xm_get_module_length(context));
  moduleChannels_.store(static_cast<uint8_t>(xm_get_number_of_channels(context)));
  format_.store(format);
  setOutputRate(kDefaultRate);
  sampleRate_.store(kDefaultRate);
  channels_.store(2);
  decoded_.store(true);
  buffering_.store(false);
  mode_ = kModeModule;
  state_.store(kStatePlaying);
  if (audioReportAllowed()) {
    bringupLog(kTag, "module: format %u, %u bytes, %u channels, %u positions, \"%s\"",
               static_cast<unsigned>(format), static_cast<unsigned>(size),
               static_cast<unsigned>(moduleChannels_.load()),
               static_cast<unsigned>(length_.load()), title_);
  }
}

void AudioPlayer::moduleStep() {
  if (paused_.load()) {
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }
  auto* context = static_cast<xm_context_t*>(tracker_);
  if (xm_get_loop_count(context) > 0) {
    // Песня кончилась: дальше трекер дал бы тишину.
    releaseModule();
    mode_ = kModeIdle;
    finished_.store(true);
    paused_.store(false);
    state_.store(kStateIdle);
    if (audioReportAllowed()) {
      bringupLog(kTag, "module played to the end: %u ms",
                 static_cast<unsigned>(playedMs_.load()));
    }
    return;
  }
  xm_render(context, mix_, kModuleFrames);
  const float gain = 32767.0f * static_cast<float>(volume_.load()) / 256.0f;
  for (size_t index = 0; index < kModuleFrames * 2; ++index) {
    float value = mix_[index] * gain;
    if (value > 32767.0f) {
      value = 32767.0f;
    } else if (value < -32768.0f) {
      value = -32768.0f;
    }
    pcm_[index] = static_cast<int16_t>(value);
  }
  writeI2s(kModuleFrames * 2 * sizeof(int16_t));
  uint8_t index = 0;
  uint8_t row = 0;
  xm_get_position(context, &index, nullptr, &row, nullptr);
  position_.store(index);
  row_.store(row);
  // Счётчик отсчётов трекера (generated_samples) в компоненте не ведётся и
  // всегда 0: сыгранное считаем сами.
  const uint32_t played = framesPlayed_.load() + static_cast<uint32_t>(kModuleFrames);
  framesPlayed_.store(played);
  playedMs_.store(static_cast<uint32_t>(static_cast<uint64_t>(played) * 1000U / kDefaultRate));
}

// Вывод PCM приложения: из очереди (int16 стерео) — в ЦАП блоками.
void AudioPlayer::pcmStep() {
  if (paused_.load()) {
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }
  // Кадров вдвое меньше отсчётов буфера; при удвоении кадров (источник ниже 16 кГц,
  // play) — вчетверо: иначе блок не удваивался и звучал вдвое быстрее и выше.
  const size_t blockBytes = (repeat_ == 2 ? kPcmSamples / 4 : kPcmSamples / 2) * 2 * sizeof(int16_t);
  size_t available = input_.available() & ~static_cast<size_t>(3);
  if (available == 0) {
    vTaskDelay(pdMS_TO_TICKS(2));
    return;
  }
  if (available > blockBytes) {
    available = blockBytes;
  }
  input_.read(reinterpret_cast<uint8_t*>(pcm_), available);
  const size_t frames = available / 4;
  const uint32_t played = framesPlayed_.fetch_add(static_cast<uint32_t>(frames)) +
                          static_cast<uint32_t>(frames);
  const uint32_t rate = pcmRate_.load();
  if (rate != 0) {
    playedMs_.store(static_cast<uint32_t>(static_cast<uint64_t>(played) * 1000U / rate));
  }
  play(frames);
}

void AudioPlayer::consume(size_t count) {
  if (count >= inLength_) {
    inLength_ = 0;
    return;
  }
  memmove(in_, in_ + count, inLength_ - count);
  inLength_ -= count;
}

void AudioPlayer::starve() {
  // Счётчик растёт один раз на паузу: Z80 не успела дочитать файл с карты.
  // Звук продолжится, когда очередь снова наберётся.
  if (decoded_.load() && !starving_) {
    starving_ = true;
    underruns_.fetch_add(1);
    bufferTarget_ = kRefillBufferBytes < input_.capacity() / 2
                        ? kRefillBufferBytes
                        : input_.capacity() / 2;
    buffering_.store(true);
  }
  vTaskDelay(pdMS_TO_TICKS(5));
}

void AudioPlayer::finishStream() {
  mode_ = kModeIdle;
  finished_.store(true);
  buffering_.store(false);
  paused_.store(false);
  state_.store(kStateIdle);
  if (audioReportAllowed()) {
    bringupLog(kTag, "stream played to the end: %u ms, bad frames %u, starved %u",
               static_cast<unsigned>(playedMs_.load()),
               static_cast<unsigned>(decodeErrors_.load()),
               static_cast<unsigned>(underruns_.load()));
  }
}

void AudioPlayer::streamStep() {
  if (paused_.load()) {
    // Данные остаются в очереди; драйвер I2S сам шлёт в ЦАП нули.
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }
  if (inLength_ < kInputBytes) {
    inLength_ += input_.read(in_ + inLength_, kInputBytes - inLength_);
  }
  const bool draining = state_.load() == kStateDraining;
  const bool drained = input_.available() == 0;

  if (!id3Checked_) {
    if (inLength_ < 10) {
      if (draining && drained) {
        finishStream();
      } else {
        starve();
      }
      return;
    }
    // Метка ID3v2 в начале файла бывает больше сотни килобайт (обложка).
    // Декодеру она не нужна, а случайные #FF внутри выглядят как начало кадра.
    if (memcmp(in_, "ID3", 3) == 0) {
      skip_ = 10 + ((static_cast<size_t>(in_[6] & 0x7F) << 21) |
                    (static_cast<size_t>(in_[7] & 0x7F) << 14) |
                    (static_cast<size_t>(in_[8] & 0x7F) << 7) |
                    static_cast<size_t>(in_[9] & 0x7F));
      if ((in_[5] & 0x10) != 0) {
        skip_ += 10;
      }
    }
    id3Checked_ = true;
  }

  if (skip_ != 0) {
    const size_t part = skip_ < inLength_ ? skip_ : inLength_;
    consume(part);
    skip_ -= part;
    if (part == 0) {
      if (draining && drained) {
        finishStream();
      } else {
        starve();
      }
    }
    return;
  }

  if (buffering_.load()) {
    // Ждём запаса. Закрытый поток доигрывается как есть; полная очередь —
    // тоже повод начать: больше в неё не войдёт.
    if (input_.available() + inLength_ < bufferTarget_ && !draining &&
        input_.freeSpace() >= kInputBytes) {
      vTaskDelay(pdMS_TO_TICKS(5));
      return;
    }
    buffering_.store(false);
  }

  const int offset = MP3FindSyncWord(in_, static_cast<int>(inLength_));
  if (offset < 0) {
    // Начала кадра в буфере нет. Последний байт может оказаться его первой
    // половиной, поэтому он остаётся.
    if (inLength_ > 1) {
      consume(inLength_ - 1);
    }
    if (drained) {
      if (draining) {
        finishStream();
      } else {
        starve();
      }
    }
    return;
  }
  if (offset > 0) {
    consume(static_cast<size_t>(offset));
  }

  unsigned char* cursor = in_;
  int left = static_cast<int>(inLength_);
  const int result = MP3Decode(decoder_, &cursor, &left, pcm_, 0);
  const size_t used = inLength_ - static_cast<size_t>(left);

  if (result == ERR_MP3_INDATA_UNDERFLOW) {
    if (inLength_ == kInputBytes) {
      // Буфер полон, а кадру всё мало: это было ложное начало кадра.
      consume(1);
    } else if (drained) {
      if (draining) {
        finishStream();
      } else {
        starve();
      }
    }
    return;
  }
  if (result == ERR_MP3_MAINDATA_UNDERFLOW) {
    // Кадр принят, но звука ещё нет: его данные ссылаются на предыдущие кадры.
    consume(used != 0 ? used : 1);
    return;
  }
  if (result != ERR_MP3_NONE) {
    decodeErrors_.fetch_add(1);
    consume(1);
    return;
  }
  consume(used);

  MP3FrameInfo info = {};
  MP3GetLastFrameInfo(decoder_, &info);
  if (info.outputSamps <= 0 || info.samprate <= 0 ||
      (info.nChans != 1 && info.nChans != 2) ||
      static_cast<size_t>(info.outputSamps) > kPcmSamples) {
    decodeErrors_.fetch_add(1);
    return;
  }
  const size_t frames =
      static_cast<size_t>(info.outputSamps) / static_cast<size_t>(info.nChans);
  if (info.nChans == 1) {
    // Моно раздаётся в оба канала; разворачиваем с конца, на месте.
    for (size_t index = frames; index-- > 0;) {
      const int16_t sample = pcm_[index];
      pcm_[2 * index] = sample;
      pcm_[2 * index + 1] = sample;
    }
  }
  setOutputRate(static_cast<uint32_t>(info.samprate));
  sampleRate_.store(static_cast<uint32_t>(info.samprate));
  channels_.store(static_cast<uint8_t>(info.nChans));
  bitrateKbps_.store(static_cast<uint16_t>(info.bitrate / 1000));
  if (!decoded_.load() && audioReportAllowed()) {
    bringupLog(kTag, "stream: first frame %d Hz, %d ch, %d kbit/s, volume %u",
               info.samprate, info.nChans, info.bitrate / 1000,
               static_cast<unsigned>(volume_.load()));
  }
  decoded_.store(true);
  starving_ = false;

  play(frames);

  const uint32_t played =
      framesPlayed_.fetch_add(static_cast<uint32_t>(frames)) +
      static_cast<uint32_t>(frames);
  playedMs_.store(static_cast<uint32_t>(
      static_cast<uint64_t>(played) * 1000U /
      static_cast<uint32_t>(info.samprate)));
}

void AudioPlayer::play(size_t frames) {
  size_t total = frames;
  if (repeat_ == 2 && frames * 4 <= kPcmSamples) {
    for (size_t index = frames; index-- > 0;) {
      const int16_t left = pcm_[2 * index];
      const int16_t right = pcm_[2 * index + 1];
      pcm_[4 * index] = left;
      pcm_[4 * index + 1] = right;
      pcm_[4 * index + 2] = left;
      pcm_[4 * index + 3] = right;
    }
    total = frames * 2;
  }
  const int32_t volume = volume_.load();
  if (volume != kFullVolume) {
    for (size_t index = 0; index < total * 2; ++index) {
      pcm_[index] = static_cast<int16_t>((pcm_[index] * volume) >> 8);
    }
  }
  writeI2s(total * 2 * sizeof(int16_t));
}

void AudioPlayer::toneStep() {
  if (stepIndex_ >= stepCount_) {
    // Последняя запись только что вернулась — DMA полон, и недоливов от конца
    // тона в счёте ещё нет.
    const uint32_t underflows = dmaUnderflows_.load() - toneUnderflows_;
    const uint32_t now = millis();
    mode_ = kModeIdle;
    state_.store(kStateIdle);
    bringupLog(kTag, "tone: %u steps from %ums, %ums, w%u max%uus gap%uus udf%u flt%u",
               static_cast<unsigned>(stepCount_), static_cast<unsigned>(toneStartMs_),
               static_cast<unsigned>(now - toneStartMs_), static_cast<unsigned>(toneWrites_),
               static_cast<unsigned>(toneMaxUs_), static_cast<unsigned>(toneMaxGapUs_),
               static_cast<unsigned>(underflows), static_cast<unsigned>(i2sFaults_.load()));
    return;
  }
  const ToneStep& step = steps_[stepIndex_];
  if (stepPosition_ == 0) {
    stepFrames_ = static_cast<uint32_t>(
        static_cast<uint64_t>(step.milliseconds) * outputRate_ / 1000U);
    phase_ = 0.0f;
    if (stepFrames_ == 0) {
      ++stepIndex_;
      return;
    }
  }
  const uint32_t remaining = stepFrames_ - stepPosition_;
  const uint32_t frames =
      remaining < kToneBlockFrames ? remaining : kToneBlockFrames;
  const float increment =
      static_cast<float>(step.hertz) / static_cast<float>(outputRate_);
  const float amplitude = 32767.0f * (static_cast<float>(step.level) / 255.0f) *
                          (static_cast<float>(volume_.load()) / 256.0f);
  for (uint32_t index = 0; index < frames; ++index) {
    const uint32_t position = stepPosition_ + index;
    const uint32_t tail = stepFrames_ - position;
    float envelope = 1.0f;
    if (position < kToneRampFrames) {
      envelope = static_cast<float>(position) / kToneRampFrames;
    }
    if (tail < kToneRampFrames) {
      const float fall = static_cast<float>(tail) / kToneRampFrames;
      envelope = fall < envelope ? fall : envelope;
    }
    const int16_t sample =
        static_cast<int16_t>(sinf(phase_ * kTwoPi) * amplitude * envelope);
    phase_ += increment;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
    }
    pcm_[2 * index] = (step.channels & kChannelLeft) != 0 ? sample : 0;
    pcm_[2 * index + 1] = (step.channels & kChannelRight) != 0 ? sample : 0;
  }
  const int64_t started = esp_timer_get_time();
  if (toneLastUs_ != 0) {
    const uint32_t gap = static_cast<uint32_t>(started - toneLastUs_);
    toneMaxGapUs_ = gap > toneMaxGapUs_ ? gap : toneMaxGapUs_;
  }
  writeI2s(frames * 2 * sizeof(int16_t));
  toneLastUs_ = esp_timer_get_time();
  const uint32_t spent = static_cast<uint32_t>(toneLastUs_ - started);
  toneMaxUs_ = spent > toneMaxUs_ ? spent : toneMaxUs_;
  ++toneWrites_;
  stepPosition_ += frames;
  if (stepPosition_ >= stepFrames_) {
    ++stepIndex_;
    stepPosition_ = 0;
  }
}

bool AudioPlayer::pcmOpen(uint32_t rate, uint8_t channels) {
  if (!ready_ || rate < 8000 || rate > 96000 || (channels != 1 && channels != 2)) {
    return false;
  }
  streamOpen_.store(false);
  moduleOpen_.store(false);
  Command command = {};
  command.type = kCommandPcmOpen;
  command.argument = channels;
  command.value = rate;
  framesPlayed_.store(0);
  playedMs_.store(0);
  return waitOpened(command);
}

size_t AudioPlayer::pcmWrite(const float* frames, size_t count) {
  if (!ready_ || frames == nullptr || pcmRate_.load() == 0) {
    return 0;
  }
  const uint8_t channels = pcmChannels_ == 1 ? 1 : 2;
  int16_t chunk[256 * 2];
  size_t done = 0;
  uint32_t waited = 0;
  while (done < count) {
    size_t part = count - done;
    if (part > 256) {
      part = 256;
    }
    const size_t room = input_.freeSpace() / 4;
    if (room == 0) {
      if (waited >= kPcmWriteWaitMs) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
      waited += 2;
      continue;
    }
    if (part > room) {
      part = room;
    }
    const float* source = frames + done * channels;
    for (size_t index = 0; index < part; ++index) {
      float left = source[index * channels];
      float right = channels == 2 ? source[index * channels + 1] : left;
      left = left > 1.0f ? 1.0f : (left < -1.0f ? -1.0f : left);
      right = right > 1.0f ? 1.0f : (right < -1.0f ? -1.0f : right);
      chunk[index * 2] = static_cast<int16_t>(left * 32767.0f);
      chunk[index * 2 + 1] = static_cast<int16_t>(right * 32767.0f);
    }
    input_.write(reinterpret_cast<const uint8_t*>(chunk), part * 4);
    done += part;
  }
  return done;
}

uint32_t AudioPlayer::pcmQueuedMs() const {
  const uint32_t rate = pcmRate_.load();
  if (rate == 0) {
    return 0;
  }
  const uint32_t ring = static_cast<uint32_t>(static_cast<uint64_t>(input_.available() / 4) * 1000U / rate);
  // И то, что DMA ещё не доиграл после последней записи.
  const uint32_t since = static_cast<uint32_t>(esp_timer_get_time() / 1000) - lastWriteMs_.load();
  const uint32_t tail = dmaTailMs_.load();
  return ring + (since < tail ? tail - since : 0);
}

void AudioPlayer::pcmFlush() {
  Command command = {};
  command.type = kCommandPcmFlush;
  waitOpened(command);
}

void AudioPlayer::pcmClose() {
  Command command = {};
  command.type = kCommandPcmClose;
  waitOpened(command);
}

uint8_t AudioPlayer::volume() const {
  const uint16_t value = volume_.load();
  return value >= kFullVolume ? 255 : static_cast<uint8_t>(value);
}

bool AudioPlayer::open() {
  if (!ready_) {
    return false;
  }
  streamOpen_.store(false);
  moduleOpen_.store(false);
  xSemaphoreTake(opened_, 0);
  Command command = {};
  command.type = kCommandOpen;
  // Очередь MP3 чистит сама задача звука (она её читатель), поэтому её ответа
  // нужно дождаться: до него класть в очередь новые данные нельзя.
  if (xQueueSend(commands_, &command, pdMS_TO_TICKS(kCommandTimeoutMs)) !=
          pdTRUE ||
      xSemaphoreTake(opened_, pdMS_TO_TICKS(kCommandTimeoutMs)) != pdTRUE) {
    return false;
  }
  streamOpen_.store(state_.load() == kStatePlaying);
  if (audioReportAllowed()) {
    bringupLog(kTag, "stream open: %s, queue %u bytes",
               streamOpen_.load() ? "ok" : "FAILED",
               static_cast<unsigned>(input_.freeSpace()));
  }
  return streamOpen_.load();
}

// Отдать команду задаче и дождаться, что она её исполнила (opened_).
bool AudioPlayer::waitOpened(const Command& command) {
  xSemaphoreTake(opened_, 0);
  return xQueueSend(commands_, &command, pdMS_TO_TICKS(kCommandTimeoutMs)) ==
             pdTRUE &&
         xSemaphoreTake(opened_, pdMS_TO_TICKS(kCommandTimeoutMs)) == pdTRUE;
}

bool AudioPlayer::openModule(uint32_t size) {
  if (!ready_ || size == 0 || size > kModuleMaxBytes) {
    return false;
  }
  streamOpen_.store(false);
  moduleOpen_.store(false);
  uint8_t* file = static_cast<uint8_t*>(
      heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (file == nullptr) {
    bringupLog(kTag, "module of %u bytes: no memory (PSRAM free %u)",
               static_cast<unsigned>(size),
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return false;
  }
  Command command = {};
  command.type = kCommandOpenModule;
  command.data = file;
  command.value = size;
  if (!waitOpened(command)) {
    // Команда могла дойти до задачи позже: тогда буфер освободит она. Иначе он
    // теряется — так бывает, только если задача звука встала.
    return false;
  }
  moduleWrite_ = file;
  moduleWriteSize_ = size;
  moduleWritten_ = 0;
  moduleOpen_.store(true);
  if (audioReportAllowed()) {
    bringupLog(kTag, "module open: %u bytes", static_cast<unsigned>(size));
  }
  return true;
}

AudioPlayer::FeedResult AudioPlayer::feed(const uint8_t* data, size_t length) {
  if (ready_ && moduleOpen_.load()) {
    // Модуль: файл копится в буфере, играть задача начнёт, когда он целиком.
    if (data == nullptr && length != 0) {
      return kFeedFull;
    }
    if (length > moduleWriteSize_ - moduleWritten_) {
      return kFeedClosed;              // длиннее, чем объявлено при открытии
    }
    memcpy(moduleWrite_ + moduleWritten_, data, length);
    moduleWritten_ += static_cast<uint32_t>(length);
    if (moduleWritten_ == moduleWriteSize_) {
      moduleOpen_.store(false);
      Command command = {};
      command.type = kCommandStartModule;
      xQueueSend(commands_, &command, pdMS_TO_TICKS(kCommandTimeoutMs));
    }
    return kFeedAccepted;
  }
  if (!ready_ || !streamOpen_.load()) {
    return kFeedClosed;
  }
  if (length == 0) {
    return kFeedAccepted;
  }
  // Блок берётся целиком либо не берётся: Z80 повторит его без учёта частей.
  if (data == nullptr || input_.freeSpace() < length) {
    return kFeedFull;
  }
  input_.write(data, length);
  return kFeedAccepted;
}

void AudioPlayer::close(bool drain) {
  if (!ready_) {
    return;
  }
  streamOpen_.store(false);
  moduleOpen_.store(false);
  if (audioReportAllowed()) {
    bringupLog(kTag,
               "stream close (%s): played %u ms, in queue %u, bad frames %u, starved %u, format %s",
               drain ? "play the rest" : "stop",
               static_cast<unsigned>(playedMs_.load()),
               static_cast<unsigned>(input_.available()),
               static_cast<unsigned>(decodeErrors_.load()),
               static_cast<unsigned>(underruns_.load()),
               decoded_.load() ? "known" : "NOT RECOGNISED");
  }
  Command command = {};
  command.type = kCommandClose;
  command.argument = drain ? 1 : 0;
  xQueueSend(commands_, &command, pdMS_TO_TICKS(kCommandTimeoutMs));
}

bool AudioPlayer::seek(int8_t delta, uint16_t& position) {
  if (!ready_ || state_.load() != kStatePlaying ||
      format_.load() == kFormatMp3) {
    return false;
  }
  Command command = {};
  command.type = kCommandSeek;
  command.value = static_cast<uint8_t>(delta);
  if (!waitOpened(command)) {
    return false;
  }
  position = position_.load();
  return true;
}

bool AudioPlayer::flush(uint32_t positionMs) {
  if (!ready_ || !streamOpen_.load()) {
    return false;
  }
  Command command = {};
  command.type = kCommandFlush;
  command.value = positionMs;
  // Новые данные Z80 пришлёт после ответа: до него очередь должна опустеть.
  return waitOpened(command);
}

size_t AudioPlayer::info(uint8_t* output, size_t capacity) const {
  if (output == nullptr || capacity < 6) {
    return 0;
  }
  output[0] = format_.load();
  output[1] = moduleChannels_.load();
  writeLe16(output + 2, length_.load());
  const uint8_t state = state_.load();
  const char* title =
      state == kStatePlaying || state == kStateDraining ? title_ : "";
  size_t length = strnlen(title, sizeof(title_) - 1);
  if (length > capacity - 5) {
    length = capacity - 5;
  }
  memcpy(output + 4, title, length);
  output[4 + length] = 0;
  return 5 + length;
}

bool AudioPlayer::sendTone(const ToneStep* steps, size_t count) {
  if (!ready_ || steps == nullptr || count == 0 || count > kMaxToneSteps) {
    return false;
  }
  streamOpen_.store(false);
  moduleOpen_.store(false);
  Command command = {};
  command.type = kCommandTone;
  command.stepCount = static_cast<uint8_t>(count);
  memcpy(command.steps, steps, count * sizeof(ToneStep));
  return xQueueSend(commands_, &command, pdMS_TO_TICKS(kCommandTimeoutMs)) ==
         pdTRUE;
}

bool AudioPlayer::tone(uint8_t channels, uint16_t hertz, uint16_t milliseconds,
                       uint8_t level) {
  // Выше четверти частоты отсчётов синус из отдельных точек уже не тон.
  if (hertz < 20 || hertz > kDefaultRate / 4 || milliseconds == 0) {
    return false;
  }
  const ToneStep step = {static_cast<uint8_t>(channels & kChannelBoth), level,
                         hertz, milliseconds};
  const bool queued = sendTone(&step, 1);
  if (audioReportAllowed()) {
    bringupLog(kTag, "tone: channels %u, %u Hz, %u ms, level %u, volume %u: %s",
               static_cast<unsigned>(step.channels), static_cast<unsigned>(hertz),
               static_cast<unsigned>(milliseconds), static_cast<unsigned>(level),
               static_cast<unsigned>(volume_.load()),
               queued ? "queued" : "REFUSED");
  }
  return queued;
}

void AudioPlayer::setVolume(uint8_t volume) {
  volume_.store(volumeScale(volume));
}

void AudioPlayer::pause(bool on) {
  if (paused_.exchange(on) != on && audioReportAllowed()) {
    bringupLog(kTag, "stream %s at %u ms, in queue %u", on ? "paused" : "resumed",
               static_cast<unsigned>(playedMs_.load()),
               static_cast<unsigned>(input_.available()));
  }
}

size_t AudioPlayer::freeSpace() const {
  if (!ready_) {
    return 0;
  }
  // У модуля «свободно» — сколько файла ещё ждут.
  if (moduleOpen_.load()) {
    return moduleWriteSize_ - moduleWritten_;
  }
  return input_.freeSpace();
}

size_t AudioPlayer::status(uint8_t* output, size_t capacity) const {
  if (output == nullptr || capacity < kStatusSize) {
    return 0;
  }
  const uint16_t volume = volume_.load();
  output[0] = state_.load();
  output[1] = static_cast<uint8_t>((finished_.load() ? 0x01 : 0) |
                                   (decoded_.load() ? 0x02 : 0) |
                                   (buffering_.load() ? 0x04 : 0) |
                                   (paused_.load() ? 0x08 : 0));
  writeLe32(output + 2, static_cast<uint32_t>(freeSpace()));
  writeLe32(output + 6, static_cast<uint32_t>(input_.available()));
  writeLe16(output + 10, static_cast<uint16_t>(sampleRate_.load()));
  output[12] = channels_.load();
  writeLe16(output + 13, bitrateKbps_.load());
  // Секунды, а не миллисекунды: Z80 делить 32-битное число на 1000 незачем.
  const uint32_t seconds = playedMs_.load() / 1000U;
  writeLe16(output + 15,
            static_cast<uint16_t>(seconds > 0xFFFFU ? 0xFFFFU : seconds));
  writeLe16(output + 17, i2sFaults_.load());
  writeLe16(output + 19, decodeErrors_.load());
  writeLe16(output + 21, underruns_.load());
  output[23] = volume >= kFullVolume ? 255 : static_cast<uint8_t>(volume);
  // С прошивки 0.1.20: что играет и где (у модуля — позиция таблицы порядка).
  output[24] = format_.load();
  output[25] = row_.load();
  writeLe16(output + 26, position_.load());
  writeLe16(output + 28, length_.load());
  output[30] = moduleChannels_.load();
  output[31] = 0;
  return kStatusSize;
}

bool AudioPlayer::probeLines(uint8_t* output, size_t capacity) {
  if (!ready_ || output == nullptr || capacity < kProbeBytes ||
      state_.load() != kStateIdle) {
    return false;
  }
  static constexpr int kPins[3] = {board::kI2sBckPin, board::kI2sLrckPin,
                                   board::kI2sDataPin};
  static constexpr char kNames[3][5] = {"BCK", "LRCK", "DIN"};
  // Время установления: выходу — доли миллисекунды, подтяжке в десятки кОм —
  // больше.
  static constexpr uint32_t kDrivenSettleUs = 300;
  static constexpr uint32_t kPulledSettleUs = 2000;

  adc_oneshot_unit_handle_t unit = nullptr;
  adc_oneshot_unit_init_cfg_t unitConfig = {};
  unitConfig.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unitConfig, &unit) != ESP_OK) {
    return false;
  }
  adc_channel_t channels[3] = {};
  uint32_t signals[3] = {};
  bool ok = true;
  for (int index = 0; index < 3; ++index) {
    adc_unit_t unitId = ADC_UNIT_1;
    signals[index] = GPIO.func_out_sel_cfg[kPins[index]].func_sel;
    if (adc_oneshot_io_to_channel(kPins[index], &unitId, &channels[index]) !=
            ESP_OK ||
        unitId != ADC_UNIT_1) {
      ok = false;
    }
  }
  adc_oneshot_chan_cfg_t channelConfig = {};
  channelConfig.atten = ADC_ATTEN_DB_12;
  channelConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
  // Настройка канала отключает у вывода вход, выход и подтяжки; выход потом
  // включается заново уже как обычный GPIO.
  for (int index = 0; ok && index < 3; ++index) {
    ok = adc_oneshot_config_channel(unit, channels[index], &channelConfig) ==
         ESP_OK;
  }

  int millivolts[3][8] = {};
  for (int line = 0; ok && line < 3; ++line) {
    const gpio_num_t pin = static_cast<gpio_num_t>(kPins[line]);
    const int first = line == 0 ? 1 : 0;
    const int second = line == 2 ? 1 : 2;
    for (int combo = 0; combo < 4; ++combo) {
      const gpio_num_t firstPin = static_cast<gpio_num_t>(kPins[first]);
      const gpio_num_t secondPin = static_cast<gpio_num_t>(kPins[second]);
      gpio_set_level(firstPin, (combo >> 1) & 1);
      gpio_set_direction(firstPin, GPIO_MODE_OUTPUT);
      gpio_set_level(secondPin, combo & 1);
      gpio_set_direction(secondPin, GPIO_MODE_OUTPUT);

      gpio_set_level(pin, 1);
      gpio_set_direction(pin, GPIO_MODE_OUTPUT);
      esp_rom_delay_us(kDrivenSettleUs);
      int raw = 0;
      adc_oneshot_read(unit, channels[line], &raw);
      millivolts[line][combo] = raw * 3100 / 4095;

      // Разрядить линию и оставить на ней одну подтяжку.
      gpio_set_level(pin, 0);
      esp_rom_delay_us(50);
      gpio_set_direction(pin, GPIO_MODE_DISABLE);
      gpio_pullup_en(pin);
      esp_rom_delay_us(kPulledSettleUs);
      raw = 0;
      adc_oneshot_read(unit, channels[line], &raw);
      millivolts[line][4 + combo] = raw * 3100 / 4095;
      gpio_pullup_dis(pin);
    }
  }
  adc_oneshot_del_unit(unit);
  // Вернуть выводам их сигналы I2S.
  for (int index = 0; index < 3; ++index) {
    const gpio_num_t pin = static_cast<gpio_num_t>(kPins[index]);
    gpio_set_level(pin, 0);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(kPins[index], signals[index], false, false);
  }
  driveI2sPads();
  if (!ok) {
    return false;
  }
  for (int line = 0; line < 3; ++line) {
    for (int value = 0; value < 8; ++value) {
      const int scaled = millivolts[line][value] / 20;
      output[line * 8 + value] =
          static_cast<uint8_t>(scaled > 255 ? 255 : scaled);
    }
    bringupLog(kTag,
               "lines %s, mV, the other two at 00 01 10 11: driven high %d %d %d %d, pulled up %d %d %d %d",
               kNames[line], millivolts[line][0], millivolts[line][1],
               millivolts[line][2], millivolts[line][3], millivolts[line][4],
               millivolts[line][5], millivolts[line][6], millivolts[line][7]);
  }
  return true;
}

bool AudioPlayer::logPads() const {
  if (!ready_) {
    return false;
  }
  // Сигналы матрицы GPIO: 22 — BCK, 24 — WS, 25 — данные I2S0. Окна счётчика
  // выбраны так, чтобы он не переполнился: BCK — это миллионы фронтов в секунду.
  logPad(kTag, "BCK", board::kI2sBckPin, 2000);
  logPad(kTag, "LRCK", board::kI2sLrckPin, 20000);
  logPad(kTag, "DATA", board::kI2sDataPin, 20000);
  bringupLog(kTag, "pads: rate %u Hz, state %u",
             static_cast<unsigned>(outputRate_),
             static_cast<unsigned>(state_.load()));
  return true;
}

void AudioPlayer::logPadVoltages() {
  measurePad(kTag, "BCK", board::kI2sBckPin, kNoSignal);
  measurePad(kTag, "LRCK", board::kI2sLrckPin, kNoSignal);
  measurePad(kTag, "DATA", board::kI2sDataPin, kNoSignal);
}

void AudioPlayer::describe(char* output, size_t capacity) const {
  if (output == nullptr || capacity == 0) {
    return;
  }
  switch (state_.load()) {
    case kStateOffline:
      snprintf(output, capacity, "off");
      break;
    case kStateTone:
      snprintf(output, capacity, "tone");
      break;
    case kStatePlaying:
    case kStateDraining:
      if (format_.load() == kFormatMp3) {
        snprintf(output, capacity, "%ux%u",
                 static_cast<unsigned>(sampleRate_.load()),
                 static_cast<unsigned>(channels_.load()));
      } else {
        snprintf(output, capacity, "module%u",
                 static_cast<unsigned>(format_.load()));
      }
      break;
    case kStateLoading:
      snprintf(output, capacity, "loading");
      break;
    default:
      snprintf(output, capacity, "idle");
      break;
  }
}

}  // namespace zifi

#endif  // ZIFI_HAS_AUDIO
