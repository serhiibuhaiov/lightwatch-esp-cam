#include "http_out.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_memory_utils.h>
#include <esp_tls_errors.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/platform.h>

#include <deque>

#include "logger.h"
#include "psram_alloc.h"

namespace HttpOut {

const char* const kDefaultContentType = "application/json";

namespace {

const char* TAG = "http";
constexpr uint32_t kStackBytes = 8192;
constexpr uint32_t kDram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

// ---- mbedTLS memory ---------------------------------------------------------------------------------------------------
// Every mbedTLS block goes through here (the Wi-Fi supplicant's included): PSRAM when the board has it, internal RAM
// otherwise or when PSRAM is full. heap_caps_get_allocated_size() tells a block's size on free, so a block mbedTLS took
// before the allocator was installed is freed correctly too (the counts just never saw it).
// Measured on the board (2026-10-17, api.telegram.org): one HTTPS request holds ~48 KB at its peak. In internal RAM that
// took the free 8-bit RAM from ~57 KB down to ~1 KB (and a request failed for it); in PSRAM the internal RAM went down
// by ~9 KB only (the client's buffers, the socket), and the request took as long (~0.7 s).

std::atomic<uint32_t> gTlsInternal{0}, gTlsPsramBytes{0};          // held now
std::atomic<uint32_t> gTlsPeakInternal{0}, gTlsPeakPsram{0};       // since the last resetPeaks()
std::atomic<uint32_t> gDramLow{UINT32_MAX};                         // lowest free 8-bit internal RAM seen at an allocation

void raise(std::atomic<uint32_t>& peak, uint32_t v) {
  uint32_t cur = peak.load();
  while (v > cur && !peak.compare_exchange_weak(cur, v)) {
  }
}

void lower(std::atomic<uint32_t>& low, uint32_t v) {
  uint32_t cur = low.load();
  while (v < cur && !low.compare_exchange_weak(cur, v)) {
  }
}

void* tlsCalloc(size_t n, size_t size) {
  if (size && n > SIZE_MAX / size) return nullptr;
  void* p = nullptr;
  bool external = false;
  if (psramAvailable()) {
    p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    external = p != nullptr;
  }
  if (!p) p = heap_caps_calloc(n, size, kDram);
  if (p) {
    const uint32_t got = heap_caps_get_allocated_size(p);
    if (external) {
      raise(gTlsPeakPsram, gTlsPsramBytes += got);
    } else {
      raise(gTlsPeakInternal, gTlsInternal += got);
    }
  }
  lower(gDramLow, heap_caps_get_free_size(kDram));
  return p;
}

void sub(std::atomic<uint32_t>& held, uint32_t v) {
  uint32_t cur = held.load();
  while (!held.compare_exchange_weak(cur, cur > v ? cur - v : 0)) {
  }
}

void tlsFree(void* p) {
  if (!p) return;
  sub(esp_ptr_external_ram(p) ? gTlsPsramBytes : gTlsInternal, heap_caps_get_allocated_size(p));
  heap_caps_free(p);
}

// ---- the list and the worker ------------------------------------------------------------------------------------------

std::deque<std::shared_ptr<Job>> gJobs;
SemaphoreHandle_t gMux = nullptr;    // guards gJobs
SemaphoreHandle_t gReady = nullptr;  // counts the jobs, the worker sleeps on it

bool validUrl(const char* s, size_t n, String& why) {
  const bool https = n >= 8 && strncmp(s, "https://", 8) == 0;
  const bool http = n >= 7 && strncmp(s, "http://", 7) == 0;
  if (!http && !https) {
    why = "must start with http:// or https://";
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c <= ' ' || c >= 0x7f) {
      why = "must not hold spaces, control or non-ASCII characters (percent-encode them)";
      return false;
    }
  }
  const size_t from = https ? 8 : 7;
  if (from >= n || s[from] == '/' || s[from] == ':' || s[from] == '?') {
    why = "has no host";
    return false;
  }
  return true;
}

// Sorts a failure of the connection out of what esp-tls remembers: dns, connect, tls or timeout.
void classify(esp_http_client_handle_t h, esp_err_t err, Result& r) {
  int tlsCode = 0, flags = 0;
  const esp_err_t last = esp_http_client_get_and_clear_last_tls_error(h, &tlsCode, &flags);
  char buf[160];
  if (last == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
    r.error = "dns";
    r.detail = "the host name could not be resolved";
  } else if (last == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
    r.error = "timeout";
    r.detail = "no TCP connection within the timeout";
  } else if (last > ESP_ERR_ESP_TLS_BASE && last < ESP_ERR_MBEDTLS_CERT_PARTLY_OK) {  // the generic (socket) codes
    r.error = "connect";
    snprintf(buf, sizeof(buf), "%s (errno %d)", esp_err_to_name(last), esp_http_client_get_errno(h));
    r.detail = buf;
  } else if (last != ESP_OK) {
    r.error = "tls";
    snprintf(buf, sizeof(buf), "%s, mbedTLS -0x%04x, certificate flags 0x%x", esp_err_to_name(last), -tlsCode, flags);
    r.detail = buf;
  } else {
    r.error = err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_HTTP_CONNECTING ? "timeout" : "connect";
    r.detail = esp_err_to_name(err);
  }
}

Result perform(Job& job) {
  Result r;
  const uint32_t t0 = millis();
  esp_http_client_config_t c = {};
  c.url = job.url.c_str();
  c.method = job.method == Method::Post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  c.timeout_ms = static_cast<int>(job.timeoutMs);
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.disable_auto_redirect = true;  // a redirect is an answer (3xx), not followed
  c.buffer_size = 1024;
  c.buffer_size_tx = 1024;
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (!h) {
    r.error = "url";
    r.detail = "the client could not be set up (bad URL or no memory)";
    r.ms = millis() - t0;
    return r;
  }
  if (job.method == Method::Post) {
    esp_http_client_set_header(h, "Content-Type", job.contentType.size() ? job.contentType.c_str() : kDefaultContentType);
  }
  const int len = static_cast<int>(job.body.size());
  esp_err_t err = esp_http_client_open(h, job.method == Method::Post ? len : 0);
  r.connectMs = millis() - t0;
  if (err != ESP_OK) {
    classify(h, err, r);
  } else {
    const uint32_t t1 = millis();
    int sent = 0;
    while (sent < len) {
      const int n = esp_http_client_write(h, job.body.c_str() + sent, len - sent);
      if (n <= 0) break;
      sent += n;
    }
    r.sendMs = millis() - t1;
    if (sent < len) {
      r.error = "send";
      r.detail = "the connection broke after " + String(sent) + " of " + String(len) + " bytes of the body";
    } else {
      const uint32_t t2 = millis();
      const int64_t length = esp_http_client_fetch_headers(h);
      r.status = esp_http_client_get_status_code(h);
      if (length < 0 || r.status <= 0) {
        r.status = -1;
        r.error = length == -ESP_ERR_HTTP_EAGAIN ? "timeout" : "connect";
        r.detail = length == -ESP_ERR_HTTP_EAGAIN ? "no answer within the timeout" : "the connection closed without an answer";
      } else {
        r.responseBytes = esp_http_client_is_chunked_response(h) ? -1 : static_cast<int32_t>(length);
        char buf[kMaxResponseBytes], scratch[128];
        size_t kept = 0;
        for (;;) {  // keep the start, read the rest away so the server sees a complete exchange
          const bool full = kept == sizeof(buf);
          const int n = esp_http_client_read(h, full ? scratch : buf + kept,
                                             full ? static_cast<int>(sizeof(scratch)) : static_cast<int>(sizeof(buf) - kept));
          if (n <= 0) break;
          if (!full) kept += static_cast<size_t>(n);
        }
        r.response = String(buf, kept);
        r.ok = r.status >= 200 && r.status < 300;
        if (!r.ok) {
          r.error = "http";
          r.detail = "the server answered " + String(r.status);
        }
      }
      r.waitMs = millis() - t2;
    }
  }
  esp_http_client_close(h);
  esp_http_client_cleanup(h);
  r.ms = millis() - t0;
  return r;
}

std::shared_ptr<Job> nextJob() {
  xSemaphoreTake(gReady, portMAX_DELAY);
  xSemaphoreTake(gMux, portMAX_DELAY);
  std::shared_ptr<Job> job;
  if (!gJobs.empty()) {
    job = gJobs.front();
    gJobs.pop_front();
  }
  xSemaphoreGive(gMux);
  return job;
}

void worker(void*) {
  for (;;) {
    std::shared_ptr<Job> job = nextJob();
    if (!job) continue;
    const uint32_t queuedMs = millis() - job->submittedMs;
    Result r;
    if (job->cancelled.load()) {
      r.error = "cancelled";
      r.detail = "the request was withdrawn before it started";
    } else {
      const uint32_t before = heap_caps_get_free_size(kDram);
      gTlsPeakInternal.store(gTlsInternal.load());
      gTlsPeakPsram.store(gTlsPsramBytes.load());
      gDramLow.store(before);
      r = perform(*job);
      r.tlsPsram = psramAvailable();
      r.tlsPeakInternal = gTlsPeakInternal.load();
      r.tlsPeakPsram = gTlsPeakPsram.load();
      r.dramBefore = before;
      r.dramAfter = heap_caps_get_free_size(kDram);
      lower(gDramLow, r.dramAfter);
      r.dramLow = gDramLow.load();
    }
    r.queuedMs = queuedMs;
    r.stackFreeMin = uxTaskGetStackHighWaterMark(nullptr);  // bytes on the ESP32 (StackType_t is a byte)
    const String host = hostOf(job->url.c_str());
    if (r.ok) {
      Logger::log(job->quiet ? Logger::Level::Debug : Logger::Level::Info, TAG, "%s %s: %d in %lu ms (connect %lu; queued %lu)",
                  methodName(job->method), host.c_str(), r.status, static_cast<unsigned long>(r.ms),
                  static_cast<unsigned long>(r.connectMs), static_cast<unsigned long>(queuedMs));
    } else {
      Logger::log(job->quiet ? Logger::Level::Debug : Logger::Level::Warn, TAG, "%s %s: FAILED (%s) after %lu ms (connect %lu; queued %lu): %s",
                  methodName(job->method), host.c_str(), r.error.c_str(), static_cast<unsigned long>(r.ms),
                  static_cast<unsigned long>(r.connectMs), static_cast<unsigned long>(queuedMs), r.detail.c_str());
    }
    job->result = r;
    job->done.store(true, std::memory_order_release);
  }
}

}  // namespace

const char* methodName(Method m) { return m == Method::Post ? "POST" : "GET"; }

bool Text::assign(const char* s, size_t n) {
  char* p = static_cast<char*>(psramMalloc(n + 1));
  if (!p) return false;
  memcpy(p, s, n);
  p[n] = '\0';
  free(p_);
  p_ = p;
  n_ = n;
  return true;
}

bool parse(JsonVariantConst in, Job& job, String& err, const String& where) {
  const String at = where.isEmpty() ? "" : where + ".";
  JsonVariantConst m = in["method"];
  if (m.isNull() || (m.is<const char*>() && strcmp(m.as<const char*>(), "GET") == 0)) {
    job.method = Method::Get;
  } else if (m.is<const char*>() && strcmp(m.as<const char*>(), "POST") == 0) {
    job.method = Method::Post;
  } else {
    err = at + "method must be \"GET\" or \"POST\"";
    return false;
  }
  JsonVariantConst u = in["url"];
  if (!u.is<const char*>()) {
    err = at + "url is required (http://... or https://...)";
    return false;
  }
  const char* url = u.as<const char*>();
  const size_t urlLen = strlen(url);
  String why;
  if (urlLen > kMaxUrlBytes) {
    err = at + "url is longer than " + String(static_cast<unsigned>(kMaxUrlBytes)) + " bytes";
    return false;
  }
  if (!validUrl(url, urlLen, why)) {
    err = at + "url " + why;
    return false;
  }
  JsonVariantConst ct = in["content_type"], b = in["body"];
  if (job.method == Method::Get && (!ct.isNull() || !b.isNull())) {
    err = at + "content_type and body are for POST only";
    return false;
  }
  const char* type = "";
  if (!ct.isNull()) {
    type = ct.is<const char*>() ? ct.as<const char*>() : nullptr;
    const size_t n = type ? strlen(type) : 0;
    bool printable = n > 0 && n <= kMaxContentTypeBytes;
    for (size_t i = 0; printable && i < n; ++i) printable = type[i] >= ' ' && type[i] < 0x7f;
    if (!printable) {
      err = at + "content_type must be 1.." + String(static_cast<unsigned>(kMaxContentTypeBytes)) + " printable ASCII characters";
      return false;
    }
  }
  const char* body = "";
  if (!b.isNull()) {
    if (!b.is<const char*>()) {
      err = at + "body must be a string (a JSON body goes in as text: \"{\\\"chat_id\\\":...}\")";
      return false;
    }
    body = b.as<const char*>();
    if (strlen(body) > kMaxBodyBytes) {
      err = at + "body is longer than " + String(static_cast<unsigned>(kMaxBodyBytes)) + " bytes";
      return false;
    }
  }
  JsonVariantConst t = in["timeout_ms"];
  uint32_t timeoutMs = kDefaultTimeoutMs;
  if (!t.isNull()) {
    if (!t.is<int>() || t.as<int>() < static_cast<int>(kMinTimeoutMs) || t.as<int>() > static_cast<int>(kMaxTimeoutMs)) {
      err = at + "timeout_ms must be an integer " + String(kMinTimeoutMs) + ".." + String(kMaxTimeoutMs);
      return false;
    }
    timeoutMs = static_cast<uint32_t>(t.as<int>());
  }
  if (!job.url.assign(url, urlLen) || !job.contentType.assign(type, strlen(type)) || !job.body.assign(body, strlen(body))) {
    err = "out of memory";
    return false;
  }
  job.timeoutMs = timeoutMs;
  return true;
}

void installTlsAllocator() { mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree); }

void begin() {
  if (gMux) return;
  gMux = xSemaphoreCreateMutex();
  gReady = xSemaphoreCreateCounting(kQueueSize, 0);
  xTaskCreate(worker, "http_out", kStackBytes, nullptr, 1, nullptr);
}

bool submit(const std::shared_ptr<Job>& job) {
  if (!gMux || !job) return false;
  job->submittedMs = millis();
  xSemaphoreTake(gMux, portMAX_DELAY);
  const bool room = gJobs.size() < kQueueSize;
  if (room) gJobs.push_back(job);
  xSemaphoreGive(gMux);
  if (room) xSemaphoreGive(gReady);
  return room;
}

String hostOf(const char* url) {
  const char* s = strstr(url, "://");
  s = s ? s + 3 : url;
  size_t n = 0;
  while (s[n] && s[n] != '/' && s[n] != '?' && s[n] != '#') ++n;
  const char* at = static_cast<const char*>(memchr(s, '@', n));  // user:password@host: never shown
  if (at) {
    n -= static_cast<size_t>(at + 1 - s);
    s = at + 1;
  }
  return String(s, n);
}

}  // namespace HttpOut
