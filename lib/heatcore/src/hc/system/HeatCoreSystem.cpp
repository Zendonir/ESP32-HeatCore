#include "hc/system/HeatCoreSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/core/Log.h"

namespace hc {

namespace {
constexpr const char* TAG = "system";
const char* const kSectionNames[] = {"system", "network", "mqtt",    "io",      "heating", "sensors",
                                     "safety", "rf",      "display", "rules",   "model"};
static_assert(sizeof(kSectionNames) / sizeof(kSectionNames[0]) == static_cast<size_t>(ConfigSection::Count), "");

bool validTopic(const std::string& t) {
  return !t.empty() && t.size() <= 23 && t.find_first_of("#+ ") == std::string::npos && t[0] != '/';
}
}  // namespace

const char* configSectionName(ConfigSection s) { return kSectionNames[static_cast<int>(s)]; }
bool configSectionFromName(const char* s, ConfigSection& out) {
  for (size_t i = 0; i < static_cast<size_t>(ConfigSection::Count); ++i)
    if (s && !std::strcmp(s, kSectionNames[i])) {
      out = static_cast<ConfigSection>(i);
      return true;
    }
  return false;
}

HeatCoreSystem::HeatCoreSystem(const IClock& clock)
    : bus(96, 200),
      alarms(bus, clock),
      io(bus, clock),
      sensors(bus, clock),
      heating(bus, clock),
      safety(alarms, bus, clock),
      rfRx(rfReg, bus, clock),
      rfTx(rfReg, bus, clock),
      model(bus, clock),
      display(bus, clock),
      automation(*this, bus),
      clock_(clock) {
  rfSensors.addDecoder(&nexusDecoder, false);  // unconfirmed on hardware: opt-in
  rfRx.setSensorHub(&rfSensors);
  rfSensors.setListener(
      [](void* ctx, const rf::SensorReading& r) { static_cast<HeatCoreSystem*>(ctx)->onRfSensorReading(r); }, this);
}

void HeatCoreSystem::begin() {
  if (begun_) return;
  begun_ = true;
  automation.revalidateAll();
  model.removeMissing(heating.rooms);
  Millis now = clock_.nowMs();
  lastSecond_ = now;
  lastMinuteMs_ = now;
  lastStatsSave_ = now;
  // Outputs start in their safe state; the controller decides afterwards.
  for (int i = 0; i < kNumOutputs; ++i) io.out[i].level = (safeOutputMask() >> i) & 1u;
  HC_LOGI(TAG, "HeatCore %s started: %u rooms, %u RF devices, %u rules", kFirmwareVersion,
          static_cast<unsigned>(heating.rooms.size()), static_cast<unsigned>(rfReg.rx.size()),
          static_cast<unsigned>(automation.rules().size()));
}

void HeatCoreSystem::loop() {
  if (!begun_) begin();
  Millis now = clock_.nowMs();
  lastLoop_ = now;
  rfRx.tick();
  io.tickInputs(now);
  for (int i = 0; i < 48; ++i) {
    Event e;
    if (!bus.pop(e)) break;
    processEvent(e);
  }
  if (elapsed(now, lastSecond_) >= 1000) {
    lastSecond_ = now;
    sensors.tick(now);
    display.setRoomCount(static_cast<int>(displayRooms().size()));
    display.tick(now, alarms.highest(), alarms.count());
    updateLoopback(now);
    // timed global mode (vacation/away) ends automatically
    if (heating.globalModeTimed) {
      int64_t ep = clock_.epoch();
      bool expired = heating.globalModeUntilEpoch && ep ? ep >= heating.globalModeUntilEpoch
                                                        : reached(now, heating.globalModeUntilMs);
      if (expired) {
        heating.setGlobalMode(GlobalMode::Normal, 0, now, 0, 0, EventSource::Schedule);
        markDirty(ConfigSection::Heating);
      }
    }
    LocalTime lt = clock_.local();
    if (lt.valid && lt.minute != lastMinute_) {
      lastMinute_ = lt.minute;
      Event e(EventType::TimeMinute, EventSource::Schedule);
      e.ts = now;
      e.value = static_cast<float>(lt.minuteOfDay());
      e.a = lt.weekday;
      bus.post(e);
    }
  }
  uint32_t interval = static_cast<uint32_t>(sysCfg.controlIntervalSec) * 1000u;
  if (counters.controlCycles == 0 || elapsed(now, lastControl_) >= interval) controlCycle(now);
  computeOutputs(now);
  if (elapsed(now, lastMinuteMs_) >= 60000u) {
    lastMinuteMs_ = now;
    minuteTasks(now);
  }
  if (elapsed(now, lastStatsSave_) >= 30u * 60u * 1000u) {
    lastStatsSave_ = now;
    markDirty(ConfigSection::Rf, false);
    markDirty(ConfigSection::Rules, false);
    markDirty(ConfigSection::Model, false);
  }
}

// ------------------------------------------------------------- events

void HeatCoreSystem::processEvent(const Event& e) {
  ++counters.events;
  switch (e.type) {
    case EventType::InputGesture: handleInputGesture(e); break;
    case EventType::RoomWindowChanged:
      model.noteContext(e.ref, e.value != 0.f ? "Fenster geoeffnet" : "Fenster geschlossen");
      break;
    case EventType::RoomSetpointChanged:
      model.noteContext(e.ref, e.value > 0.f ? "manuelle Sollwertaenderung" : "manuelle Aenderung beendet");
      break;
    case EventType::RoomModeChanged: model.noteContext(e.ref, "Betriebsart geaendert"); break;
    case EventType::RoomPresenceChanged:
      model.noteContext(e.ref, e.value != 0.f ? "Anwesenheit erkannt" : "keine Anwesenheit");
      break;
    case EventType::GlobalModeChanged:
      for (auto& r : heating.rooms)
        model.noteContext(r.id, (std::string("globale Betriebsart: ") + e.ref).c_str());
      break;
    case EventType::Notification:
      if (notify) notify(e.text, static_cast<int>(e.value));
      break;
    default: break;
  }
  automation.onEvent(e);
  bus.record(e);
  if (onEvent) onEvent(e);
}

void HeatCoreSystem::handleInputGesture(const Event& e) {
  if (e.source == EventSource::DigitalInput) {
    if (e.a < 1 || e.a > kNumInputs) return;
    const InputConfig& c = io.inCfg[static_cast<size_t>(e.a - 1)];
    if (c.kind == InputKind::Contact && (e.gesture == Gesture::Open || e.gesture == Gesture::Closed))
      applyFunction(c.function, c.room, e.ref, e.gesture == Gesture::Open, 0, e);
    runBindings(c.bindings, e);
    return;
  }
  if (e.source != EventSource::Rf) return;
  rf::RxDevice* d = rfReg.rxDevice(static_cast<uint16_t>(e.a));
  if (!d) return;
  char src[16];
  std::snprintf(src, sizeof(src), "rf:%u", d->id);
  uint32_t demandSec = d->supervisionSec ? d->supervisionSec * 2 : 3600;
  switch (e.gesture) {
    case Gesture::Open:
    case Gesture::Closed: applyFunction(d->function, d->room, src, e.gesture == Gesture::Open, demandSec, e); break;
    case Gesture::Unavailable: {
      bool on = d->function == rf::InputFunction::HeatDemand ? false : d->failsafeOn;
      applyFunction(d->function, d->room, src, on, demandSec, e);
      alarms.raise(std::string("rf.device.") + std::to_string(d->id), "Funkgeraet '" + d->name + "' meldet sich nicht",
                   AlarmLevel::Warning, EventSource::Rf);
      break;
    }
    case Gesture::Available: alarms.clear(std::string("rf.device.") + std::to_string(d->id)); break;
    default: break;
  }
  if (e.b >= 0) {
    if (const rf::RxButton* b = d->button(static_cast<uint8_t>(e.b))) runBindings(b->bindings, e);
  }
}

void HeatCoreSystem::applyFunction(rf::InputFunction fn, const std::string& roomId, const std::string& source, bool on,
                                   uint32_t demandSec, const Event& e) {
  if (fn == rf::InputFunction::None) return;
  Room* r = heating.room(roomId);
  if (!r) return;
  Millis now = clock_.nowMs();
  switch (fn) {
    case rf::InputFunction::Window: heating.setWindow(*r, source, on, now, e.depth, 0, e.source); break;
    case rf::InputFunction::Presence: heating.setPresence(r, on, now, e.depth, 0, e.source); break;
    case rf::InputFunction::HeatDemand: heating.setExternalDemand(*r, source, on, demandSec, now); break;
    default: break;
  }
}

void HeatCoreSystem::runBindings(const std::vector<InputBinding>& bindings, const Event& e) {
  for (const auto& b : bindings) {
    if (b.gesture != e.gesture) continue;
    ActionOrigin origin{e.source, e.depth, 0, false};
    for (const auto& a : b.actions) {
      std::string err = execute(a, origin);
      ++counters.bindingsExecuted;
      if (!err.empty()) HC_LOGW(TAG, "binding %s %s: %s", e.ref, actionTypeName(a.type), err.c_str());
    }
  }
}

// ------------------------------------------------------------ control

void HeatCoreSystem::controlCycle(Millis now) {
  float dt = counters.controlCycles == 0 ? 0.f : elapsed(now, lastControl_) / 1000.f;
  lastControl_ = now;
  ++counters.controlCycles;
  for (auto& r : heating.rooms) {
    RoomTemperature rt = sensors.roomTemperature(r.id);
    r.hasTemp = rt.valid;
    r.temperature = rt.value;
    r.tempSensor = rt.sensorId;
    r.tempFallback = rt.fallback;
  }
  safety.evaluate(heating, sensors, now);
  LocalTime lt = clock_.local();
  for (auto& r : heating.rooms) {
    heating.computeSetpoint(r, lt, safety.cfg.minSetpoint, safety.cfg.maxSetpoint, now);
    ModelAdvice adv;
    if (r.hasTemp && !r.tempFallback) adv = model.advice(r.id, r.demand, r.temperature);
    heating.updateDemand(r, adv, safety.cfg.sensorFailMode, safety.cfg.sensorFailDutyPct, safety.cfg.sensorFailCycleMin,
                         now);
  }
  heating.updateCircuits(now);
  checkCircuitFaults(now);
  // valve open fraction per room for the thermal model (actual output state)
  if (dt > 0.f) {
    for (auto& r : heating.rooms) {
      int total = 0, open = 0;
      for (int i = 0; i < kNumOutputs; ++i) {
        const Circuit& c = heating.circuits[static_cast<size_t>(i)];
        if (io.outCfg[static_cast<size_t>(i)].function != OutputFunction::Circuit || c.room != r.id) continue;
        ++total;
        if (io.out[static_cast<size_t>(i)].level != heating.circuitSafeLevel(c)) ++open;
      }
      if (!total) continue;
      auto it = std::find_if(duty_.begin(), duty_.end(), [&](const DutyAcc& d) { return d.room == r.id; });
      if (it == duty_.end()) {
        duty_.push_back({r.id, 0.f, 0.f});
        it = duty_.end() - 1;
      }
      it->openSec += dt * open / total;
      it->totalSec += dt;
    }
  }
  automation.tick();
}

void HeatCoreSystem::checkCircuitFaults(Millis now) {
  for (int i = 0; i < kNumOutputs; ++i) {
    Circuit& c = heating.circuits[static_cast<size_t>(i)];
    bool used = io.outCfg[static_cast<size_t>(i)].function == OutputFunction::Circuit && !c.room.empty();
    std::string why;
    if (used) {
      const Room* r = heating.room(c.room);
      const ThermalModel* m = model.find(c.room);
      if (!io.out[static_cast<size_t>(i)].hwOk) why = "Ausgang nicht schreibbar";
      else if (!r) why = "Raum existiert nicht";
      else if (r->sensorFail) why = "Raumtemperatur fehlt";
      else if (m && m->faultSuspected()) why = "keine Erwaermung trotz Waermeanforderung (Verdacht)";
    }
    bool fault = !why.empty();
    if (fault != c.fault) {
      c.fault = fault;
      c.faultText = why;
      std::string code = "circuit." + std::to_string(c.index);
      if (fault) {
        Event e(EventType::CircuitFault, EventSource::System);
        e.ts = now;
        e.a = c.index;
        e.setRef(c.room.c_str());
        e.setText(why.c_str());
        bus.post(e);
        alarms.raise(code, c.name + ": " + why, AlarmLevel::Warning, EventSource::System);
      } else {
        alarms.clear(code);
      }
    } else {
      c.faultText = why;
    }
  }
}

uint16_t HeatCoreSystem::safeOutputMask() const {
  uint16_t m = 0;
  for (int i = 0; i < kNumOutputs; ++i) {
    bool lvl = false;
    if (io.outCfg[static_cast<size_t>(i)].function == OutputFunction::Circuit)
      lvl = heating.circuitSafeLevel(heating.circuits[static_cast<size_t>(i)]);
    if (lvl) m |= static_cast<uint16_t>(1u << i);
  }
  return m;
}

void HeatCoreSystem::computeOutputs(Millis now) {
  for (int i = 0; i < kNumOutputs; ++i) {
    size_t k = static_cast<size_t>(i);
    const OutputConfig& cfg = io.outCfg[k];
    OutputState& st = io.out[k];
    if (st.manualActive && reached(now, st.manualUntil)) io.clearManual(i);
    if (requestTimed_[k] && st.request && reached(now, requestUntil_[k])) {
      io.setRequest(i, false, now);
      requestTimed_[k] = false;
    }
    if (cfg.function == OutputFunction::General && cfg.maxOnSec && st.request &&
        elapsed(now, st.requestOnMs) > cfg.maxOnSec * 1000u)
      io.setRequest(i, false, now);
    bool requested = false, safeLevel = false, isCircuit = false, roomOT = false;
    switch (cfg.function) {
      case OutputFunction::Disabled:
        io.clearManual(i);
        break;
      case OutputFunction::Circuit: {
        const Circuit& c = heating.circuits[k];
        isCircuit = true;
        safeLevel = heating.circuitSafeLevel(c);
        const Room* r = c.room.empty() ? nullptr : heating.room(c.room);
        requested = r ? heating.circuitOutputLevel(c) : safeLevel;
        roomOT = r && r->overTemp;
        break;
      }
      case OutputFunction::General: requested = st.request; break;
    }
    if (st.manualActive) requested = st.manualState;
    std::string reason;
    bool level = safety.filterOutput(i, requested, isCircuit, safeLevel, roomOT, st, now, reason);
    st.blocked = reason;
    io.applyLevel(i, level, now);
  }
}

void HeatCoreSystem::minuteTasks(Millis now) {
  float outdoor = 0.f;
  bool haveOutdoor = sysCfg.outdoorFromSensor && sensors.firstOfRole(SensorRole::Outdoor, outdoor);
  for (auto& r : heating.rooms) {
    ThermalInput in;
    in.tempValid = r.hasTemp;
    in.temp = r.temperature;
    in.tempFallback = r.tempFallback;
    in.outdoorValid = haveOutdoor;
    in.outdoor = outdoor;
    auto it = std::find_if(duty_.begin(), duty_.end(), [&](const DutyAcc& d) { return d.room == r.id; });
    if (it == duty_.end() || it->totalSec <= 0.f) continue;  // room without circuits: nothing to learn
    in.duty = it->openSec / it->totalSec;
    it->openSec = it->totalSec = 0.f;
    in.windowOpen = r.windowOpen();
    in.manualSetpoint = r.manualActive;
    bool circuitFault = false;
    for (int i = 0; i < kNumOutputs; ++i) {
      const Circuit& c = heating.circuits[static_cast<size_t>(i)];
      if (c.room != r.id) continue;
      if (io.out[static_cast<size_t>(i)].manualActive) in.manualOutput = true;
      if (!io.out[static_cast<size_t>(i)].hwOk) circuitFault = true;
    }
    in.fault = r.overTemp || r.sensorFail || safety.flowTripped() || circuitFault;
    model.observe(r.id, in, now);
  }
}

// ------------------------------------------------------------ hardware

void HeatCoreSystem::reportOutputWrite(bool ok) {
  if (ok) {
    if (consecutiveWriteErrors_ >= 3) alarms.clear("hw.outputs");
    consecutiveWriteErrors_ = 0;
    for (auto& o : io.out) o.hwOk = true;
    return;
  }
  ++platform.outputWriteErrors;
  if (++consecutiveWriteErrors_ == 3) {
    for (auto& o : io.out) o.hwOk = false;
    alarms.raise("hw.outputs", "Ausgangs-Expander antwortet nicht (I2C)", AlarmLevel::Critical, EventSource::System);
  }
}

void HeatCoreSystem::reportInputs(uint16_t activeMask, bool ok) {
  if (!ok) {
    ++platform.inputReadErrors;
    return;
  }
  io.onInputs(activeMask, clock_.nowMs());
}

void HeatCoreSystem::reportRtl433Line(const char* line) {
  rf::SensorReading r;
  std::string err;
  ++platform.rs485Lines;
  if (!rf::parseRtl433Json(line, r, err)) {
    ++platform.rs485Errors;
    return;
  }
  rfSensors.injectExternal(r, clock_.nowMs());
}

void HeatCoreSystem::onRfSensorReading(const rf::SensorReading& r) {
  std::string key = r.key();
  sensors.updateExtended(key, r.temperatureC, r.hasHumidity, r.humidity, r.batteryOk, r.quality, clock_.nowMs());
  const Sensor* s = sensors.find(key);
  if (s && r.batteryOk == 0) alarms.raise("sensor.battery." + key, "Batterie schwach: " + s->name, AlarmLevel::Info,
                                          EventSource::Sensor);
  else if (s && r.batteryOk == 1) alarms.clear("sensor.battery." + key);
}

bool HeatCoreSystem::takeTxJob(rf::TxJob& job, std::vector<rf::Pulse>& pulses) {
  if (!rfTx.takeJob(job, pulses)) return false;
  uint32_t air = rf::transmissionDurationUs(pulses) / 1000u;
  rfRx.suppressFor(air + 150, job.code, job.bits);
  return true;
}

void HeatCoreSystem::completeTxJob(const rf::TxJob& job, bool ok, uint32_t airtimeMs, const char* reason) {
  rfTx.complete(job, ok, airtimeMs, reason);
  markDirty(ConfigSection::Rf, false);
}

std::string HeatCoreSystem::startRfLoopback() {
  if (loopback.running) return "Selbsttest laeuft bereits";
  uint32_t code = 0xA50000u | (clock_.nowMs() & 0xFFFFu);
  std::string err = rfTx.enqueueRaw(code, 24, 350, 6, "Selbsttest Funkstrecke", EventSource::Web, true, &loopbackJob_);
  if (!err.empty()) return err;
  loopback = LoopbackResult{};
  loopback.running = true;
  loopbackMark_ = rfRx.stats().echoes;
  loopbackStart_ = clock_.nowMs();
  return std::string();
}

void HeatCoreSystem::updateLoopback(Millis now) {
  if (!loopback.running) return;
  for (auto& h : rfTx.history()) {
    if (h.jobId != loopbackJob_) continue;
    if (elapsed(now, h.ms) < 300) return;  // let the receiver finish the last frames
    loopback.running = false;
    loopback.done = true;
    loopback.echoes = rfRx.echoesSince(loopbackMark_);
    loopback.ok = h.ok && loopback.echoes >= 2;
    loopback.detail = !h.ok ? "Senden fehlgeschlagen: " + h.reason
                            : (loopback.ok ? std::to_string(loopback.echoes) + " von 6 Rahmen empfangen"
                                           : "Eigene Aussendung nicht empfangen (Empfaenger/Antenne pruefen)");
    return;
  }
  if (elapsed(now, loopbackStart_) > 15000) {
    loopback.running = false;
    loopback.done = true;
    loopback.ok = false;
    loopback.detail = "Zeitueberschreitung - Sender nicht bereit";
  }
}

// --------------------------------------------------------------- display

std::vector<std::string> HeatCoreSystem::displayRooms() const {
  std::vector<std::string> ids;
  if (!display.cfg.rooms.empty()) {
    for (auto& id : display.cfg.rooms)
      if (heating.room(id)) ids.push_back(id);
  } else {
    for (auto& r : heating.rooms)
      if (r.showOnDisplay) ids.push_back(r.id);
  }
  return ids;
}

void HeatCoreSystem::buildDisplaySnapshot(DisplaySnapshot& s) const {
  s = DisplaySnapshot{};
  s.deviceName = sysCfg.deviceName;
  s.ip = platform.ip;
  s.network = platform.network;
  LocalTime lt = clock_.local();
  s.timeValid = lt.valid;
  if (lt.valid) {
    char b[24];
    std::snprintf(b, sizeof(b), "%02d:%02d", lt.hour, lt.minute);
    s.time = b;
    std::snprintf(b, sizeof(b), "%02d.%02d.%04d", lt.day, lt.month, lt.year);
    s.date = b;
  }
  for (int i = 0; i < kNumOutputs; ++i) {
    const Circuit& c = heating.circuits[static_cast<size_t>(i)];
    auto& cell = s.circuits[static_cast<size_t>(i)];
    cell.used = io.outCfg[static_cast<size_t>(i)].function == OutputFunction::Circuit && !c.room.empty();
    cell.open = cell.used && io.out[static_cast<size_t>(i)].level != heating.circuitSafeLevel(c);
    cell.demand = c.request;
    cell.fault = c.fault;
    if (cell.open) ++s.activeCircuits;
  }
  s.sensorsTotal = static_cast<int>(sensors.sensors().size());
  s.sensorsOk = static_cast<int>(sensors.countAvailable());
  s.alarms = alarms.list();
  for (auto& id : displayRooms()) {
    const Room* r = heating.room(id);
    if (!r) continue;
    DisplaySnapshot::RoomLine l;
    l.id = r->id;
    l.name = r->name;
    l.hasTemp = r->hasTemp;
    l.temp = r->temperature;
    l.setpoint = r->setpoint;
    l.demand = r->demand;
    l.window = r->windowOpen();
    l.reason = r->reason;
    for (int i = 0; i < kNumOutputs; ++i) {
      const Circuit& c = heating.circuits[static_cast<size_t>(i)];
      if (c.room != r->id) continue;
      if (s.circuits[static_cast<size_t>(i)].open) l.valveOpen = true;
      if (c.fault) l.fault = true;
    }
    if (r->sensorFail || r->overTemp) l.fault = true;
    s.rooms.push_back(l);
    if (const ThermalModel* m = model.find(r->id)) {
      DisplaySnapshot::ModelInfo mi;
      mi.room = r->name;
      mi.status = modelStatusName(m->status());
      mi.trendKh = m->trendKh();
      mi.delayMin = m->delayMin();
      mi.delayStd = m->delayStdMin();
      Prediction p = m->predict(60.f, r->demand ? 1.f : 0.f);
      mi.predValid = p.valid;
      mi.predMean = p.mean;
      mi.predStd = p.stddev;
      mi.gain = m->heatingGain();
      s.models.push_back(mi);
    }
  }
  s.cpuPct = platform.cpuPct;
  s.freeHeap = platform.freeHeap;
  s.freePsram = platform.freePsram;
  s.uptimeSec = platform.uptimeSec;
  s.firmware = kFirmwareVersion;
  s.sd = platform.sd;
  s.mqtt = platform.mqtt;
  s.netDetail = platform.netDetail;
}

// ------------------------------------------------------------ MQTT

std::string HeatCoreSystem::baseTopic() const {
  return mqttCfg.baseTopic.empty() ? "heatcore/" + platform.deviceId : mqttCfg.baseTopic;
}

void HeatCoreSystem::mqttInbound(const std::string& suffix, const std::string& payload) {
  Event e(EventType::MqttMessage, EventSource::Mqtt);
  e.ts = clock_.nowMs();
  e.setRef(suffix.c_str());
  e.setText(payload.c_str());
  bus.post(e);
}

// ---------------------------------------------------------- actions

bool HeatCoreSystem::outputAllowedFor(int n, EventSource src) const {
  if (n < 1 || n > kNumOutputs) return false;
  const OutputConfig& c = io.outCfg[static_cast<size_t>(n - 1)];
  if (c.function != OutputFunction::General) return false;
  if (src == EventSource::Web) return true;
  if (src == EventSource::Mqtt) return c.haAllowed;
  return c.automationAllowed;  // automation rules, DI and RF bindings
}

std::string HeatCoreSystem::checkAction(const Action& a, const ActionOrigin& origin) const {
  EventSource src = origin.source;
  auto needRoom = [&]() -> std::string { return heating.room(a.room) ? std::string() : "Raum unbekannt: " + a.room; };
  switch (a.type) {
    case ActionType::SetSetpoint: {
      std::string e = needRoom();
      if (!e.empty()) return e;
      if (!std::isfinite(a.value) || a.value < safety.cfg.minSetpoint || a.value > safety.cfg.maxSetpoint) {
        char b[64];
        std::snprintf(b, sizeof(b), "Sollwert ausserhalb %.1f..%.1f C", safety.cfg.minSetpoint, safety.cfg.maxSetpoint);
        return b;
      }
      return std::string();
    }
    case ActionType::AdjustSetpoint: {
      std::string e = needRoom();
      if (!e.empty()) return e;
      if (!std::isfinite(a.value) || a.value == 0.f || std::fabs(a.value) > 5.f) return "Aenderung 0..+-5 K";
      return std::string();
    }
    case ActionType::SetRoomMode: {
      std::string e = needRoom();
      if (!e.empty()) return e;
      RoomMode m;
      return roomModeFromName(a.text.c_str(), m) ? std::string() : "Raumbetriebsart unbekannt: " + a.text;
    }
    case ActionType::SetGlobalMode: {
      GlobalMode m;
      if (!globalModeFromName(a.text.c_str(), m)) return "Betriebsart unbekannt: " + a.text;
      if (a.durationSec > 60u * 86400u) return "Dauer max. 60 Tage";
      return std::string();
    }
    case ActionType::ActivateRoomGroup: {
      auto g = heating.groups();
      if (std::find(g.begin(), g.end(), a.target) == g.end()) return "Raumgruppe unbekannt: " + a.target;
      RoomMode m;
      return roomModeFromName(a.text.c_str(), m) ? std::string() : "Raumbetriebsart unbekannt: " + a.text;
    }
    case ActionType::SetOutput:
      if (a.index < 1 || a.index > kNumOutputs) return "Ausgang 1..16";
      if (!outputAllowedFor(a.index, src))
        return "Ausgang " + std::to_string(a.index) +
               " ist kein freigegebener allgemeiner Ausgang (Heizkreise/Sicherheitsausgaenge gesperrt)";
      return std::string();
    case ActionType::RfSend: {
      bool restricted = src != EventSource::Web && src != EventSource::Mqtt;
      std::string err;
      auto t = rfReg.resolveTx(a.target, restricted, &err);
      if (t.empty()) return err;
      if (src == EventSource::Mqtt)
        for (auto& x : t)
          if (!x.command->haExposed) return "Befehl ist nicht fuer Home Assistant freigegeben";
      if (!rfTx.config().enabled) return "Sender deaktiviert";
      return std::string();
    }
    case ActionType::MqttPublish:
      if (!validTopic(a.target)) return "MQTT-Thema 1..23 Zeichen ohne Platzhalter/Leerzeichen";
      if (a.text.size() > 200) return "Nutzdaten max. 200 Zeichen";
      return std::string();
    case ActionType::Notify: return a.text.empty() || a.text.size() > 100 ? "Meldung 1..100 Zeichen" : std::string();
    case ActionType::DisplayPage: return a.index >= 1 && a.index <= kNumPages ? std::string() : "Seite 1..5";
    case ActionType::DisplayedRoomAdjust:
      return std::isfinite(a.value) && a.value != 0.f && std::fabs(a.value) <= 5.f ? std::string() : "Aenderung 0..+-5 K";
    case ActionType::RaiseAlarm:
    case ActionType::ClearAlarm:
      if (!AlarmManager::isUserCode(a.target) || a.target.size() > 23)
        return "Alarmcode muss mit 'user.' beginnen (max. 23 Zeichen) - Systemalarme sind geschuetzt";
      return std::string();
    case ActionType::WindowState: return needRoom();
    case ActionType::Presence: return a.room.empty() ? std::string() : needRoom();
    case ActionType::HeatDemand: {
      std::string e = needRoom();
      if (!e.empty()) return e;
      return a.durationSec <= 4u * 3600u ? std::string() : "Anforderungsdauer max. 4 h";
    }
    case ActionType::DisplayNext:
    case ActionType::DisplayPrev:
    case ActionType::DisplayWake:
    case ActionType::DisplayRoomNext: return std::string();
    default: return "Aktion nicht unterstuetzt";
  }
}

std::string HeatCoreSystem::validateAction(const Action& a) const {
  return checkAction(a, ActionOrigin{EventSource::Automation, 1, 0, true});
}

std::string HeatCoreSystem::validateBindings(const std::vector<InputBinding>& b, EventSource src) const {
  for (auto& x : b)
    for (auto& a : x.actions) {
      std::string e = checkAction(a, ActionOrigin{src, 0, 0, true});
      if (!e.empty()) return AutomationEngine::describeAction(a) + ": " + e;
    }
  return std::string();
}

std::string HeatCoreSystem::execute(const Action& a, const ActionOrigin& origin) {
  std::string err = checkAction(a, origin);
  if (!err.empty()) {
    ++counters.actionsRejected;
    return err;
  }
  if (origin.dryRun) return std::string();
  Millis now = clock_.nowMs();
  EventSource src = origin.source;
  uint8_t depth = origin.depth;
  uint16_t rule = origin.ruleId;
  float minSp = safety.cfg.minSetpoint, maxSp = safety.cfg.maxSetpoint;
  switch (a.type) {
    case ActionType::SetSetpoint:
      heating.setManualSetpoint(*heating.room(a.room), a.value, minSp, maxSp, now, depth, rule, src);
      break;
    case ActionType::AdjustSetpoint:
      heating.adjustSetpoint(*heating.room(a.room), a.value, minSp, maxSp, now, depth, rule, src);
      break;
    case ActionType::SetRoomMode: {
      RoomMode m;
      roomModeFromName(a.text.c_str(), m);
      heating.setRoomMode(*heating.room(a.room), m, depth, rule, src);
      markDirty(ConfigSection::Heating);
      break;
    }
    case ActionType::SetGlobalMode: {
      GlobalMode m;
      globalModeFromName(a.text.c_str(), m);
      heating.setGlobalMode(m, a.durationSec, now, depth, rule, src);
      markDirty(ConfigSection::Heating);
      break;
    }
    case ActionType::ActivateRoomGroup: {
      RoomMode m;
      roomModeFromName(a.text.c_str(), m);
      for (Room* r : heating.roomsInGroup(a.target)) heating.setRoomMode(*r, m, depth, rule, src);
      markDirty(ConfigSection::Heating);
      break;
    }
    case ActionType::SetOutput: return requestOutput(a.index, a.value != 0.f, a.durationSec, src);
    case ActionType::RfSend:
      return rfTx.enqueue(a.target, src, depth, rule, src != EventSource::Web && src != EventSource::Mqtt);
    case ActionType::MqttPublish:
      if (!mqttCfg.enabled) return "MQTT deaktiviert";
      if (mqttPublish) mqttPublish("out/" + a.target, a.text, false);
      break;
    case ActionType::Notify: {
      Event e(EventType::Notification, src);
      e.ts = now;
      e.depth = depth;
      e.ruleId = rule;
      e.setText(a.text.c_str());
      e.value = a.value;
      bus.post(e);
      break;
    }
    case ActionType::DisplayPage: display.select(a.index); break;
    case ActionType::DisplayNext: display.next(); break;
    case ActionType::DisplayPrev: display.prev(); break;
    case ActionType::DisplayWake: display.wake(); break;
    case ActionType::DisplayRoomNext: display.roomNext(); break;
    case ActionType::DisplayedRoomAdjust: {
      auto ids = displayRooms();
      if (ids.empty()) return "Kein Raum auf dem Display";
      Room* r = heating.room(ids[static_cast<size_t>(display.roomIndex()) % ids.size()]);
      if (!r) return "Raum unbekannt";
      heating.adjustSetpoint(*r, a.value, minSp, maxSp, now, depth, rule, src);
      display.wake();
      break;
    }
    case ActionType::RaiseAlarm: {
      int lvl = static_cast<int>(a.value);
      AlarmLevel l = lvl >= 2 ? AlarmLevel::Critical : (lvl <= 0 ? AlarmLevel::Info : AlarmLevel::Warning);
      alarms.raise(a.target, a.text.empty() ? a.target : a.text, l, src, depth, rule);
      break;
    }
    case ActionType::ClearAlarm: alarms.clear(a.target, depth, rule); break;
    case ActionType::WindowState:
      heating.setWindow(*heating.room(a.room), "action", a.value != 0.f, now, depth, rule, src);
      break;
    case ActionType::Presence:
      heating.setPresence(a.room.empty() ? nullptr : heating.room(a.room), a.value != 0.f, now, depth, rule, src);
      break;
    case ActionType::HeatDemand:
      heating.setExternalDemand(*heating.room(a.room), "action", a.value != 0.f, a.durationSec, now);
      break;
    default: return "Aktion nicht unterstuetzt";
  }
  return std::string();
}

std::string HeatCoreSystem::manualOutput(int n, bool on, uint32_t durationSec, EventSource src) {
  if (n < 1 || n > kNumOutputs) return "Ausgang 1..16";
  if (io.outCfg[static_cast<size_t>(n - 1)].function == OutputFunction::Disabled) return "Ausgang deaktiviert";
  if (src != EventSource::Web) return "Handbetrieb nur ueber das lokale Webinterface";
  uint32_t d = safety.clampOverrideSec(durationSec, src);
  io.setManual(n - 1, on, clock_.nowMs() + d * 1000u, src);
  return std::string();
}

void HeatCoreSystem::clearManualOutput(int n) {
  if (n >= 1 && n <= kNumOutputs) io.clearManual(n - 1);
}

std::string HeatCoreSystem::requestOutput(int n, bool on, uint32_t durationSec, EventSource src) {
  if (!outputAllowedFor(n, src)) return "Ausgang nicht freigegeben";
  size_t k = static_cast<size_t>(n - 1);
  Millis now = clock_.nowMs();
  if (on) {
    bool timed = src != EventSource::Web || durationSec > 0;
    uint32_t d = src == EventSource::Web ? durationSec : safety.clampOverrideSec(durationSec, src);
    requestTimed_[k] = timed;
    requestUntil_[k] = now + d * 1000u;
  } else {
    requestTimed_[k] = false;
  }
  io.setRequest(n - 1, on, now);
  return std::string();
}

std::string HeatCoreSystem::runRule(uint16_t id, EventSource src) {
  const Rule* r = automation.rule(id);
  if (!r) return "Regel nicht gefunden";
  bool manual = false;
  for (auto& t : r->triggers) manual |= t.type == TriggerType::Manual;
  if (!manual) return "Regel hat keinen manuellen Trigger";
  if (!r->enabled) return "Regel ist deaktiviert";
  Event e(EventType::ManualTrigger, src);
  e.ts = clock_.nowMs();
  e.a = id;
  bus.post(e);
  return std::string();
}

// ------------------------------------------------ IAutomationContext

bool HeatCoreSystem::temperature(const std::string& ref, float& v) const {
  if (ref.rfind("sensor:", 0) == 0) {
    const Sensor* s = sensors.find(ref.substr(7));
    if (!s || !s->available()) return false;
    v = s->value;
    return true;
  }
  const Room* r = heating.room(ref);
  if (!r || !r->hasTemp) return false;
  v = r->temperature;
  return true;
}

bool HeatCoreSystem::setpoint(const std::string& room, float& v) const {
  const Room* r = heating.room(room);
  if (!r) return false;
  v = r->setpoint;
  return true;
}

std::string HeatCoreSystem::roomMode(const std::string& room) const {
  const Room* r = heating.room(room);
  return r ? roomModeName(r->mode) : "";
}

int HeatCoreSystem::presence(const std::string& room) const {
  if (room.empty()) return heating.homePresent() ? 1 : 0;
  const Room* r = heating.room(room);
  return r ? r->presence : -1;
}

bool HeatCoreSystem::windowOpen(const std::string& room) const {
  const Room* r = heating.room(room);
  return r && r->windowOpen();
}

bool HeatCoreSystem::sensorAvailable(const std::string& id) const {
  const Sensor* s = sensors.find(id);
  return s && s->available();
}

int HeatCoreSystem::inputState(const std::string& ref) const {
  if (ref.rfind("di:", 0) == 0) {
    int n = std::atoi(ref.c_str() + 3);
    if (n < 1 || n > kNumInputs || !io.in[static_cast<size_t>(n - 1)].known) return -1;
    return io.inputLogical(n - 1) ? 1 : 0;
  }
  if (ref.rfind("rf:", 0) == 0) {
    const rf::RxDevice* d = rfReg.rxDevice(static_cast<uint16_t>(std::atoi(ref.c_str() + 3)));
    if (!d || !d->available) return -1;
    return d->state;
  }
  return -1;
}

bool HeatCoreSystem::outputOn(int output) const {
  return output >= 1 && output <= kNumOutputs && io.out[static_cast<size_t>(output - 1)].level;
}

bool HeatCoreSystem::groupState(const std::string& group, const std::string& state) const {
  bool any = false, allClosed = true, found = false;
  for (auto& r : heating.rooms) {
    if (std::find(r.groups.begin(), r.groups.end(), group) == r.groups.end()) continue;
    found = true;
    any |= r.demand;
    allClosed &= !r.windowOpen();
  }
  if (!found) return false;
  if (state == "any_heating") return any;
  if (state == "none_heating") return !any;
  if (state == "all_window_closed") return allClosed;
  return false;
}

bool HeatCoreSystem::refExists(const std::string& kind, const std::string& ref) const {
  if (kind == "room") return heating.room(ref) != nullptr;
  if (kind == "sensor") return sensors.find(ref) != nullptr;
  if (kind == "group") {
    auto g = heating.groups();
    return std::find(g.begin(), g.end(), ref) != g.end();
  }
  if (kind == "circuit") {
    int n = std::atoi(ref.c_str());
    return n >= 1 && n <= kNumOutputs;
  }
  if (kind == "input") {
    if (ref.rfind("di:", 0) == 0) {
      int n = std::atoi(ref.c_str() + 3);
      return n >= 1 && n <= kNumInputs && io.inCfg[static_cast<size_t>(n - 1)].kind != InputKind::Disabled;
    }
    if (ref.rfind("rf:", 0) == 0) {
      int dev = 0, btn = -1;
      if (std::sscanf(ref.c_str() + 3, "%d/%d", &dev, &btn) < 1) return false;
      const rf::RxDevice* d = rfReg.rxDevice(static_cast<uint16_t>(dev));
      if (!d) return false;
      return btn < 0 || d->button(static_cast<uint8_t>(btn)) != nullptr;
    }
  }
  return false;
}

void HeatCoreSystem::systemAlarm(const std::string& code, const std::string& message, bool active) {
  if (active) alarms.raise(code, message, AlarmLevel::Warning, EventSource::Automation);
  else alarms.clear(code);
}

// ---------------------------------------------------------- persistence

void HeatCoreSystem::markDirty(ConfigSection s, bool important) {
  size_t i = static_cast<size_t>(s);
  Millis now = clock_.nowMs();
  if (!dirty_[i]) dirtySince_[i] = now;
  dirty_[i] = true;
  if (important && !dirtyImportant_[i]) {
    dirtyImportant_[i] = true;
    dirtySince_[i] = now;
  }
}

std::vector<ConfigSection> HeatCoreSystem::takeDirty() {
  std::vector<ConfigSection> out;
  Millis now = clock_.nowMs();
  for (size_t i = 0; i < dirty_.size(); ++i) {
    if (!dirty_[i]) continue;
    uint32_t wait = dirtyImportant_[i] ? 3000u : 15u * 60u * 1000u;
    if (elapsed(now, dirtySince_[i]) < wait) continue;
    dirty_[i] = dirtyImportant_[i] = false;
    out.push_back(static_cast<ConfigSection>(i));
  }
  return out;
}

void HeatCoreSystem::sectionToJson(ConfigSection s, JsonObject o, bool includeSecrets) const {
  o["version"] = 1;
  switch (s) {
    case ConfigSection::System: systemConfigToJson(sysCfg, o, includeSecrets); break;
    case ConfigSection::Network: networkConfigToJson(netCfg, o, includeSecrets); break;
    case ConfigSection::Mqtt: mqttConfigToJson(mqttCfg, o, includeSecrets); break;
    case ConfigSection::Io: io.toJson(o, false); break;
    case ConfigSection::Heating: heating.toJson(o, false); break;
    case ConfigSection::Sensors:
      o["autoAdoptOneWire"] = sensors.autoAdoptOneWire;
      sensors.toJson(o["sensors"].to<JsonArray>(), false);
      break;
    case ConfigSection::Safety: safety.toJson(o, false); break;
    case ConfigSection::Rf: {
      rfReg.toJson(o, true);
      JsonObject st = o["settings"].to<JsonObject>();
      st["rxEnabled"] = rfRx.enabled;
      JsonObject codec = st["codec"].to<JsonObject>();
      codec["glitchUs"] = const_cast<rf::RfReceiver&>(rfRx).codec().glitchUs;
      codec["maxTimingError"] = const_cast<rf::RfReceiver&>(rfRx).codec().maxTimingError;
      JsonObject tx = st["tx"].to<JsonObject>();
      tx["enabled"] = rfTx.config().enabled;
      tx["dutyCyclePct"] = rfTx.config().dutyCyclePct;
      tx["minIntervalMs"] = rfTx.config().minIntervalMs;
      tx["defaultTimeoutMs"] = rfTx.config().defaultTimeoutMs;
      JsonObject dec = st["decoders"].to<JsonObject>();
      for (auto& d : rfSensors.decoders()) dec[d.decoder->name()] = d.enabled;
      break;
    }
    case ConfigSection::Display: display.toJson(o, false); break;
    case ConfigSection::Rules:
      o["maxDepth"] = automation.maxDepth;
      automation.toJson(o["rules"].to<JsonArray>(), false);
      break;
    case ConfigSection::Model: model.toJson(o, false); break;
    default: break;
  }
}

std::string HeatCoreSystem::sectionFromJson(ConfigSection s, JsonObjectConst o) {
  std::string err;
  switch (s) {
    case ConfigSection::System: err = systemConfigFromJson(sysCfg, o); break;
    case ConfigSection::Network: err = networkConfigFromJson(netCfg, o); break;
    case ConfigSection::Mqtt: err = mqttConfigFromJson(mqttCfg, o); break;
    case ConfigSection::Io: {
      IoManager probe(bus, clock_);
      probe.outCfg = io.outCfg;
      probe.inCfg = io.inCfg;
      probe.aiCfg = io.aiCfg;
      err = probe.fromJson(o);
      if (err.empty())
        for (int i = 0; i < kNumInputs && err.empty(); ++i) {
          err = validateBindings(probe.inCfg[static_cast<size_t>(i)].bindings, EventSource::DigitalInput);
          if (!err.empty()) err = "Eingang " + std::to_string(i + 1) + ": " + err;
          const InputConfig& ic = probe.inCfg[static_cast<size_t>(i)];
          if (err.empty() && ic.function != rf::InputFunction::None && !heating.room(ic.room))
            err = "Eingang " + std::to_string(i + 1) + ": Raum unbekannt";
        }
      if (err.empty()) {
        io.outCfg = probe.outCfg;
        io.inCfg = probe.inCfg;
        io.aiCfg = probe.aiCfg;
      }
      break;
    }
    case ConfigSection::Heating:
      err = heating.fromJson(o);
      if (err.empty()) model.removeMissing(heating.rooms);
      break;
    case ConfigSection::Sensors:
      sensors.autoAdoptOneWire = o["autoAdoptOneWire"] | false;
      err = sensors.fromJson(o["sensors"].as<JsonArrayConst>());
      break;
    case ConfigSection::Safety: err = safety.fromJson(o); break;
    case ConfigSection::Rf: {
      rf::RfRegistry probe;
      err = probe.fromJson(o);
      if (err.empty()) {
        for (auto& d : probe.rx) {
          for (auto& b : d.buttons) {
            err = validateBindings(b.bindings, EventSource::Rf);
            if (!err.empty()) {
              err = d.name + "/" + b.name + ": " + err;
              break;
            }
          }
          if (err.empty() && d.function != rf::InputFunction::None && !heating.room(d.room))
            err = d.name + ": Raum unbekannt";
          if (!err.empty()) break;
        }
      }
      if (!err.empty()) break;
      // keep runtime state of existing devices
      for (auto& d : probe.rx)
        if (const rf::RxDevice* old = rfReg.rxDevice(d.id)) {
          d.state = old->state;
          d.available = old->available;
          d.stats.seen = old->stats.seen;
          d.stats.lastSeenMs = old->stats.lastSeenMs;
        }
      rfReg.rx = std::move(probe.rx);
      rfReg.tx = std::move(probe.tx);
      rfReg.ignored = std::move(probe.ignored);
      rfReg.touch();
      JsonObjectConst st = o["settings"];
      if (!st.isNull()) {
        rfRx.enabled = st["rxEnabled"] | true;
        rfRx.codec().glitchUs = st["codec"]["glitchUs"] | rfRx.codec().glitchUs;
        rfRx.codec().maxTimingError = st["codec"]["maxTimingError"] | rfRx.codec().maxTimingError;
        rf::TxConfig& tc = rfTx.config();
        tc.enabled = st["tx"]["enabled"] | tc.enabled;
        float duty = st["tx"]["dutyCyclePct"] | tc.dutyCyclePct;
        tc.dutyCyclePct = duty < 0.01f ? 0.01f : (duty > 10.f ? 10.f : duty);
        tc.minIntervalMs = st["tx"]["minIntervalMs"] | tc.minIntervalMs;
        tc.defaultTimeoutMs = st["tx"]["defaultTimeoutMs"] | tc.defaultTimeoutMs;
        for (JsonPairConst kv : st["decoders"].as<JsonObjectConst>())
          rfSensors.setEnabled(kv.key().c_str(), kv.value() | false);
      }
      break;
    }
    case ConfigSection::Display: err = display.fromJson(o); break;
    case ConfigSection::Rules:
      automation.maxDepth = o["maxDepth"] | automation.maxDepth;
      if (automation.maxDepth < 1) automation.maxDepth = 1;
      if (automation.maxDepth > 5) automation.maxDepth = 5;
      err = automation.fromJson(o["rules"].as<JsonArrayConst>());
      break;
    case ConfigSection::Model: model.fromJson(o); break;
    default: err = "unbekannter Abschnitt";
  }
  if (err.empty()) {
    if (begun_ && (s == ConfigSection::Heating || s == ConfigSection::Rf || s == ConfigSection::Sensors ||
                   s == ConfigSection::Io))
      automation.revalidateAll();  // references may have changed
    markDirty(s);
  }
  return err;
}

}  // namespace hc
