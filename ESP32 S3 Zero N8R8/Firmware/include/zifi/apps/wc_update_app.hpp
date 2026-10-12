#pragma once

#include "zifi/app.hpp"
#include "zifi/wc_update_service.hpp"

namespace zifi {

class VfsBridge;

// Проверка и обновление Wild Commander с GitHub (плагин WC Update): команды
// WCU_START/APPLY/STOP/SYNC. Своя задача ядра 0 пишет на карту Evo через мост
// VFS — монопольно, поэтому перед FTP и SMB она останавливается, а при старте
// останавливает их сама.
class WcUpdateApp : public App {
 public:
  WcUpdateApp(AppHost& host, VfsBridge& bridge)
      : host_(host), service_(bridge, eventEntry, this) {}

  const char* name() const override { return "wc update"; }
  bool networkCommand(uint8_t command, NetworkCommand& how) const override;
  void sendNetworkFailure(uint8_t command) override;
  void processNetwork(IntercoreExchange& exchange) override;
  bool stopForFileServer() override { return service_.stop(kStopTimeoutMs); }
  bool keepsWifiAwake() const override { return service_.running(); }

 private:
  // Сколько ждать конца сеанса по команде STOP. Задача заканчивает текущий
  // обмен с SD; его пределы — до трёх минут, но плагин дольше не ждёт:
  // следующий запуск всё равно начнётся с перезапуска ESP.
  static constexpr uint32_t kStopTimeoutMs = 20000;
  // Событие обновлятора теряться не должно: это строки списка файлов.
  static constexpr uint32_t kEventWaitMs = 5000;

  static bool eventEntry(void* context, uint8_t command, const uint8_t* data,
                         uint16_t length);

  AppHost& host_;
  WcUpdateService service_;
};

}  // namespace zifi
