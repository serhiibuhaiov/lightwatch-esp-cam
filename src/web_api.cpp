#include "web_api.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "bootlog.h"
#include "config.h"
#include "devices.h"
#include "logger.h"
#include "scenario.h"
#include "storage.h"
#include "tuya.h"

// web/index.html, embedded by PlatformIO (board_build.embed_txtfiles); NUL terminated.
extern const char kIndexStart[] asm("_binary_web_index_html_start");
extern const char kIndexEnd[] asm("_binary_web_index_html_end");
// web/openapi.json, the same way.
extern const char kSpecStart[] asm("_binary_web_openapi_json_start");
extern const char kSpecEnd[] asm("_binary_web_openapi_json_end");

namespace WebApi {
namespace {

const char* TAG = "web";
constexpr size_t kMaxBody = 8192;
constexpr size_t kMaxScenariosBody = 24576;  // 8 scenarios x 24 steps with long names are ~19 KB
constexpr size_t kDefaultTail = 4096;

AsyncWebServer server(80);

// Handlers run in the async_tcp task. They never block for long: SD access goes
// through Storage (mutex, short operations) and big files are streamed in chunks.

// The largest PUT body the route accepts.
size_t maxBodyFor(AsyncWebServerRequest* req) { return req->url() == "/api/scenarios" ? kMaxScenariosBody : kMaxBody; }

void sendJson(AsyncWebServerRequest* req, int code, const String& body) {
  req->send(code, "application/json", body);
}

void sendError(AsyncWebServerRequest* req, int code, const String& message) {
  JsonDocument doc;
  doc["error"] = message;
  String out;
  serializeJson(doc, out);
  sendJson(req, code, out);
}

// True if the request carries valid credentials. Sends nothing.
bool credentialsOk(AsyncWebServerRequest* req, bool& locked) {
  String user, pass;
  locked = !Config::webCredentials(user, pass);
  if (locked) return false;
  return req->authenticate(user.c_str(), pass.c_str());
}

// True if the request may proceed; otherwise the response has been sent.
bool authorize(AsyncWebServerRequest* req) {
  bool locked = false;
  if (credentialsOk(req, locked)) return true;
  if (locked) {
    sendError(req, 403, "web_password is not set in config.json on the SD card");
    return false;
  }
  AsyncWebServerResponse* r = req->beginResponse(401, "application/json", "{\"error\":\"unauthorized\"}");
  r->addHeader("WWW-Authenticate", "Basic realm=\"esp32cam\"");
  req->send(r);
  return false;
}

// ---- GET / (web UI) -------------------------------------------------------------------

// The page holds no secrets, but it sits behind the same Basic auth as the API, so the
// browser asks for the password once and then sends it with every fetch() the page makes.
void onIndex(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  AsyncWebServerResponse* r = req->beginResponse(
      200, "text/html; charset=utf-8", reinterpret_cast<const uint8_t*>(kIndexStart),
      static_cast<size_t>(kIndexEnd - kIndexStart) - 1);
  r->addHeader("Cache-Control", "no-cache");
  req->send(r);
}

// ---- GET /api/openapi.json -------------------------------------------------------------

// Description of this API for the "API" page (and for Postman, Swagger UI and the like). Behind the same Basic auth.
void onOpenApi(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  AsyncWebServerResponse* r = req->beginResponse(
      200, "application/json", reinterpret_cast<const uint8_t*>(kSpecStart), static_cast<size_t>(kSpecEnd - kSpecStart) - 1);
  r->addHeader("Cache-Control", "no-cache");
  req->send(r);
}

// ---- GET /api/config ----------------------------------------------------------

void onConfigGet(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  // Default: the saved config (what a restart will use, and what PUT round-trips).
  // ?active=1: what the running firmware actually uses.
  const bool active = req->hasParam("active") && req->getParam("active")->value() == "1";
  AsyncWebServerResponse* r = req->beginResponse(
      200, "application/json",
      Config::toJson(active ? Config::snapshot() : Config::savedSnapshot(), false));
  r->addHeader("X-Restart-Pending", Config::restartPending() ? "true" : "false");
  req->send(r);
}

// ---- PUT /api/config ------------------------------------------------------------

void rebootTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(1500));  // let the HTTP response leave
  LOGW(TAG, "restarting on request");
  Logger::flushBlocking();
  ESP.restart();
}

// The body arrives in chunks before the request handler runs. Collect it in a
// malloc'ed buffer (freed by the library together with the request).
void onConfigBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  if (total == 0 || total > maxBodyFor(req)) return;  // onConfigPut answers 400/413
  if (index == 0) {
    bool locked = false;
    if (!credentialsOk(req, locked)) return;  // do not allocate for strangers
    req->_tempObject = malloc(total + 1);
  }
  if (!req->_tempObject) return;
  memcpy(static_cast<uint8_t*>(req->_tempObject) + index, data, len);
  if (index + len == total) static_cast<char*>(req->_tempObject)[total] = '\0';
}

void onConfigPut(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  if (req->contentLength() > maxBodyFor(req)) {
    sendError(req, 413, "body is larger than " + String(static_cast<unsigned>(maxBodyFor(req))) + " bytes");
    return;
  }
  if (!req->_tempObject) {
    // With a form content type (curl --data-binary sends one by default) the
    // library parses the body as form fields and never calls onConfigBody.
    if (req->contentType().startsWith("application/x-www-form-urlencoded")) {
      sendError(req, 415, "send the body with Content-Type: application/json");
    } else {
      sendError(req, 400, "empty body");
    }
    return;
  }

  String body(static_cast<const char*>(req->_tempObject));
  Config::SaveResult r = Config::saveFromJson(body);
  if (!r.ok) {
    sendError(req, r.invalid ? 400 : 500, r.error);
    return;
  }

  const bool reboot = req->hasParam("reboot") && req->getParam("reboot")->value() == "1";
  JsonDocument doc;
  doc["ok"] = true;
  doc["sd"] = r.sd;
  doc["flash"] = r.flash;
  doc["restart_required"] = Config::restartPending();  // settings are read once at boot
  doc["rebooting"] = reboot;
  String out;
  serializeJson(doc, out);
  sendJson(req, 200, out);

  if (reboot) xTaskCreate(rebootTask, "reboot", 8192, nullptr, 1, nullptr);
}

// ---- GET /api/status ----------------------------------------------------------------

const char* sdStateName(Storage::SdState st) {
  switch (st) {
    case Storage::SdState::Ok: return "ok";
    case Storage::SdState::Fault: return "fault";
    default: return "missing";
  }
}

void onStatus(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  const Storage::SdState sd = Storage::sdState();
  JsonDocument doc;
  doc["uptime_s"] = millis() / 1000;
  doc["heap_free"] = ESP.getFreeHeap();
  doc["time_valid"] = time(nullptr) > 1700000000;
  doc["config_source"] = Config::sourceName(Config::loadedFrom());
  doc["restart_pending"] = Config::restartPending();
  doc["sd"] = sdStateName(sd);
  doc["file_logging"] = sd == Storage::SdState::Ok;
  {
    // How long Tuya jobs waited (queue + device lock) since boot, per lane: commands (buttons/API/scenarios) and the monitor.
    const Tuya::Stats ts = Tuya::stats();
    JsonObject t = doc["tuya"].to<JsonObject>();
    const char* names[2] = {"commands", "monitor"};
    for (size_t i = 0; i < 2; ++i) {
      JsonObject c = t[names[i]].to<JsonObject>();
      c["jobs"] = ts.jobs[i];
      c["wait_avg_ms"] = ts.jobs[i] ? ts.waitSumMs[i] / ts.jobs[i] : 0;
      c["wait_max_ms"] = ts.waitMaxMs[i];
      c["wait_over_1s"] = ts.waitOver1s[i];
      c["busy"] = ts.busy[i];
    }
  }
  Scenario::summary(doc["scenarios"].to<JsonArray>());  // name, trigger, state, progress of every scenario

  // Earlier boots, oldest first, the last one is the current boot.
  JsonArray hist = doc["boot_history"].to<JsonArray>();
  BootLog::Entry be[16];
  size_t bn = BootLog::entries(be, 16);
  for (size_t i = 0; i < bn; ++i) {
    JsonObject e = hist.add<JsonObject>();
    e["reason"] = BootLog::reasonName(be[i].reason);
    e["stage"] = BootLog::stageName(be[i].stage);
    e["time_quality"] = BootLog::qualityName(be[i].quality);
    if (be[i].epoch) {
      e["time"] = be[i].epoch;  // UTC epoch seconds; "estimated" = not earlier than this
    } else {
      e["time"] = nullptr;
    }
    e["uptime_min"] = be[i].upMin;  // the boot ran at least this long
    if (i == bn - 1) e["current"] = true;
  }
  JsonObject w = doc["wifi"].to<JsonObject>();
  w["connected"] = WiFi.status() == WL_CONNECTED;
  w["ip"] = WiFi.localIP().toString();
  w["rssi"] = WiFi.RSSI();
  String out;
  serializeJson(doc, out);
  sendJson(req, 200, out);
}

bool takeJsonBody(AsyncWebServerRequest* req, String& body);              // defined below
bool parseUnsigned(const String& text, uint64_t maxValue, uint64_t& out);  // defined below

// ---- /api/scenarios ---------------------------------------------------------------------------

void onScenariosGet(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  if (req->url() != "/api/scenarios") {  // the library also routes /api/scenarios/... here
    sendError(req, 404, "not found");
    return;
  }
  sendJson(req, 200, Scenario::configJson());
}

void onScenariosPut(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  if (req->url() != "/api/scenarios") {
    sendError(req, 404, "not found");
    return;
  }
  String body;
  if (!takeJsonBody(req, body)) return;
  const Scenario::SaveResult r = Scenario::save(body);
  if (!r.ok) {
    sendError(req, r.invalid ? 400 : r.noSd ? 503 : 500, r.error);
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["count"] = r.count;
  String out;
  serializeJson(doc, out);
  sendJson(req, 200, out);
}

// POST /api/scenarios/run|stop?name=<scenario>
void onScenarioRun(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  if (!req->hasParam("name")) {
    sendError(req, 400, "name=<scenario> is required");
    return;
  }
  switch (Scenario::runNow(req->getParam("name")->value())) {
    case Scenario::RunResult::NotFound: sendError(req, 404, "no such scenario"); return;
    case Scenario::RunResult::AlreadyRunning: sendError(req, 409, "the scenario is already running"); return;
    default: break;
  }
  sendJson(req, 200, "{\"ok\":true}");
}

void onScenarioStop(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  if (!req->hasParam("name")) {
    sendError(req, 400, "name=<scenario> is required");
    return;
  }
  switch (Scenario::stop(req->getParam("name")->value())) {
    case Scenario::StopResult::NotFound: sendError(req, 404, "no such scenario"); return;
    case Scenario::StopResult::NotRunning: sendError(req, 409, "the scenario is not running"); return;
    default: break;
  }
  sendJson(req, 200, "{\"ok\":true}");
}

// ---- GET/PUT /api/devices/config (the editor) -------------------------------------------

void onDevicesConfigGet(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  sendJson(req, 200, Devices::configJson());  // names, addresses, versions; never the keys
}

// The body of a PUT as collected by onConfigBody. Sends the error response and returns false when
// there is no usable body.
bool takeJsonBody(AsyncWebServerRequest* req, String& body) {
  if (req->contentLength() > maxBodyFor(req)) {
    sendError(req, 413, "body is larger than " + String(static_cast<unsigned>(maxBodyFor(req))) + " bytes");
    return false;
  }
  if (!req->_tempObject) {
    if (req->contentType().startsWith("application/x-www-form-urlencoded")) {
      sendError(req, 415, "send the body with Content-Type: application/json");
    } else {
      sendError(req, 400, "empty body");
    }
    return false;
  }
  body = String(static_cast<const char*>(req->_tempObject));
  return true;
}

void onDevicesConfigPut(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  String body;
  if (!takeJsonBody(req, body)) return;
  const Devices::SaveResult r = Devices::save(body);
  if (!r.ok) {
    sendError(req, r.invalid ? 400 : r.noSd ? 503 : 500, r.error);
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["count"] = r.count;
  // The scenarios name devices; one that was renamed or removed would only fail when the scenario runs.
  std::vector<String> names;
  for (const Devices::Device& d : Devices::list()) names.push_back(d.name);
  JsonArray warnings = doc["warnings"].to<JsonArray>();
  for (const String& w : Scenario::missingDevices(names)) warnings.add(w);
  String out;
  serializeJson(doc, out);
  sendJson(req, 200, out);
}

// ---- /api/devices ----------------------------------------------------------------------

JsonDocument devicesSummary() {
  const Devices::LoadInfo info = Devices::info();
  JsonDocument doc;
  doc["sd"] = info.sd;
  doc["file_found"] = info.fileFound;
  doc["count"] = info.count;
  doc["poll_interval_s"] = Devices::pollIntervalSec();
  if (!info.error.isEmpty()) doc["error"] = info.error;
  JsonArray arr = doc["devices"].to<JsonArray>();
  const uint32_t now = millis();
  for (const Devices::Device& d : Devices::list()) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = d.name;
    o["ip"] = d.ip;
    o["version"] = Tuya::versionName(d.version);
    o["dp"] = d.dp;
    o["key_set"] = true;  // the key itself is never sent
    o["ignore"] = d.ignore;
    JsonObject meters = o["meters"].to<JsonObject>();  // data points of the readings, null = standard number
    for (const auto& m : {std::make_pair("power", d.dpPower), std::make_pair("current", d.dpCurrent),
                          std::make_pair("voltage", d.dpVoltage), std::make_pair("energy", d.dpEnergy)}) {
      if (m.second < 0) {
        meters[m.first] = nullptr;
      } else {
        meters[m.first] = m.second;
      }
    }

    // The cached state from the background poll: reading it never touches the device.
    Devices::State st;
    Devices::state(d.name, st);
    JsonObject s = o["state"].to<JsonObject>();
    s["known"] = st.known;
    s["online"] = st.online;
    s["fails"] = st.fails;
    if (st.lastOkMs) s["age_s"] = (now - st.lastOkMs) / 1000;  // seconds since the last success
    if (st.lastTryMs) s["last_try_age_s"] = (now - st.lastTryMs) / 1000;
    if (st.latencyMs) s["latency_ms"] = st.latencyMs;
    if (!st.error.isEmpty()) {
      s["error"] = st.error;
      s["detail"] = st.detail;
    }
    JsonDocument dps;
    if (!st.dpsJson.isEmpty() && !deserializeJson(dps, st.dpsJson)) {
      s["dps"] = dps;
      JsonVariantConst sw = dps[String(d.dp)];
      if (sw.is<bool>()) s["on"] = sw.as<bool>();  // the configured switch data point
    }
  }
  return doc;
}

void onDevicesList(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  if (req->url() != "/api/devices") {  // the library also routes /api/devices/... here
    sendError(req, 404, "not found");
    return;
  }
  String out;
  serializeJson(devicesSummary(), out);
  sendJson(req, 200, out);
}

// GET /api/devices/history?name=<device>[&limit=N]: the last readings with their times (see Devices::historyJson).
void onDevicesHistory(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  if (!req->hasParam("name")) {
    sendError(req, 400, "name=<device> is required");
    return;
  }
  uint64_t limit = 0;
  if (req->hasParam("limit") && (!parseUnsigned(req->getParam("limit")->value(), Devices::kHistoryCapacity, limit) || limit < 1)) {
    sendError(req, 400, "limit must be an integer 1.." + String(static_cast<unsigned>(Devices::kHistoryCapacity)));
    return;
  }
  String out;
  if (!Devices::historyJson(req->getParam("name")->value(), static_cast<size_t>(limit), out)) {
    sendError(req, 404, "no such device in /devices.json");
    return;
  }
  sendJson(req, 200, out);
}

void onDevicesReload(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  Devices::load();
  String out;
  serializeJson(devicesSummary(), out);
  sendJson(req, 200, out);
}

// JSON answer for a finished job. The decrypted payload is parsed so the data points
// come back as an object; anything unparsable is returned as text in "raw".
String deviceResultJson(const Tuya::Job& job) {
  const Tuya::Result& r = job.result;
  JsonDocument doc;
  doc["ok"] = r.ok;
  doc["device"] = job.device;
  doc["op"] = Tuya::opName(job.op);
  doc["ms"] = r.ms;
  JsonObject ph = doc["phases_ms"].to<JsonObject>();
  ph["connect"] = r.connectMs;
  ph["handshake"] = r.handshakeMs;
  ph["command"] = r.commandMs;
  ph["queued"] = r.queuedMs;  // waited in the queue before the worker took it
  if (job.op == Tuya::Op::Refresh) ph["update"] = r.updateMs;
  if (!r.request.isEmpty()) doc["request"] = r.request;  // the command JSON that was sent
  if (!r.before.isEmpty()) {
    JsonDocument before;
    if (!deserializeJson(before, r.before)) {
      JsonVariantConst dps = before["dps"];
      if (dps.isNull()) dps = before["data"]["dps"];
      doc["before"] = dps.isNull() ? before.as<JsonVariantConst>() : dps;
    } else {
      doc["before_raw"] = r.before;
    }
  }
  if (!r.frames.isEmpty()) doc["frames"] = r.frames;  // other frames the device sent after UPDATEDPS
  if (!r.ok) {
    doc["error"] = r.error;
    doc["detail"] = r.detail;
  }
  if (r.retcode >= 0) doc["retcode"] = r.retcode;
  if (!r.json.isEmpty()) {
    JsonDocument payload;
    if (!deserializeJson(payload, r.json)) {
      JsonVariantConst dps = payload["dps"];
      if (dps.isNull()) dps = payload["data"]["dps"];
      if (!dps.isNull()) {
        doc["dps"] = dps;
      } else {
        doc["payload"] = payload;
      }
    } else {
      doc["raw"] = r.json;
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// POST /api/devices/status|on|off?name=<device>[&timeout_ms=N]
// The operation takes seconds, so it runs in the "tuya" task. The response is a chunked
// one that the library keeps polling (RESPONSE_TRY_AGAIN) until the job is done, so the
// async_tcp task never blocks. The HTTP status is 200 once the answer starts: look at "ok".
// A non-negative decimal integer no larger than `maxValue`; false for anything else (sign, letters,
// empty text, overflow). String::toInt() is 32-bit and silently overflows, so it is not used here.
bool parseUnsigned(const String& text, uint64_t maxValue, uint64_t& out) {
  if (text.isEmpty() || text.length() > 10) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    v = v * 10 + static_cast<uint64_t>(text[i] - '0');
  }
  if (v > maxValue) return false;
  out = v;
  return true;
}

void onDeviceOp(AsyncWebServerRequest* req, Tuya::Op op) {
  if (!authorize(req)) return;
  if (!req->hasParam("name")) {
    sendError(req, 400, "name=<device> is required");
    return;
  }
  // The parameters are checked BEFORE the device is looked up, so a request that is rejected here can
  // never reach (and tests can safely use an unknown device name for it).
  uint32_t timeoutMs = 8000;
  if (req->hasParam("timeout_ms")) {
    uint64_t v = 0;
    if (!parseUnsigned(req->getParam("timeout_ms")->value(), 30000, v) || v < 500) {
      sendError(req, 400, "timeout_ms must be in 500..30000");
      return;
    }
    timeoutMs = static_cast<uint32_t>(v);
  }
  // Refresh: `dps` (list of data points to re-read; default: the device's meters), `settle_ms` (how long to listen
  // after UPDATEDPS) and `compare=1` (also query before UPDATEDPS) are diagnostics.
  Tuya::RefreshOpts refreshOpts;
  bool dpsGiven = false;
  if (op == Tuya::Op::Refresh) {
    if (req->hasParam("settle_ms")) {
      uint64_t v = 0;
      if (!parseUnsigned(req->getParam("settle_ms")->value(), 5000, v)) {
        sendError(req, 400, "settle_ms must be in 0..5000");
        return;
      }
      refreshOpts.settleMs = static_cast<uint32_t>(v);
    }
    if (req->hasParam("compare")) {
      const String c = req->getParam("compare")->value();
      if (c != "0" && c != "1") {
        sendError(req, 400, "compare must be 0 or 1");
        return;
      }
      refreshOpts.compare = c == "1";
    }
    if (req->hasParam("dps")) {
      const String list = req->getParam("dps")->value();
      String norm;
      size_t count = 0, from = 0;
      for (;;) {
        int comma = list.indexOf(',', from);
        String item = comma < 0 ? list.substring(from) : list.substring(from, comma);
        uint64_t v = 0;
        if (!parseUnsigned(item, 255, v) || ++count > 16) {
          sendError(req, 400, "dps must be up to 16 numbers 0..255 separated by commas");
          return;
        }
        if (!norm.isEmpty()) norm += ',';
        norm += String(static_cast<unsigned long>(v));
        if (comma < 0) break;
        from = comma + 1;
      }
      refreshOpts.dpIds = norm;
      dpsGiven = true;
    }
  } else if (req->hasParam("dps") || req->hasParam("settle_ms") || req->hasParam("compare")) {
    sendError(req, 400, "dps, settle_ms and compare are only for refresh");
    return;
  }
  // Diagnostics: `t` replaces the time sent in a control command (to see how a device reacts to a clock
  // that is not synchronised, e.g. seconds since boot instead of the Unix time).
  int64_t unixTime = -1;
  if (req->hasParam("t")) {
    uint64_t v = 0;
    if (op == Tuya::Op::Query || op == Tuya::Op::Refresh || !parseUnsigned(req->getParam("t")->value(), 4294967295ULL, v)) {
      sendError(req, 400, "t=<unix seconds 0..4294967295> is only for on/off");
      return;
    }
    unixTime = static_cast<int64_t>(v);
  }
  Devices::Device d;
  if (!Devices::find(req->getParam("name")->value(), d)) {
    sendError(req, 404, "no such device in /devices.json");
    return;
  }
  if (op == Tuya::Op::Refresh && !dpsGiven) refreshOpts.dpIds = Devices::meterDps(d);
  std::shared_ptr<Tuya::Job> job = Tuya::submit(op, d.name, d.target(), d.dp, timeoutMs, false, unixTime,
                                                op == Tuya::Op::Refresh ? &refreshOpts : nullptr);
  if (!job) {
    sendError(req, 503, "the device queue is full, try again");
    return;
  }
  auto body = std::make_shared<String>();
  AsyncWebServerResponse* r = req->beginChunkedResponse(
      "application/json", [job, body](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
        if (!job->done.load(std::memory_order_acquire)) return RESPONSE_TRY_AGAIN;
        if (body->isEmpty()) *body = deviceResultJson(*job);
        if (index >= body->length()) return 0;
        const size_t n = std::min(maxLen, body->length() - index);
        memcpy(buffer, body->c_str() + index, n);
        return n;
      });
  req->send(r);
}

// ---- GET /api/logs ------------------------------------------------------------------

// Keeps the last `tail` bytes and drops the partial first line.
void trimToTail(String& text, size_t tail) {
  if (text.length() <= tail) return;
  text.remove(0, text.length() - tail);
  int nl = text.indexOf('\n');
  if (nl >= 0) text.remove(0, nl + 1);
}

size_t tailParam(AsyncWebServerRequest* req) {
  size_t tail = static_cast<size_t>(req->getParam("tail")->value().toInt());
  return tail == 0 ? kDefaultTail : tail;
}

void sendLogFile(AsyncWebServerRequest* req) {
  String name = req->getParam("file")->value();
  const bool current = name == "current";
  if (!Storage::sdMounted()) {
    // No card: the only log is the RAM buffer (newest lines), served as "current".
    if (!current) {
      sendError(req, 503, "SD card is not mounted: only file=current (RAM log) is available");
      return;
    }
    String pending;
    Logger::copyPending(pending);
    if (req->hasParam("tail")) trimToTail(pending, tailParam(req));
    req->send(200, "text/plain; charset=utf-8", pending);
    return;
  }
  if (current) name = Logger::currentFileName();
  // "current" may legitimately have no file yet (boot quiet period): the lines
  // still waiting in RAM are served for tail requests.
  if (!(current && name.isEmpty()) && !Logger::isValidLogName(name)) {
    sendError(req, 400, "file must be log-NNNN.txt or current");
    return;
  }

  String path = name.isEmpty() ? String("") : Logger::pathFor(name);
  const bool exists = !path.isEmpty() && Storage::sdSize(path.c_str()) >= 0;
  if (!exists && !(current && req->hasParam("tail"))) {
    sendError(req, 404, "no such log file");
    return;
  }

  if (req->hasParam("tail")) {
    size_t tail = tailParam(req);
    String text;
    if (exists && !Storage::sdReadTail(path.c_str(), tail, text)) {
      sendError(req, 500, "read failed");
      return;
    }
    if (current) {
      String pending;
      Logger::copyPending(pending);
      text += pending;
      trimToTail(text, tail);
    }
    req->send(200, "text/plain; charset=utf-8", text);
    return;
  }

  // Whole file: each chunk is an independent short SD read under the Storage
  // mutex, so the logger keeps appending while a download is in progress.
  AsyncWebServerResponse* r = req->beginChunkedResponse(
      "text/plain; charset=utf-8",
      [path](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
        return Storage::sdReadAt(path.c_str(), index, buffer, maxLen);
      });
  req->send(r);
}

void onLogs(AsyncWebServerRequest* req) {
  if (!authorize(req)) return;
  if (req->hasParam("file")) {
    sendLogFile(req);
    return;
  }

  std::vector<Storage::FileInfo> files;
  Storage::sdList("/logs", files);
  files.erase(std::remove_if(files.begin(), files.end(),
                             [](const Storage::FileInfo& f) { return !Logger::isValidLogName(f.name); }),
              files.end());
  std::sort(files.begin(), files.end(),
            [](const Storage::FileInfo& a, const Storage::FileInfo& b) { return a.name > b.name; });

  JsonDocument doc;
  doc["sd"] = sdStateName(Storage::sdState());
  doc["current"] = Logger::currentFileName();
  JsonArray arr = doc["files"].to<JsonArray>();
  for (const auto& f : files) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = f.name;
    o["size"] = f.size;
  }
  String out;
  serializeJson(doc, out);
  sendJson(req, 200, out);
}

}  // namespace

void begin() {
  server.on("/", HTTP_GET, onIndex);
  server.on("/index.html", HTTP_GET, onIndex);
  server.on("/api/config", HTTP_GET, onConfigGet);
  server.on("/api/config", HTTP_PUT, onConfigPut, nullptr, onConfigBody);
  server.on("/api/logs", HTTP_GET, onLogs);
  server.on("/api/status", HTTP_GET, onStatus);
  server.on("/api/openapi.json", HTTP_GET, onOpenApi);
  // /run and /stop first: the library also routes /api/scenarios/... to the /api/scenarios handlers.
  server.on("/api/scenarios/run", HTTP_POST, onScenarioRun);
  server.on("/api/scenarios/stop", HTTP_POST, onScenarioStop);
  server.on("/api/scenarios", HTTP_GET, onScenariosGet);
  server.on("/api/scenarios", HTTP_PUT, onScenariosPut, nullptr, onConfigBody);
  // The editor routes come first: the library also routes /api/devices/... to the /api/devices handler.
  server.on("/api/devices/config", HTTP_GET, onDevicesConfigGet);
  server.on("/api/devices/config", HTTP_PUT, onDevicesConfigPut, nullptr, onConfigBody);
  server.on("/api/devices/history", HTTP_GET, onDevicesHistory);
  server.on("/api/devices", HTTP_GET, onDevicesList);
  server.on("/api/devices/reload", HTTP_POST, onDevicesReload);
  server.on("/api/devices/status", HTTP_POST, [](AsyncWebServerRequest* req) { onDeviceOp(req, Tuya::Op::Query); });
  server.on("/api/devices/refresh", HTTP_POST, [](AsyncWebServerRequest* req) { onDeviceOp(req, Tuya::Op::Refresh); });
  server.on("/api/devices/on", HTTP_POST, [](AsyncWebServerRequest* req) { onDeviceOp(req, Tuya::Op::On); });
  server.on("/api/devices/off", HTTP_POST, [](AsyncWebServerRequest* req) { onDeviceOp(req, Tuya::Op::Off); });
  server.onNotFound([](AsyncWebServerRequest* req) { sendError(req, 404, "not found"); });
  server.begin();
  LOGI(TAG, "web API listening on port 80");
}

}  // namespace WebApi
