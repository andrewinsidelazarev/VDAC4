#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

#include "zifi/app.hpp"
#include "zifi/vdac4_update_service.hpp"

namespace zifi {

class VfsBridge;

// Обновление всей платы VDAC4 с GitHub (плагин VDAC4 Online Update 2): команды VUP_START/APPLY/
// STOP/SYNC. Карту Evo пишет через мост VFS монопольно, как WC Update: перед FTP и SMB сеанс
// останавливается, при старте останавливает их и сеанс WC Update (peer).
class Vdac4UpdateApp : public App {
 public:
  Vdac4UpdateApp(AppHost& host, VfsBridge& bridge)
      : host_(host), service_(bridge, eventEntry, this) {}

  // Второе приложение с мостом VFS (WC Update): сеанс у ESP один.
  void setPeer(App* peer) { peer_ = peer; }

  const char* name() const override { return "vdac4 update"; }
  bool networkCommand(uint8_t command, NetworkCommand& how) const override;
  void sendNetworkFailure(uint8_t command) override;
  void processNetwork(IntercoreExchange& exchange) override;
  bool stopForFileServer() override { return service_.stop(kStopTimeoutMs); }
  bool keepsWifiAwake() const override { return service_.running(); }

 private:
  static constexpr uint32_t kStopTimeoutMs = 20000;
  static constexpr uint32_t kEventWaitMs = 5000;

  static bool eventEntry(void* context, uint8_t command, const uint8_t* data, uint16_t length);

  AppHost& host_;
  Vdac4UpdateService service_;
  App* peer_ = nullptr;
};

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
