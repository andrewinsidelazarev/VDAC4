#pragma once

#include "zifi/app.hpp"
#include "zifi/weather_service.hpp"

namespace zifi {

class NetClient;

// NET_NTP: точное время с учётом пояса из zifi.ini, 14 символов ГГГГММДДЧЧММСС.
class TimeApp : public App {
 public:
  explicit TimeApp(AppHost& host) : host_(host) {}

  const char* name() const override { return "time"; }
  bool networkCommand(uint8_t command, NetworkCommand& how) const override;
  void sendNetworkFailure(uint8_t command) override;
  void processNetwork(IntercoreExchange& exchange) override;

 private:
  AppHost& host_;
};

// WEATHER_GET: запись погоды для заставки Wild Commander. Место берётся из
// сохранённого zifi.ini (country:, zip:). При ошибке ответ короткий
// [0][версия], а причина уходит докладом #EE перед ним.
class WeatherApp : public App {
 public:
  WeatherApp(AppHost& host, NetClient& net) : host_(host), weather_(net) {}

  const char* name() const override { return "weather"; }
  bool networkCommand(uint8_t command, NetworkCommand& how) const override;
  void sendNetworkFailure(uint8_t command) override;
  void processNetwork(IntercoreExchange& exchange) override;

 private:
  AppHost& host_;
  WeatherService weather_;
};

}  // namespace zifi
