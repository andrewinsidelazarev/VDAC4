#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "zifi/spsc_ring.hpp"

namespace zifi {

// Обмен ESP32-S3 с Z80 по SPI платы VDAC4: логика без обращений к железу.
// Этот же код исполняется на ПК в тестах (tests/spi_link_host.cpp).
//
// Канал построен на общем буфере режима SPI Slave HD: 64 байта, которые ведущий
// (ZX Evolution) читает и пишет командами RDBUF/WRBUF, а контроллер SPI ESP32-S3
// обслуживает сам, без участия программы. Поэтому Z80 никогда не ждёт ESP внутри
// обмена по SPI: она записывает запрос, а ответ забирает, когда он появится.
//
// Байты 0..59 — область обмена. Запрос Z80 и ответ ESP лежат в ней по очереди:
//
//   запрос: [C3][номер][флаги][длина][место][данные...][сумма]
//   ответ:  [3C][номер][статус][длина][принято][данные...][сумма]
//
//   номер   — растёт на единицу с каждым новым запросом. Повтор с тем же номером
//             означает, что Z80 не дождалась ответа: ESP возвращает прежний ответ,
//             данные второй раз не принимает и не отдаёт;
//   длина   — число байтов данных в этом сообщении, не больше 54;
//   место   — сколько байтов Z80 готова принять в ответе;
//   принято — сколько байтов запроса ESP взяла. Остаток Z80 пришлёт снова;
//   сумма   — дополнение суммы всех предыдущих байтов сообщения до #FF.
//
// Ответ пишется с конца: сначала байты 4..59, последним — слово 0..3 с признаком
// 3C. Увидев 3C и свой номер, Z80 может читать остальное: оно уже на месте.
//
// Блок через DMA. Общий буфер несёт 54 байта за обмен, и каждый байт стоит Z80
// десятки тактов. Запрос с флагом «блок» означает, что Z80 может забрать
// kBulkSize байтов через DMA ведомого SPI (команда 04) своим DMA TS-Config.
// Если в очереди набралось столько, ESP выставляет блок для чтения и отвечает
// статусом «блок» без данных в ответе; Z80 читает блок и завершает чтение
// командой 08.
//
// Суммой обмена блок не покрыт. Его целость проверяет ESP: Z80 тем же DMA
// отсылает принятый блок обратно (команда 03, завершение — 07), ESP сравнивает
// его с отправленным и сообщает итог в ответе на следующий запрос с флагом
// «блок». Это дешевле контрольной суммы на Z80: байт обратной посылки стоит ей
// считанные такты. Пока итога нет, Z80 держит блок у себя и приложению не
// отдаёт. Статус «не совпало» означает, что копию нужно выбросить: вместе со
// статусом «блок» тот же блок выставлен снова, без него — DMA до конца сеанса
// выключен (kBulkAttempts неудач подряд), и те же байты придут обычными
// ответами. Данные из очереди, стоящие за блоком, до его подтверждения не
// отдаются.
//
// Блок, который Z80 так и не прочитала, а сеанс начала заново (её программу
// перезапустили), ESP предлагает со статусом «устарел»: его нужно прочитать и
// выбросить без обратной посылки, иначе DMA останется занятым.
//
// Блок от Z80. Запрос с флагом «отдаю блок» говорит, что Z80 хочет передать
// kBulkSize байтов через DMA (команда 03, конец — 07); байт 4 такого запроса
// (обычно «место») несёт XOR этих байтов, данных в ответ Z80 не ждёт. Если
// ESP может принять (DMA свободен, в очереди от Z80 есть место), она
// открывает приём и отвечает «готова». Z80 пишет блок и следующим запросом с
// флагом «блок записан» просит итог: ESP сверяет XOR и кладёт блок в очередь
// перед данными этого запроса («принят») либо выбрасывает («не сошёлся» — Z80
// пришлёт его снова). XOR считается у Z80 заодно с суммой кадра и ничего ей
// не стоит; ошибки линии во время DMA этим ловятся так же, как суммой
// обмена. Статус «умею» стоит в каждом ответе прошивки, которая так умеет:
// по нему Z80 узнаёт это при начале сеанса.
//
// Байты 60..63 — слово состояния, его пишет только ESP:
//   [60] 5A — для Z80 ничего нет, A5 — есть данные (любое другое значение значит,
//        что прошивка не работает);
//   [61] D7 — подпись, [62] — версия обмена, [63] — случайное число этого запуска.
// Простой Z80 читает один байт [60] и начинает обмен, только когда там A5.
// Приём блока от Z80 через DMA (за железом следит SpiLink). Вне класса: GCC не
// даёт брать значения по умолчанию вложенной структуры, пока класс не закончен.
struct SpiLinkPutInput {
  bool available = false;            // приём через DMA можно открыть
  const uint8_t* received = nullptr;  // что пришло после «готова», если пришло
  size_t receivedLength = 0;
  size_t capacity = 8192;           // available RX DMA transaction size
};

class SpiLinkCore {
 public:
  static constexpr size_t kBufferSize = 64;
  static constexpr size_t kTurnSize = 60;
  static constexpr size_t kHeaderSize = 5;
  static constexpr size_t kMaxChunk = kTurnSize - kHeaderSize - 1;
  static constexpr size_t kStateOffset = 60;
  static constexpr size_t kStateSize = 4;

  // С байтов 0 и 60 Z80 начинает чтение общего буфера. У их значений старшая
  // половина — инверсия младшей: по этому драйвер Z80 отличает первый байт
  // данных от того, что Evo отдаёт до них (zifi_spi.asm, SpiLink_OpenRead).
  // Новое значение должно быть таким же — см. проверку в spi_link_core.cpp.
  static constexpr uint8_t kRequestMarker = 0xC3;
  static constexpr uint8_t kResponseMarker = 0x3C;
  static constexpr uint8_t kStateEmpty = 0x5A;
  static constexpr uint8_t kStatePending = 0xA5;
  static constexpr uint8_t kMagic = 0xD7;
  static constexpr uint8_t kVersion = 2; // raw media DMA (old requests unchanged)

  // Флаги запроса.
  // Выбросить всё, что ESP ещё не отдала Z80 (ZiFi_ClearRx).
  static constexpr uint8_t kFlagDropToZ80 = 0x01;
  // Z80 может принять блок kBulkSize байтов через DMA.
  static constexpr uint8_t kFlagBulk = 0x02;
  // Z80 хочет отдать блок kBulkSize байтов через DMA; байт 4 запроса — его XOR.
  static constexpr uint8_t kFlagPut = 0x04;
  // Блок записан: сверить и принять.
  static constexpr uint8_t kFlagPutDone = 0x08;
  // Raw media DMA control: command, valid length LE16, optional MOD_PUT offset LE32.
  // DMA rounds up to 512 bytes; no XOR, padding never reaches the decoder.
  static constexpr uint8_t kFlagMedia = 0x10;
  static constexpr size_t kMediaMax = 8192;
  size_t putDmaSize() const { return (putSize_ + 511u) & ~size_t(511u); }
  // Начало сеанса (ZiFi_Init). Номер новой программы может случайно совпасть с
  // последним номером прежней, поэтому такой запрос повтором не считается.
  static constexpr uint8_t kFlagNewSession = 0x80;

  // Статус ответа: у ESP остались данные, следующий обмен можно начинать сразу.
  static constexpr uint8_t kStatusMore = 0x01;
  // Блок kBulkSize байтов выставлен для чтения через DMA; данных в ответе нет.
  static constexpr uint8_t kStatusBulk = 0x02;
  // Вместе с kStatusBulk: выставленный блок устарел, прочитать и выбросить.
  static constexpr uint8_t kStatusBulkStale = 0x04;
  // Обратная посылка блока не совпала с отправленным либо не пришла: копию
  // блока, которую держит Z80, нужно выбросить.
  static constexpr uint8_t kStatusEchoBad = 0x08;
  // Приём блока от Z80 открыт: можно писать.
  static constexpr uint8_t kStatusPutReady = 0x10;
  // Записанный блок сошёлся с XOR и принят.
  static constexpr uint8_t kStatusPutOk = 0x20;
  // Записанный блок не сошёлся (или не пришёл): прислать снова.
  static constexpr uint8_t kStatusPutBad = 0x40;
  // Прошивка умеет принимать блоки от Z80 через DMA.
  static constexpr uint8_t kStatusPutCapable = 0x80;

  static constexpr size_t kBulkSize = 512;
  // Столько раз подряд блок может не пройти проверку, пока DMA не выключен.
  static constexpr uint8_t kBulkAttempts = 3;

  // Что сейчас с DMA (за этим следит владелец железа, SpiLink).
  enum BulkState : uint8_t {
    kBulkOff,     // DMA нет: блоки не предлагать
    kBulkFree,    // свободен; верной обратной посылки последнего блока нет
    kBulkLoaded,  // блок выставлен и до сих пор не прочитан
    kBulkEchoed,  // свободен; последний блок прочитан и вернулся без искажений
  };

  // Приём блока от Z80 (за железом следит SpiLink).
  using PutInput = SpiLinkPutInput;

  struct Stats {
    uint32_t turns;          // обработано новых запросов
    uint32_t repeats;        // повторов запроса (ответ выдан заново)
    uint32_t badRequests;    // запросов с неверной длиной или суммой
    uint32_t shortAccepts;   // запросов, данные которых не поместились целиком
    uint32_t bytesFromZ80;
    uint32_t bytesToZ80;
    uint32_t bulkBlocks;     // блоков, отданных через DMA
    uint32_t bulkBad;        // блоков, не прошедших проверку обратной посылкой
    uint32_t bulkGaveUp;     // сколько раз DMA выключался до конца сеанса
    uint32_t putBlocks;      // блоков, принятых от Z80 через DMA
    uint32_t putBad;         // блоков от Z80, не сошедшихся с XOR
  };

  SpiLinkCore();

  // Ёмкости — степени двойки (требование SpscByteRing). Память даёт вызывающий.
  bool begin(uint8_t* fromZ80Storage, size_t fromZ80Capacity,
             uint8_t* toZ80Storage, size_t toZ80Capacity);

  // --- задача обмена ---------------------------------------------------------

  // request — байты 0..59 общего буфера. Если это верный запрос, в response
  // складывается ответ длиной responseLength и возвращается true.
  // bulk и bulkBuffer (kBulkSize байтов) — состояние DMA и место под блок. Если
  // в статусе ответа есть kStatusBulk без kStatusBulkStale, блок лежит в
  // bulkBuffer (при повторе запроса и при повторной выдаче — тот же, что был
  // положен в прошлый раз), и вызывающий обязан выставить его для чтения и
  // приготовиться принять обратно. Буфер между вызовами менять нельзя.
  // put — приём блока от Z80. Если в статусе ответа kStatusPutReady,
  // вызывающий обязан открыть приём через DMA и отдать принятое в
  // put.received при следующем запросе.
  bool handleRequest(const uint8_t* request, uint8_t* response,
                     size_t& responseLength, BulkState bulk = kBulkOff,
                     uint8_t* bulkBuffer = nullptr,
                     const PutInput& put = PutInput{});

  // Слово состояния для байтов 60..63.
  void stateWord(uint8_t word[kStateSize], uint8_t bootNonce) const;

  // --- приложение (цикл протокола) -------------------------------------------

  size_t read(uint8_t* output, size_t capacity);
  size_t write(const uint8_t* data, size_t length);
  size_t writeSpace() const;
  size_t writeCapacity() const;
  size_t pendingToZ80() const;
  // Принято от Z80 и ещё не забрано циклом протокола (для сторожа обмена).
  size_t pendingFromZ80() const;
  // Число всех запросов, включая повторы: по нему видно, что Z80 на связи.
  uint32_t activity() const;
  Stats stats() const;

  static uint8_t checksum(const uint8_t* data, size_t length);

 private:
  SpscByteRing fromZ80_;
  SpscByteRing toZ80_;
  uint8_t lastResponse_[kTurnSize];
  size_t lastLength_;
  uint8_t lastTid_;
  bool haveLast_;
  bool blockOut_;        // блок взят из очереди и ещё не подтверждён
  bool bulkGaveUp_;      // DMA выключен до начала нового сеанса
  uint8_t blockTries_;   // сколько раз этот блок не прошёл проверку
  // Сколько байтов неподтверждённого блока осталось отдать обычными ответами.
  std::atomic<uint32_t> blockInline_;
  // Копия blockOut_ для других задач (pendingToZ80).
  std::atomic<bool> blockPending_;
  bool putArmed_;        // Z80 ответили «готова», блока ещё не было
  bool putMedia_;
  uint8_t putCommand_;
  uint8_t putPrefix_[4];
  size_t putPrefixSize_;
  size_t putSize_;
  uint8_t putXor_;       // XOR, который Z80 назвала для этого блока
  std::atomic<uint32_t> turns_;
  std::atomic<uint32_t> repeats_;
  std::atomic<uint32_t> badRequests_;
  std::atomic<uint32_t> shortAccepts_;
  std::atomic<uint32_t> bytesFromZ80_;
  std::atomic<uint32_t> bytesToZ80_;
  std::atomic<uint32_t> bulkBlocks_;
  std::atomic<uint32_t> bulkBad_;
  std::atomic<uint32_t> bulkGaveUps_;
  std::atomic<uint32_t> putBlocks_;
  std::atomic<uint32_t> putBad_;
};

}  // namespace zifi
