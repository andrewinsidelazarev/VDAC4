#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

constexpr uint8_t kSync = 0x5A;
// Internal framing of raw SPI media DMA: AUDIO_DATA and MOD_PUT, no data checksum.
constexpr uint8_t kMediaSync = 0xA6;
constexpr size_t kMaxMediaPayload = 8192 + 4; // MOD_PUT adds a file offset
constexpr size_t kMaxPayload = 1024;
constexpr uint32_t kFrameTimeoutMs = 500;
// Наибольшее тело события ESP -> Z80 (kEvent*): столько вмещает очередь
// событий между ядрами.
constexpr size_t kMaxEventPayload = 63;

enum Command : uint8_t {
  kEcho = 0x00,
  kWifiConnect = 0x01,
  kSysInfo = 0x02,
  kWifiIni = 0x03,
  kPing = 0x04,
  kGetStep = 0x05,
  kFtpStart = 0x06,
  kFtpStop = 0x07,
  kUpdateStart = 0x08,
  kUpdateStop = 0x09,
  kFtpRamStats = 0x0A,
  kSmbStart = 0x0B,
  kSmbStop = 0x0C,
  kOnlineUpdate = 0x0D,
  kOnlineUpdateCheck = 0x0E,
  kSysReset = 0x0F,
  kNetOpen = 0x10,
  kNetSend = 0x11,
  kNetRecv = 0x12,
  kNetClose = 0x13,
  kNetHttpGet = 0x14,
  kNetPing = 0x20,
  kNetIpConfig = 0x21,
  kNetNtp = 0x22,
  kNetProxyStatus = 0x23,
  // Запись погоды для заставки WC: место из zifi.ini (country:, zip:),
  // payload пуст. Ответ kRespWeatherGet, формат в weather_parse.hpp.
  kWeatherGet = 0x24,
  // Проверка и обновление Wild Commander по каталогу на GitHub (плагин
  // VDAC4WCU.WMF). START: [repo]\0[ветка]\0[каталог]\0[защищённые пути]\0\0;
  // затем ESP сам читает SD через VFS-запросы и шлёт kEventWcuState/Entry.
  // APPLY: номера файлов по байту; STOP завершает сеанс.
  kWcuStart = 0x25,
  kWcuApply = 0x26,
  kWcuStop = 0x27,
  // Выдать заново все строки списка и последнее состояние: плагин заметил,
  // что событие потерялось (дыра в номерах, итог не пришёл).
  kWcuSync = 0x28,
  // Часы ESP по часам Evo (плагин NTP Time Sync, когда NTP не ответил):
  // [год LE16][месяц][число][часы][минуты][секунды] — местное время по time: из zifi.ini.
  kSysClock = 0x29,
  // Окно проигрывателя (PLAYER.ELF): спектр или уровни каналов, позиция,
  // длительность. Не #3E: этот код у kLinkProbe, его перехватывает ядро.
  kAudioView = 0x2F,

  // Звук платы VDAC4 (I2S -> PCM5102). Все команды отвечают сразу, без ACK:
  // поток MP3 идёт кадрами AUDIO_DATA, и лишний кадр на каждый был бы обузой.
  // Форматы — Docs/VDAC4.md. Прошивка переходника ZiFi этих команд не знает.
  kAudioOpen = 0x30,    // начать поток MP3
  kAudioData = 0x31,    // данные аудио: до 1024 байт в кадре, до 8192 через raw DMA
  kAudioClose = 0x32,   // [0] оборвать, [1] доиграть накопленное
  kAudioStatus = 0x33,
  kAudioTone = 0x34,    // [каналы][частота LE16][мс LE16][уровень]
  kAudioVolume = 0x35,  // [0..255]
  kAudioPause = 0x36,   // [1] пауза, [0] продолжить
  kAudioProbe = 0x37,   // тест линий ЦАП: напряжения на BCK, LRCK, DIN
  kAudioSeek = 0x38,    // модуль: [шаг по таблице порядка, со знаком]
  kAudioFlush = 0x39,   // MP3 после перемотки: [время нового места, мс LE32]
  kAudioInfo = 0x3A,    // формат, каналы, длина и название того, что играет
  kAudioCover = 0x3B,   // кусок метки ID3 для обложки: [смещение LE32][данные]
  kAudioThumb = 0x3C,   // миниатюра обложки: [часть 0..3]
  // Картинка файлом из папки трека (cover.jpg) — команда приложения PLAYER.ELF.
  kAudioPicture = 0x3D, // [длина файла LE32][смещение LE32][данные]
  kAudioTags = 0x3F,    // метки: название, исполнитель, альбом, год, номер

  // Модули ESP (приложения и декодеры — ELF из /WC/ESP/, modules/sdk): доставка
  // в кэш flash и запуск приложения (ModuleApp, Docs/VDAC4.md).
  kModQuery = 0x69,     // [флаги][длина LE32][время FAT LE32][имя, 0]
  kModPut = 0x6A,       // [смещение LE32][данные]
  kModCommit = 0x6B,
  kAppStart = 0x6C,     // [имя, 0]
  kAppStop = 0x6D,

  // Наладка платы VDAC4: проверка обмена через DMA ведомого SPI (команды
  // 03/04 контроллера и DMA TS-Config на стороне Evo). Тело: [действие]
  // [длина LE16][зерно узора]; действия — SpiLink::DmaProbe. Ответ сразу, без ACK.
  kLinkProbe = 0x3E,

  kVfsStat = 0x40,
  kVfsOpenDir = 0x41,
  kVfsReadDir = 0x42,
  kVfsFsInfo = 0x43,
  kVfsFatWindow = 0x44,
  kVfsOpen = 0x50,
  kVfsRead = 0x51,
  kVfsWriteLegacy = 0x52,
  kVfsClose = 0x53,
  kVfsDelete = 0x54,
  kVfsMkdir = 0x55,
  kVfsBlock = 0x56,
  kVfsWriteWindow = 0x57,
  kVfsReadWindow = 0x58,
  kVfsRename = 0x59,
  kVfsExtend = 0x5A,
  kVfsSeek = 0x5B,
  kVfsSetEof = 0x5C,
  kVfsMoveRename = 0x5D,
  kVfsSetMetadata = 0x5E,

  kEventFtpClient = 0x60,
  kEventFtpCommand = 0x61,
  kEventSmbClient = 0x62,
  kEventSmbCommand = 0x63,
  // Ход текущей передачи файла отдельной строкой. Идёт редко и с готовым
  // текстом, поэтому плагину не нужно ни считать байты, ни делить их.
  kEventSmbProgress = 0x64,
  // [этап][проценты] автономной загрузки firmware.bin с GitHub.
  kEventOnlineUpdateProgress = 0x65,
  // Готовая ASCII-шкала уровня Wi-Fi для поля Status FTP/SMB-плагина. Отдельное
  // событие не вызывает SYS_INFO и не смешивается с его диагностическим текстом.
  kEventWifiSignal = 0x66,
  // Обновлятор WC: [этап][текущий LE16][всего LE16][процент][текст] и запись
  // файла [номер][состояние][флаги][длина на SD LE24][длина на GitHub LE24]
  // [путь]; путь длиннее события — его хвост. Формат — Docs/PROTOCOL.md.
  kEventWcuState = 0x67,
  kEventWcuEntry = 0x68,

  kRespSysInfo = 0x82,
  kRespWifiConnect = 0x81,
  kRespWifiIni = 0x83,
  kRespGetStep = 0x85,
  kRespFtpStart = 0x86,
  kRespFtpStop = 0x87,
  kRespUpdateStart = 0x88,
  kRespUpdateStop = 0x89,
  kRespFtpRamStats = 0x8A,
  kRespSmbStart = 0x8B,
  kRespSmbStop = 0x8C,
  kRespOnlineUpdate = 0x8D,
  kRespOnlineUpdateCheck = 0x8E,
  kRespNetOpen = 0x90,
  kRespNetSend = 0x91,
  kRespNetRecv = 0x92,
  kRespNetClose = 0x93,
  kRespNetHttpGet = 0x94,
  kRespNetIpConfig = 0xA0,
  kRespNetPing = 0xA1,
  kRespNetNtp = 0xA2,
  kRespNetProxyStatus = 0xA3,
  kRespWeatherGet = 0xA4,
  kRespWcuStart = 0xA5,  // [1 — сеанс запущен]
  kRespWcuApply = 0xA6,  // [1 — принято]
  kRespWcuStop = 0xA7,   // [1 — сеанс остановлен]
  kRespSysClock = 0xA9,  // [1 — часы ESP выставлены, 0 — время негодное]
  kRespWcuSync = 0xA8,   // [1 — повтор списка поставлен в очередь]
  kRespAudioView = 0xAF,    // [вид][число][длительность с LE16][позиция с LE16][значения 0..12]
  kRespAudioOpen = 0xB0,    // [1 — поток открыт][свободно LE32]
  kRespAudioData = 0xB1,    // [1 принято / 0 нет места / 2 поток закрыт][свободно LE32]
  kRespAudioClose = 0xB2,   // [1]
  kRespAudioStatus = 0xB3,  // запись состояния, 32 байта (до 0.1.20 — 24)
  kRespAudioTone = 0xB4,    // [1 — тон запущен]
  kRespAudioVolume = 0xB5,  // [1]
  kRespAudioPause = 0xB6,   // [1]
  kRespAudioProbe = 0xB7,   // [1 — измерено / 0 — идёт звук][24 байта, мВ/20]
  kRespAudioSeek = 0xB8,    // [1 — перешёл][позиция LE16]
  kRespAudioFlush = 0xB9,   // [1 — очередь пуста][свободно LE32]
  kRespAudioInfo = 0xBA,    // [формат][каналы][длина LE16][название, 0]
  kRespAudioCover = 0xBB,   // [0 — шли с места / 1 — собрана / 2 — нет][место LE32][свободно LE32]
  kRespAudioThumb = 0xBC,   // [0 — сводится / 1 — вот / 2 — нет][часть][512 байт]
  kRespAudioPicture = 0xBD, // [0 — шли дальше / 1 — собрана / 2 — нет][принято LE32][свободно LE32]
  kRespAudioTags = 0xBF,    // [название, 0][исполнитель, 0][альбом, 0][год, 0][номер, 0]
  kRespModQuery = 0xE9,     // [0 есть / 1 шли / 2 нет места / 3 плохое имя][свободно LE32]
  kRespModPut = 0xEA,       // [1 принято / 0 не то место][принято всего LE32]
  kRespModCommit = 0xEB,    // [1 положен / 0 нет]
  kRespAppStart = 0xEC,     // [итог int8][имя, 0][версия, 0]
  kRespAppStop = 0xED,      // [1]
  // [итог][действие][длина LE16][несовпавших байтов LE16][первый несовпавший LE16]
  kRespLinkProbe = 0xBE,
  kReady = 0xF0,
  kError = 0xEE,
  kAck = 0xFE,
};

enum OnlineUpdateRelation : uint8_t {
  kOnlineUpdateSame = 0,
  kOnlineUpdateNewer = 1,
  kOnlineUpdateOlder = 2,
  kOnlineUpdateDifferent = 3,
};

struct PacketView {
  uint8_t command = 0;
  const uint8_t* data = nullptr;
  uint16_t length = 0;
};

class FrameParser {
 public:
  FrameParser();

  bool feed(uint8_t byte, uint32_t nowMs, PacketView& packet);
  bool checkTimeout(uint32_t nowMs);
  void reset();

  uint32_t badChecksumCount() const { return badChecksumCount_; }
  uint32_t resyncCount() const { return resyncCount_; }

 private:
  enum State : uint8_t { kWaitSync, kCommand, kLengthLow, kLengthHigh, kData, kChecksum };

  State state_;
  bool rawMedia_;
  uint8_t command_;
  uint16_t length_;
  uint16_t position_;
  uint8_t checksum_;
  uint32_t lastByteMs_;
  uint32_t badChecksumCount_;
  uint32_t resyncCount_;
  uint8_t payload_[kMaxMediaPayload];
};

using ByteWriter = bool (*)(void* context, const uint8_t* data, size_t length);

bool writeFrame(ByteWriter writer, void* context, uint8_t command,
                const uint8_t* data = nullptr, uint16_t length = 0);

uint16_t crc16CcittFalse(const uint8_t* data, size_t length);
uint16_t crc16CcittFalseUpdate(uint16_t crc, const uint8_t* data,
                               size_t length);
uint32_t crc32IsoHdlc(const uint8_t* data, size_t length);

inline uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

inline uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

inline void writeLe16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

inline void writeLe32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace zifi
