#pragma once
//
// Tuya local protocol client (LAN, TCP port 6668), versions 3.4 and 3.5.
//
// Written from the protocol description in TinyTuya (PROTOCOL.md), not copied from
// the lynk-tuya-esp-local library. One operation = connect, session-key handshake,
// one command, close. Every received frame is authenticated (HMAC for 3.4, GCM tag
// for 3.5) before it is used, and the retcode of the device is checked.
//
// A device with Target::keep (keep_connected in devices.json) instead gets one connection that stays open: every
// operation of both lanes goes over it, and a task of its own ("tuya_hb", every 0.5 s) sends a heartbeat on it when it
// was idle for kHeartbeatIdleMs and waits kHeartbeatTimeoutMs for the answer (none: the connection is closed; only polls
// and commands connect). Some sockets misbehave under a new connection and handshake every few seconds (the boiler socket
// switched itself off while it was polled that way, and never while it was not polled).
// What a kept device sends on its own (status pushes: the data points that changed) is read by that task as soon as it
// arrives (within kHeartbeatWakeMs) and handed to the push observer (setPushObserver).
//
// Blocking: an operation takes up to `timeoutMs` plus the connect time (at most kConnectMaxMs). Never call
// query()/setSwitch() from the async web task or from loop(): use submit(), which runs them in
// two worker tasks: "tuya_cmd" (commands: buttons, API, scenarios) and "tuya_mon" (the monitor's polls).
// The lanes work side by side, but never on the same device (IP) at once: see submit().
//
// The local key is never written to the log. Command and answer payloads (data points only) are.
//
#include <Arduino.h>

#include <atomic>
#include <memory>
#include <vector>

#include "logger.h"

namespace Tuya {

enum class Version : uint8_t { V34, V35 };

struct Target {
  String ip;                      // dotted IPv4
  String key;                     // the 16-character local key
  Version version = Version::V34;
  bool keep = false;              // keep one connection open between operations (see above)
};

struct Result {
  bool ok = false;
  String error;                   // short code when !ok: connect, timeout, closed, auth, handshake, protocol, device, busy
  String detail;                  // human readable explanation when !ok
  int32_t retcode = -1;           // retcode of the device answer, -1 if none
  String request;                 // the command JSON that was sent ("{}" for a status query)
  String json;                    // decrypted JSON payload of the answer ("" for a bare ACK)
  uint32_t ms = 0;                // how long the operation took
  uint32_t connectMs = 0;         // TCP connect
  uint32_t handshakeMs = 0;       // session key negotiation
  uint32_t commandMs = 0;         // command sent until its answer arrived (for a refresh: the final status query)
  uint32_t queuedMs = 0;          // queued jobs: submit() until the worker started it (0 for a direct call)
  bool reused = false;            // went over a kept connection that was open before (no connect, no handshake)
  bool retried = false;           // that kept connection did not answer and the operation was tried once more on a new one
  uint32_t updateMs = 0;          // refresh only: UPDATEDPS sent until the settle wait ended
  String before;                  // refresh with compare: the status answer BEFORE UPDATEDPS ("" otherwise)
  String frames;                  // refresh: every other frame the device sent (cmd, retcode, payload), one per line
};

// Refresh = UPDATEDPS (0x12) + wait + status query, in one session. UPDATEDPS asks the device to re-read the
// listed data points (power meters keep a cached value that the plain status query returns as it is).
struct RefreshOpts {
  String dpIds;                   // "18,19,20": the data points to re-read (decimal, comma separated)
  uint32_t settleMs = 500;        // how long to listen for the device's own answer/push after UPDATEDPS
  bool compare = false;           // diagnostics: also query once BEFORE UPDATEDPS, to see what changed
};

// `label` (the device name) prefixes every log line. Requests and answers are logged in full
// (payload JSON included, never the key): at INFO for user actions, at DEBUG when `background`
// (the periodic poll), so polling does not flood the log. Failures of user actions are WARN.

// Status query (DP_QUERY_NEW 0x10): `json` holds the data points, e.g. {"dps":{"1":true,...}}.
Result query(const Target& target, uint32_t timeoutMs, const char* label = "", bool background = false);

// Switch one boolean data point (CONTROL_NEW 0x0d). `unixTime` goes into the "t" field.
Result setSwitch(const Target& target, uint16_t dp, bool on, uint32_t unixTime, uint32_t timeoutMs,
                 const char* label = "", bool background = false);

// UPDATEDPS (0x12, body {"dpId":[...]}, no version header) then a status query; see RefreshOpts.
Result refresh(const Target& target, const RefreshOpts& opts, uint32_t timeoutMs, const char* label = "", bool background = false);

const char* versionName(Version v);   // "3.4" | "3.5"

// ---- background execution -------------------------------------------------

enum class Op : uint8_t { Query, On, Off, Refresh };
const char* opName(Op op);            // "status" | "on" | "off" | "refresh"

struct Job {
  Op op = Op::Query;
  String device;                      // name, for the answer
  Target target;
  uint16_t dp = 1;
  uint32_t timeoutMs = 8000;
  int64_t unixTime = -1;              // "t" of a control command; -1 = the current time (diagnostics may set another)
  bool background = false;            // periodic poll: quieter logging
  RefreshOpts refresh;                // Op::Refresh only
  uint32_t submittedMs = 0;           // millis() at submit(), to tell how long the job waited
  std::atomic<bool> cancelled{false}; // the submitter gave up: a job that has not started yet is dropped (error "cancelled")
  std::atomic<bool> done{false};      // set last, after `result` is complete
  Result result;
};

// The longest TCP connect an operation waits for: to a device whose last connect got through (1.5 s to any other). lwIP
// resends a lost SYN after 3 s, so this rides out one lost packet on a weak Wi-Fi.
constexpr uint32_t kConnectMaxMs = 3500;

// Places in the two lanes' lists (see submit()).
constexpr size_t kCommandQueueSize = 16, kMonitorQueueSize = 4;

// How long jobs waited (queue plus the device lock) since boot, per lane: [0] commands (buttons, API, scenarios),
// [1] the monitor's polls. Meant to show whether a lane is ever a bottleneck.
struct Stats {
  uint32_t jobs[2] = {0, 0};          // jobs started
  uint32_t waitSumMs[2] = {0, 0};     // sum of the waits (average = sum / jobs)
  uint32_t waitMaxMs[2] = {0, 0};
  uint32_t waitOver1s[2] = {0, 0};    // jobs that waited longer than 1 s
  uint32_t busy[2] = {0, 0};          // jobs that ended "busy": the device was held by another operation
};
Stats stats();

// True while a command (not a poll) for this device IP is queued or running. The monitor leaves such a device alone.
bool commandPending(const String& ip);

// TCP connections opened to this IP since boot (by both lanes, kept or not).
uint32_t connections(const String& ip);

// Closes the kept connections of every IP not in `ips` (the monitor calls it every cycle with the polled devices that
// have keep_connected). A connection in use right now is closed at the next call.
void keepOnly(const std::vector<String>& ips);

// A kept connection idle for this long gets a heartbeat (HEART_BEAT 0x09, empty body), like TinyTuya and localtuya do
// (they send one every 10-12 s); its answer is awaited this long. An operation that gets no answer (or "closed") over a
// kept connection is tried once more on a new one, so a connection that died between two heartbeats costs one timeout.
constexpr uint32_t kHeartbeatIdleMs = 10000;
constexpr uint32_t kHeartbeatTimeoutMs = 2000;
// The heartbeat task wakes this often; it also reads the status pushes of the kept connections then.
constexpr uint32_t kHeartbeatWakeMs = 500;

// True while the device at `ip` has a kept connection that is open (it was not lost or closed since its last operation).
bool kept(const String& ip);

// The lowest free stack of the heartbeat task since boot, in bytes (it also runs the push observer).
uint32_t heartbeatStackFreeMin();

// Starts the two worker tasks. Call once from setup().
void begin();

// Called in the worker task for every finished job, before `done` is set. Keep it short. Jobs that ended "busy" or
// "cancelled" never touched the device.
void setObserver(void (*observer)(const Job&));

// Called for every status push (STATUS 0x08) a device sends on its own (mostly over a kept connection, but also one that
// comes before the answer of an operation): `device` is the name its last
// operation was submitted with, `json` the decrypted payload ({"protocol":4,"t":..,"data":{"dps":{..}}} or {"dps":{..}}).
// Runs in the task that holds the device's lock (the heartbeat task, or a worker that found it before an answer). Keep it short.
void setPushObserver(void (*observer)(const String& device, const Target& target, const String& json));

// Queues a job. background=false goes to the command lane (FIFO, 16 places), background=true to the monitor lane
// (FIFO, 4 places); each lane runs one job at a time. A job first takes the lock of its device's IP: a command waits
// up to 5 s for it, a poll does not wait and ends with error "busy" (the device was not touched). A running job is
// never interrupted. Returns nullptr if the lane's list is full.
std::shared_ptr<Job> submit(Op op, const String& deviceName, const Target& target, uint16_t dp,
                            uint32_t timeoutMs, bool background = false, int64_t unixTime = -1,
                            const RefreshOpts* refresh = nullptr);

}  // namespace Tuya
