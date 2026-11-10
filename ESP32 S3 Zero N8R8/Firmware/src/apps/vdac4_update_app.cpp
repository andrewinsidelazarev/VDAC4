#include "zifi/apps/vdac4_update_app.hpp"

#if ZIFI_HAS_SPI_LINK

#include <WiFi.h>

#include "zifi/net_client.hpp"
#include "zifi/z80_transport.hpp"

namespace zifi {

bool Vdac4UpdateApp::eventEntry(void* context, uint8_t command, const uint8_t* data,
                                uint16_t length) {
  auto* self = static_cast<Vdac4UpdateApp*>(context);
  self->host_.noteFileActivity();
  // Строки списка терять нельзя: ждём место в очереди.
  return self->host_.sendEvent(command, data, length, kEventWaitMs);
}

bool Vdac4UpdateApp::networkCommand(uint8_t command, NetworkCommand& how) const {
  (void)how;
  return command == kVupStart || command == kVupApply || command == kVupStop ||
         command == kVupSync;
}

void Vdac4UpdateApp::sendNetworkFailure(uint8_t command) {
  const uint8_t failed[1] = {};
  uint8_t response = kRespVupSync;
  switch (command) {
    case kVupStart:
      response = kRespVupStart;
      break;
    case kVupApply:
      response = kRespVupApply;
      break;
    case kVupStop:
      response = kRespVupStop;
      break;
    default:
      break;
  }
  host_.transport().send(response, failed, sizeof(failed));
}

void Vdac4UpdateApp::processNetwork(IntercoreExchange& exchange) {
  exchange.responseLength = 1;
  exchange.response[0] = 0;
  switch (exchange.requestCommand) {
    case kVupStart: {
      exchange.responseCommand = kRespVupStart;
      if (host_.otaRunning()) {
        exchange.setError("vup:ota active");
        return;
      }
      host_.netClient().close();
      if (!host_.stopFileServers() || (peer_ != nullptr && !peer_->stopForFileServer()) ||
          !service_.stop(kStopTimeoutMs)) {
        exchange.setError("vup:previous stopping");
        return;
      }
      if (WiFi.status() != WL_CONNECTED) {
        exchange.setError("vup:no wifi");
        return;
      }
      char error[64] = {};
      if (!service_.start(exchange.request, exchange.requestLength, error, sizeof(error))) {
        exchange.setError("vup:%s", error);
        return;
      }
      exchange.response[0] = 1;
      return;
    }
    case kVupApply:
      exchange.responseCommand = kRespVupApply;
      exchange.response[0] = service_.apply(exchange.request, exchange.requestLength) ? 1 : 0;
      return;
    case kVupStop:
      exchange.responseCommand = kRespVupStop;
      exchange.response[0] = service_.stop(kStopTimeoutMs) ? 1 : 0;
      return;
    case kVupSync:
      exchange.responseCommand = kRespVupSync;
      exchange.response[0] = service_.sync() ? 1 : 0;
      return;
    default:
      return;
  }
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
