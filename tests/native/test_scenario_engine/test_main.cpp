// Host tests of the scenario engine (lib/scenario_engine): pio test -e native
//
// FakeEnv plays the board: a clock moved by the test, Wi-Fi, sockets (working, dead, ignored, "the command has no
// effect") and the Tuya command lane, which runs one job at a time (a command 700 ms, a query 300 ms, a dead socket
// 1.5 s) from a list of limited size. Hours of pauses pass in milliseconds.
#include <unity.h>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>

#include "engine.h"

using namespace ScenarioEngine;

namespace {

struct FakeDevice {
  bool exists = true, ignored = false, offline = false;
  bool dead = false;       // does not answer: every job fails with "connect" after 1.5 s
  bool noEffect = false;   // takes the command, but the relay does not move
  bool relay = false;      // the real relay
  int cached = -1;         // what the board's cache says (set by a successful query)
};

struct FakeJob {
  Op op;
  std::string device;
  uint32_t endMs = 0;
  bool withdrawn = false, applied = false, ok = false;
  std::string error;
};

class FakeEnv : public Env {
 public:
  std::map<std::string, Def> defs;
  uint32_t now = 1000;
  bool wifi = true;
  size_t listCapacity = 16;
  std::map<std::string, FakeDevice> devices;
  std::map<uint32_t, FakeJob> jobs;
  uint32_t nextId = 1;
  uint32_t laneFreeMs = 0;
  std::vector<std::pair<Op, std::string>> sent;  // what reached a socket, in order
  std::vector<std::string> logLines;

  bool known(const std::string& name) override { return defs.count(name) > 0; }
  bool def(const std::string& name, Def& out) override {
    auto it = defs.find(name);
    if (it == defs.end()) return false;
    out = it->second;
    return true;
  }
  uint32_t nowMs() override { return now; }
  uint32_t epochNow = 0;  // 0: the clock is not set
  uint32_t epoch() override { return epochNow ? epochNow + (now - epochAtMs) / 1000 : 0; }
  uint32_t epochAtMs = 0;
  void setClock(uint32_t unix) {
    epochNow = unix;
    epochAtMs = now;
  }
  std::vector<std::pair<std::string, Run>> history;  // every run that ended, as the engine reported it
  void finished(const std::string& name, const Run& r) override { history.emplace_back(name, r); }
  bool wifiUp() override { return wifi; }
  DeviceInfo device(const std::string& name) override {
    DeviceInfo d;
    auto it = devices.find(name);
    if (it == devices.end() || !it->second.exists) return d;
    d.exists = true;
    d.ignored = it->second.ignored;
    d.offline = it->second.offline;
    d.switchState = it->second.cached;
    return d;
  }
  size_t pending() const {
    size_t n = 0;
    for (const auto& kv : jobs) n += (!kv.second.applied && !kv.second.withdrawn) ? 1 : 0;
    return n;
  }
  uint32_t submit(Op op, const std::string& name) override {
    if (pending() >= listCapacity) return 0;
    FakeJob j;
    j.op = op;
    j.device = name;
    const FakeDevice& d = devices[name];
    const uint32_t len = d.dead ? 1500 : op == Op::Query ? 300 : 700;
    const uint32_t startMs = std::max(now, laneFreeMs);
    j.endMs = startMs + len;
    laneFreeMs = j.endMs;
    jobs[nextId] = j;
    return nextId++;
  }
  JobStatus poll(uint32_t id) override {
    FakeJob& j = jobs.at(id);
    JobStatus s;
    s.done = j.applied;
    s.ok = j.ok;
    s.error = j.error;
    s.detail = j.ok ? "" : "no answer";
    s.ms = j.applied ? (devices[j.device].dead ? 1500 : 700) : 0;
    return s;
  }
  void release(uint32_t id, bool withdraw) override {
    auto it = jobs.find(id);
    if (it == jobs.end()) return;
    if (withdraw && !it->second.applied) {
      it->second.withdrawn = true;
      return;
    }
    jobs.erase(it);
  }
  void log(bool, const std::string& line) override { logLines.push_back(line); }

  // The lane: jobs finish in order; a finished job acts on the socket (and a query on the cache).
  void work() {
    for (auto& kv : jobs) {
      FakeJob& j = kv.second;
      if (j.applied || j.withdrawn || static_cast<int32_t>(now - j.endMs) < 0) continue;
      j.applied = true;
      FakeDevice& d = devices[j.device];
      if (d.dead) {
        j.ok = false;
        j.error = "connect";
        continue;
      }
      j.ok = true;
      sent.emplace_back(j.op, j.device);
      if (j.op == Op::Query) {
        d.cached = d.relay ? 1 : 0;
      } else if (!d.noEffect) {
        d.relay = j.op == Op::On;
      }
    }
  }
};

FakeEnv* env;
Engine* eng;

// Lets `ms` of time pass, the engine looking whenever it asked to.
void runFor(uint32_t ms) {
  const uint32_t end = env->now + ms;
  while (static_cast<int32_t>(end - env->now) > 0) {
    env->work();
    const uint32_t sleep = eng->tick();
    const uint32_t left = end - env->now;
    env->now += std::max<uint32_t>(1, std::min(sleep, left));
    env->work();
  }
  eng->tick();
}

struct S {  // a step as the tests write it: by device name
  std::string device;
  bool on;
  int32_t pause;
  bool enabled;
};
S step(const std::string& device, bool on = true, int32_t pause = -1, bool enabled = true) { return S{device, on, pause, enabled}; }

Def def(const std::string& name, std::vector<S> steps, uint16_t defaultPause = 0) {
  Def d;
  d.name = name;
  d.defaultPauseS = defaultPause;
  for (const S& s : steps) {
    StepDef st;
    auto it = std::find(d.devices.begin(), d.devices.end(), s.device.c_str());
    if (it == d.devices.end()) it = d.devices.insert(d.devices.end(), Text(s.device.c_str()));
    st.dev = static_cast<uint16_t>(it - d.devices.begin());
    st.on = s.on;
    st.pauseAfterS = s.pause;
    st.enabled = s.enabled;
    d.steps.push_back(st);
  }
  return d;
}

void setDefs(std::vector<Def> defs) {
  env->defs.clear();
  for (Def& d : defs) env->defs[d.name] = d;
  eng->defsChanged();
}

const Run& R(const std::string& name) { return *eng->run(name); }
StepState S(const std::string& name, size_t i) { return R(name).steps.at(i).state; }

void assertState(RunState want, const std::string& name) { TEST_ASSERT_EQUAL_STRING(runStateName(want), runStateName(R(name).state)); }
void assertStep(StepState want, const std::string& name, size_t i) { TEST_ASSERT_EQUAL_STRING(stepStateName(want), stepStateName(S(name, i))); }
bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

void setUp() {
  env = new FakeEnv();
  eng = new Engine(*env);
}
void tearDown() {
  delete eng;
  delete env;
}

// ---- one run -----------------------------------------------------------------------------------------------------

void test_steps_switch_and_verify_with_a_pause_after_a_switched_step() {
  env->devices["a"] = FakeDevice();
  env->devices["b"] = FakeDevice();
  setDefs({def("s", {step("a", true, 30), step("b", false)})});
  TEST_ASSERT_TRUE(eng->start("s", "manual", env->now) == Engine::StartResult::Started);
  runFor(5000);
  assertState(RunState::Running, "s");
  assertStep(StepState::Ok, "s", 0);
  TEST_ASSERT_TRUE(contains(stepDetail(R("s"), 0), "switched on; pause 30 s before the next step"));
  assertStep(StepState::Pending, "s", 1);
  TEST_ASSERT_EQUAL_INT(-1, R("s").current);
  TEST_ASSERT_TRUE(eng->start("s", "manual", env->now) == Engine::StartResult::AlreadyRunning);
  runFor(30000);
  assertState(RunState::Done, "s");
  assertStep(StepState::Ok, "s", 1);
  TEST_ASSERT_TRUE(env->devices["a"].relay);
  TEST_ASSERT_FALSE(env->devices["b"].relay);
  TEST_ASSERT_FALSE(R("s").alive);
}

void test_missing_ignored_and_disabled_steps_cost_no_time_and_no_pause() {
  env->devices["ign"].ignored = true;
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("ghost"), step("ign"), step("a", true, -1, false), step("a")}, 600)});
  eng->start("s", "manual", env->now);
  runFor(3000);
  assertState(RunState::Done, "s");
  assertStep(StepState::Failed, "s", 0);
  TEST_ASSERT_EQUAL_STRING("no such device in /devices.json", stepDetail(R("s"), 0).c_str());
  assertStep(StepState::Skipped, "s", 1);
  assertStep(StepState::Disabled, "s", 2);
  assertStep(StepState::Ok, "s", 3);
}

void test_a_command_without_effect_fails_after_three_attempts_two_seconds_apart() {
  env->devices["a"].noEffect = true;
  setDefs({def("s", {step("a"), step("ghost")})});
  eng->start("s", "manual", env->now);
  runFor(4000);
  TEST_ASSERT_EQUAL_UINT8(2, R("s").steps[0].attempts);  // 1 s per attempt + 2 s gap
  TEST_ASSERT_TRUE(contains(stepDetail(R("s"), 0), "previous attempt: the command was accepted, but the status does not show it on"));
  runFor(5000);
  assertStep(StepState::Failed, "s", 0);
  TEST_ASSERT_EQUAL_UINT8(3, R("s").steps[0].attempts);
  assertState(RunState::Done, "s");  // and the scenario went on
}

void test_a_dead_socket_fails_with_the_cause() {
  env->devices["a"].dead = true;
  setDefs({def("s", {step("a")})});
  eng->start("s", "manual", env->now);
  runFor(15000);
  assertStep(StepState::Failed, "s", 0);
  TEST_ASSERT_TRUE(stepDetail(R("s"), 0).rfind("connect:", 0) == 0);
}

// ---- stop -----------------------------------------------------------------------------------------------------------

void test_stop_while_waiting_for_the_start() {
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("a")})});
  eng->start("s", "manual", env->now + 60000);
  runFor(1000);
  assertState(RunState::Waiting, "s");
  TEST_ASSERT_TRUE(eng->stop("s") == Engine::StopResult::Stopping);
  runFor(50);
  assertState(RunState::Stopped, "s");
  TEST_ASSERT_TRUE(eng->stop("s") == Engine::StopResult::NotRunning);
  TEST_ASSERT_EQUAL(0, env->sent.size());
}

void test_stop_in_an_hour_long_pause_acts_at_once_and_the_next_command_never_goes_out() {
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("a", true, 3600), step("a", false)})});
  eng->start("s", "manual", env->now);
  runFor(10 * 60 * 1000);
  assertStep(StepState::Ok, "s", 0);
  eng->stop("s");
  runFor(20);
  assertState(RunState::Stopped, "s");
  assertStep(StepState::Pending, "s", 1);
  runFor(2 * 3600 * 1000);
  TEST_ASSERT_TRUE(env->devices["a"].relay);  // the "off" never went out
}

void test_stop_between_attempts_marks_the_step() {
  env->devices["a"].noEffect = true;
  setDefs({def("s", {step("a")})});
  eng->start("s", "manual", env->now);
  runFor(1500);  // first attempt over, in the 2 s gap
  eng->stop("s");
  runFor(20);
  assertState(RunState::Stopped, "s");
  assertStep(StepState::Stopped, "s", 0);
  TEST_ASSERT_EQUAL_UINT8(1, R("s").steps[0].attempts);
}

// ---- many runs and the places in the command lane --------------------------------------------------------------------

void test_twenty_runs_with_hour_long_pauses_all_finish_and_never_hold_more_than_four_places() {
  std::vector<Def> defs;
  for (int i = 0; i < 20; ++i) {
    const std::string dev = "d" + std::to_string(i);
    env->devices[dev] = FakeDevice();
    defs.push_back(def("s" + std::to_string(i), {step(dev, true, 3600), step(dev, false, 1800), step(dev, true)}));
  }
  setDefs(defs);
  for (int i = 0; i < 20; ++i) eng->start("s" + std::to_string(i), "manual", env->now);
  runFor(60 * 1000);
  TEST_ASSERT_EQUAL(20, eng->alive());
  for (int i = 0; i < 20; ++i) assertStep(StepState::Ok, "s" + std::to_string(i), 0);  // 20 x 1 s of lane time
  runFor(3 * 3600 * 1000);
  for (int i = 0; i < 20; ++i) {
    const std::string n = "s" + std::to_string(i);
    assertState(RunState::Done, n);
    for (size_t k = 0; k < 3; ++k) assertStep(StepState::Ok, n, k);
    TEST_ASSERT_TRUE(env->devices["d" + std::to_string(i)].relay);
  }
  TEST_ASSERT_EQUAL(0, eng->alive());
  TEST_ASSERT_EQUAL_UINT32(kMaxInFlight, eng->stats().maxInFlightSeen);
  TEST_ASSERT_TRUE(eng->stats().slotWaits > 0);
  TEST_ASSERT_EQUAL_UINT32(0, eng->stats().inFlight);
  TEST_ASSERT_EQUAL_UINT32(20 * 3 * 2, eng->stats().jobs);
  TEST_ASSERT_TRUE(env->jobs.empty());  // every job released
}

void test_a_full_tuya_list_makes_the_run_wait_and_after_120_s_the_attempt_fails() {
  env->devices["a"] = FakeDevice();
  env->listCapacity = 0;  // the buttons and the API hold the whole list
  setDefs({def("s", {step("a")})});
  eng->start("s", "manual", env->now);
  runFor(60000);
  TEST_ASSERT_EQUAL_UINT8(1, R("s").steps[0].attempts);
  TEST_ASSERT_EQUAL_UINT32(1, eng->stats().queueFull);
  env->listCapacity = 16;
  runFor(5000);
  assertStep(StepState::Ok, "s", 0);  // got through once there was room

  eng->start("s", "manual", env->now);
  env->listCapacity = 0;
  runFor(123000);  // given up at 120 s; the next attempt (after the 2 s gap) shows why
  TEST_ASSERT_TRUE(contains(stepDetail(R("s"), 0), "did not get through the device queue in time"));
}

void test_a_command_stuck_in_the_list_is_withdrawn_after_120_s() {
  env->devices["slow"] = FakeDevice();
  env->devices["a"] = FakeDevice();
  env->laneFreeMs = env->now + 200000;  // something holds the lane for 200 s
  setDefs({def("s", {step("a")})});
  eng->start("s", "manual", env->now);
  runFor(123000);
  TEST_ASSERT_TRUE(contains(stepDetail(R("s"), 0), "did not get through the device queue in time"));
  bool withdrawn = false;
  for (const auto& kv : env->jobs) withdrawn = withdrawn || kv.second.withdrawn;
  TEST_ASSERT_TRUE(withdrawn);
}

// ---- the power-up run ------------------------------------------------------------------------------------------------

void test_a_power_up_run_waits_for_wifi_as_long_as_it_takes_then_settles() {
  env->devices["a"] = FakeDevice();
  env->wifi = false;
  setDefs({def("p", {step("a")})});
  eng->start("p", "power-on", env->now + 45000);
  runFor(3 * 3600 * 1000);  // three hours without Wi-Fi
  assertState(RunState::Waiting, "p");
  TEST_ASSERT_EQUAL_STRING("wifi", R("p").waitReason.c_str());
  env->wifi = true;
  runFor(9000);
  assertState(RunState::Waiting, "p");  // 10 s for the sockets
  runFor(6000);
  assertState(RunState::Done, "p");
}

void test_a_power_up_run_keeps_trying_a_step_for_90_s() {
  env->devices["a"].dead = true;
  env->devices["a"].offline = true;
  setDefs({def("p", {step("a")})});
  eng->start("p", "power-on", env->now);
  runFor(60000);
  assertState(RunState::Running, "p");
  runFor(40000);
  assertStep(StepState::Failed, "p", 0);
  const uint8_t attempts = R("p").steps[0].attempts;
  TEST_ASSERT_TRUE(attempts >= 12 && attempts <= 15);  // 1.5 s + 5 s gap over 90 s
  // The measurement: every attempt on a device the monitor called offline, none worked, 1.5 s of lane time each.
  TEST_ASSERT_EQUAL_UINT32(attempts, eng->stats().offlineAttempts);
  TEST_ASSERT_EQUAL_UINT32(0, eng->stats().offlineAttemptsOk);
  TEST_ASSERT_EQUAL_UINT32(attempts * 1500u, eng->stats().offlineAttemptMs);
}

void test_starting_a_waiting_power_up_run_by_hand_skips_the_waits() {
  env->devices["a"] = FakeDevice();
  env->wifi = false;
  setDefs({def("p", {step("a")})});
  eng->start("p", "power-on", env->now + 45000);
  runFor(50000);
  TEST_ASSERT_TRUE(eng->start("p", "manual", env->now) == Engine::StartResult::Started);
  runFor(5000);
  TEST_ASSERT_EQUAL_STRING("manual", R("p").trigger.c_str());
  TEST_ASSERT_TRUE(R("p").state == RunState::Running || R("p").state == RunState::Done);
}

// ---- definitions changing --------------------------------------------------------------------------------------------

void test_an_edit_while_waiting_counts_a_running_run_keeps_its_steps() {
  env->devices["a"] = FakeDevice();
  env->devices["b"] = FakeDevice();
  setDefs({def("s", {step("a")})});
  eng->start("s", "manual", env->now + 10000);
  setDefs({def("s", {step("b"), step("b", false)})});
  runFor(15000);
  TEST_ASSERT_EQUAL(2, R("s").steps.size());
  TEST_ASSERT_EQUAL_STRING("b", R("s").def.device(0).c_str());

  setDefs({def("t", {step("a", true, 60), step("a", false)})});  // "s" removed, "t" added
  TEST_ASSERT_NULL(eng->run("s"));                                     // it had finished: forgotten
  eng->start("t", "manual", env->now);
  runFor(5000);
  setDefs({def("t", {step("b")})});
  runFor(70000);
  assertState(RunState::Done, "t");
  TEST_ASSERT_EQUAL(2, R("t").steps.size());  // the run went on with the steps it started with
  TEST_ASSERT_FALSE(env->devices["a"].relay);
}

void test_a_scenario_removed_while_its_run_is_alive_is_stopped() {
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("a", true, 600), step("a")})});
  eng->start("s", "manual", env->now);
  runFor(5000);
  setDefs({});
  runFor(100);
  assertState(RunState::Stopped, "s");
  TEST_ASSERT_TRUE(eng->start("s", "manual", env->now) == Engine::StartResult::NotFound);
}


// ---- after a restart: on_interrupt, snapshot and resume ------------------------------------------------------------

// The board restarts: a new engine, the Tuya jobs are gone, time goes on by `awayMs`.
void reboot(uint32_t awayMs) {
  delete eng;
  env->jobs.clear();
  env->now += awayMs;
  env->laneFreeMs = env->now;
  eng = new Engine(*env);
}

void test_decide_turns_resume_into_restart_when_it_would_be_wrong() {
  OnInterrupt p;  // the defaults: power restart, the rest resume, 120 min
  TEST_ASSERT_TRUE(decide(Cause::Software, p, true, 60, false).action == Policy::Resume);
  TEST_ASSERT_TRUE(decide(Cause::Crash, p, false, 0, false).action == Policy::Resume);     // unknown gap: taken as short
  TEST_ASSERT_TRUE(decide(Cause::Brownout, p, true, 7200, false).action == Policy::Resume); // exactly 120 min
  TEST_ASSERT_TRUE(decide(Cause::Brownout, p, true, 7201, false).action == Policy::Restart);
  TEST_ASSERT_TRUE(decide(Cause::Software, p, true, 60, true).action == Policy::Restart);   // edited meanwhile
  TEST_ASSERT_TRUE(decide(Cause::Power, p, true, 60, false).action == Policy::Restart);    // the default for power
  p.power = Policy::Resume;
  TEST_ASSERT_TRUE(decide(Cause::Power, p, true, 60, false).action == Policy::Resume);
  const Decision d = decide(Cause::Power, p, false, 0, false);  // no clock: nobody knows how long the light was off
  TEST_ASSERT_TRUE(d.action == Policy::Restart);
  TEST_ASSERT_TRUE(contains(d.why, "not known"));
  p.software = Policy::Drop;
  TEST_ASSERT_TRUE(decide(Cause::Software, p, true, 60, true).action == Policy::Drop);  // drop stays drop
  Policy x;
  TEST_ASSERT_TRUE(policyFromName("resume", x) && x == Policy::Resume);
  TEST_ASSERT_FALSE(policyFromName("Resume", x));
}

void test_a_run_in_an_hour_long_pause_continues_with_what_is_left_of_it() {
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("a", true, 3600), step("a", false)})});
  eng->start("s", "manual", env->now);
  runFor(10 * 60 * 1000);
  std::vector<Snapshot> saved = eng->snapshot();
  TEST_ASSERT_EQUAL(1, saved.size());
  TEST_ASSERT_TRUE(saved[0].phase == Phase::Pause);
  TEST_ASSERT_TRUE(saved[0].leftMs > 2995000 && saved[0].leftMs <= 3005000);  // 3600 s from ~1 s, 600 s later
  TEST_ASSERT_EQUAL_UINT32(defHash(env->defs["s"]), saved[0].defHash);

  reboot(5 * 60 * 1000);  // away for 5 minutes
  TEST_ASSERT_TRUE(eng->resume(saved[0], 5 * 60 * 1000, "SW") == Engine::StartResult::Started);
  runFor(50);
  assertState(RunState::Running, "s");
  assertStep(StepState::Ok, "s", 0);
  TEST_ASSERT_EQUAL_STRING("SW at step 2", R("s").resumed.c_str());
  runFor(2650 * 1000);
  assertStep(StepState::Pending, "s", 1);  // 10 + 5 + 44 min: not yet
  runFor(60 * 1000);
  assertState(RunState::Done, "s");
  TEST_ASSERT_FALSE(env->devices["a"].relay);
  // The history: one record, with the whole time (before, away and after) and the step times on that scale.
  TEST_ASSERT_EQUAL(1, env->history.size());
  const Run& h = env->history[0].second;
  TEST_ASSERT_TRUE(h.state == RunState::Done);
  TEST_ASSERT_TRUE(h.durationS >= 3601 && h.durationS <= 3610);
  TEST_ASSERT_TRUE(h.steps[0].atS >= 1 && h.steps[0].atS <= 2);
  TEST_ASSERT_TRUE(h.steps[1].atS >= 3601 && h.steps[1].atS <= 3610);
  TEST_ASSERT_EQUAL_STRING("SW at step 2", h.resumed.c_str());
}

void test_the_step_a_restart_cut_is_done_again() {
  env->devices["a"] = FakeDevice();
  env->laneFreeMs = env->now + 5000;  // the lane is busy: the command is still queued
  setDefs({def("s", {step("a")})});
  eng->start("s", "manual", env->now);
  runFor(1000);
  std::vector<Snapshot> saved = eng->snapshot();
  TEST_ASSERT_TRUE(saved[0].phase == Phase::Command);
  reboot(2000);
  eng->resume(saved[0], 2000, "PANIC");
  runFor(3000);
  assertState(RunState::Done, "s");
  assertStep(StepState::Ok, "s", 0);
  TEST_ASSERT_EQUAL_UINT8(1, R("s").steps[0].attempts);
  TEST_ASSERT_TRUE(env->devices["a"].relay);
}

void test_a_continued_run_waits_for_wifi_before_its_next_command() {
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("a", true, 60), step("a", false)})});
  eng->start("s", "manual", env->now);
  runFor(5000);
  std::vector<Snapshot> saved = eng->snapshot();
  reboot(1000);
  env->wifi = false;
  eng->resume(saved[0], 1000, "BROWNOUT");
  runFor(70000);  // the pause is over: step 2 waits for the network
  assertState(RunState::Running, "s");
  TEST_ASSERT_EQUAL_STRING("wifi", R("s").waitReason.c_str());
  assertStep(StepState::Pending, "s", 1);
  TEST_ASSERT_TRUE(env->devices["a"].relay);
  env->wifi = true;
  runFor(9000);
  assertStep(StepState::Pending, "s", 1);  // 10 s for the sockets after the network came
  runFor(3000);
  assertState(RunState::Done, "s");
  TEST_ASSERT_FALSE(env->devices["a"].relay);
}

void test_a_run_waiting_for_its_start_keeps_the_rest_of_the_delay() {
  env->devices["a"] = FakeDevice();
  env->wifi = false;
  setDefs({def("p", {step("a")})});
  eng->start("p", "power-on", env->now + 45000);
  runFor(20000);
  std::vector<Snapshot> saved = eng->snapshot();
  TEST_ASSERT_TRUE(saved[0].state == RunState::Waiting && saved[0].defHash == 0);
  reboot(5000);
  eng->resume(saved[0], 5000, "SW");
  runFor(1000);
  assertState(RunState::Waiting, "p");
  TEST_ASSERT_TRUE(R("p").startAtMs - env->now <= 20000 && R("p").startAtMs - env->now > 18000);
  env->wifi = true;
  runFor(40000);
  assertState(RunState::Done, "p");
  TEST_ASSERT_EQUAL_STRING("SW while waiting", R("p").resumed.c_str());
}

void test_a_run_not_continued_is_shown_and_recorded_as_interrupted() {
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("a", true, 600), step("a", false)})});
  eng->start("s", "manual", env->now);
  runFor(5000);
  std::vector<Snapshot> saved = eng->snapshot();
  reboot(1000);
  eng->interrupted(saved[0], "on_interrupt.software = drop");
  assertState(RunState::Interrupted, "s");
  TEST_ASSERT_FALSE(R("s").alive);
  TEST_ASSERT_EQUAL_STRING("on_interrupt.software = drop", R("s").skipReason.c_str());
  assertStep(StepState::Ok, "s", 0);
  assertStep(StepState::Pending, "s", 1);
  TEST_ASSERT_EQUAL(1, env->history.size());
  TEST_ASSERT_TRUE(env->history[0].second.state == RunState::Interrupted);
  TEST_ASSERT_EQUAL(0, eng->snapshot().size());
  TEST_ASSERT_TRUE(eng->start("s", "manual", env->now) == Engine::StartResult::Started);  // a new run is fine
}

void test_a_run_whose_scenario_is_gone_cannot_be_continued() {
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("a", true, 600), step("a", false)})});
  eng->start("s", "manual", env->now);
  runFor(3000);
  std::vector<Snapshot> saved = eng->snapshot();
  TEST_ASSERT_EQUAL(1, saved.size());
  reboot(1000);
  setDefs({});
  TEST_ASSERT_TRUE(eng->resume(saved[0], 1000, "SW") == Engine::StartResult::NotFound);
  TEST_ASSERT_NULL(eng->run("s"));
}

void test_dirty_marks_what_is_worth_saving_and_the_history_gets_every_end() {
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("a", true, 30), step("a")}), def("w", {step("a")})});
  TEST_ASSERT_FALSE(eng->takeDirty());
  eng->start("s", "manual", env->now);
  TEST_ASSERT_TRUE(eng->takeDirty());
  TEST_ASSERT_FALSE(eng->takeDirty());
  runFor(3000);  // step 1 done, in the pause
  TEST_ASSERT_TRUE(eng->takeDirty());
  runFor(10000);  // nothing happens in the pause
  TEST_ASSERT_FALSE(eng->takeDirty());
  eng->start("w", "manual", env->now + 60000);
  eng->stop("w");  // stopped while waiting: an end too
  runFor(40000);
  TEST_ASSERT_TRUE(eng->takeDirty());
  TEST_ASSERT_EQUAL(2, env->history.size());
  TEST_ASSERT_EQUAL_STRING("w", env->history[0].first.c_str());
  TEST_ASSERT_TRUE(env->history[0].second.state == RunState::Stopped);
  TEST_ASSERT_TRUE(env->history[1].second.state == RunState::Done);
}

void test_the_start_time_is_counted_back_when_the_clock_comes_later() {
  env->devices["a"] = FakeDevice();
  setDefs({def("s", {step("a", true, 100), step("a")})});
  eng->start("s", "manual", env->now);
  runFor(50000);
  env->setClock(1800000000);  // NTP, 50 s into the run
  runFor(60000);
  assertState(RunState::Done, "s");
  const Run& h = env->history.back().second;
  TEST_ASSERT_TRUE(h.startEpoch >= 1800000000 - 51 && h.startEpoch <= 1800000000 - 49);
}

// ---- memory ---------------------------------------------------------------------------------------------------------

size_t gCountedBytes = 0;
void* countingAlloc(size_t n) {
  gCountedBytes += n;
  return std::malloc(n);
}

void test_the_steps_names_and_reasons_of_a_run_come_from_the_allocator_set() {
  setAllocator(&countingAlloc);  // the board sets PSRAM
  env->devices["a"].noEffect = true;
  std::vector<decltype(step("a"))> steps{step("a")};  // (S is also a function here)
  for (int k = 0; k < 23; ++k) steps.push_back(step("a-device-with-a-long-name-" + std::to_string(k)));
  setDefs({def("s", steps)});
  const size_t afterDefs = gCountedBytes;
  eng->start("s", "manual", env->now);
  runFor(20000);
  assertState(RunState::Done, "s");
  // The run's copy of the 24 steps and their names, its step states and the 24 reasons went through it.
  TEST_ASSERT_GREATER_THAN(24 * (sizeof(StepDef) + sizeof(StepRun) + 30 + 20), gCountedBytes - afterDefs);
  TEST_ASSERT_TRUE(contains(stepDetail(R("s"), 0), "the status does not show it on"));
  TEST_ASSERT_EQUAL_STRING("no such device in /devices.json", stepDetail(R("s"), 23).c_str());
  setAllocator(nullptr);  // back to malloc
  const size_t before = gCountedBytes;
  setDefs({def("t", {step("a")})});
  TEST_ASSERT_EQUAL(before, gCountedBytes);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_steps_switch_and_verify_with_a_pause_after_a_switched_step);
  RUN_TEST(test_missing_ignored_and_disabled_steps_cost_no_time_and_no_pause);
  RUN_TEST(test_a_command_without_effect_fails_after_three_attempts_two_seconds_apart);
  RUN_TEST(test_a_dead_socket_fails_with_the_cause);
  RUN_TEST(test_stop_while_waiting_for_the_start);
  RUN_TEST(test_stop_in_an_hour_long_pause_acts_at_once_and_the_next_command_never_goes_out);
  RUN_TEST(test_stop_between_attempts_marks_the_step);
  RUN_TEST(test_twenty_runs_with_hour_long_pauses_all_finish_and_never_hold_more_than_four_places);
  RUN_TEST(test_a_full_tuya_list_makes_the_run_wait_and_after_120_s_the_attempt_fails);
  RUN_TEST(test_a_command_stuck_in_the_list_is_withdrawn_after_120_s);
  RUN_TEST(test_a_power_up_run_waits_for_wifi_as_long_as_it_takes_then_settles);
  RUN_TEST(test_a_power_up_run_keeps_trying_a_step_for_90_s);
  RUN_TEST(test_starting_a_waiting_power_up_run_by_hand_skips_the_waits);
  RUN_TEST(test_an_edit_while_waiting_counts_a_running_run_keeps_its_steps);
  RUN_TEST(test_a_scenario_removed_while_its_run_is_alive_is_stopped);
  RUN_TEST(test_decide_turns_resume_into_restart_when_it_would_be_wrong);
  RUN_TEST(test_a_run_in_an_hour_long_pause_continues_with_what_is_left_of_it);
  RUN_TEST(test_the_step_a_restart_cut_is_done_again);
  RUN_TEST(test_a_continued_run_waits_for_wifi_before_its_next_command);
  RUN_TEST(test_a_run_waiting_for_its_start_keeps_the_rest_of_the_delay);
  RUN_TEST(test_a_run_not_continued_is_shown_and_recorded_as_interrupted);
  RUN_TEST(test_a_run_whose_scenario_is_gone_cannot_be_continued);
  RUN_TEST(test_dirty_marks_what_is_worth_saving_and_the_history_gets_every_end);
  RUN_TEST(test_the_start_time_is_counted_back_when_the_clock_comes_later);
  RUN_TEST(test_the_steps_names_and_reasons_of_a_run_come_from_the_allocator_set);
  return UNITY_END();
}
