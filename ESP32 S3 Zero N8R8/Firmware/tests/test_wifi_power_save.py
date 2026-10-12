"""Энергосбережение Wi-Fi на время работы файлового сервера.

Пока с FTP/SMB-сервером идёт обмен, радио ESP не спит: спящий ESP ждал маяка
точки доступа до ~0,1 с на каждый запрос, а на занятом эфире терял кадры.
Энергосбережение возвращается при остановке сервера и через 10 минут без
обмена — даже если Evo сбросили кнопкой и плагин не прислал команду остановки.
Приложения, работающие с сетью (обновлятор WC, поток радио приложения-модуля),
держат Wi-Fi без энергосбережения всё время работы (с 0.1.34; до того — тем же
путём, что сервер, и через 10 минут радио засыпало). Проверяется исходный текст
main.cpp и приложений (src/apps): сама прошивка на ПК не собирается.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    depth = 0
    for index in range(source.index("{", start), len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"нет конца функции {signature}")


class WifiPowerSaveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        cls.protocol = (ROOT / "include" / "zifi" / "protocol.hpp").read_text(
            encoding="utf-8")
        cls.wcu = (ROOT / "src" / "apps" / "wc_update_app.cpp").read_text(encoding="utf-8")
        cls.wcu_header = (ROOT / "include" / "zifi" / "apps" / "wc_update_app.hpp").read_text(
            encoding="utf-8")
        cls.modules = (ROOT / "src" / "apps" / "module_app.cpp").read_text(encoding="utf-8")
        cls.modules_header = (ROOT / "include" / "zifi" / "apps" / "module_app.hpp").read_text(
            encoding="utf-8")

    def test_idle_limit_is_ten_minutes(self) -> None:
        self.assertIn("constexpr uint32_t kWifiAwakeIdleMs = 10UL * 60UL * 1000UL;",
                      self.main)

    def test_sleep_is_switched_only_by_the_policy(self) -> None:
        policy = function_body(self.main, "void Application::updateWifiPowerSave(")
        self.assertEqual(self.main.count("WiFi.setSleep("), 1)
        self.assertIn("WiFi.setSleep(!awake);", policy)
        self.assertIn("fileServerRunning && !fileServerWasRunning_", policy,
                      "запуск сервера считается обменом")
        self.assertIn("vfsBridge_.pendingSinceMs()", policy,
                      "файловые запросы к Z80 — тоже обмен")
        self.assertIn("< kWifiAwakeIdleMs", policy)
        self.assertIn("bool awake = appAwake;", policy, "приложению — без срока")
        self.assertIn("if (!awake && fileServerRunning)", policy)

    def test_network_loop_applies_the_policy(self) -> None:
        loop = function_body(self.main, "void Application::networkTaskLoop()")
        # Обновлятор WC — тоже обмен с Z80: пока он работает, радио не спит.
        # С 0.1.20 он приложение, и ядро спрашивает об этом все приложения; с
        # 0.1.34 приложения идут отдельно от сервера — без срока в 10 минут.
        self.assertIn("updateWifiPowerSave(fileServerRunning, appsKeepWifiAwake(), "
                      "signalNow);", loop)
        apps = function_body(self.main, "bool Application::appsKeepWifiAwake()")
        self.assertIn("app->keepsWifiAwake()", apps)
        self.assertIn("bool keepsWifiAwake() const override { return service_.running(); }",
                      self.wcu_header)

    def test_hot_chip_forces_power_save(self) -> None:
        # Кристалл горячее 75 °C — Wi-Fi спит при любой работе, назад — ниже 70 °C.
        self.assertIn("constexpr int16_t kWifiHotTenths = 750;", self.main)
        self.assertIn("constexpr int16_t kWifiCoolTenths = 700;", self.main)
        policy = function_body(self.main, "void Application::updateWifiPowerSave(")
        self.assertIn("moduleApp_.temperatureTenths()", policy)
        self.assertIn("!wifiHot_ && tenths > kWifiHotTenths", policy)
        self.assertIn("wifiHot_ && tenths < kWifiCoolTenths", policy)
        hot = policy.index("if (wifiHot_) {")
        self.assertLess(policy.index("bool awake = appAwake;"), hot)
        self.assertLess(hot, policy.index("WiFi.setSleep(!awake);"), "перегрев — последнее слово")
        self.assertIn("int16_t ModuleApp::temperatureTenths() const { return g_temperature; }",
                      self.modules)

    def test_module_stream_keeps_wifi_awake(self) -> None:
        # Приложение-модуль (PLAYER.ELF) качает радио: пока поток net_open открыт,
        # Wi-Fi не спит (в энергосбережении поток не быстрее ~57 КБ/с).
        self.assertIn("bool keepsWifiAwake() const override;", self.modules_header)
        awake = function_body(self.modules, "bool ModuleApp::keepsWifiAwake() const")
        self.assertIn("streamUsed_[i]", awake)

    def test_wc_update_events_mark_activity(self) -> None:
        entry = function_body(self.wcu, "bool WcUpdateApp::eventEntry(")
        self.assertIn("self->host_.noteFileActivity();", entry)
        self.assertIn("void noteFileActivity() override { fileActivityMs_ = millis(); }",
                      self.main)

    def test_client_events_mark_activity_but_signal_bar_does_not(self) -> None:
        entry = function_body(self.main, "bool Application::networkEventEntry(")
        self.assertIn("command >= kEventFtpClient && command <= kEventSmbProgress",
                      entry)
        self.assertIn("self->fileActivityMs_ = millis();", entry)
        codes = {name: int(value, 16) for name, value in re.findall(
            r"(kEvent\w+) = (0x[0-9A-Fa-f]+)", self.protocol)}
        client = range(codes["kEventFtpClient"], codes["kEventSmbProgress"] + 1)
        for name in ("kEventFtpCommand", "kEventSmbClient", "kEventSmbCommand"):
            self.assertIn(codes[name], client)
        self.assertNotIn(codes["kEventWifiSignal"], client)
        self.assertNotIn(codes["kEventOnlineUpdateProgress"], client)


if __name__ == "__main__":
    unittest.main()
