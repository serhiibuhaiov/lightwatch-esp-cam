#pragma once
//
// The scenario engine: every run of every scenario as a small state machine, all driven by one task (scenario.cpp).
// A run that waits (for its start, for Wi-Fi, in a pause between steps, between attempts) is a deadline here, not a
// sleeping task, so a run costs a few hundred bytes instead of a 6 KB stack and many can be alive at once.
//
// Plain C++, no Arduino: the board's side (clock, Wi-Fi, devices, Tuya jobs, the log) comes through Env, so the host
// tests drive it with a fake clock (pio test -e native, tests/native/test_scenario_engine).
//
// A step behaves as before the engine: the command (on/off) is always sent, then a status query must show the new
// state; up to 3 attempts 2 s apart, or, in a run started by the power-up, attempts 5 s apart for 90 s after waiting
// for Wi-Fi (+10 s for the sockets). A missing device fails the step at once, an ignored one skips it. The pause after
// a step is kept only after a step that switched something. What is new: at most kMaxInFlight jobs of the engine are in
// the Tuya command list at once (the others wait here, first come first served), and a stop acts at once unless a
// command is already on its way (then right after it).
//
// An HTTP step sends one request (GET or POST with a static body) through the HTTP lane: it succeeds when the server
// answers 2xx, with the same attempts, gaps and patience as a socket step and no status query after it; at most
// kMaxHttpInFlight of them are in that lane at once. The pause after it is kept when it succeeded.
//
// A wait step waits its seconds, always (also after a step that failed or was skipped), after the pause of the step
// before it if that one is due. A stop ends it at once. A run continued after a restart in the middle of a wait starts
// that wait over (the step it was in is done again, like any step).
//
// Hours (lib/time_window, local time from Env::localMinute()): a step with `hours` is looked at once, when it begins (after
// the pause of the step before it): outside them it is skipped (no command, no pause after it) and the run goes on; begun
// inside, its attempts go on past the end. A scenario with `hours` starts a run (any trigger but "manual") only inside
// them, looked at when the run's start is due (for a power-up: after Wi-Fi and the settle time); outside them the run ends
// as skipped. Without a clock (no RTC: after a power cut the time comes with NTP) both wait for it up to clock_wait_min,
// then skip ("no clock").
//
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "time_window.h"

namespace ScenarioEngine {

// ---- where the runs live ---------------------------------------------------------------------------------------------
// The steps, the device names and the reasons of the runs (and of the board's scenario list) are allocated through
// Alloc. By default that is malloc(); the board points it at PSRAM (setAllocator): 24 runs x 24 steps would otherwise take
// ~30 KB of the 8-bit internal RAM that the network and the SD card driver need, and when that runs out, writing to the
// card fails. The function must return memory that free() releases, nullptr when there is none.
using AllocFn = void* (*)(size_t bytes);
void setAllocator(AllocFn fn);
void* allocBytes(size_t bytes);  // through the function set; out of memory: std::bad_alloc (abort without exceptions)

template <class T>
struct Alloc {
  using value_type = T;
  Alloc() noexcept = default;
  template <class U>
  Alloc(const Alloc<U>&) noexcept {}
  T* allocate(size_t n) { return static_cast<T*>(allocBytes(n * sizeof(T))); }
  void deallocate(T* p, size_t) noexcept { std::free(p); }
  template <class U>
  bool operator==(const Alloc<U>&) const noexcept { return true; }
  template <class U>
  bool operator!=(const Alloc<U>&) const noexcept { return false; }
};
using Text = std::basic_string<char, std::char_traits<char>, Alloc<char>>;
template <class T>
using List = std::vector<T, Alloc<T>>;
inline std::string str(const Text& t) { return std::string(t.data(), t.size()); }

constexpr uint8_t kMaxInFlight = 4;           // engine jobs queued or running in the Tuya command lane at once
constexpr uint8_t kMaxHttpInFlight = 2;       // engine requests in the HTTP lane at once (it has 4 places; the API uses it too)
constexpr uint8_t kAttempts = 3;              // tries per step
constexpr uint32_t kRetryGapMs = 2000;
constexpr uint32_t kQueueWaitMs = 120000;     // a command that has not finished this long after it asked for a place is given up
constexpr uint32_t kNetSettleMs = 10000;      // power-up run: after Wi-Fi came up, let the sockets join the network
constexpr uint32_t kPatientWindowMs = 90000;  // power-up run: retry window of one step
constexpr uint32_t kPatientGapMs = 5000;
constexpr uint32_t kBusyTickMs = 100;         // how often the engine looks while jobs are in flight
constexpr uint32_t kIdleTickMs = 1000;        // the longest sleep (Wi-Fi is watched while a run waits for it)

enum class Op : uint8_t { On, Off, Query };
enum class StepKind : uint8_t { Device, Http, Wait };
constexpr uint32_t kMaxWaitS = 3600;  // a wait step: 1..kMaxWaitS seconds

// The request of an HTTP step. URL and body can be long (2 KB, 16 KB): kept in Def::requests, allocated like the rest.
struct HttpReq {
  bool post = false;
  Text url, contentType, body;  // contentType and body: POST only ("" = none)
  uint32_t timeoutMs = 10000;
};

// A step names its device by an index into Def::devices: a scenario with 24 steps on the same few sockets keeps each name
// once (16 scenarios x 24 steps would otherwise hold ~400 copies of the names in internal RAM). An HTTP step names its
// request the same way, by an index into Def::requests.
struct StepDef {
  StepKind kind = StepKind::Device;
  uint16_t dev = 0;          // index into Def::devices (a device step) or Def::requests (an HTTP step)
  bool on = true;            // a device step: switch on or off
  bool enabled = true;
  int32_t pauseAfterS = -1;  // -1: the scenario's default (a wait step has none)
  uint32_t waitS = 0;        // a wait step: how long
  TimeWindow::Window hours;  // a device or HTTP step: only inside these hours (none: always)
  // An HTTP step: its own attempts and gap between them (0: the usual ones, kAttempts / kRetryGapMs, or the power-up's
  // patience). Either one set: exactly that, in every run.
  uint8_t attempts = 0;
  uint16_t retryGapS = 0;
};
constexpr uint8_t kMaxHttpAttempts = 20;
constexpr uint16_t kMaxRetryGapS = 600;

struct Def {
  std::string name;
  uint16_t defaultPauseS = 30;
  TimeWindow::Window hours;  // a run starts (any trigger but "manual") only inside these hours (none: always)
  List<Text> devices;      // the distinct device names of the steps
  List<HttpReq> requests;  // the requests of the HTTP steps, one per step
  List<StepDef> steps;

  bool isHttp(size_t step) const { return steps[step].kind == StepKind::Http; }
  bool isWait(size_t step) const { return steps[step].kind == StepKind::Wait; }
  const Text& device(size_t step) const { return devices[steps[step].dev]; }  // a device step only
  const HttpReq& request(size_t step) const { return requests[steps[step].dev]; }  // an HTTP step only
  // What the log, the API and the history call the step: the device name, or "POST api.telegram.org" (never the path
  // or the query of the URL: a token may be there).
  std::string label(size_t step) const;
};

// "api.telegram.org" out of "https://api.telegram.org/bot.../sendMessage" (no user:password@ either).
std::string hostOf(const Text& url);

struct DeviceInfo {
  bool exists = false;
  bool offline = false;  // the monitor counts it offline right now (used for the measurement only)
  int switchState = -1;  // the cached switch state: 1 on, 0 off, -1 unknown
};

struct JobStatus {
  bool done = false;
  bool ok = false;
  uint32_t ms = 0;  // how long the operation took (connect + handshake + command)
  std::string error, detail;
};

struct Run;

class Env {
 public:
  virtual ~Env() = default;
  // The definitions live outside the engine (one copy only): is there such a scenario, and its steps now.
  virtual bool known(const std::string& scenario) = 0;
  virtual bool def(const std::string& scenario, Def& out) = 0;
  virtual uint32_t nowMs() = 0;
  virtual uint32_t epoch() = 0;  // Unix time, 0 while the clock is not set
  virtual int localMinute() = 0;  // the local time of day in minutes (0..1439, config time.tz), -1 while the clock is not set
  virtual bool wifiUp() = 0;
  virtual DeviceInfo device(const std::string& name) = 0;
  // Queues a command (On/Off) or a status query for the device in the command lane; 0 = the list is full right now.
  virtual uint32_t submit(Op op, const std::string& device) = 0;
  // Queues the request of an HTTP step in the HTTP lane; 0 = its list is full right now. The ids share one space with submit().
  // JobStatus::detail of a request that succeeded says what the server answered ("answered 200 in 812 ms").
  // `quiet`: the request of a quiet run (Run::quiet): the HTTP lane logs it at DEBUG, failures too. `scenario` and
  // `trigger`: the run it belongs to (for the placeholders of the request, see lib/http_template).
  virtual uint32_t submitHttp(const HttpReq& req, bool quiet, const std::string& scenario, const std::string& trigger) = 0;
  virtual JobStatus poll(uint32_t job) = 0;
  // The engine is done with the job. `withdraw`: it has not finished, drop it if it has not started yet.
  virtual void release(uint32_t job, bool withdraw) = 0;
  virtual void log(bool warn, const std::string& line) = 0;
  // A line about a quiet run (see Engine::start): kept out of the usual log (the board writes it at DEBUG).
  virtual void logQuiet(const std::string& line) { (void)line; }
  // A run has ended (done, stopped, interrupted, removed before it started): for the history of the runs.
  virtual void finished(const std::string& scenario, const Run& run) = 0;
};

enum class RunState : uint8_t { Idle, Skipped, Waiting, Running, Done, Stopped, Interrupted };
enum class StepState : uint8_t { Pending, Disabled, Running, Ok, Skipped, Failed, Stopped };
const char* runStateName(RunState s);    // idle | skipped | waiting | running | done | stopped | interrupted
const char* stepStateName(StepState s);  // pending | disabled | running | ok | skipped | failed | stopped

// Kept small (8 bytes, 24 runs x 24 steps fit easily): the text the API shows is put together by stepDetail() when asked for.
struct StepRun {
  StepState state = StepState::Pending;
  uint8_t attempts = 0;
  uint16_t pauseS = 0;  // a pause after this step was kept (seconds)
  uint32_t atS = 0;     // when it ended, seconds since the run began (0: not ended)
};

// The reasons of the steps that have one (failed: why; running after a failed attempt: why that one failed). Few steps
// have one, so they are kept apart from the steps instead of an empty string in every step.
class Whys {
 public:
  const Text& get(size_t step) const {
    static const Text none;
    for (const auto& w : v_) {
      if (w.first == step) return w.second;
    }
    return none;
  }
  void set(size_t step, const Text& text) {  // "" removes it
    for (size_t i = 0; i < v_.size(); ++i) {
      if (v_[i].first != step) continue;
      if (text.empty()) {
        v_.erase(v_.begin() + static_cast<long>(i));
      } else {
        v_[i].second = text;
      }
      return;
    }
    if (!text.empty()) v_.emplace_back(static_cast<uint16_t>(step), text);
  }

 private:
  List<std::pair<uint16_t, Text>> v_;
};

// Wait: in a wait step; WaitClock: waiting for the clock for the hours of the start or of a step (appended last: a saved
// /runs.json keeps the phases as numbers).
enum class Phase : uint8_t { WaitStart, WaitNet, Settle, NextStep, Pause, Command, Verify, Gap, Finished, Wait, WaitClock };

struct Run {
  // What the API shows.
  RunState state = RunState::Idle;
  std::string trigger;     // what started it: power-on | manual | power-below | power-above
  Text skipReason;         // state Skipped / Interrupted: why
  std::string waitReason;  // while waiting: "wifi" (the network), "clock" (the time, for the hours)
  uint32_t startAtMs = 0;
  int current = -1;        // the step in progress; -1 in a pause or when none
  List<StepRun> steps;
  Whys whys;
  bool alive = false;      // started and not finished
  bool quiet = false;      // a frequent run (an interval scenario): its lines go to Env::logQuiet, it is not in the
                           // snapshots (/runs.json) and does not mark the state dirty
  bool stop = false;       // asked to stop
  Text resumed;            // the restarts this run went through and was continued after: "SW at step 2; PANIC at step 3"
  uint32_t startEpoch = 0; // Unix time the run began (0 unknown)
  uint32_t durationS = 0;  // an ended run: how long it went

  // The machine.
  Phase phase = Phase::Finished;
  Def def;                 // the steps as they were when the run left the wait (kept after the end for the device names)
  bool patient = false;    // started by the power-up
  size_t step = 0;         // the step being worked on (or the next one to look at)
  uint8_t attempt = 0;
  uint32_t deadlineMs = 0;       // end of the settle time, a pause or a gap
  uint32_t windowEndMs = 0;      // patient: end of the retry window of the step
  uint32_t job = 0;              // the job in flight, 0 = none
  bool jobHttp = false;          // that job is an HTTP request (counted against kMaxHttpInFlight)
  bool wantsSlot = false;        // waits for one of the kMaxInFlight places (Command/Verify without a job yet)
  bool slotWaitCounted = false;
  bool queueFullCounted = false;
  uint32_t askedMs = 0;          // when the current command/query asked for a place (kQueueWaitMs counts from here)
  uint32_t pauseS = 0;           // pause before the next enabled step
  size_t pauseFrom = 0;          // the step the pause belongs to
  Text lastWhy;
  unsigned switched = 0, skipped = 0, failed = 0;
  uint32_t startedMs = 0;        // when it began running in this boot
  uint32_t carriedMs = 0;        // how long it had been going before a restart it was continued after
  bool needNet = false;          // continued after a restart: wait for Wi-Fi before the next command
  bool netGate = false;          // in WaitNet/Settle for that reason (not for the start of a power-up run)
  bool clockWait = false;        // waiting for the clock for the hours of the start or of the current step
  uint32_t clockUntilMs = 0;     // ... until then, then it is skipped
  bool offlineAttempt = false;   // measurement: the monitor counted the device offline when the attempt began
  uint32_t attemptJobMs = 0;     // measurement: time the attempt's jobs held the command lane
};

struct Stats {
  uint32_t jobs = 0;           // jobs the engine queued
  uint32_t httpRequests = 0;   // HTTP requests the engine queued (in the HTTP lane, not counted in `jobs`)
  uint32_t inFlight = 0;       // now
  uint32_t maxInFlightSeen = 0;
  uint32_t slotWaits = 0;      // commands/queries that had to wait for a place (all kMaxInFlight taken)
  uint32_t slotWaitMaxMs = 0;
  uint32_t queueFull = 0;      // commands/queries that found the Tuya command list full (buttons, the API) and waited
  // Attempts on a device the monitor counted offline when the attempt began: how many, how many still worked, and how
  // long their jobs held the command lane. Decides whether such attempts should be spaced out.
  uint32_t offlineAttempts = 0, offlineAttemptsOk = 0, offlineAttemptMs = 0;
};

// One alive run as /runs.json keeps it, to continue it after a restart (see Engine::snapshot/resume).
struct Snapshot {
  std::string name, trigger;
  uint32_t defHash = 0;     // defHash() of the steps the run works with: an edit since means "start again"
  RunState state = RunState::Waiting;
  Phase phase = Phase::WaitStart;
  bool patient = false;
  uint32_t step = 0;
  uint8_t attempt = 0;
  uint32_t leftMs = 0;      // until the deadline of WaitStart / Settle / Pause / Gap
  uint32_t runMs = 0;       // how long the run had been going
  uint32_t startEpoch = 0;
  Text resumed;
  Def def;                  // the steps the run works with (empty while it waits for its start)
  List<StepRun> steps;
  Whys whys;
  unsigned switched = 0, skipped = 0, failed = 0;
};

// FNV-1a over everything in a definition that changes what a run does (devices, actions, enabled, pauses).
uint32_t defHash(const Def& d);

// ---- what to do with a run that a restart interrupted (on_interrupt) ----------------------------------------------------

enum class Policy : uint8_t { Resume, Restart, Drop };
enum class Cause : uint8_t { Power, Software, Crash, Brownout };
const char* policyName(Policy p);  // resume | restart | drop
bool policyFromName(const std::string& text, Policy& out);
const char* causeName(Cause c);    // power | software | crash | brownout

struct OnInterrupt {
  Policy power = Policy::Restart;    // the light went out (POWERON; on the debug board also flashing and RST)
  Policy software = Policy::Resume;  // a software restart (the Wi-Fi watchdog, "save and restart")
  Policy crash = Policy::Resume;     // a panic or a watchdog
  Policy brownout = Policy::Resume;  // the supply of the board sagged
  uint16_t resumeWithinMin = 120;    // a longer interruption turns "resume" into "restart"
  Policy of(Cause c) const;
};
constexpr uint16_t kMaxResumeWithinMin = 1440;

struct Decision {
  Policy action = Policy::Restart;
  std::string why;  // for the log and the history
};
// `gapKnown`/`gapS`: how long the board was away (known when the clock was set before and after). `defChanged`: the
// scenario was edited since the run began. "resume" becomes "restart" when the scenario was edited, when the power went
// and nobody knows for how long, or when the interruption was longer than resumeWithinMin.
Decision decide(Cause cause, const OnInterrupt& policy, bool gapKnown, uint32_t gapS, bool defChanged);

// What the API shows for step `i` of a run: "switched on after 2 attempts; pause 30 s before the next step", the reason of a
// failure, "previous attempt: ..." while retrying, "stopped", ...
std::string stepDetail(const Run& r, size_t i);

class Engine {
 public:
  explicit Engine(Env& env) : env_(env) {}

  // The definitions changed (Env::known/def answer differently now). A run that has not left its wait takes the steps it
  // finds then; a running one keeps its copy. Runs of scenarios that are gone are stopped (alive) or forgotten.
  void defsChanged();

  enum class StartResult { Started, NotFound, AlreadyRunning };
  // Starts a run at `startAtMs`. A run that is still waiting for its start is started now instead (its trigger becomes
  // `trigger`); one that is running gives AlreadyRunning.
  // `waitNet`: wait for Wi-Fi before the first command (a run started again after a restart).
  // `quiet`: a run that comes back every few seconds (an interval scenario): see Run::quiet.
  StartResult start(const std::string& name, const std::string& trigger, uint32_t startAtMs, bool waitNet = false, bool quiet = false);
  enum class StopResult { Stopping, NotFound, NotRunning };
  StopResult stop(const std::string& name);
  // A power_on scenario that does not start (not a power-up): shown as skipped with the reason.
  void skip(const std::string& name, const std::string& reason);
  // A skipped scenario that is no longer a power_on one (or was disabled): back to idle.
  void unskip(const std::string& name);

  // Moves every run on; returns how long the engine may sleep (it is woken earlier on start/stop/setDefs).
  uint32_t tick();

  // The alive runs, to keep in /runs.json, one at a time (a copy of all of them at once would cost tens of KB of RAM).
  // `dirty`: something worth saving changed since the last takeDirty().
  void forEachSnapshot(const std::function<void(const Snapshot&)>& fn) const;
  std::vector<Snapshot> snapshot() const;  // all at once: for the tests
  bool takeDirty();
  // After a restart: continues the saved run (`elapsedMs` of the interruption are taken off its wait or pause; the step
  // it was in is done again), after waiting for Wi-Fi. NotFound if the scenario is gone. `cause` goes into Run::resumed.
  StartResult resume(const Snapshot& s, uint32_t elapsedMs, const std::string& cause);
  // After a restart: the saved run is not continued. Shown (and recorded in the history) as interrupted with `why`.
  void interrupted(const Snapshot& s, const std::string& why);

  const Run* run(const std::string& name) const;
  size_t alive() const;
  const Stats& stats() const { return stats_; }

 private:
  void advance(const std::string& name, Run& r, uint32_t now);
  void beginRunning(const std::string& name, Run& r, uint32_t now, Def* found);
  void enter(const std::string& name, Run& r, uint32_t now);
  bool clockGate(const std::string& name, Run& r, uint32_t now, const TimeWindow::Window& w, const std::string& what, std::string& outside);
  void nextStep(const std::string& name, Run& r, uint32_t now);
  void startAttempt(const std::string& name, Run& r, uint32_t now);
  void attemptFailed(const std::string& name, Run& r, uint32_t now, const std::string& why);
  void attemptSucceeded(const std::string& name, Run& r, uint32_t now, const std::string& note = std::string());
  void endAttemptMeasurement(Run& r, bool ok);
  void finish(const std::string& name, Run& r, RunState state);
  void stopNow(const std::string& name, Run& r, bool markStep, uint8_t attempts);
  void grantSlots(uint32_t now);
  void dropJob(Run& r, bool withdraw);
  uint32_t runSeconds(const Run& r, uint32_t now) const;
  void stepEnded(Run& r, uint32_t now);
  uint32_t signature() const;
  void logf(bool warn, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

  Env& env_;
  std::map<std::string, Run, std::less<std::string>, Alloc<std::pair<const std::string, Run>>> runs_;  // by scenario name
  Stats stats_;
  uint32_t httpInFlight_ = 0;  // engine requests in the HTTP lane now
  bool quiet_ = false;         // the run being worked on is quiet: logf() goes to Env::logQuiet
  bool dirty_ = false;
  uint32_t lastSignature_ = 0;
};

}  // namespace ScenarioEngine
