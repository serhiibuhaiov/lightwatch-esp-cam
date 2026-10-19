#include "engine.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <new>

namespace ScenarioEngine {
namespace {

AllocFn gAlloc = &std::malloc;

bool due(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }
uint32_t until(uint32_t now, uint32_t at) {
  const int32_t d = static_cast<int32_t>(at - now);
  return d > 0 ? static_cast<uint32_t>(d) : 0;
}
const char* verb(bool on) { return on ? "on" : "off"; }

// "towel on" or "POST api.telegram.org": the step in a log line.
std::string what(const Def& d, size_t step) {
  return d.isHttp(step) || d.isWait(step) ? d.label(step) : d.label(step) + " " + verb(d.steps[step].on);
}

}  // namespace

void setAllocator(AllocFn fn) { gAlloc = fn ? fn : &std::malloc; }

std::string hostOf(const Text& url) {
  size_t from = url.find("://");
  from = from == Text::npos ? 0 : from + 3;
  size_t to = from;
  while (to < url.size() && url[to] != '/' && url[to] != '?' && url[to] != '#') ++to;
  const size_t at = url.rfind('@', to == 0 ? 0 : to - 1);
  if (at != Text::npos && at >= from && at < to) from = at + 1;  // user:password@host: never shown
  return std::string(url.data() + from, to - from);
}

std::string Def::label(size_t step) const {
  if (isWait(step)) return "wait " + std::to_string(steps[step].waitS) + " s";
  if (!isHttp(step)) return str(device(step));
  const HttpReq& q = request(step);
  return std::string(q.post ? "POST " : "GET ") + hostOf(q.url);
}

void* allocBytes(size_t bytes) {
  void* p = gAlloc(bytes ? bytes : 1);
  if (!p) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    throw std::bad_alloc();
#else
    std::abort();
#endif
  }
  return p;
}

const char* runStateName(RunState s) {
  switch (s) {
    case RunState::Skipped: return "skipped";
    case RunState::Waiting: return "waiting";
    case RunState::Running: return "running";
    case RunState::Done: return "done";
    case RunState::Stopped: return "stopped";
    case RunState::Interrupted: return "interrupted";
    default: return "idle";
  }
}

const char* stepStateName(StepState s) {
  switch (s) {
    case StepState::Disabled: return "disabled";
    case StepState::Running: return "running";
    case StepState::Ok: return "ok";
    case StepState::Skipped: return "skipped";
    case StepState::Failed: return "failed";
    case StepState::Stopped: return "stopped";
    default: return "pending";
  }
}

void Engine::logf(bool warn, const char* fmt, ...) {
  char buf[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (quiet_) {
    env_.logQuiet(buf);
  } else {
    env_.log(warn, buf);
  }
}

// ---- outside requests ----------------------------------------------------------------------------

// ---- on_interrupt ---------------------------------------------------------------------------------------------------

const char* policyName(Policy p) {
  switch (p) {
    case Policy::Resume: return "resume";
    case Policy::Drop: return "drop";
    default: return "restart";
  }
}

bool policyFromName(const std::string& text, Policy& out) {
  if (text == "resume") out = Policy::Resume;
  else if (text == "restart") out = Policy::Restart;
  else if (text == "drop") out = Policy::Drop;
  else return false;
  return true;
}

const char* causeName(Cause c) {
  switch (c) {
    case Cause::Power: return "power";
    case Cause::Software: return "software";
    case Cause::Brownout: return "brownout";
    default: return "crash";
  }
}

Policy OnInterrupt::of(Cause c) const {
  switch (c) {
    case Cause::Power: return power;
    case Cause::Software: return software;
    case Cause::Brownout: return brownout;
    default: return crash;
  }
}

Decision decide(Cause cause, const OnInterrupt& policy, bool gapKnown, uint32_t gapS, bool defChanged) {
  Decision d;
  d.action = policy.of(cause);
  const std::string rule = std::string("on_interrupt.") + causeName(cause) + " = " + policyName(d.action);
  d.why = rule;
  if (d.action != Policy::Resume) return d;
  if (defChanged) {
    d.action = Policy::Restart;
    d.why = rule + ", but the scenario was edited since the run began: started again";
  } else if (!gapKnown && cause == Cause::Power) {
    d.action = Policy::Restart;
    d.why = rule + ", but how long the power was off is not known (no clock yet): started again";
  } else if (gapKnown && gapS > static_cast<uint32_t>(policy.resumeWithinMin) * 60) {
    d.action = Policy::Restart;
    d.why = rule + ", but the board was away " + std::to_string(gapS / 60) + " min, longer than resume_within_min " +
            std::to_string(policy.resumeWithinMin) + ": started again";
  }
  return d;
}

uint32_t defHash(const Def& d) {
  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      h ^= (v >> (8 * i)) & 0xff;
      h *= 16777619u;
    }
  };
  auto mixText = [&](const Text& s) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 16777619u;
    }
    mix(static_cast<uint32_t>(s.size()));
  };
  mix(d.defaultPauseS);
  mix(static_cast<uint32_t>(d.steps.size()));
  for (size_t i = 0; i < d.steps.size(); ++i) {
    if (d.isWait(i)) {
      mix(0x57414954u);
      mix(d.steps[i].waitS);
    } else if (d.isHttp(i)) {  // (a device step hashes as it always did: a saved run of an older firmware still matches)
      const HttpReq& q = d.request(i);
      mix(0x48545450u | (q.post ? 1u : 0u));
      mixText(q.url);
      mixText(q.contentType);
      mixText(q.body);
      mix(q.timeoutMs);
    } else {
      mixText(d.device(i));
    }
    mix((d.steps[i].on ? 1u : 0u) | (d.steps[i].enabled ? 2u : 0u));
    mix(static_cast<uint32_t>(d.steps[i].pauseAfterS));
  }
  return h;
}

std::string stepDetail(const Run& r, size_t i) {
  if (i >= r.steps.size()) return std::string();
  const StepRun& s = r.steps[i];
  switch (s.state) {
    case StepState::Ok: {
      if (i < r.def.steps.size() && r.def.isWait(i)) return "waited " + std::to_string(r.def.steps[i].waitS) + " s";
      const bool http = i < r.def.steps.size() && r.def.isHttp(i);
      const bool on = i < r.def.steps.size() && r.def.steps[i].on;
      std::string out = http ? "sent: " + str(r.whys.get(i)) : std::string("switched ") + verb(on);
      if (s.attempts > 1) out += " after " + std::to_string(s.attempts) + " attempts";
      if (s.pauseS) out += "; pause " + std::to_string(s.pauseS) + " s before the next step";
      return out;
    }
    case StepState::Failed: return str(r.whys.get(i));
    case StepState::Skipped: return "the device was ignored";  // runs from firmware before ignore stopped skipping steps
    case StepState::Stopped: return "stopped";
    case StepState::Running: return s.attempts > 1 ? "previous attempt: " + str(r.whys.get(i)) : std::string();
    default: return std::string();
  }
}

void Engine::defsChanged() {
  for (auto it = runs_.begin(); it != runs_.end();) {
    if (env_.known(it->first)) {
      ++it;
    } else if (it->second.alive) {  // removed: stop a live run, forget a finished one
      it->second.stop = true;
      ++it;
    } else {
      it = runs_.erase(it);
    }
  }
}

Engine::StartResult Engine::start(const std::string& name, const std::string& trigger, uint32_t startAtMs, bool waitNet, bool quiet) {
  if (!env_.known(name)) return StartResult::NotFound;
  auto it = runs_.find(name);
  if (it != runs_.end() && it->second.alive) {
    Run& r = it->second;
    if (r.state == RunState::Waiting) {  // cut the wait short; the run carries on
      r.startAtMs = env_.nowMs();
      r.trigger = trigger;
      logf(false, "[%s] %s start: the waiting run starts now", name.c_str(), trigger.c_str());
      return StartResult::Started;
    }
    return StartResult::AlreadyRunning;
  }
  Run& r = runs_[name];
  r = Run();
  r.state = RunState::Waiting;
  r.trigger = trigger;
  r.startAtMs = startAtMs;
  r.alive = true;
  r.needNet = waitNet;
  r.quiet = quiet;
  r.phase = Phase::WaitStart;
  if (!quiet) dirty_ = true;
  quiet_ = quiet;
  if (trigger != "power-on") logf(false, "[%s] start requested (%s)", name.c_str(), trigger.c_str());
  quiet_ = false;
  return StartResult::Started;
}

Engine::StopResult Engine::stop(const std::string& name) {
  if (!env_.known(name)) return StopResult::NotFound;
  auto it = runs_.find(name);
  if (it == runs_.end() || !it->second.alive) return StopResult::NotRunning;
  it->second.stop = true;
  logf(false, "[%s] stop requested", name.c_str());  // asked for by hand: in the usual log even for a quiet run
  return StopResult::Stopping;
}

void Engine::skip(const std::string& name, const std::string& reason) {
  Run& r = runs_[name];
  r = Run();
  r.state = RunState::Skipped;
  r.skipReason.assign(reason.data(), reason.size());
}

void Engine::unskip(const std::string& name) {
  auto it = runs_.find(name);
  if (it != runs_.end() && !it->second.alive && it->second.state == RunState::Skipped) it->second.state = RunState::Idle;
}

const Run* Engine::run(const std::string& name) const {
  auto it = runs_.find(name);
  return it == runs_.end() ? nullptr : &it->second;
}

size_t Engine::alive() const {
  size_t n = 0;
  for (const auto& kv : runs_) n += kv.second.alive ? 1 : 0;
  return n;
}

// ---- the machine -----------------------------------------------------------------------------------

uint32_t Engine::tick() {
  const uint32_t now = env_.nowMs();
  for (auto& kv : runs_) {
    if (!kv.second.alive) continue;
    quiet_ = kv.second.quiet;
    advance(kv.first, kv.second, now);
    quiet_ = false;
  }
  grantSlots(now);
  const uint32_t sig = signature();
  if (sig != lastSignature_) {
    lastSignature_ = sig;
    dirty_ = true;
  }

  uint32_t sleep = kIdleTickMs;
  for (const auto& kv : runs_) {
    const Run& r = kv.second;
    if (!r.alive) continue;
    switch (r.phase) {
      case Phase::WaitStart: sleep = std::min(sleep, until(now, r.startAtMs)); break;
      case Phase::WaitNet: sleep = std::min<uint32_t>(sleep, 250); break;
      case Phase::Settle:
      case Phase::Pause:
      case Phase::Wait:
      case Phase::Gap: sleep = std::min(sleep, until(now, r.deadlineMs)); break;
      case Phase::Command:
      case Phase::Verify: sleep = std::min(sleep, kBusyTickMs); break;
      default: sleep = 0; break;  // NextStep: look again at once
    }
  }
  return std::max<uint32_t>(sleep, 10);
}

void Engine::advance(const std::string& name, Run& r, uint32_t now) {
  for (int guard = 0; guard < 256; ++guard) {  // every step of a scenario can be passed in one go (disabled, missing)
    switch (r.phase) {
      case Phase::WaitStart:
        if (r.stop) {
          logf(false, "[%s] stopped while waiting", name.c_str());
          finish(name, r, RunState::Stopped);
          return;
        }
        if (!due(now, r.startAtMs)) return;
        if (r.trigger == "power-on" && !env_.wifiUp()) {
          logf(false, "[%s] waiting for Wi-Fi", name.c_str());
          r.waitReason = "wifi";
          r.phase = Phase::WaitNet;
          return;
        }
        beginRunning(name, r, now);
        continue;

      case Phase::WaitNet:
      case Phase::Settle:
        if (r.stop) {
          logf(false, "[%s] stopped while waiting for Wi-Fi", name.c_str());
          finish(name, r, RunState::Stopped);
          return;
        }
        if (!r.netGate && r.trigger != "power-on") {  // started by hand meanwhile: no more waiting
          beginRunning(name, r, now);
          continue;
        }
        if (r.phase == Phase::WaitNet) {
          if (!env_.wifiUp()) return;
          logf(false, "[%s] Wi-Fi is up, %u s for the sockets to join", name.c_str(), static_cast<unsigned>(kNetSettleMs / 1000));
          r.deadlineMs = now + kNetSettleMs;
          r.phase = Phase::Settle;
          return;
        }
        if (!due(now, r.deadlineMs)) return;
        if (r.netGate) {  // a run continued after a restart: on with its steps
          r.netGate = false;
          r.waitReason.clear();
          r.phase = Phase::NextStep;
          continue;
        }
        beginRunning(name, r, now);
        continue;

      case Phase::NextStep:
        nextStep(name, r, now);
        if (r.phase == Phase::NextStep) continue;  // a step was passed (missing, ignored): look at the next one
        if (r.phase == Phase::Finished || r.phase == Phase::Pause || r.phase == Phase::Command || r.phase == Phase::Wait) return;
        continue;

      case Phase::Pause:
        if (r.stop) {
          finish(name, r, RunState::Stopped);
          return;
        }
        if (!due(now, r.deadlineMs)) return;
        r.phase = Phase::NextStep;
        continue;

      case Phase::Wait: {
        if (r.stop) {
          stopNow(name, r, true, 1);
          return;
        }
        if (!due(now, r.deadlineMs)) return;
        StepRun& sr = r.steps[r.step];
        sr.state = StepState::Ok;
        sr.attempts = 1;
        stepEnded(r, now);
        ++r.step;  // no pause after a wait step: its seconds are the pause
        r.phase = Phase::NextStep;
        continue;
      }

      case Phase::Gap:
        if (r.stop) {
          stopNow(name, r, true, r.attempt);
          return;
        }
        if (!due(now, r.deadlineMs)) return;
        startAttempt(name, r, now);
        return;

      case Phase::Command:
      case Phase::Verify: {
        const bool verify = r.phase == Phase::Verify;
        const bool http = r.def.isHttp(r.step);
        const char* lateWhy = http     ? "the request did not get through the HTTP queue in time"
                              : verify ? "the command was accepted, but the status query did not get through in time"
                                       : "the command did not get through the device queue in time";
        if (r.job == 0) {  // still waiting for a place (or for room in the Tuya list)
          if (!verify && r.stop) {  // nothing has gone out for this attempt yet
            stopNow(name, r, true, static_cast<uint8_t>(r.attempt - 1));
            return;
          }
          if (!due(now, r.askedMs + kQueueWaitMs)) return;
          r.wantsSlot = false;
          attemptFailed(name, r, now, lateWhy);
          continue;
        }
        const JobStatus st = env_.poll(r.job);
        if (!st.done) {
          if (!due(now, r.askedMs + kQueueWaitMs)) return;
          // Given up: a job still waiting in the list is withdrawn, so it does not reach the socket minutes later.
          dropJob(r, true);
          attemptFailed(name, r, now, lateWhy);
          continue;
        }
        dropJob(r, false);
        r.attemptJobMs += st.ms;
        if (http) {  // the server's answer is the result: no status query after it
          if (st.ok) {
            attemptSucceeded(name, r, now, st.detail);
          } else {
            attemptFailed(name, r, now, st.error + ": " + st.detail);
          }
          continue;
        }
        if (!verify) {
          if (!st.ok) {
            attemptFailed(name, r, now, st.error + ": " + st.detail);
            continue;
          }
          // The query's answer updates the cached state (the observer runs before the job is done).
          r.phase = Phase::Verify;
          r.wantsSlot = true;
          r.slotWaitCounted = r.queueFullCounted = false;
          r.askedMs = now;
          return;
        }
        if (!st.ok) {
          attemptFailed(name, r, now, "the command was accepted, but the status query failed: " + st.error);
          continue;
        }
        const bool on = r.def.steps[r.step].on;
        if (env_.device(str(r.def.device(r.step))).switchState != (on ? 1 : 0)) {
          attemptFailed(name, r, now, std::string("the command was accepted, but the status does not show it ") + verb(on));
          continue;
        }
        attemptSucceeded(name, r, now);
        continue;
      }

      case Phase::Finished:
        return;
    }
  }
}

void Engine::beginRunning(const std::string& name, Run& r, uint32_t now) {
  r.waitReason.clear();
  r.startedMs = now;
  r.carriedMs = 0;
  r.startEpoch = env_.epoch();
  Def def;
  if (!env_.def(name, def)) {
    logf(true, "[%s] removed before it started", name.c_str());
    r.skipReason = "the scenario was removed before it started";
    finish(name, r, RunState::Skipped);
    return;
  }
  r.def = std::move(def);  // the steps as they are now: an edit made while the run was waiting counts
  r.state = RunState::Running;
  r.patient = r.trigger == "power-on";
  r.steps.assign(r.def.steps.size(), StepRun());
  for (size_t i = 0; i < r.def.steps.size(); ++i) {
    if (!r.def.steps[i].enabled) r.steps[i].state = StepState::Disabled;
  }
  r.step = 0;
  r.pauseS = 0;
  r.switched = r.skipped = r.failed = 0;
  logf(false, "[%s] started (%s), %u step(s)", name.c_str(), r.trigger.c_str(), static_cast<unsigned>(r.def.steps.size()));
  r.phase = Phase::NextStep;
}

void Engine::nextStep(const std::string& name, Run& r, uint32_t now) {
  const size_t n = r.def.steps.size();
  while (r.step < n && !r.def.steps[r.step].enabled) ++r.step;
  if (r.step >= n) {
    finish(name, r, RunState::Done);
    return;
  }
  if (r.pauseS > 0) {  // only after a step that really switched something
    if (r.stop) {
      finish(name, r, RunState::Stopped);
      return;
    }
    logf(false, "[%s] pause %u s before step %u", name.c_str(), static_cast<unsigned>(r.pauseS), static_cast<unsigned>(r.step + 1));
    if (r.pauseFrom < r.steps.size()) r.steps[r.pauseFrom].pauseS = static_cast<uint16_t>(r.pauseS);
    r.current = -1;
    r.deadlineMs = now + r.pauseS * 1000UL;
    r.pauseS = 0;
    r.phase = Phase::Pause;
    return;
  }
  if (r.stop) {
    stopNow(name, r, true, 0);
    return;
  }
  if (r.def.isWait(r.step)) {  // needs no network: a continued run waits for Wi-Fi before its next command, not here
    r.current = static_cast<int>(r.step);
    StepRun& sr = r.steps[r.step];
    sr.state = StepState::Running;
    sr.attempts = 1;
    r.whys.set(r.step, Text());
    r.deadlineMs = now + r.def.steps[r.step].waitS * 1000UL;
    logf(false, "[%s] step %u: %s", name.c_str(), static_cast<unsigned>(r.step + 1), r.def.label(r.step).c_str());
    r.phase = Phase::Wait;
    return;
  }
  if (r.needNet) {  // continued after a restart: the next command needs the network
    r.needNet = false;
    if (!env_.wifiUp()) {
      logf(false, "[%s] waiting for Wi-Fi before step %u", name.c_str(), static_cast<unsigned>(r.step + 1));
      r.netGate = true;
      r.waitReason = "wifi";
      r.current = -1;
      r.phase = Phase::WaitNet;
      return;
    }
  }
  r.current = static_cast<int>(r.step);
  if (r.def.isHttp(r.step)) {
    r.attempt = 0;
    r.windowEndMs = now + kPatientWindowMs;
    r.lastWhy.clear();
    startAttempt(name, r, now);
    return;
  }
  const Text& device = r.def.device(r.step);
  const DeviceInfo d = env_.device(str(device));
  StepRun& sr = r.steps[r.step];
  if (!d.exists) {
    sr.state = StepState::Failed;
    sr.attempts = 0;
    r.whys.set(r.step, "no such device in /devices.json");
    stepEnded(r, now);
    logf(true, "[%s] step %u: no device '%s' in /devices.json", name.c_str(), static_cast<unsigned>(r.step + 1), device.c_str());
    ++r.failed;
    ++r.step;
    return;
  }
  r.attempt = 0;
  r.windowEndMs = now + kPatientWindowMs;
  r.lastWhy.clear();
  startAttempt(name, r, now);
}

void Engine::startAttempt(const std::string& name, Run& r, uint32_t now) {
  ++r.attempt;
  if (r.stop) {
    stopNow(name, r, true, static_cast<uint8_t>(r.attempt - 1));
    return;
  }
  StepRun& sr = r.steps[r.step];
  sr.state = StepState::Running;
  sr.attempts = r.attempt;
  r.whys.set(r.step, r.attempt > 1 ? r.lastWhy : Text());
  r.offlineAttempt = !r.def.isHttp(r.step) && env_.device(str(r.def.device(r.step))).offline;
  r.attemptJobMs = 0;
  r.phase = Phase::Command;
  r.wantsSlot = true;
  r.slotWaitCounted = r.queueFullCounted = false;
  r.askedMs = now;
}

void Engine::endAttemptMeasurement(Run& r, bool ok) {
  if (!r.offlineAttempt) return;
  ++stats_.offlineAttempts;
  if (ok) ++stats_.offlineAttemptsOk;
  stats_.offlineAttemptMs += r.attemptJobMs;
  r.offlineAttempt = false;
}

void Engine::attemptFailed(const std::string& name, Run& r, uint32_t now, const std::string& why) {
  endAttemptMeasurement(r, false);
  const std::string step = what(r.def, r.step);
  r.lastWhy.assign(why.data(), why.size());
  logf(true, "[%s] step %u: %s attempt %u failed: %s", name.c_str(), static_cast<unsigned>(r.step + 1), step.c_str(),
       static_cast<unsigned>(r.attempt), why.c_str());
  const uint32_t gap = r.patient ? kPatientGapMs : kRetryGapMs;
  const bool more = r.patient ? static_cast<int32_t>(r.windowEndMs - now) > static_cast<int32_t>(gap) && r.attempt < 255 : r.attempt < kAttempts;
  if (more) {
    r.deadlineMs = now + gap;
    r.phase = Phase::Gap;
    return;
  }
  StepRun& sr = r.steps[r.step];
  sr.state = StepState::Failed;
  sr.attempts = r.attempt;
  r.whys.set(r.step, r.lastWhy);
  stepEnded(r, now);
  logf(true, "[%s] step %u: %s FAILED after %u attempts, going on", name.c_str(), static_cast<unsigned>(r.step + 1),
       r.def.label(r.step).c_str(), static_cast<unsigned>(r.attempt));
  ++r.failed;
  ++r.step;
  r.phase = Phase::NextStep;
}

void Engine::attemptSucceeded(const std::string& name, Run& r, uint32_t now, const std::string& note) {
  endAttemptMeasurement(r, true);
  const StepDef& st = r.def.steps[r.step];
  StepRun& sr = r.steps[r.step];
  sr.state = StepState::Ok;
  sr.attempts = r.attempt;
  r.whys.set(r.step, Text(note.data(), note.size()));  // an HTTP step: what the server answered (stepDetail shows it)
  stepEnded(r, now);
  if (r.def.isHttp(r.step)) {
    logf(false, "[%s] step %u: %s: %s (attempt %u)", name.c_str(), static_cast<unsigned>(r.step + 1), r.def.label(r.step).c_str(),
         note.c_str(), static_cast<unsigned>(r.attempt));
  } else {
    logf(false, "[%s] step %u: %s is %s (attempt %u)", name.c_str(), static_cast<unsigned>(r.step + 1), r.def.device(r.step).c_str(),
         verb(st.on), static_cast<unsigned>(r.attempt));
  }
  ++r.switched;
  r.pauseS = st.pauseAfterS >= 0 ? static_cast<uint32_t>(st.pauseAfterS) : r.def.defaultPauseS;
  r.pauseFrom = r.step;
  ++r.step;
  r.phase = Phase::NextStep;
}

void Engine::stopNow(const std::string& name, Run& r, bool markStep, uint8_t attempts) {
  if (r.job) dropJob(r, true);
  if (markStep && r.step < r.steps.size()) {
    StepRun& sr = r.steps[r.step];
    sr.state = StepState::Stopped;
    sr.attempts = attempts;
    r.whys.set(r.step, "");
    stepEnded(r, env_.nowMs());
  }
  finish(name, r, RunState::Stopped);
}

void Engine::finish(const std::string& name, Run& r, RunState state) {
  const bool ran = r.state == RunState::Running;
  r.durationS = ran ? runSeconds(r, env_.nowMs()) : 0;
  if (ran && r.startEpoch == 0) {  // the clock came later: count back
    const uint32_t e = env_.epoch();
    if (e > r.durationS) r.startEpoch = e - r.durationS;
  }
  r.state = state;
  r.current = -1;
  r.alive = false;
  r.phase = Phase::Finished;
  r.wantsSlot = false;
  r.needNet = r.netGate = false;
  r.waitReason.clear();
  if (!r.quiet) dirty_ = true;
  env_.finished(name, r);
  if (ran) {
    logf(false, "[%s] %s: %u switched, %u skipped, %u failed", name.c_str(), state == RunState::Stopped ? "stopped" : "finished", r.switched,
         r.skipped, r.failed);
  }
}

void Engine::dropJob(Run& r, bool withdraw) {
  env_.release(r.job, withdraw);
  r.job = 0;
  if (r.jobHttp) {
    if (httpInFlight_) --httpInFlight_;
  } else if (stats_.inFlight) {
    --stats_.inFlight;
  }
  r.jobHttp = false;
}

// Hands the free places (kMaxInFlight in the Tuya lane, kMaxHttpInFlight in the HTTP lane) to the runs that asked for
// one, oldest request first.
void Engine::grantSlots(uint32_t now) {
  std::vector<Run*> asking;
  for (auto& kv : runs_) {
    Run& r = kv.second;
    if (r.alive && r.wantsSlot && r.job == 0 && (r.phase == Phase::Command || r.phase == Phase::Verify)) asking.push_back(&r);
  }
  std::vector<size_t> order(asking.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return now - asking[a]->askedMs > now - asking[b]->askedMs; });

  bool full = false, httpFull = false;
  for (size_t k : order) {
    Run& r = *asking[k];
    if (r.def.isHttp(r.step)) {  // the HTTP lane: its own places, no slot statistics (those are about the sockets)
      if (httpFull || httpInFlight_ >= kMaxHttpInFlight) continue;
      const uint32_t id = env_.submitHttp(r.def.request(r.step), r.quiet, r.def.name, r.trigger);
      if (!id) {  // its list is full (the API's test requests): try again on the next look
        httpFull = true;
        continue;
      }
      r.job = id;
      r.jobHttp = true;
      r.wantsSlot = false;
      ++httpInFlight_;
      ++stats_.httpRequests;
      continue;
    }
    if (full || stats_.inFlight >= kMaxInFlight) {
      if (!r.slotWaitCounted) {
        r.slotWaitCounted = true;
        ++stats_.slotWaits;
      }
      continue;
    }
    const StepDef& st = r.def.steps[r.step];
    const Op op = r.phase == Phase::Verify ? Op::Query : st.on ? Op::On : Op::Off;
    const uint32_t id = env_.submit(op, str(r.def.device(r.step)));
    if (!id) {  // the Tuya command list is full (buttons, the API): try again on the next look
      if (!r.queueFullCounted) {
        r.queueFullCounted = true;
        ++stats_.queueFull;
      }
      full = true;
      continue;
    }
    r.job = id;
    r.wantsSlot = false;
    ++stats_.jobs;
    ++stats_.inFlight;
    stats_.maxInFlightSeen = std::max(stats_.maxInFlightSeen, stats_.inFlight);
    if (r.slotWaitCounted) stats_.slotWaitMaxMs = std::max(stats_.slotWaitMaxMs, now - r.askedMs);
  }
}

uint32_t Engine::runSeconds(const Run& r, uint32_t now) const { return (r.carriedMs + (now - r.startedMs)) / 1000; }

void Engine::stepEnded(Run& r, uint32_t now) {
  if (r.step < r.steps.size()) r.steps[r.step].atS = std::max<uint32_t>(1, runSeconds(r, now));
}

// Changes when something worth saving in /runs.json changes (a phase, a step, an attempt, a run starting or ending).
uint32_t Engine::signature() const {
  uint32_t h = 2166136261u;
  for (const auto& kv : runs_) {
    const Run& r = kv.second;
    if (r.quiet) continue;  // not kept in /runs.json: its steps are no reason to write it
    const uint32_t v = (r.alive ? 1u : 0u) | (static_cast<uint32_t>(r.phase) << 1) | (static_cast<uint32_t>(r.step) << 5) |
                       (static_cast<uint32_t>(r.attempt) << 13) | (static_cast<uint32_t>(r.state) << 21);
    h = (h ^ v) * 16777619u;
  }
  return h;
}

bool Engine::takeDirty() {
  const bool d = dirty_;
  dirty_ = false;
  return d;
}

std::vector<Snapshot> Engine::snapshot() const {
  std::vector<Snapshot> out;
  forEachSnapshot([&](const Snapshot& s) { out.push_back(s); });
  return out;
}

void Engine::forEachSnapshot(const std::function<void(const Snapshot&)>& fn) const {
  const uint32_t now = env_.nowMs();
  for (const auto& kv : runs_) {
    const Run& r = kv.second;
    if (!r.alive || r.quiet) continue;  // a quiet run is not continued after a restart: its next tick comes anyway
    Snapshot s;
    s.name = kv.first;
    s.trigger = r.trigger;
    s.state = r.state;
    s.phase = r.phase;
    s.patient = r.patient;
    s.step = static_cast<uint32_t>(r.step);
    s.attempt = r.attempt;
    switch (r.phase) {
      case Phase::WaitStart: s.leftMs = until(now, r.startAtMs); break;
      case Phase::Settle:
      case Phase::Pause:
      case Phase::Gap: s.leftMs = until(now, r.deadlineMs); break;
      default: break;
    }
    if (r.state == RunState::Running) {
      s.runMs = r.carriedMs + (now - r.startedMs);
      s.def = r.def;
      s.defHash = defHash(r.def);
    }
    s.startEpoch = r.startEpoch;
    s.resumed = r.resumed;
    s.steps = r.steps;
    s.whys = r.whys;
    s.switched = r.switched;
    s.skipped = r.skipped;
    s.failed = r.failed;
    fn(s);
  }
}

Engine::StartResult Engine::resume(const Snapshot& s, uint32_t elapsedMs, const std::string& cause) {
  if (!env_.known(s.name)) return StartResult::NotFound;
  auto it = runs_.find(s.name);
  if (it != runs_.end() && it->second.alive) return StartResult::AlreadyRunning;
  const uint32_t now = env_.nowMs();
  auto left = [&](uint32_t ms) { return ms > elapsedMs ? ms - elapsedMs : 0; };
  Run& r = runs_[s.name];
  r = Run();
  r.alive = true;
  r.trigger = s.trigger;
  r.patient = s.patient;
  r.startEpoch = s.startEpoch;
  if (s.state == RunState::Running) {
    Def def;
    if (!env_.def(s.name, def)) {
      runs_.erase(s.name);
      return StartResult::NotFound;
    }
    r.def = std::move(def);
    r.state = RunState::Running;
    r.steps = s.steps;
    r.steps.resize(r.def.steps.size());
    r.whys = s.whys;
    r.step = std::min<size_t>(s.step, r.def.steps.size());
    r.switched = s.switched;
    r.skipped = s.skipped;
    r.failed = s.failed;
    r.startedMs = now;
    r.carriedMs = s.runMs + elapsedMs;
    r.needNet = true;
    const std::string at = cause + " at step " + std::to_string(r.step + 1);
    r.resumed = s.resumed;
    if (!r.resumed.empty()) r.resumed += "; ";
    r.resumed += at.c_str();
    if (s.phase == Phase::Pause) {
      r.phase = Phase::Pause;
      r.deadlineMs = now + left(s.leftMs);
      r.current = -1;
      logf(false, "[%s] continued after a restart (%s): %u s of the pause left", s.name.c_str(), cause.c_str(),
           static_cast<unsigned>(left(s.leftMs) / 1000));
    } else {
      r.phase = Phase::NextStep;
      if (r.step < r.steps.size() && (r.steps[r.step].state == StepState::Running || r.steps[r.step].state == StepState::Pending)) {
        StepRun& sr = r.steps[r.step];  // the step it was in is done again
        sr.state = StepState::Pending;
        sr.attempts = 0;
        r.whys.set(r.step, "");
      }
      logf(false, "[%s] continued after a restart (%s) at step %u", s.name.c_str(), cause.c_str(), static_cast<unsigned>(r.step + 1));
    }
  } else {  // it was waiting for its start (or for Wi-Fi before it): it waits again
    r.state = RunState::Waiting;
    r.phase = Phase::WaitStart;
    r.startAtMs = now + (s.phase == Phase::WaitStart ? left(s.leftMs) : 0);
    r.resumed = s.resumed;
    if (!r.resumed.empty()) r.resumed += "; ";
    r.resumed += (cause + " while waiting").c_str();
    logf(false, "[%s] continued after a restart (%s): waits for its start", s.name.c_str(), cause.c_str());
  }
  dirty_ = true;
  return StartResult::Started;
}

void Engine::interrupted(const Snapshot& s, const std::string& why) {
  Run& r = runs_[s.name];
  r = Run();
  r.state = RunState::Interrupted;
  r.trigger = s.trigger;
  r.skipReason.assign(why.data(), why.size());
  r.def = s.def;
  r.steps = s.steps;
  r.steps.resize(r.def.steps.size());
  r.whys = s.whys;
  for (size_t i = 0; i < r.steps.size(); ++i) {
    if (r.steps[i].state == StepState::Running) {
      r.steps[i].state = StepState::Stopped;
      r.whys.set(i, "");
    }
  }
  r.switched = s.switched;
  r.skipped = s.skipped;
  r.failed = s.failed;
  r.startEpoch = s.startEpoch;
  r.resumed = s.resumed;
  r.durationS = s.runMs / 1000;
  r.phase = Phase::Finished;
  logf(true, "[%s] %s", s.name.c_str(), why.c_str());  // `why` says it all: "interrupted by a restart (SW): ..."
  env_.finished(s.name, r);
  dirty_ = true;
}

}  // namespace ScenarioEngine
