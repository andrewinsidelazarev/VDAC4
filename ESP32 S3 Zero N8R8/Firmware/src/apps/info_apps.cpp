#include "zifi/apps/info_apps.hpp"

#include <string.h>

#include "zifi/config.hpp"
#include "zifi/ntp_client.hpp"
#include "zifi/z80_transport.hpp"

namespace zifi {

bool TimeApp::networkCommand(uint8_t command, NetworkCommand& how) const {
  if (command != kNetNtp) {
    return false;
  }
  how.payload = false;
  return true;
}

void TimeApp::sendNetworkFailure(uint8_t command) {
  (void)command;
  const uint8_t failed[14] = {};
  host_.transport().send(kRespNetNtp, failed, sizeof(failed));
}

void TimeApp::processNetwork(IntercoreExchange& exchange) {
  char result[15] = {};
  char error[64];
  bool ok = false;
  if (host_.otaRunning()) {
    memset(result, '0', 14);
    snprintf(error, sizeof(error), "ota active");
  } else {
    ok = queryNtp(host_.config().timezoneHours(), result, error, sizeof(error));
  }
  if (!ok) {
    exchange.setError("ntp:%s", error);
  }
  exchange.responseCommand = kRespNetNtp;
  exchange.responseLength = 14;
  memcpy(exchange.response, result, 14);
}

bool WeatherApp::networkCommand(uint8_t command, NetworkCommand& how) const {
  (void)how;
  return command == kWeatherGet;
}

void WeatherApp::sendNetworkFailure(uint8_t command) {
  (void)command;
  const uint8_t failed[2] = {0, kWeatherRecordVersion};
  host_.transport().send(kRespWeatherGet, failed, sizeof(failed));
}

void WeatherApp::processNetwork(IntercoreExchange& exchange) {
  char error[64] = {};
  exchange.responseCommand = kRespWeatherGet;
  bool ok = false;
  if (host_.otaRunning()) {
    snprintf(error, sizeof(error), "ota active");
  } else {
    ok = weather_.get(host_.config(), exchange.response, error, sizeof(error));
  }
  if (ok) {
    exchange.responseLength = static_cast<uint16_t>(kWeatherRecordSize);
    return;
  }
  exchange.setError("weather:%s", error);
  exchange.response[0] = 0;
  exchange.response[1] = kWeatherRecordVersion;
  exchange.responseLength = 2;
}

}  // namespace zifi
