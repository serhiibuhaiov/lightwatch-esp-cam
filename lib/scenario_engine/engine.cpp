#include "engine.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace ScenarioEngine {
namespace {

bool due(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }
uint32_t until(uint32_t now, uint32_t at) {
  const int32_t d = static_cast<int32_t>(at - now);
  return d > 0 ? static_cast<uint32_t>(d) : 0;
}
const char* verb(bool on) { return on ? "on" : "off"; }

}  // namespace

const char* runStateName(RunState s) {
  switch (s) {
    case RunState::Skipped: return "skipped";
    case RunState::Waiting: return "waiting";
    case RunState::Running: return "running";
    case RunState::Done: return "done";
    case RunState::Stopped: return "stopped";
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
  env_.log(warn, buf);
}

// ---- outside requests ----------------------------------------------------------------------------

std::string stepDetail(const Run& r, size_t i) {
  if (i >= r.steps.size()) return std::string();
  const StepRun& s = r.steps[i];
  switch (s.state) {
    case StepState::Ok: {
      const bool on = i < r.def.steps.size() && r.def.steps[i].on;
      std::string out = std::string("switched ") + verb(on);
      if (s.attempts > 1) out += " after " + std::to_string(s.attempts) + " attempts";
      if (s.pauseS) out += "; pause " + std::to_string(s.pauseS) + " s before the next step";
      return out;
    }
    case StepState::Failed: return s.why;
    case StepState::Skipped: return "the device is ignored (see the device settings)";
    case StepState::Stopped: return "stopped";
    case StepState::Running: return s.attempts > 1 ? "previous attempt: " + s.why : std::string();
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

Engine::StartResult Engine::start(const std::string& name, const std::string& trigger, uint32_t startAtMs, bool holdsPowerUp) {
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
  r.holdsPowerUp = holdsPowerUp;
  r.phase = Phase::WaitStart;
  if (!holdsPowerUp) logf(false, "[%s] start requested (%s)", name.c_str(), trigger.c_str());
  return StartResult::Started;
}

Engine::StopResult Engine::stop(const std::string& name) {
  if (!env_.known(name)) return StopResult::NotFound;
  auto it = runs_.find(name);
  if (it == runs_.end() || !it->second.alive) return StopResult::NotRunning;
  it->second.stop = true;
  logf(false, "[%s] stop requested", name.c_str());
  return StopResult::Stopping;
}

void Engine::skip(const std::string& name, const std::string& reason) {
  Run& r = runs_[name];
  r = Run();
  r.state = RunState::Skipped;
  r.skipReason = reason;
}

void Engine::unskip(const std::string& name) {
  auto it = runs_.find(name);
  if (it != runs_.end() && !it->second.alive && it->second.state == RunState::Skipped) it->second.state = RunState::Idle;
}

const Run* Engine::run(const std::string& name) const {
  auto it = runs_.find(name);
  return it == runs_.end() ? nullptr : &it->second;
}

bool Engine::holdsPowerUp() const {
  for (const auto& kv : runs_) {
    if (kv.second.holdsPowerUp) return true;
  }
  return false;
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
    if (kv.second.alive) advance(kv.first, kv.second, now);
  }
  grantSlots(now);

  uint32_t sleep = kIdleTickMs;
  for (const auto& kv : runs_) {
    const Run& r = kv.second;
    if (!r.alive) continue;
    switch (r.phase) {
      case Phase::WaitStart: sleep = std::min(sleep, until(now, r.startAtMs)); break;
      case Phase::WaitNet: sleep = std::min<uint32_t>(sleep, 250); break;
      case Phase::Settle:
      case Phase::Pause:
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
        if (r.trigger != "power-on") {  // started by hand meanwhile: no more waiting
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
        beginRunning(name, r, now);
        continue;

      case Phase::NextStep:
        nextStep(name, r, now);
        if (r.phase == Phase::NextStep) continue;  // a step was passed (missing, ignored): look at the next one
        if (r.phase == Phase::Finished || r.phase == Phase::Pause || r.phase == Phase::Command) return;
        continue;

      case Phase::Pause:
        if (r.stop) {
          finish(name, r, RunState::Stopped);
          return;
        }
        if (!due(now, r.deadlineMs)) return;
        r.phase = Phase::NextStep;
        continue;

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
        const char* lateWhy = verify ? "the command was accepted, but the status query did not get through in time"
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
        if (env_.device(r.def.device(r.step)).switchState != (on ? 1 : 0)) {
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
  (void)now;
  r.holdsPowerUp = false;
  r.waitReason.clear();
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
  r.current = static_cast<int>(r.step);
  const std::string& device = r.def.device(r.step);
  const DeviceInfo d = env_.device(device);
  StepRun& sr = r.steps[r.step];
  if (!d.exists) {
    sr.state = StepState::Failed;
    sr.attempts = 0;
    sr.why = "no such device in /devices.json";
    logf(true, "[%s] step %u: no device '%s' in /devices.json", name.c_str(), static_cast<unsigned>(r.step + 1), device.c_str());
    ++r.failed;
    ++r.step;
    return;
  }
  if (d.ignored) {
    sr.state = StepState::Skipped;
    sr.attempts = 0;
    logf(false, "[%s] step %u: %s is ignored, skipped", name.c_str(), static_cast<unsigned>(r.step + 1), device.c_str());
    ++r.skipped;
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
  sr.why = r.attempt > 1 ? r.lastWhy : std::string();
  r.offlineAttempt = env_.device(r.def.device(r.step)).offline;
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
  const StepDef& st = r.def.steps[r.step];
  const std::string& device = r.def.device(r.step);
  r.lastWhy = why;
  logf(true, "[%s] step %u: %s %s attempt %u failed: %s", name.c_str(), static_cast<unsigned>(r.step + 1), device.c_str(), verb(st.on),
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
  sr.why = r.lastWhy;
  logf(true, "[%s] step %u: %s FAILED after %u attempts, going on", name.c_str(), static_cast<unsigned>(r.step + 1), device.c_str(),
       static_cast<unsigned>(r.attempt));
  ++r.failed;
  ++r.step;
  r.phase = Phase::NextStep;
}

void Engine::attemptSucceeded(const std::string& name, Run& r, uint32_t now) {
  (void)now;
  endAttemptMeasurement(r, true);
  const StepDef& st = r.def.steps[r.step];
  StepRun& sr = r.steps[r.step];
  sr.state = StepState::Ok;
  sr.attempts = r.attempt;
  sr.why.clear();
  logf(false, "[%s] step %u: %s is %s (attempt %u)", name.c_str(), static_cast<unsigned>(r.step + 1), r.def.device(r.step).c_str(), verb(st.on),
       static_cast<unsigned>(r.attempt));
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
    sr.why.clear();
  }
  finish(name, r, RunState::Stopped);
}

void Engine::finish(const std::string& name, Run& r, RunState state) {
  const bool ran = r.state == RunState::Running;
  r.state = state;
  r.current = -1;
  r.alive = false;
  r.phase = Phase::Finished;
  r.wantsSlot = false;
  r.holdsPowerUp = false;
  r.waitReason.clear();
  if (ran) {
    logf(false, "[%s] %s: %u switched, %u skipped, %u failed", name.c_str(), state == RunState::Stopped ? "stopped" : "finished", r.switched,
         r.skipped, r.failed);
  }
}

void Engine::dropJob(Run& r, bool withdraw) {
  env_.release(r.job, withdraw);
  r.job = 0;
  if (stats_.inFlight) --stats_.inFlight;
}

// Hands the free places (kMaxInFlight in all) to the runs that asked for one, oldest request first.
void Engine::grantSlots(uint32_t now) {
  std::vector<Run*> asking;
  for (auto& kv : runs_) {
    Run& r = kv.second;
    if (r.alive && r.wantsSlot && r.job == 0 && (r.phase == Phase::Command || r.phase == Phase::Verify)) asking.push_back(&r);
  }
  std::vector<size_t> order(asking.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return now - asking[a]->askedMs > now - asking[b]->askedMs; });

  bool full = false;
  for (size_t k : order) {
    Run& r = *asking[k];
    if (full || stats_.inFlight >= kMaxInFlight) {
      if (!r.slotWaitCounted) {
        r.slotWaitCounted = true;
        ++stats_.slotWaits;
      }
      continue;
    }
    const StepDef& st = r.def.steps[r.step];
    const Op op = r.phase == Phase::Verify ? Op::Query : st.on ? Op::On : Op::Off;
    const uint32_t id = env_.submit(op, r.def.device(r.step));
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

}  // namespace ScenarioEngine
