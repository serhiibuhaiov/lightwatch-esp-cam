#pragma once
//
// Outgoing HTTP(S) requests: a healthcheck ping, a Telegram message. GET or POST, a static body, the answer counts as
// success when its status is 2xx.
//
// Blocking (DNS, TCP, a TLS handshake: up to a few seconds), so the requests run in one worker task ("http_out") with
// a list of kQueueSize places, apart from the Tuya lanes: a slow server never holds up the sockets.
//
// HTTPS checks the server certificate against the CA bundle built into the core (no "insecure" mode). mbedTLS gets its
// memory through installTlsAllocator(): from PSRAM when the board has it (a session holds ~48 KB at its peak; in the
// ~60 KB of free 8-bit internal RAM that starves the SD card driver and the network), and every block is counted, so a
// request can tell how much the TLS session took and how low the internal RAM went.
//
// The log shows the method, the host and the status only: the path and the query may hold a token (Telegram's bot
// token is in the path).
//
#include <Arduino.h>
#include <ArduinoJson.h>

#include <atomic>
#include <memory>

namespace HttpOut {

constexpr size_t kMaxUrlBytes = 2048;
constexpr size_t kMaxBodyBytes = 16384;
constexpr size_t kMaxContentTypeBytes = 100;
constexpr size_t kQueueSize = 4;
constexpr size_t kMaxResponseBytes = 256;  // kept from the answer (for a failure's explanation); the rest is read and dropped
constexpr uint32_t kDefaultTimeoutMs = 10000, kMinTimeoutMs = 1000, kMaxTimeoutMs = 30000;
extern const char* const kDefaultContentType;  // of a POST that names none: "application/json"

enum class Method : uint8_t { Get, Post };
const char* methodName(Method m);  // "GET" | "POST"

// Text that lives in PSRAM when the board has it (a URL up to 2 KB, a body up to 16 KB). Not copyable.
class Text {
 public:
  Text() = default;
  Text(const Text&) = delete;
  Text& operator=(const Text&) = delete;
  ~Text() { free(p_); }
  bool assign(const char* s, size_t n);
  const char* c_str() const { return p_ ? p_ : ""; }
  size_t size() const { return n_; }

 private:
  char* p_ = nullptr;
  size_t n_ = 0;
};

struct Result {
  bool ok = false;           // the server answered 2xx
  int status = -1;           // the HTTP status, -1 when there was no answer
  String error;              // when !ok: url, dns, connect, tls, timeout, send, http (status not 2xx), busy, cancelled
  String detail;             // human readable
  String response;           // the first kMaxResponseBytes of the answer's body
  int32_t responseBytes = -1;  // Content-Length of the answer, -1 unknown (chunked)
  uint32_t ms = 0, queuedMs = 0, connectMs = 0, sendMs = 0, waitMs = 0;
  // Memory, measured around this request.
  bool tlsPsram = false;     // mbedTLS took its memory from PSRAM
  uint32_t tlsPeakInternal = 0, tlsPeakPsram = 0;  // the most mbedTLS held at once during the request, per region (bytes)
  uint32_t dramBefore = 0, dramLow = 0, dramAfter = 0;  // free 8-bit internal RAM: before, the lowest seen, after
  uint32_t stackFreeMin = 0; // the least stack the worker ever had left (bytes)
};

struct Job {
  Method method = Method::Get;
  Text url, contentType, body;
  uint32_t timeoutMs = kDefaultTimeoutMs;
  bool quiet = false;        // a frequent request (an interval scenario): success at DEBUG instead of INFO
  uint32_t submittedMs = 0;
  std::atomic<bool> cancelled{false};  // a job that has not started yet is dropped (error "cancelled")
  std::atomic<bool> done{false};       // set last, after `result` is complete
  Result result;
};

// Reads {method, url, content_type, body, timeout_ms} (the fields of an HTTP step) into `job`. False with `err` when a
// field is missing or out of its limits; `where` prefixes the message ("steps[2]").
bool parse(JsonVariantConst in, Job& job, String& err, const String& where = "");

// Points mbedTLS at the counting allocator. Call first thing in setup(), before Wi-Fi or anything else uses mbedTLS.
void installTlsAllocator();

// Starts the worker. Call once from setup().
void begin();

// Queues the job; false when the list is full. The caller keeps its shared_ptr and polls `done`.
bool submit(const std::shared_ptr<Job>& job);

// "api.telegram.org" out of "https://api.telegram.org/bot.../sendMessage": what the log may show.
String hostOf(const char* url);

}  // namespace HttpOut
