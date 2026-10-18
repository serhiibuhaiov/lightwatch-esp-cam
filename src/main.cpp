// lightwatch: an ESP32-CAM controller for Tuya sockets that starts on its own after a blackout.
//
// setup() brings up, in order: the LEDs, the logger and the boot history, the storage (SD card + a
// flash copy of the config), the config, the Tuya device list with its workers and monitor, the worker
// of the outgoing HTTP requests, Wi-Fi and NTP, the web API, and the scenarios. loop() only watches: boot stages, the clock, a card that
// shows up late, and the Wi-Fi watchdog (restart after wifi.restart_after_min without Wi-Fi).
//
// The camera is never initialised.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>

#include "bootlog.h"
#include "config.h"
#include "devices.h"
#include "http_out.h"
#include "indicator.h"
#include "logger.h"
#include "scenario.h"
#include "storage.h"
#include "tuya.h"
#include "web_api.h"

namespace {

const char* TAG = "main";
constexpr uint32_t kStabilizeMs = 3000;  // let the supply settle before touching the card

uint32_t gRestartAfterMs = 0;
uint32_t gLastWifiOkMs = 0;
bool gHaveSsid = false;  // written once in setup(); loop() must not copy the whole config every second

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  static bool downLogged = false;  // the event task is the only writer
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      downLogged = false;
      BootLog::stage('4');
      Indicator::wifiUp(true);
      Indicator::ready();  // first IP = everything started: the red LED stops blinking
      LOGI(TAG, "wifi: got IP %s", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Indicator::wifiUp(false);
      if (!downLogged) {  // auto-reconnect retries every second: log the first loss only
        downLogged = true;
        LOGW(TAG, "wifi: disconnected (reason %d), retrying", info.wifi_sta_disconnected.reason);
      }
      break;
    default:
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  HttpOut::installTlsAllocator();  // before anything (Wi-Fi included) takes memory from mbedTLS
  Indicator::begin();  // red LED blinks from the very start
  Logger::begin();  // from here on LOGx() is safe from any task
  BootLog::begin(esp_reset_reason());  // before anything that could hang or reset

  delay(kStabilizeMs);
  LOGI(TAG, "boot, reset reason: %s", resetReasonName(esp_reset_reason()));
  // Earlier boots, oldest first; the last entry is this one. Reasons: P power-on,
  // E reset pin, S software, X panic, I/T/W watchdog, B brown-out. Stages: 0 started,
  // 1 storage, 2 config, 3 Wi-Fi started, 4 got IP, 5 alive for 60 s.
  LOGI(TAG, "boot history: %s", BootLog::summary().c_str());

  Storage::begin();
  BootLog::stage('1');
  Config::Source source = Config::load();
  AppConfig cfg = Config::snapshot();
  LOGI(TAG, "config loaded from %s", Config::sourceName(source));
  if (!Storage::sdMounted()) {
    LOGE(TAG, "SD card not available: running without file logging, config from %s; "
              "log kept in RAM (GET /api/logs?file=current), re-trying the card every 30 s",
         Config::sourceName(source));
    if (source == Config::Source::Defaults) {
      LOGE(TAG, "no flash copy of the config either: Wi-Fi is not configured, insert a card with config.json");
    }
  }

  BootLog::stage('2');
  Logger::start(cfg.log);
  Devices::load();  // /devices.json on the SD card only; no card = no devices
  Tuya::begin();
  Tuya::setObserver(Devices::onJobDone);
  Tuya::setPushObserver(Devices::onPush);
  HttpOut::begin();
  Devices::startMonitor(cfg.monitor.intervalSec, cfg.monitor.timeoutMs, cfg.monitor.retries);

  Indicator::wifiConfigured(!cfg.wifi.ssid.isEmpty());  // before WiFi.begin: its events must not be dropped
  WiFi.persistent(false);
  WiFi.setHostname(cfg.device.name.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiEvent);
  if (cfg.wifi.ssid.isEmpty()) {
    LOGE(TAG, "wifi.ssid is empty: edit config.json on the SD card");
  } else {
    WiFi.begin(cfg.wifi.ssid.c_str(), cfg.wifi.password.c_str());
    LOGI(TAG, "connecting to '%s'", cfg.wifi.ssid.c_str());
  }
  // lwIP SNTP keeps the server name pointer instead of copying it, so the string
  // must outlive setup() (cfg is a local).
  static char ntpServer[65];  // time.ntp is limited to 64 chars by Config
  strlcpy(ntpServer, cfg.time.ntp.c_str(), sizeof(ntpServer));
  configTzTime(cfg.time.tz.c_str(), ntpServer);  // NTP starts syncing once the network is up
  BootLog::stage('3');
  gHaveSsid = !cfg.wifi.ssid.isEmpty();
  if (!gHaveSsid) Indicator::ready();  // no Wi-Fi to wait for

  gRestartAfterMs = static_cast<uint32_t>(cfg.wifi.restartAfterMin) * 60000UL;
  gLastWifiOkMs = millis();

  WebApi::begin();

  // Starts the power_on scenarios after a power-up (and after the Wi-Fi watchdog's restart while they still waited, see scenario.h).
  Scenario::begin(esp_reset_reason(), resetReasonName(esp_reset_reason()), cfg.startup.scenarioDelaySec);
}

void loop() {
  if (millis() > 60000) BootLog::stage('5');  // a healthy boot; cheap no-op afterwards

  static bool timeSynced = false;
  if (!timeSynced && time(nullptr) > 1700000000) {
    timeSynced = true;
    BootLog::timeSynced();  // pin this boot's exact time as soon as the clock is valid
  }
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 600000UL) {  // every 10 min: "alive now" + how long this boot ran
    lastBeat = millis();
    BootLog::heartbeat();
  }
  // A card that was missing at start and shows up later (the logger remounts it): read the lists that live on it.
  static bool sdPrev = Storage::sdMounted();
  const bool sdNow = Storage::sdMounted();
  if (sdNow && !sdPrev) {
    if (!Devices::info().sd) Devices::load();
    Scenario::sdAvailable();
  }
  sdPrev = sdNow;
  if (WiFi.status() == WL_CONNECTED) {
    gLastWifiOkMs = millis();
  } else if (gRestartAfterMs && gHaveSsid && millis() - gLastWifiOkMs > gRestartAfterMs) {
    LOGE(TAG, "no Wi-Fi for %lu min, restarting", static_cast<unsigned long>(gRestartAfterMs / 60000UL));
    Logger::flushBlocking();
    delay(500);
    ESP.restart();
  }
  delay(1000);
}
