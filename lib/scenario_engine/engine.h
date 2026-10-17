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
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ScenarioEngine {

constexpr uint8_t kMaxInFlight = 4;           // engine jobs queued or running in the Tuya command lane at once
constexpr uint8_t kAttempts = 3;              // tries per step
constexpr uint32_t kRetryGapMs = 2000;
constexpr uint32_t kQueueWaitMs = 120000;     // a command that has not finished this long after it asked for a place is given up
constexpr uint32_t kNetSettleMs = 10000;      // power-up run: after Wi-Fi came up, let the sockets join the network
constexpr uint32_t kPatientWindowMs = 90000;  // power-up run: retry window of one step
constexpr uint32_t kPatientGapMs = 5000;
constexpr uint32_t kBusyTickMs = 100;         // how often the engine looks while jobs are in flight
constexpr uint32_t kIdleTickMs = 1000;        // the longest sleep (Wi-Fi is watched while a run waits for it)

enum class Op : uint8_t { On, Off, Query };

// A step names its device by an index into Def::devices: a scenario with 24 steps on the same few sockets keeps each name
// once (16 scenarios x 24 steps would otherwise hold ~400 copies of the names in internal RAM).
struct StepDef {
  uint16_t dev = 0;          // index into Def::devices
  bool on = true;
  bool enabled = true;
  int32_t pauseAfterS = -1;  // -1: the scenario's default
};

struct Def {
  std::string name;
  uint16_t defaultPauseS = 30;
  std::vector<std::string> devices;  // the distinct device names of the steps
  std::vector<StepDef> steps;

  const std::string& device(size_t step) const { return devices[steps[step].dev]; }
};

struct DeviceInfo {
  bool exists = false;
  bool ignored = false;
  bool offline = false;  // the monitor counts it offline right now (used for the measurement only)
  int switchState = -1;  // the cached switch state: 1 on, 0 off, -1 unknown
};

struct JobStatus {
  bool done = false;
  bool ok = false;
  uint32_t ms = 0;  // how long the operation took (connect + handshake + command)
  std::string error, detail;
};

class Env {
 public:
  virtual ~Env() = default;
  // The definitions live outside the engine (one copy only): is there such a scenario, and its steps now.
  virtual bool known(const std::string& scenario) = 0;
  virtual bool def(const std::string& scenario, Def& out) = 0;
  virtual uint32_t nowMs() = 0;
  virtual bool wifiUp() = 0;
  virtual DeviceInfo device(const std::string& name) = 0;
  // Queues a command (On/Off) or a status query for the device in the command lane; 0 = the list is full right now.
  virtual uint32_t submit(Op op, const std::string& device) = 0;
  virtual JobStatus poll(uint32_t job) = 0;
  // The engine is done with the job. `withdraw`: it has not finished, drop it if it has not started yet.
  virtual void release(uint32_t job, bool withdraw) = 0;
  virtual void log(bool warn, const std::string& line) = 0;
};

enum class RunState : uint8_t { Idle, Skipped, Waiting, Running, Done, Stopped };
enum class StepState : uint8_t { Pending, Disabled, Running, Ok, Skipped, Failed, Stopped };
const char* runStateName(RunState s);    // idle | skipped | waiting | running | done | stopped
const char* stepStateName(StepState s);  // pending | disabled | running | ok | skipped | failed | stopped

// Kept small: the text the API shows is put together by stepDetail() when asked for.
struct StepRun {
  StepState state = StepState::Pending;
  uint8_t attempts = 0;
  uint16_t pauseS = 0;  // a pause after this step was kept (seconds)
  std::string why;      // failed: the reason; running after a failed attempt: why that one failed
};

enum class Phase : uint8_t { WaitStart, WaitNet, Settle, NextStep, Pause, Command, Verify, Gap, Finished };

struct Run {
  // What the API shows.
  RunState state = RunState::Idle;
  std::string trigger;     // what started it: power-on | manual | power-below | power-above
  std::string skipReason;  // state Skipped
  std::string waitReason;  // while waiting past the start time: "wifi"
  uint32_t startAtMs = 0;
  int current = -1;        // the step in progress; -1 in a pause or when none
  std::vector<StepRun> steps;
  bool alive = false;      // started and not finished
  bool stop = false;       // asked to stop
  bool holdsPowerUp = false;  // started by the power-up and not past the wait for the network yet

  // The machine.
  Phase phase = Phase::Finished;
  Def def;                 // the steps as they were when the run left the wait (kept after the end for the device names)
  bool patient = false;    // started by the power-up
  size_t step = 0;         // the step being worked on (or the next one to look at)
  uint8_t attempt = 0;
  uint32_t deadlineMs = 0;       // end of the settle time, a pause or a gap
  uint32_t windowEndMs = 0;      // patient: end of the retry window of the step
  uint32_t job = 0;              // the job in flight, 0 = none
  bool wantsSlot = false;        // waits for one of the kMaxInFlight places (Command/Verify without a job yet)
  bool slotWaitCounted = false;
  bool queueFullCounted = false;
  uint32_t askedMs = 0;          // when the current command/query asked for a place (kQueueWaitMs counts from here)
  uint32_t pauseS = 0;           // pause before the next enabled step
  size_t pauseFrom = 0;          // the step the pause belongs to
  std::string lastWhy;
  unsigned switched = 0, skipped = 0, failed = 0;
  bool offlineAttempt = false;   // measurement: the monitor counted the device offline when the attempt began
  uint32_t attemptJobMs = 0;     // measurement: time the attempt's jobs held the command lane
};

struct Stats {
  uint32_t jobs = 0;           // jobs the engine queued
  uint32_t inFlight = 0;       // now
  uint32_t maxInFlightSeen = 0;
  uint32_t slotWaits = 0;      // commands/queries that had to wait for a place (all kMaxInFlight taken)
  uint32_t slotWaitMaxMs = 0;
  uint32_t queueFull = 0;      // commands/queries that found the Tuya command list full (buttons, the API) and waited
  // Attempts on a device the monitor counted offline when the attempt began: how many, how many still worked, and how
  // long their jobs held the command lane. Decides whether such attempts should be spaced out.
  uint32_t offlineAttempts = 0, offlineAttemptsOk = 0, offlineAttemptMs = 0;
};

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
  StartResult start(const std::string& name, const std::string& trigger, uint32_t startAtMs, bool holdsPowerUp = false);
  enum class StopResult { Stopping, NotFound, NotRunning };
  StopResult stop(const std::string& name);
  // A power_on scenario that does not start (not a power-up): shown as skipped with the reason.
  void skip(const std::string& name, const std::string& reason);
  // A skipped scenario that is no longer a power_on one (or was disabled): back to idle.
  void unskip(const std::string& name);

  // Moves every run on; returns how long the engine may sleep (it is woken earlier on start/stop/setDefs).
  uint32_t tick();

  const Run* run(const std::string& name) const;
  bool holdsPowerUp() const;  // a run of the power-up still waits for the network
  size_t alive() const;
  const Stats& stats() const { return stats_; }

 private:
  void advance(const std::string& name, Run& r, uint32_t now);
  void beginRunning(const std::string& name, Run& r, uint32_t now);
  void nextStep(const std::string& name, Run& r, uint32_t now);
  void startAttempt(const std::string& name, Run& r, uint32_t now);
  void attemptFailed(const std::string& name, Run& r, uint32_t now, const std::string& why);
  void attemptSucceeded(const std::string& name, Run& r, uint32_t now);
  void endAttemptMeasurement(Run& r, bool ok);
  void finish(const std::string& name, Run& r, RunState state);
  void stopNow(const std::string& name, Run& r, bool markStep, uint8_t attempts);
  void grantSlots(uint32_t now);
  void dropJob(Run& r, bool withdraw);
  void logf(bool warn, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

  Env& env_;
  std::map<std::string, Run> runs_;  // by scenario name
  Stats stats_;
};

}  // namespace ScenarioEngine
