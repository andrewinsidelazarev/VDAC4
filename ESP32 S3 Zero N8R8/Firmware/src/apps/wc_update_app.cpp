#include "zifi/apps/wc_update_app.hpp"

#include <WiFi.h>

#include "zifi/net_client.hpp"
#include "zifi/z80_transport.hpp"

namespace zifi {

bool WcUpdateApp::eventEntry(void* context, uint8_t command,
                             const uint8_t* data, uint16_t length) {
  auto* self = static_cast<WcUpdateApp*>(context);
  self->host_.noteFileActivity();
  // В отличие от индикации FTP/SMB, строки списка терять нельзя: ждём место.
  return self->host_.sendEvent(command, data, length, kEventWaitMs);
}

bool WcUpdateApp::networkCommand(uint8_t command, NetworkCommand& how) const {
  (void)how;
  return command == kWcuStart || command == kWcuApply ||
         command == kWcuStop || command == kWcuSync;
}

void WcUpdateApp::sendNetworkFailure(uint8_t command) {
  const uint8_t failed[1] = {};
  uint8_t response = kRespWcuSync;
  switch (command) {
    case kWcuStart:
      response = kRespWcuStart;
      break;
    case kWcuApply:
      response = kRespWcuApply;
      break;
    case kWcuStop:
      response = kRespWcuStop;
      break;
    default:
      break;
  }
  host_.transport().send(response, failed, sizeof(failed));
}

void WcUpdateApp::processNetwork(IntercoreExchange& exchange) {
  exchange.responseLength = 1;
  exchange.response[0] = 0;
  switch (exchange.requestCommand) {
    case kWcuStart: {
      exchange.responseCommand = kRespWcuStart;
      if (host_.otaRunning()) {
        exchange.setError("wcu:ota active");
        return;
      }
      host_.netClient().close();
      if (!host_.stopFileServers() || !service_.stop(kStopTimeoutMs)) {
        exchange.setError("wcu:previous stopping");
        return;
      }
      if (WiFi.status() != WL_CONNECTED) {
        exchange.setError("wcu:no wifi");
        return;
      }
      char error[64] = {};
      if (!service_.start(exchange.request, exchange.requestLength, error,
                          sizeof(error))) {
        exchange.setError("wcu:%s", error);
        return;
      }
      exchange.response[0] = 1;
      return;
    }
    case kWcuApply:
      exchange.responseCommand = kRespWcuApply;
      exchange.response[0] =
          service_.apply(exchange.request, exchange.requestLength) ? 1 : 0;
      return;
    case kWcuStop:
      exchange.responseCommand = kRespWcuStop;
      exchange.response[0] = service_.stop(kStopTimeoutMs) ? 1 : 0;
      return;
    case kWcuSync:
      exchange.responseCommand = kRespWcuSync;
      exchange.response[0] = service_.sync() ? 1 : 0;
      return;
    default:
      return;
  }
}

}  // namespace zifi
