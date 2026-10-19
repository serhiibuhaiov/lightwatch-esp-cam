#include "tuya.h"

#include <WiFi.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <time.h>

#include <algorithm>
#include <deque>
#include <map>

#include "logger.h"
#include "psram_alloc.h"

namespace Tuya {
namespace {

const char* TAG = "tuya";

constexpr uint16_t kPort = 6668;
// A live device on the LAN accepts a connection in well under 0.3 s; waiting long for a dead one only keeps the single
// worker busy. But lwIP resends a lost SYN only after 3 s (CONFIG_LWIP_TCP_RTO_TIME), so on a weak Wi-Fi one lost packet
// failed the connect of a device that was there (a TCP probe right after it was accepted in 43 ms). A device whose last
// connect got through gets kConnectMaxMs, one that did not (or was never tried) kConnectTimeoutMs. The rest of the
// operation (handshake, answer) gets `timeoutMs` from the moment the connection is up.
constexpr uint32_t kConnectTimeoutMs = 1500;
constexpr size_t kMaxFrame = 1536;            // larger frames from the device are rejected
constexpr uint32_t kPrefix55AA = 0x000055AA;  // 3.4
constexpr uint32_t kSuffix55AA = 0x0000AA55;
constexpr uint32_t kPrefix6699 = 0x00006699;  // 3.5
constexpr uint32_t kSuffix6699 = 0x00009966;

constexpr uint32_t kCmdSessStart = 0x03;
constexpr uint32_t kCmdSessResp = 0x04;
constexpr uint32_t kCmdSessFinish = 0x05;
constexpr uint32_t kCmdControlNew = 0x0d;
constexpr uint32_t kCmdDpQueryNew = 0x10;
constexpr uint32_t kCmdUpdateDps = 0x12;
constexpr uint32_t kCmdHeartBeat = 0x09;

// ---- big-endian helpers and crypto primitives --------------------------------

void put32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

uint32_t get32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool sameBytes(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

void hmac256(const uint8_t* key, const uint8_t* data, size_t len, uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 16, data, len, out);
}

// AES-128-ECB in place; len must be a multiple of 16.
void aesEcb(const uint8_t* key, bool encrypt, uint8_t* buf, size_t len) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (encrypt) {
    mbedtls_aes_setkey_enc(&ctx, key, 128);
  } else {
    mbedtls_aes_setkey_dec(&ctx, key, 128);
  }
  for (size_t i = 0; i < len; i += 16) {
    mbedtls_aes_crypt_ecb(&ctx, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, buf + i, buf + i);
  }
  mbedtls_aes_free(&ctx);
}

bool gcmEncrypt(const uint8_t* key, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, const uint8_t* in,
                size_t len, uint8_t* out, uint8_t tag[16]) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, len, iv, 12, aad, aadLen, in, out, 16, tag) == 0;
  mbedtls_gcm_free(&ctx);
  return ok;
}

// False when the authentication tag does not match.
bool gcmDecrypt(const uint8_t* key, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, const uint8_t* in,
                size_t len, const uint8_t tag[16], uint8_t* out) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_gcm_auth_decrypt(&ctx, len, iv, 12, aad, aadLen, tag, 16, in, out) == 0;
  mbedtls_gcm_free(&ctx);
  return ok;
}

// ---- one connection ---------------------------------------------------------------

struct Frame {
  uint32_t seq = 0;
  uint32_t cmd = 0;
  uint32_t retcode = 0;
  uint8_t* data = nullptr;  // decrypted payload, points into the receive buffer
  size_t len = 0;
};

void noteConnection(const String& ip);  // counts a TCP connection for connections()
// Whether the last TCP connect to `ip` got through (false for one never tried), and recording it.
bool lastConnectOk(const String& ip);
void noteConnect(const String& ip, bool ok);
void (*gPushObserver)(const String&, const Target&, const String&) = nullptr;
constexpr uint32_t kCmdStatus = 0x08;  // a status push the device sends on its own

// One connection to a device. Without Target::keep it carries one operation and is closed (the Session is destroyed);
// a kept one stays open between operations: run() then skips the connect and the handshake.
class Session {
 public:
  Session(const Target& target, const char* label) : target_(target), label_(label ? label : "") {
    // In PSRAM when there is some: 3 KB of 8-bit DRAM per session (a kept connection holds them all the time). Only the
    // CPU touches them (AES/GCM, and WiFiClient copies into and out of the lwIP buffers).
    tx_ = static_cast<uint8_t*>(psramMalloc(kMaxFrame));
    rx_ = static_cast<uint8_t*>(psramMalloc(kMaxFrame));
  }

  ~Session() {
    close();
    free(tx_);
    free(rx_);
    // Do not leave key material around in freed memory.
    memset(realKey_, 0, sizeof(realKey_));
  }

  const Target& target() const { return target_; }
  void setLabel(const char* label) { label_ = label ? label : ""; }
  bool isOpen() const { return open_; }
  uint32_t idleMs() const { return millis() - lastUseMs_; }

  void close() {
    if (open_ || client_.connected()) client_.stop();
    open_ = false;
    memset(sessionKey_, 0, sizeof(sessionKey_));
  }

  // `dp` is the switch data point (for a status query too: frames that carry it are logged at INFO, see drain()).
  Result run(Op op, uint16_t dp, uint32_t unixTime, uint32_t timeoutMs, bool background, const RefreshOpts* refresh = nullptr) {
    const uint32_t start = millis();
    if (dp) switchDp_ = dp;
    timeoutMs_ = timeoutMs;
    detail_ = background ? Logger::Level::Debug : Logger::Level::Info;
    connectMs_ = handshakeMs_ = commandMs_ = 0;
    errCode_ = nullptr;
    errDetail_ = "";
    Result r;
    r.reused = open_;
    bool ok = execute(op, dp, unixTime, refresh, r);
    const bool closed = errCode_ && strcmp(errCode_, "closed") == 0, timedOut = errCode_ && strcmp(errCode_, "timeout") == 0;
    if (!ok && r.reused && (closed || timedOut)) {
      // The device dropped the kept connection while it was idle, or the connection is dead without the TCP knowing it
      // (the device restarted or left the Wi-Fi: what is sent gets no answer): once more on a new one, with the whole
      // timeout again (on/off may arrive twice).
      logf(Logger::Level::Info, closed ? "the kept connection was closed by the device, connecting again"
                                       : "no answer over the kept connection, connecting again");
      close();
      r = Result();
      connectMs_ = handshakeMs_ = commandMs_ = 0;
      errCode_ = nullptr;
      errDetail_ = "";
      ok = execute(op, dp, unixTime, refresh, r);
      r.retried = true;
    }
    if (!ok || !target_.keep) close();
    lastUseMs_ = millis();
    r.ms = millis() - start;
    r.connectMs = connectMs_;
    r.handshakeMs = handshakeMs_;
    r.commandMs = commandMs_;
    r.ok = ok;
    if (!ok) {
      r.error = errCode_ ? errCode_ : "protocol";
      r.detail = errDetail_;
    }
    return r;
  }

  // A kept connection that is idle (see the heartbeat task): reads what the device sent meanwhile, sends a heartbeat
  // and waits up to kHeartbeatTimeoutMs for its answer. False: the connection is gone (closed here, logged once).
  bool heartbeat() {
    if (!open_) return false;
    detail_ = Logger::Level::Debug;
    errCode_ = nullptr;
    errDetail_ = "";
    deadline_ = millis() + kHeartbeatTimeoutMs;
    if (!drain() || !sendPacket(kCmdHeartBeat, nullptr, 0, false, sessionKey_)) {
      close();
      return false;
    }
    for (;;) {
      Frame f;
      if (!recvFrame(sessionKey_, f)) {
        if (errCode_ && strcmp(errCode_, "closed") == 0) {
          logf(Logger::Level::Info, "the device closed the kept connection (seen by the heartbeat %lu s after the last request)",
               static_cast<unsigned long>(idleMs() / 1000));
        } else {
          logf(Logger::Level::Info, "no answer to the heartbeat in %lu ms (%s), connection closed",
               static_cast<unsigned long>(kHeartbeatTimeoutMs), errCode_ ? errCode_ : "protocol");
        }
        close();
        return false;
      }
      if (f.cmd == kCmdHeartBeat) break;
      logPushed(f);  // a status push that came first
      pushed(f);
    }
    lastUseMs_ = millis();
    return true;
  }

  // A kept connection (see the heartbeat task): reads what the device has sent since the last
  // look, if anything. False: the connection is gone (closed here, logged once).
  bool readPushes() {
    if (!open_) return false;
    if (client_.available() <= 0 && client_.connected()) return true;
    detail_ = Logger::Level::Debug;
    errCode_ = nullptr;
    errDetail_ = "";
    if (!drain()) {
      close();
      return false;
    }
    return true;
  }

 private:
  // One log line "[label] message".
  void logf(Logger::Level level, const char* fmt, ...) const __attribute__((format(printf, 3, 4))) {
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    Logger::log(level, TAG, "[%s] %s", label_.c_str(), msg);
  }

  // header + ": " + text, split over several lines when the text is long (a log message holds ~320 chars).
  void logLong(Logger::Level level, const String& header, const String& text) const {
    constexpr size_t kPiece = 200;
    if (text.length() <= kPiece) {
      Logger::log(level, TAG, "[%s] %s: %s", label_.c_str(), header.c_str(), text.c_str());
      return;
    }
    const size_t parts = (text.length() + kPiece - 1) / kPiece;
    for (size_t i = 0; i < parts; ++i) {
      Logger::log(level, TAG, "[%s] %s (%u/%u): %s", label_.c_str(), header.c_str(), static_cast<unsigned>(i + 1),
                  static_cast<unsigned>(parts), text.substring(i * kPiece, (i + 1) * kPiece).c_str());
    }
  }

  bool fail(const char* code, const String& detail) {
    if (!errCode_) {  // the first failure is the cause, later ones are consequences
      errCode_ = code;
      errDetail_ = detail;
    }
    return false;
  }

  bool v34() const { return target_.version == Version::V34; }

  // TCP connect and session key negotiation.
  bool connect() {
    if (target_.key.length() != 16) return fail("protocol", "the local key must be exactly 16 characters");
    memcpy(realKey_, target_.key.c_str(), 16);
    IPAddress ip;
    if (!ip.fromString(target_.ip)) return fail("connect", "invalid IP address");
    if (WiFi.status() != WL_CONNECTED) return fail("connect", "Wi-Fi is not connected");

    uint32_t t0 = millis();
    const uint32_t limitMs = lastConnectOk(target_.ip) ? kConnectMaxMs : kConnectTimeoutMs;
    const bool connected = client_.connect(ip, kPort, static_cast<int32_t>(limitMs));
    connectMs_ = millis() - t0;
    noteConnect(target_.ip, connected);
    if (!connected) {
      return fail("connect", "cannot connect to " + target_.ip + ":6668 (device off, wrong IP, or another client holds the connection)");
    }
    deadline_ = millis() + timeoutMs_;  // the handshake and the answer get the whole timeout, the connect is extra
    noteConnection(target_.ip);
    client_.setNoDelay(true);
    logf(detail_, "connected in %lu ms", static_cast<unsigned long>(connectMs_));

    nextSeq_ = 1;
    t0 = millis();
    const bool shook = handshake();
    handshakeMs_ = millis() - t0;
    if (!shook) return false;
    open_ = true;
    logf(detail_, "session key negotiated in %lu ms", static_cast<unsigned long>(handshakeMs_));
    if (target_.keep) logf(Logger::Level::Info, "connection kept open (%lu since boot)", static_cast<unsigned long>(connections(target_.ip)));
    return true;
  }

  // Reads what the device sent on its own over a kept connection while it was idle (the answers to heartbeats, status
  // pushes: one that carries the switch is logged at INFO, readings at the detail level). False: the device closed it.
  bool drain() {
    while (client_.available() > 0) {
      const uint32_t saved = deadline_;
      deadline_ = millis() + 500;
      Frame f;
      const bool got = recvFrame(sessionKey_, f);
      deadline_ = saved;
      if (!got) return false;
      if (f.cmd != kCmdHeartBeat) {
        logPushed(f);
        pushed(f);
      }
    }
    if (!client_.connected()) {
      logf(Logger::Level::Info, "the device closed the kept connection (seen %lu s after the last request)",
           static_cast<unsigned long>(idleMs() / 1000));
      return false;
    }
    return true;
  }

  // A frame the device sent on its own: one that carries the switch is logged at INFO, readings at the detail level.
  void logPushed(const Frame& f) {
    char dpKey[12];
    snprintf(dpKey, sizeof(dpKey), "\"%u\":", static_cast<unsigned>(switchDp_));
    const String text = payloadText(f);
    logLong(switchDp_ && text.indexOf(dpKey) >= 0 ? Logger::Level::Info : detail_,
            String("rx cmd=0x") + String(static_cast<unsigned>(f.cmd), HEX) + " seq=" + String(static_cast<unsigned long>(f.seq)) +
                " retcode=" + String(static_cast<unsigned long>(f.retcode)) + " payload=" + String(static_cast<unsigned>(f.len)) + "B (sent while idle)",
            text);
  }

  // Hands a status push to the push observer.
  void pushed(const Frame& f) {
    if (gPushObserver && f.cmd == kCmdStatus) gPushObserver(label_, target_, payloadText(f));
  }

  bool execute(Op op, uint16_t dp, uint32_t unixTime, const RefreshOpts* refresh, Result& r) {
    if (!tx_ || !rx_) return fail("protocol", "out of memory");
    deadline_ = millis() + timeoutMs_;
    char dpText[16] = "";
    if (op == Op::On || op == Op::Off) snprintf(dpText, sizeof(dpText), " dp=%u", static_cast<unsigned>(dp));
    logf(detail_, "%s %s:%u v%s%s timeout=%lu ms%s", opName(op), target_.ip.c_str(), kPort, versionName(target_.version), dpText,
         static_cast<unsigned long>(timeoutMs_), open_ ? " (kept connection)" : "");

    if (open_ && !drain()) {
      close();
      errCode_ = nullptr;  // not a failure of this operation: it connects again
      errDetail_ = "";
      r.reused = false;
    }
    if (!open_ && !connect()) return false;

    uint32_t t0 = millis();
    Frame f;
    if (op == Op::Query || op == Op::Refresh) {
      static const uint8_t kEmpty[2] = {'{', '}'};
      if (op == Op::Refresh) {
        if (!refresh) return fail("protocol", "refresh without options");
        if (refresh->compare) {  // diagnostics: what does the device say before UPDATEDPS?
          if (!sendPacket(kCmdDpQueryNew, kEmpty, sizeof(kEmpty), false, sessionKey_, true)) return false;
          if (!waitFor(kCmdDpQueryNew, f)) return false;
          r.before = payloadText(f);
        }
        String body = "{\"dpId\":[" + refresh->dpIds + "]}";
        r.request = body;
        const uint32_t tu = millis();
        if (!sendPacket(kCmdUpdateDps, reinterpret_cast<const uint8_t*>(body.c_str()), body.length(), false, sessionKey_, true)) return false;
        if (!listen(refresh->settleMs, kCmdUpdateDps, r.frames)) return false;
        r.updateMs = millis() - tu;
        t0 = millis();
      } else {
        r.request = "{}";
      }
      if (!sendPacket(kCmdDpQueryNew, kEmpty, sizeof(kEmpty), false, sessionKey_, true)) return false;
      if (!waitFor(kCmdDpQueryNew, f)) return false;
    } else {
      char body[112];
      int n = snprintf(body, sizeof(body), "{\"protocol\":5,\"t\":%lu,\"data\":{\"dps\":{\"%u\":%s}}}",
                       static_cast<unsigned long>(unixTime), static_cast<unsigned>(dp), op == Op::On ? "true" : "false");
      if (n <= 0 || n >= static_cast<int>(sizeof(body))) return fail("protocol", "command does not fit the buffer");
      r.request = body;
      if (!sendPacket(kCmdControlNew, reinterpret_cast<const uint8_t*>(body), n, true, sessionKey_, true)) return false;
      if (!waitFor(kCmdControlNew, f)) return false;
    }
    commandMs_ = millis() - t0;

    r.retcode = static_cast<int32_t>(f.retcode);
    r.json = payloadText(f);
    if (f.retcode != 0) {
      return fail("device", "the device answered with retcode " + String(static_cast<unsigned long>(f.retcode)) +
                                (r.json.isEmpty() ? String("") : String(": ") + r.json));
    }
    return true;
  }

  // ---- framing ---------------------------------------------------------------------

  // Builds and sends one command frame. `verHdr` prepends the 15-byte "3.x" header
  // that every command except the handshake and the status query carries.
  bool sendPacket(uint32_t cmd, const uint8_t* body, size_t bodyLen, bool verHdr, const uint8_t* key, bool textBody = false) {
    const size_t hdrLen = verHdr ? 15 : 0;
    const size_t plain = hdrLen + bodyLen;
    const uint32_t seq = nextSeq_++;
    size_t total = 0;

    if (v34()) {
      const size_t padded = (plain / 16 + 1) * 16;  // PKCS7: always at least one pad byte
      if (16 + padded + 36 > kMaxFrame) return fail("protocol", "payload too large");
      uint8_t* p = tx_ + 16;
      if (verHdr) {
        memcpy(p, "3.4", 3);
        memset(p + 3, 0, 12);
      }
      memcpy(p + hdrLen, body, bodyLen);
      memset(p + plain, static_cast<int>(padded - plain), padded - plain);
      aesEcb(key, true, p, padded);
      put32(tx_, kPrefix55AA);
      put32(tx_ + 4, seq);
      put32(tx_ + 8, cmd);
      put32(tx_ + 12, padded + 36);  // payload + HMAC + suffix
      hmac256(key, tx_, 16 + padded, tx_ + 16 + padded);
      put32(tx_ + 16 + padded + 32, kSuffix55AA);
      total = 16 + padded + 36;
    } else {
      if (18 + 12 + plain + 16 + 4 > kMaxFrame) return fail("protocol", "payload too large");
      put32(tx_, kPrefix6699);
      tx_[4] = 0;  // reserved
      tx_[5] = 0;
      put32(tx_ + 6, seq);
      put32(tx_ + 10, cmd);
      put32(tx_ + 14, 12 + plain + 16);  // IV + ciphertext + tag
      esp_fill_random(tx_ + 18, 12);     // a fresh IV for every packet
      uint8_t* p = tx_ + 30;
      if (verHdr) {
        memcpy(p, "3.5", 3);
        memset(p + 3, 0, 12);
      }
      memcpy(p + hdrLen, body, bodyLen);
      if (!gcmEncrypt(key, tx_ + 18, tx_ + 4, 14, p, plain, p, p + plain)) return fail("protocol", "GCM encrypt failed");
      put32(tx_ + 30 + plain + 16, kSuffix6699);
      total = 18 + 12 + plain + 16 + 4;
    }

    if (textBody) {  // a command: log what is sent (plaintext JSON, no key in it)
      String text;
      text.concat(reinterpret_cast<const char*>(body), bodyLen);
      logLong(detail_, String("tx cmd=0x") + String(static_cast<unsigned>(cmd), HEX) + " seq=" + String(static_cast<unsigned long>(seq)) +
                           " frame=" + String(static_cast<unsigned>(total)) + "B", text);
    } else {  // handshake (binary nonces) or heartbeat (empty): only the fact
      logf(Logger::Level::Debug, "tx cmd=0x%02x seq=%lu frame=%uB (%s)", static_cast<unsigned>(cmd), static_cast<unsigned long>(seq),
           static_cast<unsigned>(total), cmd == kCmdHeartBeat ? "heartbeat" : "handshake");
    }
    if (client_.write(tx_, total) != total) return fail("closed", "sending to the device failed");
    return true;
  }

  bool readExact(uint8_t* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
      int avail = client_.available();
      if (avail > 0) {
        int r = client_.read(dst + got, std::min(n - got, static_cast<size_t>(avail)));
        if (r > 0) {
          got += r;
          continue;
        }
      }
      if (!client_.connected() && client_.available() == 0) {
        return fail("closed", "the device closed the connection (wrong key or protocol version?)");
      }
      if (static_cast<int32_t>(deadline_ - millis()) <= 0) {
        return fail("timeout", "no answer from the device in time");
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
  }

  // Reads one frame, verifies its authenticity and decrypts it in place.
  bool recvFrame(const uint8_t* key, Frame& f) {
    if (!readExact(rx_, 4)) return false;
    // Skip garbage until a frame prefix shows up.
    while (get32(rx_) != kPrefix55AA && get32(rx_) != kPrefix6699) {
      memmove(rx_, rx_ + 1, 3);
      if (!readExact(rx_ + 3, 1)) return false;
    }

    if (get32(rx_) == kPrefix55AA) {
      if (!readExact(rx_ + 4, 12)) return false;
      const uint32_t len = get32(rx_ + 12);  // retcode + payload + HMAC + suffix
      if (len < 40 || 16 + len > kMaxFrame) return fail("protocol", "frame length out of range: " + String(static_cast<unsigned long>(len)));
      if (!readExact(rx_ + 16, len)) return false;
      if (get32(rx_ + 16 + len - 4) != kSuffix55AA) return fail("protocol", "bad frame suffix");
      uint8_t mac[32];
      hmac256(key, rx_, 16 + len - 36, mac);
      if (!sameBytes(mac, rx_ + 16 + len - 36, 32)) return fail("auth", "frame HMAC mismatch (wrong local key?)");
      f.seq = get32(rx_ + 4);
      f.cmd = get32(rx_ + 8);
      f.retcode = get32(rx_ + 16);
      f.data = rx_ + 20;
      f.len = len - 40;
      if (f.len % 16 != 0) return fail("protocol", "encrypted payload is not a multiple of 16 bytes");
      if (f.len) aesEcb(key, false, f.data, f.len);
    } else {
      if (!readExact(rx_ + 4, 14)) return false;
      const uint32_t len = get32(rx_ + 14);  // IV + ciphertext + tag
      if (len < 12 + 4 + 16 || 18 + len + 4 > kMaxFrame) return fail("protocol", "frame length out of range: " + String(static_cast<unsigned long>(len)));
      if (!readExact(rx_ + 18, len + 4)) return false;
      if (get32(rx_ + 18 + len) != kSuffix6699) return fail("protocol", "bad frame suffix");
      const size_t ctLen = len - 28;
      uint8_t* ct = rx_ + 30;
      if (!gcmDecrypt(key, rx_ + 18, rx_ + 4, 14, ct, ctLen, rx_ + 18 + len - 16, ct)) {
        return fail("auth", "frame authentication tag mismatch (wrong local key?)");
      }
      f.seq = get32(rx_ + 6);
      f.cmd = get32(rx_ + 10);
      if (ctLen < 4) return fail("protocol", "payload shorter than the retcode");
      f.retcode = get32(ct);
      f.data = ct + 4;
      f.len = ctLen - 4;
    }
    return true;
  }

  // Reads frames with the session key until one with command `cmd` arrives; other
  // frames (heartbeats, status pushes the device sends on its own) are skipped.
  bool waitFor(uint32_t cmd, Frame& f) {
    for (;;) {
      if (!recvFrame(sessionKey_, f)) return false;
      const bool wanted = f.cmd == cmd;
      logLong(detail_, String("rx cmd=0x") + String(static_cast<unsigned>(f.cmd), HEX) + " seq=" + String(static_cast<unsigned long>(f.seq)) +
                           " retcode=" + String(static_cast<unsigned long>(f.retcode)) + (wanted ? "" : " (not the answer, skipped)") +
                           " payload=" + String(static_cast<unsigned>(f.len)) + "B",
              payloadText(f));
      if (wanted) return true;
      pushed(f);
    }
  }

  // Listens for up to `ms` and logs/collects every frame the device sends (any status push and the answer to
  // UPDATEDPS), stopping early once the answer (command `lastCmd`) has arrived: the devices tried so far send the
  // fresh values first and the answer after them. Running into the end of the wait is normal; a real error
  // (connection closed, bad frame) is not.
  bool listen(uint32_t ms, uint32_t lastCmd, String& collected) {
    const uint32_t savedDeadline = deadline_;
    const uint32_t until = millis() + ms;
    if (static_cast<int32_t>(until - deadline_) < 0) deadline_ = until;
    bool ok = true;
    for (;;) {
      if (static_cast<int32_t>(deadline_ - millis()) <= 0) break;
      Frame f;
      if (!recvFrame(sessionKey_, f)) {
        if (errCode_ && strcmp(errCode_, "timeout") == 0) {  // just the end of the wait
          errCode_ = nullptr;
          errDetail_ = "";
        } else {
          ok = false;
        }
        break;
      }
      const String text = payloadText(f);
      logLong(detail_, String("rx cmd=0x") + String(static_cast<unsigned>(f.cmd), HEX) + " seq=" + String(static_cast<unsigned long>(f.seq)) +
                           " retcode=" + String(static_cast<unsigned long>(f.retcode)) + " payload=" + String(static_cast<unsigned>(f.len)) + "B (after UPDATEDPS)",
              text);
      if (!collected.isEmpty()) collected += '\n';
      collected += "cmd=0x" + String(static_cast<unsigned>(f.cmd), HEX) + " retcode=" + String(static_cast<unsigned long>(f.retcode)) + " " + text;
      if (f.cmd == lastCmd) break;
    }
    deadline_ = savedDeadline;
    return ok;
  }

  // Decrypted payload as text: PKCS7 padding (3.4) and the "3.x" header removed.
  String payloadText(const Frame& f) const {
    const uint8_t* p = f.data;
    size_t n = f.len;
    if (v34() && n > 0) {
      const uint8_t pad = p[n - 1];
      if (pad >= 1 && pad <= 16 && pad <= n) n -= pad;
    }
    if (n >= 15 && p[0] == '3' && p[1] == '.' && (p[2] == '4' || p[2] == '5')) {
      p += 15;
      n -= 15;
    }
    String s;
    if (n) s.concat(reinterpret_cast<const char*>(p), n);
    return s;
  }

  // ---- session key negotiation ----------------------------------------------------------

  bool handshake() {
    uint8_t localNonce[16];
    esp_fill_random(localNonce, sizeof(localNonce));

    if (!sendPacket(kCmdSessStart, localNonce, sizeof(localNonce), false, realKey_)) return false;
    Frame f;
    if (!recvFrame(realKey_, f)) {
      if (errCode_ && strcmp(errCode_, "timeout") == 0) {
        errCode_ = "handshake";
        errDetail_ = "the device did not answer the handshake (wrong protocol version?)";
      }
      return false;
    }
    logf(Logger::Level::Debug, "rx cmd=0x%02x seq=%lu retcode=%lu (handshake answer, %uB)", static_cast<unsigned>(f.cmd),
         static_cast<unsigned long>(f.seq), static_cast<unsigned long>(f.retcode), static_cast<unsigned>(f.len));
    if (f.cmd != kCmdSessResp) return fail("handshake", "unexpected answer to the handshake, command " + String(static_cast<unsigned long>(f.cmd)));
    if (f.len < 48) return fail("handshake", "handshake answer is too short");

    uint8_t mac[32];
    hmac256(realKey_, localNonce, sizeof(localNonce), mac);
    if (!sameBytes(mac, f.data + 16, 32)) return fail("auth", "handshake HMAC mismatch (wrong local key?)");

    uint8_t deviceNonce[16];
    memcpy(deviceNonce, f.data, 16);

    hmac256(realKey_, deviceNonce, sizeof(deviceNonce), mac);
    if (!sendPacket(kCmdSessFinish, mac, sizeof(mac), false, realKey_)) return false;

    uint8_t mix[16];
    for (int i = 0; i < 16; ++i) mix[i] = localNonce[i] ^ deviceNonce[i];
    if (v34()) {
      memcpy(sessionKey_, mix, 16);
      aesEcb(realKey_, true, sessionKey_, 16);
    } else {
      uint8_t tag[16];  // not used: the session key is the ciphertext part only
      if (!gcmEncrypt(realKey_, localNonce, nullptr, 0, mix, 16, sessionKey_, tag)) return fail("handshake", "session key derivation failed");
    }
    return true;
  }

  const Target target_;
  uint32_t timeoutMs_ = 0;      // of the current operation
  String label_;
  Logger::Level detail_ = Logger::Level::Info;  // INFO for user actions, DEBUG for the periodic poll
  WiFiClient client_;
  bool open_ = false;           // connected and the session key negotiated
  uint16_t switchDp_ = 0;       // the device's switch data point, 0 = unknown
  uint32_t lastUseMs_ = 0;
  uint8_t* tx_ = nullptr;
  uint8_t* rx_ = nullptr;
  uint8_t realKey_[16] = {0};
  uint8_t sessionKey_[16] = {0};
  uint32_t nextSeq_ = 1;
  uint32_t deadline_ = 0;
  uint32_t connectMs_ = 0;
  uint32_t handshakeMs_ = 0;
  uint32_t commandMs_ = 0;
  const char* errCode_ = nullptr;
  String errDetail_;
};

// ---- workers -------------------------------------------------------------------------

// Two lanes, each with its own FIFO list and its own worker task: the commands (buttons, API, scenarios) and the
// periodic polls of the device monitor. They run side by side, so a dead device in the poll cannot hold up a command
// and a burst of commands cannot starve the poll. What must never happen is two operations on the SAME device at
// once: a device accepts few local connections, and a poll that connected before a command could report the old
// state after it. So every operation takes the lock of the device's IP. A command waits for it (a poll holds it for
// one operation only); a poll does not wait: it ends as "busy" and the monitor leaves the device alone this cycle.
constexpr size_t kCommandLane = 0, kMonitorLane = 1;
constexpr size_t kCapacity[2] = {kCommandQueueSize, kMonitorQueueSize};
constexpr uint32_t kLockWaitMs = 5000;  // a command waits at most this long for the device (a poll takes <= timeoutMs)

struct LaneState {
  std::deque<std::shared_ptr<Job>> jobs;
  SemaphoreHandle_t ready = nullptr;  // counts the jobs of the lane, its worker sleeps on it
};
LaneState gLanes[2];
SemaphoreHandle_t gListMux = nullptr;              // guards the lists, the statistics, the locks map and the counters
std::map<String, SemaphoreHandle_t> gDeviceLocks;  // by IP, created on first use, never removed
std::map<String, uint16_t> gCommandsPending;       // by IP: command jobs queued or running
std::map<String, uint32_t> gConnections;           // by IP: TCP connections opened since boot
std::map<String, bool> gConnectOk;                 // by IP: whether the last TCP connect got through (see kConnectMaxMs)
// Kept connections by IP. The map is guarded by gListMux; a Session in it is used only by whoever holds its IP's lock.
std::map<String, Session*> gKept;
// By IP: whether the kept connection was open when it was last put back (guarded by gListMux). kept() reads this, not
// gKept, because a Session in use (by a poll, a command or the heartbeat task) is out of gKept for that time.
std::map<String, bool> gKeptOpen;
void (*gObserver)(const Job&) = nullptr;
Stats gStats;  // guarded by gListMux

SemaphoreHandle_t deviceLock(const String& ip) {
  xSemaphoreTake(gListMux, portMAX_DELAY);
  SemaphoreHandle_t& lock = gDeviceLocks[ip];
  if (!lock) lock = xSemaphoreCreateMutex();
  SemaphoreHandle_t out = lock;
  xSemaphoreGive(gListMux);
  return out;
}

void noteConnection(const String& ip) {
  if (!gListMux) return;  // a direct call before begin()
  xSemaphoreTake(gListMux, portMAX_DELAY);
  ++gConnections[ip];
  xSemaphoreGive(gListMux);
}

bool lastConnectOk(const String& ip) {
  if (!gListMux) return false;
  xSemaphoreTake(gListMux, portMAX_DELAY);
  auto it = gConnectOk.find(ip);
  const bool ok = it != gConnectOk.end() && it->second;
  xSemaphoreGive(gListMux);
  return ok;
}

void noteConnect(const String& ip, bool ok) {
  if (!gListMux) return;
  xSemaphoreTake(gListMux, portMAX_DELAY);
  gConnectOk[ip] = ok;
  xSemaphoreGive(gListMux);
}

// Takes the kept Session of `ip` out of the map (nullptr if none). The caller holds the IP's lock.
Session* takeKept(const String& ip) {
  xSemaphoreTake(gListMux, portMAX_DELAY);
  Session* s = nullptr;
  auto it = gKept.find(ip);
  if (it != gKept.end()) {
    s = it->second;
    gKept.erase(it);
  }
  xSemaphoreGive(gListMux);
  return s;
}

void putKept(const String& ip, Session* s) {
  xSemaphoreTake(gListMux, portMAX_DELAY);
  gKept[ip] = s;
  gKeptOpen[ip] = s->isOpen();
  xSemaphoreGive(gListMux);
}

// Closes and frees a Session taken out with takeKept() for good. The caller holds the IP's lock.
void dropKept(const String& ip, Session* s) {
  delete s;
  xSemaphoreTake(gListMux, portMAX_DELAY);
  gKeptOpen.erase(ip);
  xSemaphoreGive(gListMux);
}

// Runs a job of a device with Target::keep over its kept connection (opened on first use, or again after it was lost
// or the device's key or version changed). The caller holds the IP's lock.
Result runKept(const Job& job, const char* label) {
  Session* s = takeKept(job.target.ip);
  if (s && (s->target().key != job.target.key || s->target().version != job.target.version)) {
    delete s;
    s = nullptr;
  }
  if (!s) s = new Session(job.target, label);
  s->setLabel(label);
  Result r;
  if (job.op == Op::Query) {
    r = s->run(Op::Query, job.dp, 0, job.timeoutMs, job.background);
  } else if (job.op == Op::Refresh) {
    r = s->run(Op::Refresh, job.dp, 0, job.timeoutMs, job.background, &job.refresh);
  } else {
    const uint32_t t = job.unixTime >= 0 ? static_cast<uint32_t>(job.unixTime) : static_cast<uint32_t>(time(nullptr));
    r = s->run(job.op, job.dp, t, job.timeoutMs, job.background);
  }
  putKept(job.target.ip, s);  // also when it failed: closed then, it connects again next time
  return r;
}

// The heartbeat task: reads the status pushes of every kept connection and sends a heartbeat on every kept
// connection idle for kHeartbeatIdleMs. A device whose lock is taken is skipped (its connection is in use right now); a
// connection that does not answer (or that the device closed) is dropped, and the next poll or command connects again.
// It never opens a connection itself.
void heartbeats() {
  std::vector<String> due;
  xSemaphoreTake(gListMux, portMAX_DELAY);
  for (const auto& k : gKept) {
    due.push_back(k.first);  // readPushes() is cheap when nothing came
  }
  xSemaphoreGive(gListMux);
  for (const String& ip : due) {
    SemaphoreHandle_t lock = deviceLock(ip);
    if (xSemaphoreTake(lock, 0) != pdTRUE) continue;
    if (Session* s = takeKept(ip)) {
      bool alive = s->readPushes();
      if (alive && s->idleMs() >= kHeartbeatIdleMs) alive = s->heartbeat();
      if (alive) {
        putKept(ip, s);
      } else {
        dropKept(ip, s);
      }
    }
    xSemaphoreGive(lock);
  }
}

std::atomic<uint32_t> gHbStackFreeMin{UINT32_MAX};

void heartbeatTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kHeartbeatWakeMs));
    if (WiFi.status() == WL_CONNECTED) heartbeats();
    const uint32_t free = uxTaskGetStackHighWaterMark(nullptr);  // bytes on the ESP32 (StackType_t is a byte)
    if (free < gHbStackFreeMin) gHbStackFreeMin = free;
  }
}

std::shared_ptr<Job> nextJob(size_t lane) {
  xSemaphoreTake(gLanes[lane].ready, portMAX_DELAY);
  xSemaphoreTake(gListMux, portMAX_DELAY);
  std::shared_ptr<Job> job;
  if (!gLanes[lane].jobs.empty()) {
    job = gLanes[lane].jobs.front();
    gLanes[lane].jobs.pop_front();
  }
  xSemaphoreGive(gListMux);
  return job;
}

void worker(void* arg) {
  const size_t lane = reinterpret_cast<size_t>(arg);
  for (;;) {
    std::shared_ptr<Job> job = nextJob(lane);
    if (!job) continue;

    const char* label = job->device.c_str();
    SemaphoreHandle_t lock = deviceLock(job->target.ip);
    // A submitter that gave up (a scenario step that waited too long) must not have its command arrive later.
    bool cancelled = job->cancelled.load();
    // A poll does not wait for a device held by another operation, except for a heartbeat on a kept connection (it holds
    // the device for at most kHeartbeatTimeoutMs): without that wait, polls of kept devices would often end "busy".
    const TickType_t wait = lane != kMonitorLane ? pdMS_TO_TICKS(kLockWaitMs)
                            : job->target.keep  ? pdMS_TO_TICKS(kHeartbeatTimeoutMs + 500)
                                                : 0;
    bool locked = !cancelled && xSemaphoreTake(lock, wait) == pdTRUE;
    if (locked && job->cancelled.load()) {  // given up while this job waited for the device
      xSemaphoreGive(lock);
      locked = false;
      cancelled = true;
    }
    const uint32_t queuedMs = millis() - job->submittedMs;  // submit() until the device was ours
    {
      xSemaphoreTake(gListMux, portMAX_DELAY);
      ++gStats.jobs[lane];
      gStats.waitSumMs[lane] += queuedMs;
      if (queuedMs > gStats.waitMaxMs[lane]) gStats.waitMaxMs[lane] = queuedMs;
      if (queuedMs > 1000) ++gStats.waitOver1s[lane];
      if (!locked && !cancelled) ++gStats.busy[lane];
      xSemaphoreGive(gListMux);
    }

    // A device that has just lost keep_connected: its kept connection is closed before a new one is opened.
    if (locked && !job->target.keep) {
      if (Session* s = takeKept(job->target.ip)) dropKept(job->target.ip, s);
    }
    if (cancelled) {
      Result c;
      c.error = "cancelled";
      c.detail = "the request was withdrawn before it started";
      job->result = c;
    } else if (!locked) {
      Result busy;
      busy.error = "busy";
      busy.detail = "another request to this device was still running";
      job->result = busy;
    } else if (job->target.keep) {
      job->result = runKept(*job, label);
    } else if (job->op == Op::Query) {
      job->result = query(job->target, job->timeoutMs, label, job->background);
    } else if (job->op == Op::Refresh) {
      job->result = refresh(job->target, job->refresh, job->timeoutMs, label, job->background);
    } else {
      const uint32_t t = job->unixTime >= 0 ? static_cast<uint32_t>(job->unixTime) : static_cast<uint32_t>(time(nullptr));
      job->result = setSwitch(job->target, job->dp, job->op == Op::On, t, job->timeoutMs, label, job->background);
    }
    job->result.queuedMs = queuedMs;
    const Result& r = job->result;
    if (r.ok) {
      Logger::log(job->background ? Logger::Level::Debug : Logger::Level::Info, TAG,
                  "[%s] %s: OK in %lu ms (connect %lu, handshake %lu, command %lu; queued %lu)", label, opName(job->op),
                  static_cast<unsigned long>(r.ms), static_cast<unsigned long>(r.connectMs),
                  static_cast<unsigned long>(r.handshakeMs), static_cast<unsigned long>(r.commandMs),
                  static_cast<unsigned long>(queuedMs));
    } else {
      // A failed poll is not logged here: the device monitor reports the offline/online changes.
      Logger::log(job->background ? Logger::Level::Debug : Logger::Level::Warn, TAG,
                  "[%s] %s: FAILED (%s) after %lu ms (connect %lu, handshake %lu; queued %lu): %s", label, opName(job->op), r.error.c_str(),
                  static_cast<unsigned long>(r.ms), static_cast<unsigned long>(r.connectMs),
                  static_cast<unsigned long>(r.handshakeMs), static_cast<unsigned long>(queuedMs), r.detail.c_str());
    }
    // The observer runs while the device is still ours, so the cached states of two operations on one device
    // are written in the order the operations ran.
    if (gObserver) gObserver(*job);
    if (locked) xSemaphoreGive(lock);
    if (!job->background) {
      xSemaphoreTake(gListMux, portMAX_DELAY);
      auto it = gCommandsPending.find(job->target.ip);
      if (it != gCommandsPending.end() && it->second > 0 && --it->second == 0) gCommandsPending.erase(it);
      xSemaphoreGive(gListMux);
    }
    job->done.store(true, std::memory_order_release);
  }
}

}  // namespace

Result query(const Target& target, uint32_t timeoutMs, const char* label, bool background) {
  Session s(target, label);
  return s.run(Op::Query, 0, 0, timeoutMs, background);
}

Result setSwitch(const Target& target, uint16_t dp, bool on, uint32_t unixTime, uint32_t timeoutMs, const char* label,
                 bool background) {
  Session s(target, label);
  return s.run(on ? Op::On : Op::Off, dp, unixTime, timeoutMs, background);
}

Result refresh(const Target& target, const RefreshOpts& opts, uint32_t timeoutMs, const char* label, bool background) {
  Session s(target, label);
  return s.run(Op::Refresh, 0, 0, timeoutMs, background, &opts);
}

const char* versionName(Version v) { return v == Version::V34 ? "3.4" : "3.5"; }

const char* opName(Op op) {
  switch (op) {
    case Op::On: return "on";
    case Op::Off: return "off";
    case Op::Refresh: return "refresh";
    default: return "status";
  }
}

void begin() {
  if (gListMux) return;
  gListMux = xSemaphoreCreateMutex();
  for (size_t i = 0; i < 2; ++i) gLanes[i].ready = xSemaphoreCreateCounting(kCapacity[i], 0);
  xTaskCreate(worker, "tuya_cmd", 10240, reinterpret_cast<void*>(kCommandLane), 1, nullptr);
  xTaskCreate(worker, "tuya_mon", 10240, reinterpret_cast<void*>(kMonitorLane), 1, nullptr);
  xTaskCreate(heartbeatTask, "tuya_hb", 4096, nullptr, 1, nullptr);
}

void setObserver(void (*observer)(const Job&)) { gObserver = observer; }

void setPushObserver(void (*observer)(const String& device, const Target& target, const String& json)) { gPushObserver = observer; }

bool kept(const String& ip) {
  if (!gListMux) return false;
  xSemaphoreTake(gListMux, portMAX_DELAY);
  auto it = gKeptOpen.find(ip);
  const bool open = it != gKeptOpen.end() && it->second;
  xSemaphoreGive(gListMux);
  return open;
}

uint32_t heartbeatStackFreeMin() { return gHbStackFreeMin == UINT32_MAX ? 0 : gHbStackFreeMin.load(); }

uint32_t connections(const String& ip) {
  if (!gListMux) return 0;
  xSemaphoreTake(gListMux, portMAX_DELAY);
  auto it = gConnections.find(ip);
  const uint32_t n = it == gConnections.end() ? 0 : it->second;
  xSemaphoreGive(gListMux);
  return n;
}

void keepOnly(const std::vector<String>& ips) {
  if (!gListMux) return;
  std::vector<String> drop;
  xSemaphoreTake(gListMux, portMAX_DELAY);
  for (const auto& k : gKept) {
    if (std::find(ips.begin(), ips.end(), k.first) == ips.end()) drop.push_back(k.first);
  }
  xSemaphoreGive(gListMux);
  for (const String& ip : drop) {
    SemaphoreHandle_t lock = deviceLock(ip);
    if (xSemaphoreTake(lock, 0) != pdTRUE) continue;  // in use: next time
    if (Session* s = takeKept(ip)) dropKept(ip, s);
    LOGI(TAG, "%s: kept connection closed (the device is no longer kept connected or polled)", ip.c_str());
    xSemaphoreGive(lock);
  }
}

Stats stats() {
  if (!gListMux) return Stats();
  xSemaphoreTake(gListMux, portMAX_DELAY);
  const Stats s = gStats;
  xSemaphoreGive(gListMux);
  return s;
}

bool commandPending(const String& ip) {
  if (!gListMux) return false;
  xSemaphoreTake(gListMux, portMAX_DELAY);
  auto it = gCommandsPending.find(ip);
  const bool pending = it != gCommandsPending.end() && it->second > 0;
  xSemaphoreGive(gListMux);
  return pending;
}

std::shared_ptr<Job> submit(Op op, const String& deviceName, const Target& target, uint16_t dp, uint32_t timeoutMs,
                            bool background, int64_t unixTime, const RefreshOpts* refresh) {
  if (!gListMux) return nullptr;
  auto job = std::make_shared<Job>();
  job->op = op;
  job->device = deviceName;
  job->target = target;
  job->dp = dp;
  job->timeoutMs = timeoutMs;
  job->background = background;
  job->unixTime = unixTime;
  job->submittedMs = millis();
  if (refresh) job->refresh = *refresh;
  const size_t lane = background ? kMonitorLane : kCommandLane;
  xSemaphoreTake(gListMux, portMAX_DELAY);
  const bool room = gLanes[lane].jobs.size() < kCapacity[lane];
  if (room) {
    gLanes[lane].jobs.push_back(job);
    if (!background) ++gCommandsPending[target.ip];
  }
  xSemaphoreGive(gListMux);
  if (!room) return nullptr;
  xSemaphoreGive(gLanes[lane].ready);
  return job;
}

}  // namespace Tuya
